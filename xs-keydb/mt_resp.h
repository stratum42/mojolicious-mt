/* mt_resp.h - Zero-copy C/SIMD RESP2/RESP3 encoder, streaming parser,
 * and per-worker TCP/Unix connection pool for Mojo::MT::KeyDB::XS.
 *
 * Strictly zero mutable global state: all connection state lives in MY_CXT.
 */

#ifndef MT_RESP_H
#define MT_RESP_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "mt_resp_simd.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MT_KEYDB_MAX_CONNS   32
#define MT_RESP_MAX_DEPTH    64
#define MT_RESP_INIT_BUF_CAP 4096

/* --------------------------------------------------------------------------
 * Dynamic byte buffer (per-connection, reused across commands)
 * -------------------------------------------------------------------------- */

typedef struct {
  char  *data;
  size_t len;
  size_t cap;
} mt_resp_buf_t;

static inline void mt_resp_buf_init(mt_resp_buf_t *b) {
  b->data = NULL;
  b->len  = 0;
  b->cap  = 0;
}

static inline void mt_resp_buf_free(mt_resp_buf_t *b) {
  if (b->data) {
    free(b->data);
    b->data = NULL;
  }
  b->len = 0;
  b->cap = 0;
}

static inline int mt_resp_buf_reserve(mt_resp_buf_t *b, size_t extra) {
  size_t need = b->len + extra;
  if (need <= b->cap) return 1;
  size_t ncap = b->cap ? b->cap : MT_RESP_INIT_BUF_CAP;
  while (ncap < need) {
    if (ncap > (SIZE_MAX >> 1)) {
      ncap = need;
      break;
    }
    ncap <<= 1;
  }
  char *p = (char *)realloc(b->data, ncap);
  if (!p) return 0;
  b->data = p;
  b->cap  = ncap;
  return 1;
}

/* --------------------------------------------------------------------------
 * Fast integer formatting and parsing
 * -------------------------------------------------------------------------- */

static inline size_t mt_u64_len(uint64_t v) {
  size_t n = 1;
  while (v >= 10) { v /= 10; n++; }
  return n;
}

static inline char *mt_write_u64(char *dst, uint64_t v) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v > 0);
  while (n > 0) {
    *dst++ = tmp[--n];
  }
  return dst;
}

static inline int mt_parse_i64(const char *s, size_t len, int64_t *out) {
  if (len == 0) return 0;
  size_t i = 0;
  int neg = 0;
  if (s[0] == '-') {
    neg = 1;
    i = 1;
  } else if (s[0] == '+') {
    i = 1;
  }
  if (i >= len) return 0;
  uint64_t acc = 0;
  for (; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < '0' || c > '9') return 0;
    uint64_t d = (uint64_t)(c - '0');
    if (acc > (UINT64_MAX - d) / 10) return 0;
    acc = acc * 10 + d;
  }
  if (neg) {
    if (acc > (uint64_t)INT64_MAX + 1ULL) return 0;
    *out = (acc == (uint64_t)INT64_MAX + 1ULL) ? INT64_MIN : -(int64_t)acc;
  } else {
    if (acc > (uint64_t)INT64_MAX) return 0;
    *out = (int64_t)acc;
  }
  return 1;
}

/* --------------------------------------------------------------------------
 * Multi-arch SIMD CRLF scanner (via mt_resp_find_byte)
 * Returns offset of '\r' in buf[start..len-1] where buf[offset+1] == '\n',
 * or -1 if not found.
 * -------------------------------------------------------------------------- */

static inline ssize_t mt_resp_find_crlf(const char *buf, size_t len, size_t start) {
  if (len < 2 || start + 1 >= len) return -1;
  size_t i = start;

  while (i + 1 < len) {
    const unsigned char *p = mt_resp_find_byte(
      (const unsigned char *)(buf + i), len - 1 - i, (unsigned char)'\r');
    if (!p) return -1;
    size_t off = (size_t)((const char *)p - buf);
    if (buf[off + 1] == '\n') return (ssize_t)off;
    i = off + 1;
  }
  return -1;
}

