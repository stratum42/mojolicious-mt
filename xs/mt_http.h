#ifndef MOJO_MT_HTTP_H
#define MOJO_MT_HTTP_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "mt_simd.h"

static inline int mt_http_is_ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static inline const unsigned char *mt_find_lf(const unsigned char *p, const unsigned char *end) {
  size_t len = (size_t)(end - p);
  size_t idx = mt_simd_find_byte(p, len, '\n');
  return (idx < len) ? (p + idx) : NULL;
}

static inline const unsigned char *mt_find_colon(const unsigned char *p, const unsigned char *end) {
  size_t len = (size_t)(end - p);
  size_t idx = mt_simd_find_byte(p, len, ':');
  return (idx < len) ? (p + idx) : NULL;
}

/* Fast lookup for common Mojolicious %NAMES headers (lowercase input -> canonical name).
 * Matches %NAMES in lib/Mojo/Headers.pm 100%. */
static inline const char *mt_common_header_name(const char *k, size_t len) {
  switch (len) {
    case 2:
      if (memcmp(k, "te", 2) == 0) return "TE";
      return NULL;
    case 3:
      if (memcmp(k, "dnt", 3) == 0) return "DNT";
      return NULL;
    case 4:
      if (memcmp(k, "host", 4) == 0) return "Host";
      if (memcmp(k, "date", 4) == 0) return "Date";
      if (memcmp(k, "etag", 4) == 0) return "ETag";
      if (memcmp(k, "link", 4) == 0) return "Link";
      if (memcmp(k, "vary", 4) == 0) return "Vary";
      return NULL;
    case 5:
      if (memcmp(k, "allow", 5) == 0) return "Allow";
      if (memcmp(k, "range", 5) == 0) return "Range";
      return NULL;
    case 6:
      if (memcmp(k, "accept", 6) == 0) return "Accept";
      if (memcmp(k, "cookie", 6) == 0) return "Cookie";
      if (memcmp(k, "expect", 6) == 0) return "Expect";
      if (memcmp(k, "origin", 6) == 0) return "Origin";
      if (memcmp(k, "server", 6) == 0) return "Server";
      if (memcmp(k, "status", 6) == 0) return "Status";
      return NULL;
    case 7:
      if (memcmp(k, "expires", 7) == 0) return "Expires";
      if (memcmp(k, "trailer", 7) == 0) return "Trailer";
      if (memcmp(k, "upgrade", 7) == 0) return "Upgrade";
      return NULL;
    case 8:
      if (memcmp(k, "location", 8) == 0) return "Location";
      return NULL;
    case 10:
      if (memcmp(k, "connection", 10) == 0) return "Connection";
      if (memcmp(k, "set-cookie", 10) == 0) return "Set-Cookie";
      if (memcmp(k, "user-agent", 10) == 0) return "User-Agent";
      return NULL;
    case 12:
      if (memcmp(k, "content-type", 12) == 0) return "Content-Type";
      return NULL;
    case 13:
      if (memcmp(k, "accept-ranges", 13) == 0) return "Accept-Ranges";
      if (memcmp(k, "authorization", 13) == 0) return "Authorization";
      if (memcmp(k, "cache-control", 13) == 0) return "Cache-Control";
      if (memcmp(k, "content-range", 13) == 0) return "Content-Range";
      if (memcmp(k, "if-none-match", 13) == 0) return "If-None-Match";
      if (memcmp(k, "last-modified", 13) == 0) return "Last-Modified";
      if (memcmp(k, "server-timing", 13) == 0) return "Server-Timing";
      return NULL;
    case 14:
      if (memcmp(k, "accept-charset", 14) == 0) return "Accept-Charset";
      if (memcmp(k, "content-length", 14) == 0) return "Content-Length";
      return NULL;
    case 15:
      if (memcmp(k, "accept-encoding", 15) == 0) return "Accept-Encoding";
      if (memcmp(k, "accept-language", 15) == 0) return "Accept-Language";
      return NULL;
    case 16:
      if (memcmp(k, "content-encoding", 16) == 0) return "Content-Encoding";
      if (memcmp(k, "content-language", 16) == 0) return "Content-Language";
      if (memcmp(k, "content-location", 16) == 0) return "Content-Location";
      if (memcmp(k, "www-authenticate", 16) == 0) return "WWW-Authenticate";
      return NULL;
    case 17:
      if (memcmp(k, "if-modified-since", 17) == 0) return "If-Modified-Since";
      if (memcmp(k, "sec-websocket-key", 17) == 0) return "Sec-WebSocket-Key";
      if (memcmp(k, "transfer-encoding", 17) == 0) return "Transfer-Encoding";
      return NULL;
    case 18:
      if (memcmp(k, "proxy-authenticate", 18) == 0) return "Proxy-Authenticate";
      return NULL;
    case 19:
      if (memcmp(k, "content-disposition", 19) == 0) return "Content-Disposition";
      if (memcmp(k, "proxy-authorization", 19) == 0) return "Proxy-Authorization";
      return NULL;
    case 20:
      if (memcmp(k, "sec-websocket-accept", 20) == 0) return "Sec-WebSocket-Accept";
      return NULL;
    case 21:
      if (memcmp(k, "sec-websocket-version", 21) == 0) return "Sec-WebSocket-Version";
      return NULL;
    case 22:
      if (memcmp(k, "sec-websocket-protocol", 22) == 0) return "Sec-WebSocket-Protocol";
      return NULL;
    case 23:
      if (memcmp(k, "content-security-policy", 23) == 0) return "Content-Security-Policy";
      return NULL;
    case 24:
      if (memcmp(k, "sec-websocket-extensions", 24) == 0) return "Sec-WebSocket-Extensions";
      return NULL;
    case 25:
      if (memcmp(k, "strict-transport-security", 25) == 0) return "Strict-Transport-Security";
      return NULL;
    case 27:
      if (memcmp(k, "access-control-allow-origin", 27) == 0) return "Access-Control-Allow-Origin";
      return NULL;
    default:
      return NULL;
  }
}

