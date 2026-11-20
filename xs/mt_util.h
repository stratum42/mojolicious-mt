#ifndef MOJO_MT_UTIL_H
#define MOJO_MT_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__aarch64__)
#  include <arm_neon.h>
#  define MT_HAS_NEON 1
#else
#  define MT_HAS_NEON 0
#endif

/* Lookup tables (const, read-only, zero mutable global state) */
static const char MT_HEX_UPPER[16] = "0123456789ABCDEF";

static const uint8_t MT_URL_UNRESERVED[256] = {
  /* 0x00 - 0x1F */
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  /* 0x20 - 0x3F: '-' (0x2D), '.' (0x2E), '0'-'9' (0x30-0x39) */
  0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,0, 1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
  /* 0x40 - 0x5F: 'A'-'Z' (0x41-0x5A), '_' (0x5F) */
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,
  /* 0x60 - 0x7F: 'a'-'z' (0x61-0x7A), '~' (0x7E) */
  0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,0,
  /* 0x80 - 0xFF */
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const int8_t MT_HEX_VAL[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
   0, 1, 2, 3, 4, 5, 6, 7, 8, 9,-1,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static const char MT_B64_ENC[65] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* -1 = skip, -2 = pad '=', 0..63 = valid 6-bit value */
static const int8_t MT_B64_DEC[256] = {
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
  52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
  -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
  15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
  -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
  41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

/* ============================================================================
 * xml_escape: NEON-accelerated scan for [&<>"']
 * ============================================================================ */

static inline size_t mt_find_xml_special(const unsigned char *p, size_t len) {
  size_t i = 0;
#if MT_HAS_NEON
  if (len >= 16) {
    const uint8x16_t v_amp = vdupq_n_u8('&');
    const uint8x16_t v_lt  = vdupq_n_u8('<');
    const uint8x16_t v_gt  = vdupq_n_u8('>');
    const uint8x16_t v_qt  = vdupq_n_u8('"');
    const uint8x16_t v_ap  = vdupq_n_u8('\'');
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(p + i);
      uint8x16_t m = vorrq_u8(
        vorrq_u8(vceqq_u8(v, v_amp), vceqq_u8(v, v_lt)),
        vorrq_u8(vceqq_u8(v, v_gt), vorrq_u8(vceqq_u8(v, v_qt), vceqq_u8(v, v_ap)))
      );
      if (vmaxvq_u8(m) != 0) {
        break;
      }
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = p[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return i;
  }
  return len;
}

static SV *mt_xml_escape(pTHX_ SV *sv) {
  if (!sv) return newSVpvs("");
  SvGETMAGIC(sv);
  if (SvROK(sv) && sv_isobject(sv)) {
    const char *pkg = HvNAME(SvSTASH(SvRV(sv)));
    if (pkg && strEQ(pkg, "Mojo::ByteStream")) {
      return newSVsv(sv);
    }
  }
  if (!SvOK(sv)) return newSVpvs("");

  STRLEN len;
  const unsigned char *src = (const unsigned char *)SvPV_nomg_const(sv, len);
  int is_utf8 = SvUTF8(sv) ? 1 : 0;

  size_t pos = mt_find_xml_special(src, (size_t)len);
  if (pos == (size_t)len) {
    SV *out = newSVpvn((const char *)src, len);
    if (is_utf8) SvUTF8_on(out);
    return out;
  }

  /* Allocate with headroom (at least len + 32, grow if needed) */
  SV *out = newSV(len + (len >> 2) + 32);
  SvPOK_on(out);
  if (is_utf8) SvUTF8_on(out);

  size_t start = 0;
  while (pos < (size_t)len) {
    if (pos > start) {
      sv_catpvn_nomg(out, (const char *)(src + start), pos - start);
    }
    switch (src[pos]) {
      case '&':  sv_catpvs(out, "&amp;");  break;
      case '<':  sv_catpvs(out, "&lt;");   break;
      case '>':  sv_catpvs(out, "&gt;");   break;
      case '"':  sv_catpvs(out, "&quot;"); break;
      case '\'': sv_catpvs(out, "&#39;");  break;
    }
    start = pos + 1;
    pos = start + mt_find_xml_special(src + start, (size_t)len - start);
  }
  if (start < (size_t)len) {
    sv_catpvn_nomg(out, (const char *)(src + start), (size_t)len - start);
  }
  return out;
}

/* ============================================================================
 * url_escape (default pattern: [^A-Za-z0-9\-._~])
 * Returns NULL if fallback to pure Perl is needed (wide UTF-8 chars > 0x7F).
 * ============================================================================ */

static inline size_t mt_find_url_escape(const unsigned char *p, size_t len) {
  size_t i = 0;
#if MT_HAS_NEON
  if (len >= 16) {
    const uint8x16_t v_A    = vdupq_n_u8('A');
    const uint8x16_t v_a    = vdupq_n_u8('a');
    const uint8x16_t v_0    = vdupq_n_u8('0');
    const uint8x16_t v_26   = vdupq_n_u8(26);
    const uint8x16_t v_10   = vdupq_n_u8(10);
    const uint8x16_t v_dash = vdupq_n_u8('-');
    const uint8x16_t v_dot  = vdupq_n_u8('.');
    const uint8x16_t v_und  = vdupq_n_u8('_');
    const uint8x16_t v_til  = vdupq_n_u8('~');
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(p + i);
      uint8x16_t is_upper = vcltq_u8(vsubq_u8(v, v_A), v_26);
      uint8x16_t is_lower = vcltq_u8(vsubq_u8(v, v_a), v_26);
      uint8x16_t is_digit = vcltq_u8(vsubq_u8(v, v_0), v_10);
      uint8x16_t is_sym   = vorrq_u8(
        vorrq_u8(vceqq_u8(v, v_dash), vceqq_u8(v, v_dot)),
        vorrq_u8(vceqq_u8(v, v_und),  vceqq_u8(v, v_til))
      );
      uint8x16_t ok = vorrq_u8(vorrq_u8(is_upper, is_lower), vorrq_u8(is_digit, is_sym));
      if (vminvq_u8(ok) == 0) {
        break;
      }
    }
  }
#endif
  for (; i < len; i++) {
    if (!MT_URL_UNRESERVED[p[i]]) return i;
  }
  return len;
}

static SV *mt_url_escape(pTHX_ SV *sv) {
  if (!sv) return newSVpvs("");
  SvGETMAGIC(sv);
  if (!SvOK(sv)) return newSVpvs("");

  STRLEN len;
  const unsigned char *src = (const unsigned char *)SvPV_nomg_const(sv, len);
  int is_utf8 = SvUTF8(sv) ? 1 : 0;

  if (is_utf8) {
    /* If UTF-8 string contains non-ASCII chars, fall back to pure Perl for
     * exact ord($1) multi-hex parity on code points > 0xFF. */
    for (STRLEN k = 0; k < len; k++) {
      if (src[k] >= 0x80) return NULL;
    }
  }

  size_t pos = mt_find_url_escape(src, (size_t)len);
  if (pos == (size_t)len) {
    return newSVpvn((const char *)src, len);
  }

  SV *out = newSV(len * 3 + 1);
  SvPOK_on(out);
  char *dst = SvPVX(out);
  char *d   = dst;

  if (pos > 0) {
    memcpy(d, src, pos);
    d += pos;
  }
  for (size_t i = pos; i < (size_t)len; i++) {
    unsigned char c = src[i];
    if (MT_URL_UNRESERVED[c]) {
      *d++ = (char)c;
    } else {
      *d++ = '%';
      *d++ = MT_HEX_UPPER[c >> 4];
      *d++ = MT_HEX_UPPER[c & 0x0F];
    }
  }
  *d = '\0';
  SvCUR_set(out, (STRLEN)(d - dst));
  return out;
}

/* ============================================================================
 * url_unescape: NEON-accelerated scan for '%'
 * ============================================================================ */

static inline size_t mt_find_pct(const unsigned char *p, size_t len) {
  size_t i = 0;
#if MT_HAS_NEON
  if (len >= 16) {
    const uint8x16_t v_pct = vdupq_n_u8('%');
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(p + i);
      if (vmaxvq_u8(vceqq_u8(v, v_pct)) != 0) break;
    }
  }
#endif
  for (; i < len; i++) {
    if (p[i] == '%') return i;
  }
  return len;
}

static SV *mt_url_unescape(pTHX_ SV *sv) {
  if (!sv) return newSVpvs("");
  SvGETMAGIC(sv);
  if (!SvOK(sv)) return newSVpvs("");

  STRLEN len;
  const unsigned char *src = (const unsigned char *)SvPV_nomg_const(sv, len);
  int is_utf8 = SvUTF8(sv) ? 1 : 0;

  size_t pos = mt_find_pct(src, (size_t)len);
  if (pos == (size_t)len) {
    SV *out = newSVpvn((const char *)src, len);
    if (is_utf8) SvUTF8_on(out);
    return out;
  }

  /* When is_utf8 is set, %80..%FF expands to 2 UTF-8 bytes (still <= 3 input bytes) */
  SV *out = newSV(len + 1);
  SvPOK_on(out);
  if (is_utf8) SvUTF8_on(out);
  unsigned char *dst = (unsigned char *)SvPVX(out);
  unsigned char *d   = dst;

  if (pos > 0) {
    memcpy(d, src, pos);
    d += pos;
  }
  size_t i = pos;
  while (i < (size_t)len) {
    if (src[i] == '%' && i + 2 < (size_t)len) {
      int8_t hi = MT_HEX_VAL[src[i + 1]];
      int8_t lo = MT_HEX_VAL[src[i + 2]];
      if (hi >= 0 && lo >= 0) {
        unsigned char val = (unsigned char)((hi << 4) | lo);
        if (is_utf8 && val >= 0x80) {
          *d++ = (unsigned char)(0xC0 | (val >> 6));
          *d++ = (unsigned char)(0x80 | (val & 0x3F));
        } else {
          *d++ = val;
        }
        i += 3;
        continue;
      }
    }
    *d++ = src[i++];
  }
  *d = '\0';
  SvCUR_set(out, (STRLEN)(d - dst));
  return out;
}

/* ============================================================================
 * b64_encode / b64_decode (100% MIME::Base64 parity)
 * ============================================================================ */

static SV *mt_b64_encode(pTHX_ SV *sv, SV *eol_sv) {
  if (!sv) return newSVpvs("");
  SvGETMAGIC(sv);
  if (!SvOK(sv)) return newSVpvs("");

  STRLEN len;
  const unsigned char *src;
  if (SvUTF8(sv)) {
    /* Fall back to MIME::Base64 if string has wide chars > 0xFF (to croak identically),
     * or downgrade Latin-1 chars if all <= 0xFF. */
    STRLEN ulen;
    const unsigned char *up = (const unsigned char *)SvPV_nomg_const(sv, ulen);
    for (STRLEN k = 0; k < ulen; k++) {
      if (up[k] >= 0x80) return NULL; /* let MIME::Base64 handle Latin-1/wide */
    }
    src = up;
    len = ulen;
  } else {
    src = (const unsigned char *)SvPV_nomg_const(sv, len);
  }

  const char *eol = "\n";
  STRLEN eol_len  = 1;
  if (eol_sv) {
    SvGETMAGIC(eol_sv);
    if (SvOK(eol_sv)) {
      eol = SvPV_nomg_const(eol_sv, eol_len);
    } else {
      eol = "";
      eol_len = 0;
    }
  }

  if (len == 0) return newSVpvs("");

  /* Exact output size */
  size_t triplets  = (size_t)len / 3;
  size_t rem       = (size_t)len % 3;
  size_t b64_chars = (triplets + (rem ? 1 : 0)) * 4;
  size_t num_lines = eol_len ? ((b64_chars + 75) / 76) : 0;
  size_t out_len   = b64_chars + num_lines * (size_t)eol_len;

  SV *out = newSV(out_len + 1);
  SvPOK_on(out);
  char *dst = SvPVX(out);
  char *d   = dst;

  if (eol_len == 0) {
    /* Fast path: no line wrapping (HTTP/JWT/Basic Auth) */
    size_t i = 0;
    for (; i + 3 <= (size_t)len; i += 3) {
      uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | (uint32_t)src[i + 2];
      *d++ = MT_B64_ENC[(v >> 18) & 0x3F];
      *d++ = MT_B64_ENC[(v >> 12) & 0x3F];
      *d++ = MT_B64_ENC[(v >>  6) & 0x3F];
      *d++ = MT_B64_ENC[ v        & 0x3F];
    }
    if (rem == 1) {
      uint32_t v = (uint32_t)src[i] << 16;
      *d++ = MT_B64_ENC[(v >> 18) & 0x3F];
      *d++ = MT_B64_ENC[(v >> 12) & 0x3F];
      *d++ = '=';
      *d++ = '=';
    } else if (rem == 2) {
      uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
      *d++ = MT_B64_ENC[(v >> 18) & 0x3F];
      *d++ = MT_B64_ENC[(v >> 12) & 0x3F];
      *d++ = MT_B64_ENC[(v >>  6) & 0x3F];
      *d++ = '=';
    }
  } else {
    /* Line-wrapped every 76 base64 chars (57 input bytes) */
    size_t i = 0;
    size_t col = 0;
    for (; i + 3 <= (size_t)len; i += 3) {
      uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | (uint32_t)src[i + 2];
      *d++ = MT_B64_ENC[(v >> 18) & 0x3F];
      *d++ = MT_B64_ENC[(v >> 12) & 0x3F];
      *d++ = MT_B64_ENC[(v >>  6) & 0x3F];
      *d++ = MT_B64_ENC[ v        & 0x3F];
      col += 4;
      if (col == 76) {
        memcpy(d, eol, eol_len);
        d += eol_len;
        col = 0;
      }
    }
    if (rem == 1) {
      uint32_t v = (uint32_t)src[i] << 16;
      *d++ = MT_B64_ENC[(v >> 18) & 0x3F];
      *d++ = MT_B64_ENC[(v >> 12) & 0x3F];
      *d++ = '=';
      *d++ = '=';
      col += 4;
    } else if (rem == 2) {
      uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8);
      *d++ = MT_B64_ENC[(v >> 18) & 0x3F];
      *d++ = MT_B64_ENC[(v >> 12) & 0x3F];
      *d++ = MT_B64_ENC[(v >>  6) & 0x3F];
      *d++ = '=';
      col += 4;
    }
    if (col > 0) {
      memcpy(d, eol, eol_len);
      d += eol_len;
    }
  }

  *d = '\0';
  SvCUR_set(out, (STRLEN)(d - dst));
  return out;
}

static SV *mt_b64_decode(pTHX_ SV *sv) {
  if (!sv) return newSVpvs("");
  SvGETMAGIC(sv);
  if (!SvOK(sv)) return newSVpvs("");

  STRLEN len;
  const unsigned char *src = (const unsigned char *)SvPV_nomg_const(sv, len);
  if (len == 0) return newSVpvs("");

  SV *out = newSV(((len + 3) / 4) * 3 + 1);
  SvPOK_on(out);
  unsigned char *dst = (unsigned char *)SvPVX(out);
  unsigned char *d   = dst;

  uint32_t buf = 0;
  int bits = 0;
  for (STRLEN i = 0; i < len; i++) {
    int8_t v = MT_B64_DEC[src[i]];
    if (v >= 0) {
      buf = (buf << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        *d++ = (unsigned char)((buf >> bits) & 0xFF);
      }
    } else if (v == -2) {
      break; /* '=' padding */
    }
  }
  *d = '\0';
  SvCUR_set(out, (STRLEN)(d - dst));
  return out;
}

#endif /* MOJO_MT_UTIL_H */