/* --------------------------------------------------------------------------
 * RESP2/RESP3 Command Encoder (appends to mt_resp_buf_t)
 * -------------------------------------------------------------------------- */

static inline int resp_encode_cmd(mt_resp_buf_t *buf,
                                  const char * const *args,
                                  const size_t *lens,
                                  int argc) {
  if (argc <= 0) return 0;

  /* Pre-calculate exact byte length to avoid repeated reallocs */
  size_t total = 1 + mt_u64_len((uint64_t)argc) + 2; /* *<argc>\r\n */
  for (int i = 0; i < argc; i++) {
    total += 1 + mt_u64_len((uint64_t)lens[i]) + 2 + lens[i] + 2; /* $<len>\r\n<bytes>\r\n */
  }

  if (!mt_resp_buf_reserve(buf, total)) return 0;

  char *p = buf->data + buf->len;
  *p++ = '*';
  p = mt_write_u64(p, (uint64_t)argc);
  *p++ = '\r';
  *p++ = '\n';

  for (int i = 0; i < argc; i++) {
    *p++ = '$';
    p = mt_write_u64(p, (uint64_t)lens[i]);
    *p++ = '\r';
    *p++ = '\n';
    if (lens[i] > 0) {
      memcpy(p, args[i], lens[i]);
      p += lens[i];
    }
    *p++ = '\r';
    *p++ = '\n';
  }

  buf->len = (size_t)(p - buf->data);
  return 1;
}

static inline int resp_encode_sv_cmd(pTHX_ mt_resp_buf_t *buf, SV **args, int argc) {
  if (argc <= 0) return 0;

  const char *stack_ptrs[32];
  size_t      stack_lens[32];
  const char **ptrs = stack_ptrs;
  size_t      *lens = stack_lens;

  if (argc > 32) {
    ptrs = (const char **)malloc((size_t)argc * sizeof(const char *));
    lens = (size_t *)malloc((size_t)argc * sizeof(size_t));
    if (!ptrs || !lens) {
      free(ptrs);
      free(lens);
      return 0;
    }
  }

  for (int i = 0; i < argc; i++) {
    SV *sv = args[i];
    if (!sv || !SvOK(sv)) {
      ptrs[i] = "";
      lens[i] = 0;
    } else {
      STRLEN slen = 0;
      ptrs[i] = SvPV_const(sv, slen);
      lens[i] = (size_t)slen;
    }
  }

  int ok = resp_encode_cmd(buf, ptrs, lens, argc);
  if (argc > 32) {
    free(ptrs);
    free(lens);
  }
  return ok;
}

/* --------------------------------------------------------------------------
 * RESP2/RESP3 Streaming Reply Parser
 *
 * Returns:
 *   1  = complete reply parsed into *out_sv; *pos advanced past reply
 *   0  = incomplete (need more bytes); *pos unchanged
 *  -1  = protocol error (errbuf populated)
 * -------------------------------------------------------------------------- */

static inline SV *mt_resp_make_error_obj(pTHX_ const char *msg, size_t len) {
  SV *inner = newSVpvn(msg, (STRLEN)len);
  SV *rv    = newRV_noinc(inner);
  HV *stash = gv_stashpvs("Mojo::MT::KeyDB::Error", GV_ADD);
  return sv_bless(rv, stash);
}