static inline int mt_is_common_header(const char *k, size_t len) {
  return mt_common_header_name(k, len) != NULL;
}

typedef struct {
  const unsigned char *name;
  size_t               nlen;
  const unsigned char *val;
  size_t               vlen;
} mt_http_hdr_t;

#define MT_HTTP_MAX_STACK_HDRS 128

/* Single-pass C/NEON HTTP/1.1 header block parser.
 * Populates headers_hv ($self->{headers}) and names_hv ($self->{names}) directly.
 * Returns bytes consumed (>= 0) on complete header block, or -1 to fall back to pure Perl
 * (incomplete data, obs-fold, limit exceeded, malformed header, or UTF-8 flagged buffer).
 */
static IV mt_http_parse_headers(pTHX_ HV *headers_hv, HV *names_hv, SV *buf_sv,
                                IV max_line_size, IV max_lines) {
  if (!buf_sv || !SvOK(buf_sv) || SvUTF8(buf_sv)) return -1;

  STRLEN buflen;
  const unsigned char *buf = (const unsigned char *)SvPV_const(buf_sv, buflen);
  if (buflen == 0) return -1;

  const unsigned char *p   = buf;
  const unsigned char *end = buf + buflen;

  mt_http_hdr_t hdrs[MT_HTTP_MAX_STACK_HDRS];
  size_t count = 0;
  int finished = 0;

  while (p < end) {
    const unsigned char *nl = mt_find_lf(p, end);
    if (!nl) return -1; /* incomplete header block */

    size_t line_len = (size_t)(nl - p) + 1;
    if ((IV)line_len > max_line_size || (IV)count >= max_lines) {
      return -1; /* let pure Perl set limit=1 / state=finished */
    }

    const unsigned char *lend = nl;
    if (lend > p && *(lend - 1) == '\r') lend--;

    /* Empty line terminates headers */
    if (lend == p) {
      p = nl + 1;
      finished = 1;
      break;
    }

    /* Obs-fold / leading whitespace -> fall back to pure Perl */
    if (mt_http_is_ws(*p)) return -1;

    const unsigned char *colon = mt_find_colon(p, lend);
    if (!colon || colon == p) return -1; /* malformed / no colon */

    if (count >= MT_HTTP_MAX_STACK_HDRS) return -1;

    const unsigned char *val = colon + 1;
    while (val < lend && mt_http_is_ws(*val)) val++;

    /* Reject embedded CR inside value (pure Perl add() croaks on \x0d\x0a) */
    for (const unsigned char *q = val; q < lend; q++) {
      if (*q == '\r') return -1;
    }

    hdrs[count].name = p;
    hdrs[count].nlen = (size_t)(colon - p);
    hdrs[count].val  = val;
    hdrs[count].vlen = (size_t)(lend - val);
    count++;

    p = nl + 1;
  }

  if (!finished) return -1;

  /* Commit all parsed headers into headers_hv and names_hv */
  char lcbuf[256];
  for (size_t i = 0; i < count; i++) {
    size_t nlen = hdrs[i].nlen;
    char *lc = (nlen < sizeof(lcbuf)) ? lcbuf : (char *)malloc(nlen + 1);
    if (!lc) return -1;

    for (size_t j = 0; j < nlen; j++) {
      unsigned char c = hdrs[i].name[j];
      lc[j] = (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : (char)c;
    }
    lc[nlen] = '\0';

    if (!mt_is_common_header(lc, nlen)) {
      if (!hv_exists(names_hv, lc, (I32)nlen)) {
        SV *orig = newSVpvn((const char *)hdrs[i].name, nlen);
        if (!hv_store(names_hv, lc, (I32)nlen, orig, 0)) {
          SvREFCNT_dec(orig);
        }
      }
    }

    SV **slot = hv_fetch(headers_hv, lc, (I32)nlen, 1);
    if (slot) {
      AV *av;
      if (SvROK(*slot) && SvTYPE(SvRV(*slot)) == SVt_PVAV) {
        av = (AV *)SvRV(*slot);
      } else {
        av = newAV();
        sv_setsv(*slot, sv_2mortal(newRV_noinc((SV *)av)));
      }
      av_push(av, newSVpvn((const char *)hdrs[i].val, hdrs[i].vlen));
    }

    if (lc != lcbuf) free(lc);
  }

  return (IV)(p - buf);
}

/* Single-pass C/NEON HTTP/1.1 request start-line parser.
 * Returns:
 *   >= 0 : bytes consumed (method, target, version stored in out SVs)
 *   -1   : incomplete (no LF yet) or UTF-8 flagged (caller should fall back or wait)
 *   -2   : malformed start-line (caller should set 'Bad request start-line' error)
 */
static IV mt_http_parse_start_line(pTHX_ SV *buf_sv, SV **method_sv,
                                   SV **target_sv, SV **version_sv) {
  if (!buf_sv || !SvOK(buf_sv) || SvUTF8(buf_sv)) return -1;

  STRLEN buflen;
  const unsigned char *buf = (const unsigned char *)SvPV_const(buf_sv, buflen);
  if (buflen == 0) return -1;

  const unsigned char *p   = buf;
  const unsigned char *end = buf + buflen;

  /* Skip leading whitespace (matches s/^\s*(.*?)\x0d?\x0a//) */
  while (p < end && mt_http_is_ws(*p)) p++;

  const unsigned char *nl = mt_find_lf(p, end);
  if (!nl) return -1;

  const unsigned char *lend = nl;
  if (lend > p && *(lend - 1) == '\r') lend--;

  /* Match ^(\S+)\s+(\S+)\s+HTTP/(\d\.\d)$ */
  const unsigned char *m_start = p;
  while (p < lend && !mt_http_is_ws(*p)) p++;
  size_t m_len = (size_t)(p - m_start);
  if (m_len == 0 || p >= lend) return -2;

  while (p < lend && mt_http_is_ws(*p)) p++;
  const unsigned char *t_start = p;
  while (p < lend && !mt_http_is_ws(*p)) p++;
  size_t t_len = (size_t)(p - t_start);
  if (t_len == 0 || p >= lend) return -2;

  while (p < lend && mt_http_is_ws(*p)) p++;

  /* Remaining must be exactly "HTTP/d.d" (8 bytes) */
  if ((size_t)(lend - p) != 8) return -2;
  if (memcmp(p, "HTTP/", 5) != 0) return -2;
  if (p[5] < '0' || p[5] > '9' || p[6] != '.' || p[7] < '0' || p[7] > '9') return -2;

  *method_sv  = newSVpvn((const char *)m_start, m_len);
  *target_sv  = newSVpvn((const char *)t_start, t_len);
  *version_sv = newSVpvn((const char *)(p + 5), 3);

  return (IV)((nl - buf) + 1);
}

/* Default HTTP status messages matching %MESSAGES in lib/Mojo/Message/Response.pm */
static inline const char *mt_http_default_message(IV code) {
  switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Request Entity Too Large";
    case 414: return "Request-URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Request Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 509: return "Bandwidth Limit Exceeded";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    default:  return "";
  }
}

