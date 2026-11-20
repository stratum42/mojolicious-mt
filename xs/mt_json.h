#ifndef MOJO_MT_JSON_H
#define MOJO_MT_JSON_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__ARM_NEON) || defined(__aarch64__)
#  include <arm_neon.h>
#  ifndef MT_HAS_NEON
#    define MT_HAS_NEON 1
#  endif
#else
#  ifndef MT_HAS_NEON
#    define MT_HAS_NEON 0
#  endif
#endif

#define MT_JSON_MAX_DEPTH 512

/* ---- Growable output buffer with 2KiB stack fast-path ---- */

typedef struct {
  char  *buf;
  size_t len;
  size_t cap;
  int    on_heap;
  int    has_non_ascii;
  char   inline_buf[2048];
} mt_jbuf_t;

static inline void mt_jbuf_init(mt_jbuf_t *jb) {
  jb->buf           = jb->inline_buf;
  jb->len           = 0;
  jb->cap           = sizeof(jb->inline_buf);
  jb->on_heap       = 0;
  jb->has_non_ascii = 0;
}

static inline void mt_jbuf_free(mt_jbuf_t *jb) {
  if (jb->on_heap && jb->buf) free(jb->buf);
  jb->buf     = NULL;
  jb->on_heap = 0;
}

static int mt_jbuf_grow(mt_jbuf_t *jb, size_t need) {
  size_t req  = jb->len + need;
  if (req <= jb->cap) return 1;
  size_t ncap = jb->cap * 2;
  while (ncap < req) ncap *= 2;
  char *nb;
  if (jb->on_heap) {
    nb = (char *)realloc(jb->buf, ncap);
  } else {
    nb = (char *)malloc(ncap);
    if (nb && jb->len > 0) memcpy(nb, jb->buf, jb->len);
  }
  if (!nb) return 0;
  jb->buf     = nb;
  jb->cap     = ncap;
  jb->on_heap = 1;
  return 1;
}

static inline int mt_jbuf_reserve(mt_jbuf_t *jb, size_t need) {
  if (jb->len + need <= jb->cap) return 1;
  return mt_jbuf_grow(jb, need);
}

static inline int mt_jbuf_putc(mt_jbuf_t *jb, char c) {
  if (!mt_jbuf_reserve(jb, 1)) return 0;
  jb->buf[jb->len++] = c;
  return 1;
}

static inline int mt_jbuf_puts(mt_jbuf_t *jb, const char *s, size_t n) {
  if (!mt_jbuf_reserve(jb, n)) return 0;
  memcpy(jb->buf + jb->len, s, n);
  jb->len += n;
  return 1;
}

/* ---- NEON-accelerated JSON string encoder ---- */

static inline size_t mt_json_scan_str(const unsigned char *p, size_t len, int is_utf8) {
  size_t i = 0;
#if MT_HAS_NEON
  if (len >= 16) {
    const uint8x16_t v_sp  = vdupq_n_u8(0x20);
    const uint8x16_t v_qt  = vdupq_n_u8('"');
    const uint8x16_t v_bs  = vdupq_n_u8('\\');
    const uint8x16_t v_sl  = vdupq_n_u8('/');
    const uint8x16_t v_hi  = vdupq_n_u8(0x80);
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(p + i);
      uint8x16_t m = vorrq_u8(
        vorrq_u8(vcltq_u8(v, v_sp), vceqq_u8(v, v_qt)),
        vorrq_u8(vceqq_u8(v, v_bs), vceqq_u8(v, v_sl))
      );
      if (!is_utf8) {
        m = vorrq_u8(m, vcgeq_u8(v, v_hi));
      }
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = p[i];
    if (c < 0x20 || c == '"' || c == '\\' || c == '/' || (!is_utf8 && c >= 0x80)) {
      return i;
    }
  }
  return len;
}