static int resp_parse_reply(pTHX_ const char *buf, size_t len, size_t *pos,
                            SV **out_sv, int *is_err, int wrap_errors,
                            char *errbuf, size_t errbuf_len, int depth) {
  if (depth > MT_RESP_MAX_DEPTH) {
    snprintf(errbuf, errbuf_len, "RESP nesting depth exceeded (%d)", MT_RESP_MAX_DEPTH);
    return -1;
  }
  if (*pos >= len) return 0;

  size_t cur = *pos;
  char type = buf[cur++];
  if (is_err) *is_err = 0;

  switch (type) {
    case '+': { /* Simple String */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      *out_sv = newSVpvn(buf + cur, (STRLEN)((size_t)crlf - cur));
      *pos = (size_t)crlf + 2;
      return 1;
    }

    case '-': { /* Simple Error */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      size_t mlen = (size_t)crlf - cur;
      if (is_err) *is_err = 1;
      *out_sv = wrap_errors
        ? mt_resp_make_error_obj(aTHX_ buf + cur, mlen)
        : newSVpvn(buf + cur, (STRLEN)mlen);
      *pos = (size_t)crlf + 2;
      return 1;
    }

    case ':': { /* Integer */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      int64_t iv = 0;
      if (!mt_parse_i64(buf + cur, (size_t)crlf - cur, &iv)) {
        snprintf(errbuf, errbuf_len, "Invalid RESP integer");
        return -1;
      }
      *out_sv = newSViv((IV)iv);
      *pos = (size_t)crlf + 2;
      return 1;
    }

    case '$':   /* Bulk String */
    case '!':   /* RESP3 Blob Error */
    case '=': { /* RESP3 Verbatim String */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      int64_t blen = 0;
      if (!mt_parse_i64(buf + cur, (size_t)crlf - cur, &blen)) {
        snprintf(errbuf, errbuf_len, "Invalid RESP bulk length");
        return -1;
      }
      size_t body_start = (size_t)crlf + 2;
      if (blen < 0) {
        if (blen != -1) {
          snprintf(errbuf, errbuf_len, "Invalid negative RESP bulk length");
          return -1;
        }
        *out_sv = newSV(0); /* undef */
        *pos = body_start;
        return 1;
      }
      size_t ulen = (size_t)blen;
      if (body_start + ulen + 2 > len) return 0;
      if (buf[body_start + ulen] != '\r' || buf[body_start + ulen + 1] != '\n') {
        snprintf(errbuf, errbuf_len, "Missing CRLF after RESP bulk string");
        return -1;
      }
      if (type == '!') {
        if (is_err) *is_err = 1;
        *out_sv = wrap_errors
          ? mt_resp_make_error_obj(aTHX_ buf + body_start, ulen)
          : newSVpvn(buf + body_start, (STRLEN)ulen);
      } else if (type == '=' && ulen >= 4 && buf[body_start + 3] == ':') {
        *out_sv = newSVpvn(buf + body_start + 4, (STRLEN)(ulen - 4));
      } else {
        *out_sv = newSVpvn(buf + body_start, (STRLEN)ulen);
      }
      *pos = body_start + ulen + 2;
      return 1;
    }

    case '*':   /* Array */
    case '~': { /* RESP3 Set */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      int64_t count = 0;
      if (!mt_parse_i64(buf + cur, (size_t)crlf - cur, &count)) {
        snprintf(errbuf, errbuf_len, "Invalid RESP array length");
        return -1;
      }
      size_t p = (size_t)crlf + 2;
      if (count < 0) {
        if (count != -1) {
          snprintf(errbuf, errbuf_len, "Invalid negative RESP array length");
          return -1;
        }
        *out_sv = newSV(0); /* undef */
        *pos = p;
        return 1;
      }
      AV *av = newAV();
      if (count > 0) av_extend(av, (SSize_t)(count - 1));
      for (int64_t i = 0; i < count; i++) {
        SV *elem = NULL;
        int elem_err = 0;
        int rc = resp_parse_reply(aTHX_ buf, len, &p, &elem, &elem_err, 1,
                                  errbuf, errbuf_len, depth + 1);
        if (rc <= 0) {
          SvREFCNT_dec((SV *)av);
          return rc;
        }
        av_push(av, elem);
      }
      *out_sv = newRV_noinc((SV *)av);
      *pos = p;
      return 1;
    }

    case '_': { /* RESP3 Null */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      *out_sv = newSV(0); /* undef */
      *pos = (size_t)crlf + 2;
      return 1;
    }

    case '#': { /* RESP3 Boolean */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      if ((size_t)crlf - cur != 1 || (buf[cur] != 't' && buf[cur] != 'f')) {
        snprintf(errbuf, errbuf_len, "Invalid RESP3 boolean");
        return -1;
      }
      *out_sv = newSViv(buf[cur] == 't' ? 1 : 0);
      *pos = (size_t)crlf + 2;
      return 1;
    }

    case ',': { /* RESP3 Double */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      size_t dlen = (size_t)crlf - cur;
      if (dlen == 0 || dlen >= 64) {
        snprintf(errbuf, errbuf_len, "Invalid RESP3 double");
        return -1;
      }
      char tmp[64];
      memcpy(tmp, buf + cur, dlen);
      tmp[dlen] = '\0';
      double dv = 0.0;
      if (strcmp(tmp, "inf") == 0) {
        dv = INFINITY;
      } else if (strcmp(tmp, "-inf") == 0) {
        dv = -INFINITY;
      } else if (strcmp(tmp, "nan") == 0) {
        dv = NAN;
      } else {
        char *endp = NULL;
        dv = strtod(tmp, &endp);
        if (!endp || *endp != '\0') {
          snprintf(errbuf, errbuf_len, "Invalid RESP3 double value");
          return -1;
        }
      }
      *out_sv = newSVnv((NV)dv);
      *pos = (size_t)crlf + 2;
      return 1;
    }

    case '%': { /* RESP3 Map */
      ssize_t crlf = mt_resp_find_crlf(buf, len, cur);
      if (crlf < 0) return 0;
      int64_t pairs = 0;
      if (!mt_parse_i64(buf + cur, (size_t)crlf - cur, &pairs) || pairs < 0) {
        snprintf(errbuf, errbuf_len, "Invalid RESP3 map length");
        return -1;
      }
      size_t p = (size_t)crlf + 2;
      HV *hv = newHV();
      for (int64_t i = 0; i < pairs; i++) {
        SV *ksv = NULL;
        SV *vsv = NULL;
        int kerr = 0, verr = 0;
        int rc = resp_parse_reply(aTHX_ buf, len, &p, &ksv, &kerr, 1,
                                  errbuf, errbuf_len, depth + 1);
        if (rc <= 0) {
          SvREFCNT_dec((SV *)hv);
          return rc;
        }
        rc = resp_parse_reply(aTHX_ buf, len, &p, &vsv, &verr, 1,
                              errbuf, errbuf_len, depth + 1);
        if (rc <= 0) {
          SvREFCNT_dec(ksv);
          SvREFCNT_dec((SV *)hv);
          return rc;
        }
        STRLEN klen = 0;
        const char *kptr = SvOK(ksv) ? SvPV_const(ksv, klen) : "";
        (void)hv_store(hv, kptr, (I32)klen, vsv, 0);
        SvREFCNT_dec(ksv);
      }
      *out_sv = newRV_noinc((SV *)hv);
      *pos = p;
      return 1;
    }

    default:
      snprintf(errbuf, errbuf_len, "Unexpected RESP type byte: 0x%02x", (unsigned char)type);
      return -1;
  }
}