static inline size_t mt_u64_to_dec(char *buf, uint64_t v) {
  char tmp[24];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (v % 10));
    v /= 10;
  } while (v != 0);
  for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
  buf[n] = '\0';
  return n;
}

typedef struct {
  const char *key;
  size_t      klen;
  const char *name;
  size_t      nlen;
  AV         *av;       /* non-NULL when values come from a Perl AV* */
  const char *val;      /* non-NULL when single C string value */
  size_t      vlen;
} mt_http_out_hdr_t;

/* Single-allocation HTTP/1.1 response serializer.
 * Builds "HTTP/<ver> <code> <msg>\r\n<sorted headers>\r\n\r\n<body>" in one SvGROW buffer.
 * Returns NULL if caller should fall back to pure Perl (UTF-8 flagged bytes, >128 headers, etc.).
 */
static SV *mt_http_build_response(pTHX_ IV code, SV *msg_sv, HV *headers_hv,
                                  SV *body_sv, SV *date_sv, int keep_alive,
                                  HV *names_hv, SV *version_sv) {
  if (code <= 0) code = 404;

  const char *ver_ptr = "1.1";
  STRLEN ver_len = 3;
  if (version_sv && SvOK(version_sv)) {
    if (SvUTF8(version_sv)) return NULL;
    ver_ptr = SvPV_const(version_sv, ver_len);
  }

  const char *msg_ptr = NULL;
  STRLEN msg_len = 0;
  if (msg_sv && SvOK(msg_sv)) {
    if (SvUTF8(msg_sv)) return NULL;
    msg_ptr = SvPV_const(msg_sv, msg_len);
  }
  if (!msg_ptr || msg_len == 0) {
    msg_ptr = mt_http_default_message(code);
    msg_len = strlen(msg_ptr);
  }

  const char *body_ptr = "";
  STRLEN body_len = 0;
  if (body_sv && SvOK(body_sv)) {
    if (SvUTF8(body_sv)) return NULL;
    body_ptr = SvPV_const(body_sv, body_len);
  }

  const char *date_ptr = NULL;
  STRLEN date_len = 0;
  if (date_sv && SvOK(date_sv)) {
    if (SvUTF8(date_sv)) return NULL;
    date_ptr = SvPV_const(date_sv, date_len);
  }

  mt_http_out_hdr_t entries[MT_HTTP_MAX_STACK_HDRS];
  size_t count = 0;
  int has_cl = 0, has_date = 0, has_conn = 0;

  if (headers_hv) {
    hv_iterinit(headers_hv);
    HE *he;
    while ((he = hv_iternext(headers_hv)) != NULL) {
      if (count >= MT_HTTP_MAX_STACK_HDRS - 3) return NULL;
      I32 klen_i32 = 0;
      const char *key = hv_iterkey(he, &klen_i32);
      if (klen_i32 < 0) return NULL; /* UTF-8 key -> fall back */
      size_t klen = (size_t)klen_i32;

      if (klen == 14 && memcmp(key, "content-length", 14) == 0) has_cl   = 1;
      else if (klen == 4  && memcmp(key, "date", 4) == 0)       has_date = 1;
      else if (klen == 10 && memcmp(key, "connection", 10) == 0) has_conn = 1;

      const char *name = mt_common_header_name(key, klen);
      size_t nlen = klen;
      if (!name && names_hv) {
        SV **nslot = hv_fetch(names_hv, key, (I32)klen, 0);
        if (nslot && *nslot && SvOK(*nslot)) {
          if (SvUTF8(*nslot)) return NULL;
          STRLEN nl = 0;
          name = SvPV_const(*nslot, nl);
          nlen = (size_t)nl;
        }
      }
      if (!name) {
        name = key;
        nlen = klen;
      }

      SV *val_sv = hv_iterval(headers_hv, he);
      if (val_sv && SvROK(val_sv) && SvTYPE(SvRV(val_sv)) == SVt_PVAV) {
        AV *av = (AV *)SvRV(val_sv);
        SSize_t av_len = av_len(av) + 1;
        if (av_len <= 0) continue;
        entries[count].key  = key;
        entries[count].klen = klen;
        entries[count].name = name;
        entries[count].nlen = nlen;
        entries[count].av   = av;
        entries[count].val  = NULL;
        entries[count].vlen = 0;
        count++;
      } else if (val_sv && SvOK(val_sv)) {
        if (SvUTF8(val_sv)) return NULL;
        STRLEN vl = 0;
        const char *vp = SvPV_const(val_sv, vl);
        entries[count].key  = key;
        entries[count].klen = klen;
        entries[count].name = name;
        entries[count].nlen = nlen;
        entries[count].av   = NULL;
        entries[count].val  = vp;
        entries[count].vlen = (size_t)vl;
        count++;
      }
    }
  }

  char cl_buf[24];
  if (!has_cl) {
    size_t cl_len = mt_u64_to_dec(cl_buf, (uint64_t)body_len);
    entries[count].key  = "content-length";
    entries[count].klen = 14;
    entries[count].name = "Content-Length";
    entries[count].nlen = 14;
    entries[count].av   = NULL;
    entries[count].val  = cl_buf;
    entries[count].vlen = cl_len;
    count++;
  }

  if (!has_date && date_ptr && date_len > 0) {
    entries[count].key  = "date";
    entries[count].klen = 4;
    entries[count].name = "Date";
    entries[count].nlen = 4;
    entries[count].av   = NULL;
    entries[count].val  = date_ptr;
    entries[count].vlen = (size_t)date_len;
    count++;
  }

  if (!has_conn && keep_alive == 0) {
    entries[count].key  = "connection";
    entries[count].klen = 10;
    entries[count].name = "Connection";
    entries[count].nlen = 10;
    entries[count].av   = NULL;
    entries[count].val  = "close";
    entries[count].vlen = 5;
    count++;
  }

  /* Insertion sort by lowercase key (matches Mojo::Headers::names alphabetical order) */
  for (size_t i = 1; i < count; i++) {
    mt_http_out_hdr_t cur = entries[i];
    size_t j = i;
    while (j > 0) {
      size_t minlen = entries[j - 1].klen < cur.klen ? entries[j - 1].klen : cur.klen;
      int cmp = memcmp(entries[j - 1].key, cur.key, minlen);
      if (cmp < 0 || (cmp == 0 && entries[j - 1].klen <= cur.klen)) break;
      entries[j] = entries[j - 1];
      j--;
    }
    entries[j] = cur;
  }

  /* Format status code and compute exact output size */
  char code_buf[24];
  size_t code_len = mt_u64_to_dec(code_buf, (uint64_t)code);

  size_t total = 5 + ver_len + 1 + code_len + 1 + msg_len + 2; /* HTTP/1.1 200 OK\r\n */
  for (size_t i = 0; i < count; i++) {
    if (entries[i].av) {
      AV *av = entries[i].av;
      SSize_t n = av_len(av) + 1;
      for (SSize_t k = 0; k < n; k++) {
        SV **elem = av_fetch(av, k, 0);
        if (!elem || !*elem || !SvOK(*elem)) continue;
        if (SvUTF8(*elem)) return NULL;
        STRLEN vl = 0;
        (void)SvPV_const(*elem, vl);
        total += entries[i].nlen + 2 + (size_t)vl + 2;
      }
    } else {
      total += entries[i].nlen + 2 + entries[i].vlen + 2;
    }
  }
  total += 2 + (size_t)body_len; /* \r\n + body */

  SV *out = newSV(total + 1);
  char *dst = SvPVX(out);

  /* Write start-line */
  memcpy(dst, "HTTP/", 5);            dst += 5;
  memcpy(dst, ver_ptr, ver_len);      dst += ver_len;
  *dst++ = ' ';
  memcpy(dst, code_buf, code_len);    dst += code_len;
  *dst++ = ' ';
  memcpy(dst, msg_ptr, msg_len);      dst += msg_len;
  *dst++ = '\r'; *dst++ = '\n';

  /* Write headers */
  for (size_t i = 0; i < count; i++) {
    if (entries[i].av) {
      AV *av = entries[i].av;
      SSize_t n = av_len(av) + 1;
      for (SSize_t k = 0; k < n; k++) {
        SV **elem = av_fetch(av, k, 0);
        if (!elem || !*elem || !SvOK(*elem)) continue;
        STRLEN vl = 0;
        const char *vp = SvPV_const(*elem, vl);
        memcpy(dst, entries[i].name, entries[i].nlen); dst += entries[i].nlen;
        *dst++ = ':'; *dst++ = ' ';
        if (vl) { memcpy(dst, vp, vl); dst += vl; }
        *dst++ = '\r'; *dst++ = '\n';
      }
    } else {
      memcpy(dst, entries[i].name, entries[i].nlen); dst += entries[i].nlen;
      *dst++ = ':'; *dst++ = ' ';
      if (entries[i].vlen) { memcpy(dst, entries[i].val, entries[i].vlen); dst += entries[i].vlen; }
      *dst++ = '\r'; *dst++ = '\n';
    }
  }

  /* End of headers + body */
  *dst++ = '\r'; *dst++ = '\n';
  if (body_len) {
    memcpy(dst, body_ptr, body_len);
    dst += body_len;
  }
  *dst = '\0';

  SvPOK_only(out);
  SvCUR_set(out, total);
  return out;
}

#endif /* MOJO_MT_HTTP_H */