static int mt_json_encode_str(mt_jbuf_t *jb, const unsigned char *p, size_t len, int is_utf8) {
  static const char hex[16] = "0123456789ABCDEF";
  if (!mt_jbuf_putc(jb, '"')) return 0;

  size_t pos = 0;
  while (pos < len) {
    size_t run = mt_json_scan_str(p + pos, len - pos, is_utf8);
    if (run > 0) {
      if (!mt_jbuf_puts(jb, (const char *)(p + pos), run)) return 0;
      if (is_utf8 && !jb->has_non_ascii) {
        for (size_t k = 0; k < run; k++) {
          if (p[pos + k] >= 0x80) { jb->has_non_ascii = 1; break; }
        }
      }
      pos += run;
    }
    if (pos >= len) break;

    unsigned char c = p[pos++];
    if (c >= 0x80) {
      /* Latin-1 byte -> 2-byte UTF-8 */
      if (!mt_jbuf_reserve(jb, 2)) return 0;
      jb->buf[jb->len++] = (char)(0xC0 | (c >> 6));
      jb->buf[jb->len++] = (char)(0x80 | (c & 0x3F));
      jb->has_non_ascii  = 1;
    } else {
      switch (c) {
        case '"':  if (!mt_jbuf_puts(jb, "\\\"", 2)) return 0; break;
        case '\\': if (!mt_jbuf_puts(jb, "\\\\", 2)) return 0; break;
        case '/':  if (!mt_jbuf_puts(jb, "\\/",  2)) return 0; break;
        case '\b': if (!mt_jbuf_puts(jb, "\\b",  2)) return 0; break;
        case '\f': if (!mt_jbuf_puts(jb, "\\f",  2)) return 0; break;
        case '\n': if (!mt_jbuf_puts(jb, "\\n",  2)) return 0; break;
        case '\r': if (!mt_jbuf_puts(jb, "\\r",  2)) return 0; break;
        case '\t': if (!mt_jbuf_puts(jb, "\\t",  2)) return 0; break;
        default: {
          char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0F]};
          if (!mt_jbuf_puts(jb, esc, 6)) return 0;
          break;
        }
      }
    }
  }
  return mt_jbuf_putc(jb, '"');
}

/* ---- Fast IV/UV formatting ---- */

static size_t mt_fmt_uv(char *buf, UV u) {
  char tmp[24];
  size_t n = 0;
  do {
    tmp[n++] = (char)('0' + (u % 10));
    u /= 10;
  } while (u);
  for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
  buf[n] = '\0';
  return n;
}

static size_t mt_fmt_iv(char *buf, IV v) {
  if (v < 0) {
    buf[0] = '-';
    UV u = (UV)(-(v + 1)) + 1;
    return 1 + mt_fmt_uv(buf + 1, u);
  }
  return mt_fmt_uv(buf, (UV)v);
}

/* ---- Canonical hash key sorting ---- */

typedef struct {
  HE         *he;
  const char *key;
  size_t      klen;
  int         is_utf8;
} mt_jkey_t;