/* --------------------------------------------------------------------------
 * Per-Worker Persistent Connection Pool (stored strictly in MY_CXT)
 * -------------------------------------------------------------------------- */

typedef struct {
  int           fd;
  int           in_use;
  int           is_unix;
  char          host[256];
  int           port;
  char          path[256];
  double        timeout;
  int           db;
  char          password[256];
  int           has_password;
  mt_resp_buf_t wbuf;
  mt_resp_buf_t rbuf;
  size_t        rpos;
} mt_keydb_conn_t;

typedef struct {
  mt_keydb_conn_t conns[MT_KEYDB_MAX_CONNS];
} mt_keydb_pool_t;

static inline void mt_keydb_pool_init(mt_keydb_pool_t *pool) {
  memset(pool, 0, sizeof(*pool));
  for (int i = 0; i < MT_KEYDB_MAX_CONNS; i++) {
    pool->conns[i].fd = -1;
  }
}

static inline void mt_keydb_conn_close_fd(mt_keydb_conn_t *c) {
  if (c->fd >= 0) {
    close(c->fd);
    c->fd = -1;
  }
  c->rbuf.len = 0;
  c->rpos     = 0;
  c->wbuf.len = 0;
}

static inline void mt_keydb_conn_reset(mt_keydb_conn_t *c) {
  mt_keydb_conn_close_fd(c);
  mt_resp_buf_free(&c->wbuf);
  mt_resp_buf_free(&c->rbuf);
  c->in_use = 0;
}