static int mt_jkey_cmp(const void *va, const void *vb) {
  const mt_jkey_t *a = (const mt_jkey_t *)va;
  const mt_jkey_t *b = (const mt_jkey_t *)vb;
  if (a->is_utf8 == b->is_utf8) {
    size_t minl = a->klen < b->klen ? a->klen : b->klen;
    int r = minl ? memcmp(a->key, b->key, minl) : 0;
    if (r != 0) return r;
    return (a->klen < b->klen) ? -1 : (a->klen > b->klen ? 1 : 0);
  }
  /* Mixed UTF-8 and Latin-1: compare Unicode code points */
  const unsigned char *pa = (const unsigned char *)a->key;
  const unsigned char *ea = pa + a->klen;
  const unsigned char *pb = (const unsigned char *)b->key;
  const unsigned char *eb = pb + b->klen;
  while (pa < ea && pb < eb) {
    uint32_t ca, cb;
    if (a->is_utf8 && *pa >= 0x80) {
      if ((*pa & 0xE0) == 0xC0 && pa + 1 < ea) {
        ca = ((uint32_t)(pa[0] & 0x1F) << 6) | (pa[1] & 0x3F); pa += 2;
      } else if ((*pa & 0xF0) == 0xE0 && pa + 2 < ea) {
        ca = ((uint32_t)(pa[0] & 0x0F) << 12) | ((uint32_t)(pa[1] & 0x3F) << 6) | (pa[2] & 0x3F); pa += 3;
      } else if ((*pa & 0xF8) == 0xF0 && pa + 3 < ea) {
        ca = ((uint32_t)(pa[0] & 0x07) << 18) | ((uint32_t)(pa[1] & 0x3F) << 12) | ((uint32_t)(pa[2] & 0x3F) << 6) | (pa[3] & 0x3F); pa += 4;
      } else { ca = *pa++; }
    } else { ca = *pa++; }

    if (b->is_utf8 && *pb >= 0x80) {
      if ((*pb & 0xE0) == 0xC0 && pb + 1 < eb) {
        cb = ((uint32_t)(pb[0] & 0x1F) << 6) | (pb[1] & 0x3F); pb += 2;
      } else if ((*pb & 0xF0) == 0xE0 && pb + 2 < eb) {
        cb = ((uint32_t)(pb[0] & 0x0F) << 12) | ((uint32_t)(pb[1] & 0x3F) << 6) | (pb[2] & 0x3F); pb += 3;
      } else if ((*pb & 0xF8) == 0xF0 && pb + 3 < eb) {
        cb = ((uint32_t)(pb[0] & 0x07) << 18) | ((uint32_t)(pb[1] & 0x3F) << 12) | ((uint32_t)(pb[2] & 0x3F) << 6) | (pb[3] & 0x3F); pb += 4;
      } else { cb = *pb++; }
    } else { cb = *pb++; }

    if (ca != cb) return (ca < cb) ? -1 : 1;
  }
  if (pa < ea) return 1;
  if (pb < eb) return -1;
  return 0;
}

static int mt_json_encode_val(pTHX_ mt_jbuf_t *jb, SV *sv, HV *bool_stash, int depth) {
  if (depth > MT_JSON_MAX_DEPTH) return 0;
  if (!sv) return mt_jbuf_puts(jb, "null", 4);
  SvGETMAGIC(sv);

  if (SvROK(sv)) {
    SV *rv = SvRV(sv);
    if (SvOBJECT(rv)) {
      HV *stash = SvSTASH(rv);
      if (stash == bool_stash || (bool_stash && sv_derived_from(sv, "JSON::PP::Boolean"))) {
        return SvTRUE(rv) ? mt_jbuf_puts(jb, "true", 4) : mt_jbuf_puts(jb, "false", 5);
      }
      /* Check TO_JSON */
      GV *to_json = gv_fetchmethod_autoload(stash, "TO_JSON", 0);
      if (to_json) {
        dSP;
        ENTER;
        SAVETMPS;
        PUSHMARK(SP);
        XPUSHs(sv);
        PUTBACK;
        int n = call_sv((SV *)GvCV(to_json), G_SCALAR);
        SPAGAIN;
        SV *res = (n > 0) ? POPs : &PL_sv_undef;
        int ok = mt_json_encode_val(aTHX_ jb, res, bool_stash, depth + 1);
        PUTBACK;
        FREETMPS;
        LEAVE;
        return ok;
      }
      /* Check stringification overload ("") */
      if (Gv_AMG(stash)) {
        dSP;
        ENTER;
        SAVETMPS;
        PUSHMARK(SP);
        XPUSHs(sv);
        XPUSHs(sv_2mortal(newSVpvs("\"\"")));
        PUTBACK;
        int n = call_pv("overload::Method", G_SCALAR);
        SPAGAIN;
        SV *meth = (n > 0) ? POPs : &PL_sv_undef;
        int has_str = SvTRUE(meth);
        PUTBACK;
        if (has_str) {
          STRLEN slen;
          const unsigned char *sp = (const unsigned char *)SvPV_const(sv, slen);
          int ok = mt_json_encode_str(jb, sp, (size_t)slen, SvUTF8(sv) ? 1 : 0);
          FREETMPS;
          LEAVE;
          return ok;
        }
        FREETMPS;
        LEAVE;
      }
      return mt_jbuf_puts(jb, "null", 4);
    }

    svtype t = SvTYPE(rv);
    if (t == SVt_PVHV) {
      HV *hv = (HV *)rv;
      if (!mt_jbuf_putc(jb, '{')) return 0;
      I32 nkeys = hv_iterinit(hv);
      if (nkeys > 0) {
        mt_jkey_t stack_keys[64];
        mt_jkey_t *keys = (nkeys <= 64) ? stack_keys : (mt_jkey_t *)malloc((size_t)nkeys * sizeof(mt_jkey_t));
        if (!keys) return 0;

        I32 count = 0;
        HE *he;
        while ((he = hv_iternext(hv)) != NULL && count < nkeys) {
          STRLEN klen;
          const char *kp = HePV(he, klen);
          keys[count].he      = he;
          keys[count].key     = kp;
          keys[count].klen    = (size_t)klen;
          keys[count].is_utf8 = HeUTF8(he) ? 1 : 0;
          count++;
        }
        if (count > 1) {
          qsort(keys, (size_t)count, sizeof(mt_jkey_t), mt_jkey_cmp);
        }
        for (I32 i = 0; i < count; i++) {
          if (i > 0 && !mt_jbuf_putc(jb, ',')) {
            if (keys != stack_keys) free(keys);
            return 0;
          }
          if (!mt_json_encode_str(jb, (const unsigned char *)keys[i].key, keys[i].klen, keys[i].is_utf8)
              || !mt_jbuf_putc(jb, ':')
              || !mt_json_encode_val(aTHX_ jb, HeVAL(keys[i].he), bool_stash, depth + 1)) {
            if (keys != stack_keys) free(keys);
            return 0;
          }
        }
        if (keys != stack_keys) free(keys);
      }
      return mt_jbuf_putc(jb, '}');
    }

    if (t == SVt_PVAV) {
      AV *av = (AV *)rv;
      if (!mt_jbuf_putc(jb, '[')) return 0;
      SSize_t len = av_len(av) + 1;
      for (SSize_t i = 0; i < len; i++) {
        if (i > 0 && !mt_jbuf_putc(jb, ',')) return 0;
        SV **svp = av_fetch(av, i, 0);
        if (!mt_json_encode_val(aTHX_ jb, svp ? *svp : &PL_sv_undef, bool_stash, depth + 1))
          return 0;
      }
      return mt_jbuf_putc(jb, ']');
    }

    if (t <= SVt_PVMG) {
      /* Scalar ref: \1 -> true, \0 -> false */
      return SvTRUE(rv) ? mt_jbuf_puts(jb, "true", 4) : mt_jbuf_puts(jb, "false", 5);
    }

    return mt_jbuf_puts(jb, "null", 4);
  }

  /* Undef -> null */
  if (!SvOK(sv)) return mt_jbuf_puts(jb, "null", 4);

  /* Core Perl 5.36+ booleans */
#ifdef SvIsBOOL
  if (SvIsBOOL(sv)) {
    return SvTRUE(sv) ? mt_jbuf_puts(jb, "true", 4) : mt_jbuf_puts(jb, "false", 5);
  }
#endif

  /* Numbers (without mutating sv!) */
  if (!SvUTF8(sv) && (SvIOK(sv) || SvNOK(sv))) {
    char numbuf[64];
    size_t nlen = 0;
    int is_finite = 1;

    if (SvIOK(sv)) {
      if (SvIsUV(sv)) nlen = mt_fmt_uv(numbuf, SvUVX(sv));
      else            nlen = mt_fmt_iv(numbuf, SvIVX(sv));
    } else {
      NV nv = SvNVX(sv);
      if (isfinite((double)nv)) {
        Gconvert(nv, NV_DIG, 0, numbuf);
        nlen = strlen(numbuf);
      } else {
        is_finite = 0;
      }
    }

    if (is_finite) {
      if (!SvPOK(sv)) {
        return mt_jbuf_puts(jb, numbuf, nlen);
      }
      /* Both POK and IOK/NOK: only numeric if string rep equals numeric rep */
      if (SvCUR(sv) == nlen && memcmp(SvPVX(sv), numbuf, nlen) == 0) {
        return mt_jbuf_puts(jb, numbuf, nlen);
      }
    }
  }

  /* String */
  STRLEN slen;
  const unsigned char *sp = (const unsigned char *)SvPV_nomg_const(sv, slen);
  return mt_json_encode_str(jb, sp, (size_t)slen, SvUTF8(sv) ? 1 : 0);
}