static inline void mt_keydb_pool_destroy(mt_keydb_pool_t *pool) {
  for (int i = 0; i < MT_KEYDB_MAX_CONNS; i++) {
    if (pool->conns[i].in_use || pool->conns[i].fd >= 0) {
      mt_keydb_conn_reset(&pool->conns[i]);
    }
  }
}

static inline int mt_keydb_conn_matches(const mt_keydb_conn_t *c,
                                        const char *host, int port,
                                        const char *path, int db,
                                        const char *password, int has_password) {
  if (!c->in_use) return 0;
  if (path && path[0]) {
    if (!c->is_unix || strcmp(c->path, path) != 0) return 0;
  } else {
    if (c->is_unix || c->port != port || strcmp(c->host, host) != 0) return 0;
  }
  if (c->db != db) return 0;
  if (c->has_password != has_password) return 0;
  if (has_password && strcmp(c->password, password) != 0) return 0;
  return 1;
}

static mt_keydb_conn_t *mt_keydb_pool_find(mt_keydb_pool_t *pool,
                                           const char *host, int port,
                                           const char *path, int db,
                                           const char *password, int has_password) {
  for (int i = 0; i < MT_KEYDB_MAX_CONNS; i++) {
    if (mt_keydb_conn_matches(&pool->conns[i], host, port, path, db, password, has_password)) {
      return &pool->conns[i];
    }
  }
  return NULL;
}

static mt_keydb_conn_t *mt_keydb_pool_acquire(mt_keydb_pool_t *pool,
                                              const char *host, int port,
                                              const char *path, double timeout,
                                              int db, const char *password, int has_password) {
  mt_keydb_conn_t *c = mt_keydb_pool_find(pool, host, port, path, db, password, has_password);
  if (c) {
    c->timeout = timeout > 0 ? timeout : 5.0;
    return c;
  }

  /* Find free slot (or evict a disconnected slot, or last slot if full) */
  int slot = -1;
  for (int i = 0; i < MT_KEYDB_MAX_CONNS; i++) {
    if (!pool->conns[i].in_use) { slot = i; break; }
  }
  if (slot < 0) {
    for (int i = 0; i < MT_KEYDB_MAX_CONNS; i++) {
      if (pool->conns[i].fd < 0) { slot = i; break; }
    }
  }
  if (slot < 0) {
    slot = MT_KEYDB_MAX_CONNS - 1;
    mt_keydb_conn_reset(&pool->conns[slot]);
  }

  c = &pool->conns[slot];
  memset(c, 0, sizeof(*c));
  c->fd     = -1;
  c->in_use = 1;
  c->is_unix = (path && path[0]) ? 1 : 0;
  if (c->is_unix) {
    snprintf(c->path, sizeof(c->path), "%s", path);
  } else {
    snprintf(c->host, sizeof(c->host), "%s", (host && host[0]) ? host : "127.0.0.1");
    c->port = port > 0 ? port : 6379;
  }
  c->timeout = timeout > 0 ? timeout : 5.0;
  c->db      = db;
  c->has_password = has_password ? 1 : 0;
  if (c->has_password && password) {
    snprintf(c->password, sizeof(c->password), "%s", password);
  }
  mt_resp_buf_init(&c->wbuf);
  mt_resp_buf_init(&c->rbuf);
  c->rpos = 0;
  return c;
}