static SV *mt_json_encode(pTHX_ SV *sv, int as_utf8_bytes, HV *bool_stash) {
  mt_jbuf_t jb;
  mt_jbuf_init(&jb);
  if (!mt_json_encode_val(aTHX_ &jb, sv, bool_stash, 0)) {
    mt_jbuf_free(&jb);
    return NULL;
  }
  SV *out = newSVpvn(jb.buf, jb.len);
  if (!as_utf8_bytes && jb.has_non_ascii) {
    SvUTF8_on(out);
  }
  mt_jbuf_free(&jb);
  return out;
}

/* ============================================================================
 * Fast C/NEON JSON Decoder (RFC 8259)
 * Returns 1 on success (storing decoded SV in *out_sv), 0 on error/fallback.
 * ============================================================================ */

typedef struct {
  const unsigned char *p;
  const unsigned char *end;
  int                  is_chars;
  int                  input_is_utf8;
  SV                  *sv_true;
  SV                  *sv_false;
} mt_jdec_t;

static inline void mt_jdec_skip_ws(mt_jdec_t *d) {
  const unsigned char *p   = d->p;
  const unsigned char *end = d->end;
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  d->p = p;
}

static inline int mt_jdec_hex4(const unsigned char *p, uint32_t *out) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    unsigned char c = p[i];
    uint32_t d;
    if (c >= '0' && c <= '9')      d = (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
    else return 0;
    v = (v << 4) | d;
  }
  *out = v;
  return 1;
}