/* --------------------------------------------------------------------------
 * Low-level socket I/O
 * -------------------------------------------------------------------------- */

static int mt_keydb_set_timeouts(int fd, double timeout) {
  struct timeval tv;
  if (timeout <= 0) timeout = 5.0;
  tv.tv_sec  = (time_t)timeout;
  tv.tv_usec = (suseconds_t)((timeout - (double)tv.tv_sec) * 1000000.0);
  if (tv.tv_sec == 0 && tv.tv_usec == 0) tv.tv_usec = 100000;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  int one = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  return 1;
}

static int mt_keydb_connect_nonblock(int fd, const struct sockaddr *sa, socklen_t salen,
                                     double timeout, char *errbuf, size_t errlen) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  int rc = connect(fd, sa, salen);
  if (rc < 0 && errno != EINPROGRESS) {
    snprintf(errbuf, errlen, "connect failed: %s", strerror(errno));
    return 0;
  }
  if (rc < 0) {
    struct pollfd pfd;
    pfd.fd      = fd;
    pfd.events  = POLLOUT;
    pfd.revents = 0;
    int ms = (int)(timeout * 1000.0);
    if (ms <= 0) ms = 5000;
    int prc;
    do {
      prc = poll(&pfd, 1, ms);
    } while (prc < 0 && errno == EINTR);
    if (prc == 0) {
      snprintf(errbuf, errlen, "connect timed out after %.2fs", timeout);
      return 0;
    }
    if (prc < 0) {
      snprintf(errbuf, errlen, "poll failed during connect: %s", strerror(errno));
      return 0;
    }
    int so_err = 0;
    socklen_t len = sizeof(so_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len) < 0 || so_err != 0) {
      snprintf(errbuf, errlen, "connect failed: %s", strerror(so_err ? so_err : errno));
      return 0;
    }
  }

  /* Restore blocking mode with SO_RCVTIMEO / SO_SNDTIMEO for direct send/recv */
  if (flags >= 0) (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  return 1;
}

static int mt_keydb_send_all(int fd, const char *data, size_t len,
                             char *errbuf, size_t errlen) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      snprintf(errbuf, errlen, "send failed: %s", strerror(errno));
      return 0;
    }
    if (n == 0) {
      snprintf(errbuf, errlen, "send returned 0 (connection closed)");
      return 0;
    }
    off += (size_t)n;
  }
  return 1;
}

static int mt_keydb_recv_reply(pTHX_ mt_keydb_conn_t *c, SV **out_sv,
                               int *is_err, int wrap_errors,
                               char *errbuf, size_t errlen) {
  for (;;) {
    if (c->rpos < c->rbuf.len) {
      int rc = resp_parse_reply(aTHX_ c->rbuf.data, c->rbuf.len, &c->rpos,
                                out_sv, is_err, wrap_errors,
                                errbuf, errlen, 0);
      if (rc == 1) {
        if (c->rpos == c->rbuf.len) {
          c->rpos     = 0;
          c->rbuf.len = 0;
        } else if (c->rpos > 4096) {
          size_t rem = c->rbuf.len - c->rpos;
          memmove(c->rbuf.data, c->rbuf.data + c->rpos, rem);
          c->rbuf.len = rem;
          c->rpos     = 0;
        }
        return 1;
      }
      if (rc < 0) {
        mt_keydb_conn_close_fd(c);
        return 0;
      }
    }

    /* Compact if cursor moved forward before reading more */
    if (c->rpos > 0) {
      size_t rem = c->rbuf.len - c->rpos;
      if (rem > 0) memmove(c->rbuf.data, c->rbuf.data + c->rpos, rem);
      c->rbuf.len = rem;
      c->rpos     = 0;
    }

    if (!mt_resp_buf_reserve(&c->rbuf, 4096)) {
      snprintf(errbuf, errlen, "out of memory growing read buffer");
      mt_keydb_conn_close_fd(c);
      return 0;
    }

    ssize_t n;
    do {
      n = recv(c->fd, c->rbuf.data + c->rbuf.len, c->rbuf.cap - c->rbuf.len, 0);
    } while (n < 0 && errno == EINTR);

    if (n <= 0) {
      if (n == 0) {
        snprintf(errbuf, errlen, "connection closed by peer");
      } else {
        snprintf(errbuf, errlen, "recv failed: %s", strerror(errno));
      }
      mt_keydb_conn_close_fd(c);
      return 0;
    }
    c->rbuf.len += (size_t)n;
  }
}