static inline void mt_jbuf_put_utf8(mt_jbuf_t *jb, uint32_t cp) {
  if (cp < 0x80) {
    mt_jbuf_putc(jb, (char)cp);
  } else if (cp < 0x800) {
    if (mt_jbuf_reserve(jb, 2)) {
      jb->buf[jb->len++] = (char)(0xC0 | (cp >> 6));
      jb->buf[jb->len++] = (char)(0x80 | (cp & 0x3F));
    }
    jb->has_non_ascii = 1;
  } else if (cp < 0x10000) {
    if (mt_jbuf_reserve(jb, 3)) {
      jb->buf[jb->len++] = (char)(0xE0 | (cp >> 12));
      jb->buf[jb->len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      jb->buf[jb->len++] = (char)(0x80 | (cp & 0x3F));
    }
    jb->has_non_ascii = 1;
  } else {
    if (mt_jbuf_reserve(jb, 4)) {
      jb->buf[jb->len++] = (char)(0xF0 | (cp >> 18));
      jb->buf[jb->len++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      jb->buf[jb->len++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      jb->buf[jb->len++] = (char)(0x80 | (cp & 0x3F));
    }
    jb->has_non_ascii = 1;
  }
}

/* Validate one UTF-8 multi-byte sequence at p..end; returns byte length (2..4) or 0 if invalid */
static inline size_t mt_utf8_validate_seq(const unsigned char *p, const unsigned char *end) {
  unsigned char b0 = p[0];
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    if (p + 1 >= end) return 0;
    if ((p[1] & 0xC0) != 0x80) return 0;
    return 2;
  }
  if (b0 >= 0xE0 && b0 <= 0xEF) {
    if (p + 2 >= end) return 0;
    unsigned char b1 = p[1], b2 = p[2];
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
    if (b0 == 0xE0 && b1 < 0xA0) return 0;  /* overlong */
    if (b0 == 0xED && b1 >= 0xA0) return 0; /* UTF-16 surrogate */
    return 3;
  }
  if (b0 >= 0xF0 && b0 <= 0xF4) {
    if (p + 3 >= end) return 0;
    unsigned char b1 = p[1], b2 = p[2], b3 = p[3];
    if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return 0;
    if (b0 == 0xF0 && b1 < 0x90) return 0;  /* overlong */
    if (b0 == 0xF4 && b1 > 0x8F) return 0;  /* > U+10FFFF */
    return 4;
  }
  return 0;
}

static inline size_t mt_jdec_scan_plain(const unsigned char *p, size_t len) {
  size_t i = 0;
#if MT_HAS_NEON
  if (len >= 16) {
    const uint8x16_t v_sp = vdupq_n_u8(0x20);
    const uint8x16_t v_qt = vdupq_n_u8('"');
    const uint8x16_t v_bs = vdupq_n_u8('\\');
    const uint8x16_t v_hi = vdupq_n_u8(0x80);
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(p + i);
      uint8x16_t m = vorrq_u8(
        vorrq_u8(vcltq_u8(v, v_sp), vceqq_u8(v, v_qt)),
        vorrq_u8(vceqq_u8(v, v_bs), vcgeq_u8(v, v_hi))
      );
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = p[i];
    if (c < 0x20 || c == '"' || c == '\\' || c >= 0x80) return i;
  }
  return len;
}

/* Decode JSON string starting right after opening '"' */
static SV *mt_jdec_string(pTHX_ mt_jdec_t *d) {
  const unsigned char *start = d->p;
  size_t rem = (size_t)(d->end - d->p);

  /* Fast path: pure unescaped ASCII string */
  size_t plain = mt_jdec_scan_plain(d->p, rem);
  if (plain < rem && d->p[plain] == '"') {
    SV *sv = newSVpvn((const char *)start, plain);
    d->p += plain + 1;
    return sv;
  }

  mt_jbuf_t jb;
  mt_jbuf_init(&jb);
  if (plain > 0) {
    if (!mt_jbuf_puts(&jb, (const char *)start, plain)) { mt_jbuf_free(&jb); return NULL; }
    d->p += plain;
  }

  while (d->p < d->end) {
    unsigned char c = *d->p;
    if (c == '"') {
      d->p++;
      SV *sv = newSVpvn(jb.buf, jb.len);
      if (jb.has_non_ascii) SvUTF8_on(sv);
      mt_jbuf_free(&jb);
      return sv;
    }
    if (c < 0x20) {
      mt_jbuf_free(&jb);
      return NULL; /* unescaped control char */
    }
    if (c == '\\') {
      d->p++;
      if (d->p >= d->end) { mt_jbuf_free(&jb); return NULL; }
      unsigned char e = *d->p++;
      switch (e) {
        case '"':  mt_jbuf_putc(&jb, '"');  break;
        case '\\': mt_jbuf_putc(&jb, '\\'); break;
        case '/':  mt_jbuf_putc(&jb, '/');  break;
        case 'b':  mt_jbuf_putc(&jb, '\b'); break;
        case 'f':  mt_jbuf_putc(&jb, '\f'); break;
        case 'n':  mt_jbuf_putc(&jb, '\n'); break;
        case 'r':  mt_jbuf_putc(&jb, '\r'); break;
        case 't':  mt_jbuf_putc(&jb, '\t'); break;
        case 'u': {
          if (d->end - d->p < 4) { mt_jbuf_free(&jb); return NULL; }
          uint32_t cp;
          if (!mt_jdec_hex4(d->p, &cp)) { mt_jbuf_free(&jb); return NULL; }
          d->p += 4;
          if ((cp & 0xF800) == 0xD800) {
            /* Surrogate pair */
            if ((cp & 0xFC00) != 0xD800) { mt_jbuf_free(&jb); return NULL; } /* lone low */
            if (d->end - d->p < 6 || d->p[0] != '\\' || d->p[1] != 'u') {
              mt_jbuf_free(&jb); return NULL;
            }
            uint32_t lo;
            if (!mt_jdec_hex4(d->p + 2, &lo) || (lo & 0xFC00) != 0xDC00) {
              mt_jbuf_free(&jb); return NULL;
            }
            d->p += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          mt_jbuf_put_utf8(&jb, cp);
          break;
        }
        default:
          mt_jbuf_free(&jb);
          return NULL;
      }
    } else if (c >= 0x80) {
      if (d->is_chars && !d->input_is_utf8) {
        /* Latin-1 byte in Perl character string -> UTF-8 */
        mt_jbuf_put_utf8(&jb, (uint32_t)c);
        d->p++;
      } else {
        size_t slen = mt_utf8_validate_seq(d->p, d->end);
        if (slen == 0) { mt_jbuf_free(&jb); return NULL; }
        if (!mt_jbuf_puts(&jb, (const char *)d->p, slen)) { mt_jbuf_free(&jb); return NULL; }
        jb.has_non_ascii = 1;
        d->p += slen;
      }
    } else {
      size_t run = mt_jdec_scan_plain(d->p, (size_t)(d->end - d->p));
      if (!mt_jbuf_puts(&jb, (const char *)d->p, run)) { mt_jbuf_free(&jb); return NULL; }
      d->p += run;
    }
  }

  mt_jbuf_free(&jb);
  return NULL; /* unterminated string */
}

static SV *mt_jdec_value(pTHX_ mt_jdec_t *d, int depth);

static SV *mt_jdec_array(pTHX_ mt_jdec_t *d, int depth) {
  if (depth > MT_JSON_MAX_DEPTH) return NULL;
  AV *av = newAV();
  mt_jdec_skip_ws(d);
  if (d->p < d->end && *d->p == ']') {
    d->p++;
    return newRV_noinc((SV *)av);
  }
  while (1) {
    SV *val = mt_jdec_value(aTHX_ d, depth);
    if (!val) { SvREFCNT_dec((SV *)av); return NULL; }
    av_push(av, val);
    mt_jdec_skip_ws(d);
    if (d->p >= d->end) { SvREFCNT_dec((SV *)av); return NULL; }
    if (*d->p == ',') { d->p++; continue; }
    if (*d->p == ']') { d->p++; return newRV_noinc((SV *)av); }
    SvREFCNT_dec((SV *)av);
    return NULL;
  }
}

static SV *mt_jdec_object(pTHX_ mt_jdec_t *d, int depth) {
  if (depth > MT_JSON_MAX_DEPTH) return NULL;
  HV *hv = newHV();
  mt_jdec_skip_ws(d);
  if (d->p < d->end && *d->p == '}') {
    d->p++;
    return newRV_noinc((SV *)hv);
  }
  while (1) {
    mt_jdec_skip_ws(d);
    if (d->p >= d->end || *d->p != '"') { SvREFCNT_dec((SV *)hv); return NULL; }
    d->p++;
    SV *key = mt_jdec_string(aTHX_ d);
    if (!key) { SvREFCNT_dec((SV *)hv); return NULL; }
    mt_jdec_skip_ws(d);
    if (d->p >= d->end || *d->p != ':') {
      SvREFCNT_dec(key);
      SvREFCNT_dec((SV *)hv);
      return NULL;
    }
    d->p++;
    SV *val = mt_jdec_value(aTHX_ d, depth);
    if (!val) {
      SvREFCNT_dec(key);
      SvREFCNT_dec((SV *)hv);
      return NULL;
    }
    if (!hv_store_ent(hv, key, val, 0)) {
      SvREFCNT_dec(val);
    }
    SvREFCNT_dec(key);

    mt_jdec_skip_ws(d);
    if (d->p >= d->end) { SvREFCNT_dec((SV *)hv); return NULL; }
    if (*d->p == ',') { d->p++; continue; }
    if (*d->p == '}') { d->p++; return newRV_noinc((SV *)hv); }
    SvREFCNT_dec((SV *)hv);
    return NULL;
  }
}

static SV *mt_jdec_number(pTHX_ mt_jdec_t *d) {
  const unsigned char *start = d->p;
  const unsigned char *p     = d->p;
  const unsigned char *end   = d->end;

  int neg = 0;
  if (p < end && *p == '-') { neg = 1; p++; }
  if (p >= end) return NULL;

  int is_float = 0;
  uint64_t ival = 0;
  size_t digits = 0;

  if (*p == '0') {
    p++;
    digits = 1;
  } else if (*p >= '1' && *p <= '9') {
    while (p < end && *p >= '0' && *p <= '9') {
      if (digits < 18) ival = ival * 10 + (uint64_t)(*p - '0');
      digits++;
      p++;
    }
  } else {
    return NULL;
  }

  if (p < end && *p == '.') {
    is_float = 1;
    p++;
    if (p >= end || *p < '0' || *p > '9') return NULL;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }

  if (p < end && (*p == 'e' || *p == 'E')) {
    is_float = 1;
    p++;
    if (p < end && (*p == '+' || *p == '-')) p++;
    if (p >= end || *p < '0' || *p > '9') return NULL;
    while (p < end && *p >= '0' && *p <= '9') p++;
  }

  d->p = p;

  if (!is_float && digits <= 18) {
    if (neg) {
      if (ival == 0) return newSVnv(-0.0);
      return newSViv(-(IV)ival);
    }
    return newSViv((IV)ival);
  }

  size_t nlen = (size_t)(p - start);
  char tmp[128];
  char *buf = (nlen < sizeof(tmp)) ? tmp : (char *)malloc(nlen + 1);
  if (!buf) return NULL;
  memcpy(buf, start, nlen);
  buf[nlen] = '\0';
  double nv = strtod(buf, NULL);
  if (buf != tmp) free(buf);
  return newSVnv((NV)nv);
}

static SV *mt_jdec_value(pTHX_ mt_jdec_t *d, int depth) {
  mt_jdec_skip_ws(d);
  if (d->p >= d->end) return NULL;

  unsigned char c = *d->p;
  if (c == '"') {
    d->p++;
    return mt_jdec_string(aTHX_ d);
  }
  if (c == '{') {
    d->p++;
    return mt_jdec_object(aTHX_ d, depth + 1);
  }
  if (c == '[') {
    d->p++;
    return mt_jdec_array(aTHX_ d, depth + 1);
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    return mt_jdec_number(aTHX_ d);
  }
  if (c == 't' && (d->end - d->p) >= 4 && memcmp(d->p, "true", 4) == 0) {
    d->p += 4;
    return SvREFCNT_inc(d->sv_true);
  }
  if (c == 'f' && (d->end - d->p) >= 5 && memcmp(d->p, "false", 5) == 0) {
    d->p += 5;
    return SvREFCNT_inc(d->sv_false);
  }
  if (c == 'n' && (d->end - d->p) >= 4 && memcmp(d->p, "null", 4) == 0) {
    d->p += 4;
    return newSV(0);
  }
  return NULL;
}

static int mt_json_decode(pTHX_ SV *sv, int is_chars, SV *sv_true, SV *sv_false, SV **out_sv) {
  if (!sv) return 0;
  SvGETMAGIC(sv);
  if (!SvOK(sv)) return 0;

  int input_is_utf8 = SvUTF8(sv) ? 1 : 0;
  /* decode_json (bytes mode) rejects UTF-8-flagged strings with wide chars -> let pure Perl throw */
  if (!is_chars && input_is_utf8) return 0;

  STRLEN len;
  const unsigned char *buf = (const unsigned char *)SvPV_nomg_const(sv, len);
  if (len == 0) return 0;

  mt_jdec_t d;
  d.p             = buf;
  d.end           = buf + len;
  d.is_chars      = is_chars;
  d.input_is_utf8 = input_is_utf8;
  d.sv_true       = sv_true;
  d.sv_false      = sv_false;

  SV *val = mt_jdec_value(aTHX_ &d, 0);
  if (!val) return 0;

  mt_jdec_skip_ws(&d);
  if (d.p != d.end) {
    SvREFCNT_dec(val);
    return 0;
  }

  *out_sv = val;
  return 1;
}

#endif /* MOJO_MT_JSON_H */