static int mt_keydb_raw_cmd_cstr(pTHX_ mt_keydb_conn_t *c,
                                 const char * const *args, const size_t *lens, int argc,
                                 char *errbuf, size_t errlen) {
  c->wbuf.len = 0;
  if (!resp_encode_cmd(&c->wbuf, args, lens, argc)) {
    snprintf(errbuf, errlen, "out of memory encoding command");
    return 0;
  }
  if (!mt_keydb_send_all(c->fd, c->wbuf.data, c->wbuf.len, errbuf, errlen)) {
    mt_keydb_conn_close_fd(c);
    return 0;
  }
  SV *reply = NULL;
  int is_err = 0;
  if (!mt_keydb_recv_reply(aTHX_ c, &reply, &is_err, 0, errbuf, errlen)) {
    return 0;
  }
  if (is_err) {
    STRLEN elen = 0;
    const char *eptr = SvPV_const(reply, elen);
    snprintf(errbuf, errlen, "KeyDB error: %.*s", (int)elen, eptr);
    SvREFCNT_dec(reply);
    mt_keydb_conn_close_fd(c);
    return 0;
  }
  SvREFCNT_dec(reply);
  return 1;
}

static int mt_keydb_ensure_connected(pTHX_ mt_keydb_conn_t *c, char *errbuf, size_t errlen) {
  if (c->fd >= 0) return 1;

  int fd = -1;
  if (c->is_unix) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      snprintf(errbuf, errlen, "socket(AF_UNIX) failed: %s", strerror(errno));
      return 0;
    }
    struct sockaddr_un su;
    memset(&su, 0, sizeof(su));
    su.sun_family = AF_UNIX;
    if (strlen(c->path) >= sizeof(su.sun_path)) {
      close(fd);
      snprintf(errbuf, errlen, "unix socket path too long");
      return 0;
    }
    strcpy(su.sun_path, c->path);
    if (!mt_keydb_connect_nonblock(fd, (struct sockaddr *)&su, sizeof(su),
                                   c->timeout, errbuf, errlen)) {
      close(fd);
      return 0;
    }
  } else {
    struct addrinfo hints, *res = NULL, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", c->port);
    int gai = getaddrinfo(c->host, port_str, &hints, &res);
    if (gai != 0 || !res) {
      snprintf(errbuf, errlen, "getaddrinfo(%s:%s) failed: %s",
               c->host, port_str, gai_strerror(gai));
      return 0;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0) continue;
      if (mt_keydb_connect_nonblock(fd, ai->ai_addr, ai->ai_addrlen,
                                    c->timeout, errbuf, errlen)) {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        break;
      }
      close(fd);
      fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
      if (!errbuf[0]) snprintf(errbuf, errlen, "unable to connect to %s:%d", c->host, c->port);
      return 0;
    }
  }

  mt_keydb_set_timeouts(fd, c->timeout);
  c->fd       = fd;
  c->rbuf.len = 0;
  c->rpos     = 0;
  c->wbuf.len = 0;

  if (c->has_password) {
    const char *args[2] = { "AUTH", c->password };
    size_t      lens[2] = { 4, strlen(c->password) };
    if (!mt_keydb_raw_cmd_cstr(aTHX_ c, args, lens, 2, errbuf, errlen)) {
      return 0;
    }
  }

  if (c->db != 0) {
    char db_str[32];
    snprintf(db_str, sizeof(db_str), "%d", c->db);
    const char *args[2] = { "SELECT", db_str };
    size_t      lens[2] = { 6, strlen(db_str) };
    if (!mt_keydb_raw_cmd_cstr(aTHX_ c, args, lens, 2, errbuf, errlen)) {
      return 0;
    }
  }

  return 1;
}

static int mt_keydb_cmd(pTHX_ mt_keydb_conn_t *c, SV **args, int argc,
                        SV **out_sv, int *is_err, char *errbuf, size_t errlen) {
  int was_connected = (c->fd >= 0);

  for (int attempt = 0; attempt < 2; attempt++) {
    if (!mt_keydb_ensure_connected(aTHX_ c, errbuf, errlen)) {
      return 0;
    }
    c->wbuf.len = 0;
    if (!resp_encode_sv_cmd(aTHX_ &c->wbuf, args, argc)) {
      snprintf(errbuf, errlen, "out of memory encoding command");
      return 0;
    }
    if (!mt_keydb_send_all(c->fd, c->wbuf.data, c->wbuf.len, errbuf, errlen)) {
      mt_keydb_conn_close_fd(c);
      if (was_connected && attempt == 0) {
        was_connected = 0;
        errbuf[0] = '\0';
        continue;
      }
      return 0;
    }
    if (!mt_keydb_recv_reply(aTHX_ c, out_sv, is_err, 0, errbuf, errlen)) {
      if (was_connected && attempt == 0) {
        was_connected = 0;
        errbuf[0] = '\0';
        continue;
      }
      return 0;
    }
    return 1;
  }
  return 0;
}

static int mt_keydb_pipeline(pTHX_ mt_keydb_conn_t *c, AV *cmds,
                             AV **out_av, char *errbuf, size_t errlen) {
  SSize_t ncmds = av_len(cmds) + 1;
  if (ncmds <= 0) {
    *out_av = newAV();
    return 1;
  }

  if (!mt_keydb_ensure_connected(aTHX_ c, errbuf, errlen)) {
    return 0;
  }

  c->wbuf.len = 0;
  for (SSize_t i = 0; i < ncmds; i++) {
    SV **svp = av_fetch(cmds, i, 0);
    if (!svp || !SvROK(*svp) || SvTYPE(SvRV(*svp)) != SVt_PVAV) {
      snprintf(errbuf, errlen, "pipeline element %ld is not an arrayref", (long)i);
      return 0;
    }
    AV *cmd_av = (AV *)SvRV(*svp);
    SSize_t argc = av_len(cmd_av) + 1;
    if (argc <= 0) {
      snprintf(errbuf, errlen, "pipeline element %ld is empty", (long)i);
      return 0;
    }
    SV *stack_svs[32];
    SV **svs = stack_svs;
    if (argc > 32) {
      svs = (SV **)malloc((size_t)argc * sizeof(SV *));
      if (!svs) {
        snprintf(errbuf, errlen, "out of memory in pipeline");
        return 0;
      }
    }
    for (SSize_t j = 0; j < argc; j++) {
      SV **ap = av_fetch(cmd_av, j, 0);
      svs[j] = ap ? *ap : &PL_sv_undef;
    }
    int ok = resp_encode_sv_cmd(aTHX_ &c->wbuf, svs, (int)argc);
    if (argc > 32) free(svs);
    if (!ok) {
      snprintf(errbuf, errlen, "out of memory encoding pipeline");
      return 0;
    }
  }

  if (!mt_keydb_send_all(c->fd, c->wbuf.data, c->wbuf.len, errbuf, errlen)) {
    mt_keydb_conn_close_fd(c);
    return 0;
  }

  AV *res = newAV();
  av_extend(res, ncmds - 1);
  for (SSize_t i = 0; i < ncmds; i++) {
    SV *reply = NULL;
    int is_err = 0;
    if (!mt_keydb_recv_reply(aTHX_ c, &reply, &is_err, 1, errbuf, errlen)) {
      SvREFCNT_dec((SV *)res);
      return 0;
    }
    av_push(res, reply);
  }
  *out_av = res;
  return 1;
}

#endif /* MT_RESP_H */
