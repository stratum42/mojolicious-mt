#ifndef MOJO_MT_SIMD_H
#define MOJO_MT_SIMD_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ============================================================================
 * Multi-architecture SIMD tier selection:
 *   - PERL_FORCE_SCALAR or EBCDIC -> always exact scalar twin
 *   - amd64 (x86_64)  : AVX2 (32B) or SSE2 (16B)
 *   - arm64 (aarch64) : NEON (16B)
 *   - power (ppc64le / ppc64 BE) : VSX / AltiVec (16B, endian-safe)
 *   - s390x (IBM Z)   : VX z13+ vector facility (16B)
 *   - fallback        : portable scalar twin
 * ============================================================================ */

#if defined(PERL_FORCE_SCALAR) || defined(EBCDIC)
#  define MT_SIMD_SCALAR 1
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(__AVX2__)
#  include <immintrin.h>
#  define MT_SIMD_AVX2 1
#elif defined(__x86_64__) || defined(_M_X64) || defined(__SSE2__)
#  include <emmintrin.h>
#  define MT_SIMD_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#  include <arm_neon.h>
#  define MT_SIMD_NEON 1
#elif (defined(__powerpc64__) || defined(__PPC64__) || defined(_ARCH_PPC64)) \
   && (defined(__VSX__) || defined(__ALTIVEC__))
#  include <altivec.h>
#  ifdef vector
#    undef vector
#  endif
#  ifdef pixel
#    undef pixel
#  endif
#  ifdef bool
#    undef bool
#  endif
#  define MT_SIMD_VSX 1
#elif (defined(__s390x__) || defined(__zarch__)) && defined(__VEC__)
#  include <vecintrin.h>
#  define MT_SIMD_VX 1
#else
#  define MT_SIMD_SCALAR 1
#endif

static inline const char *mt_simd_tier_name(void) {
#if defined(MT_SIMD_AVX2)
  return "avx2";
#elif defined(MT_SIMD_SSE2)
  return "sse2";
#elif defined(MT_SIMD_NEON)
  return "neon";
#elif defined(MT_SIMD_VSX)
  return "vsx";
#elif defined(MT_SIMD_VX)
  return "vx";
#else
  return "scalar";
#endif
}

/* ---- Internal architecture helpers ---- */

#if defined(MT_SIMD_VSX)
static inline __vector unsigned char mt_vsx_loadu(const unsigned char *p) {
#  if defined(__VSX__)
  return vec_xl(0, p);
#  else
  __vector unsigned char v;
  memcpy(&v, p, 16);
  return v;
#  endif
}

/* Endian-safe first non-zero byte index (identical on ppc64le and ppc64 BE). */
static inline size_t mt_vsx_first_match(__vector unsigned char m) {
  unsigned char lanes[16];
  memcpy(lanes, &m, 16);
  for (size_t k = 0; k < 16; k++) {
    if (lanes[k]) return k;
  }
  return 16;
}
#endif

#if defined(MT_SIMD_VX)
static inline __vector unsigned char mt_vx_loadu(const unsigned char *p) {
  return vec_xl(0, p);
}

static inline size_t mt_vx_first_match(__vector unsigned char m) {
  unsigned char lanes[16];
  memcpy(lanes, &m, 16);
  for (size_t k = 0; k < 16; k++) {
    if (lanes[k]) return k;
  }
  return 16;
}
#endif

/* ============================================================================
 * 1. mt_simd_find_byte: index of first occurrence of byte c in s[0..len),
 *    or len if not found. Never reads past s + len.
 * ============================================================================ */

static inline size_t mt_simd_find_byte(const unsigned char *s, size_t len, unsigned char c) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i vc = _mm256_set1_epi8((char)c);
    for (; i + 32 <= len; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      unsigned int mask = (unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, vc));
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i vc = _mm_set1_epi8((char)c);
    for (; i + 16 <= len; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      unsigned int mask = (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(v, vc));
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_NEON)
  if (len >= 16) {
    const uint8x16_t vc = vdupq_n_u8(c);
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(s + i);
      if (vmaxvq_u8(vceqq_u8(v, vc)) != 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char vc     = vec_splats(c);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vsx_loadu(s + i);
      __vector unsigned char m = (__vector unsigned char)vec_cmpeq(v, vc);
      if (vec_any_ne(m, v_zero)) return i + mt_vsx_first_match(m);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char vc     = vec_splats(c);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vx_loadu(s + i);
      __vector unsigned char m = (__vector unsigned char)vec_cmpeq(v, vc);
      if (vec_any_ne(m, v_zero)) return i + mt_vx_first_match(m);
    }
  }
#endif
  for (; i < len; i++) {
    if (s[i] == c) return i;
  }
  return len;
}

/* ============================================================================
 * 2. mt_simd_find_any2: index of first occurrence of a or b in s[0..len),
 *    or len if not found.
 * ============================================================================ */

static inline size_t mt_simd_find_any2(const unsigned char *s, size_t len,
                                       unsigned char a, unsigned char b) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i va = _mm256_set1_epi8((char)a);
    const __m256i vb = _mm256_set1_epi8((char)b);
    for (; i + 32 <= len; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(v, va), _mm256_cmpeq_epi8(v, vb));
      unsigned int mask = (unsigned int)_mm256_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i va = _mm_set1_epi8((char)a);
    const __m128i vb = _mm_set1_epi8((char)b);
    for (; i + 16 <= len; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      __m128i m = _mm_or_si128(_mm_cmpeq_epi8(v, va), _mm_cmpeq_epi8(v, vb));
      unsigned int mask = (unsigned int)_mm_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_NEON)
  if (len >= 16) {
    const uint8x16_t va = vdupq_n_u8(a);
    const uint8x16_t vb = vdupq_n_u8(b);
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(s + i);
      uint8x16_t m = vorrq_u8(vceqq_u8(v, va), vceqq_u8(v, vb));
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char va     = vec_splats(a);
    const __vector unsigned char vb     = vec_splats(b);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vsx_loadu(s + i);
      __vector unsigned char m = vec_or((__vector unsigned char)vec_cmpeq(v, va),
                                        (__vector unsigned char)vec_cmpeq(v, vb));
      if (vec_any_ne(m, v_zero)) return i + mt_vsx_first_match(m);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char va     = vec_splats(a);
    const __vector unsigned char vb     = vec_splats(b);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vx_loadu(s + i);
      __vector unsigned char m = vec_or((__vector unsigned char)vec_cmpeq(v, va),
                                        (__vector unsigned char)vec_cmpeq(v, vb));
      if (vec_any_ne(m, v_zero)) return i + mt_vx_first_match(m);
    }
  }
#endif
  for (; i < len; i++) {
    if (s[i] == a || s[i] == b) return i;
  }
  return len;
}

/* ============================================================================
 * 3. mt_simd_find_crlf_or_nul: index of first '\r', '\n', or '\0' in s[0..len),
 *    or len if not found.
 * ============================================================================ */

static inline size_t mt_simd_find_crlf_or_nul(const unsigned char *s, size_t len) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i v_cr  = _mm256_set1_epi8('\r');
    const __m256i v_lf  = _mm256_set1_epi8('\n');
    const __m256i v_nul = _mm256_setzero_si256();
    for (; i + 32 <= len; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      __m256i m = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_cr), _mm256_cmpeq_epi8(v, v_lf)),
        _mm256_cmpeq_epi8(v, v_nul)
      );
      unsigned int mask = (unsigned int)_mm256_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i v_cr  = _mm_set1_epi8('\r');
    const __m128i v_lf  = _mm_set1_epi8('\n');
    const __m128i v_nul = _mm_setzero_si128();
    for (; i + 16 <= len; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      __m128i m = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, v_cr), _mm_cmpeq_epi8(v, v_lf)),
        _mm_cmpeq_epi8(v, v_nul)
      );
      unsigned int mask = (unsigned int)_mm_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_NEON)
  if (len >= 16) {
    const uint8x16_t v_cr  = vdupq_n_u8('\r');
    const uint8x16_t v_lf  = vdupq_n_u8('\n');
    const uint8x16_t v_nul = vdupq_n_u8(0);
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(s + i);
      uint8x16_t m = vorrq_u8(vorrq_u8(vceqq_u8(v, v_cr), vceqq_u8(v, v_lf)), vceqq_u8(v, v_nul));
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char v_cr   = vec_splats((unsigned char)'\r');
    const __vector unsigned char v_lf   = vec_splats((unsigned char)'\n');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vsx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmpeq(v, v_cr), (__vector unsigned char)vec_cmpeq(v, v_lf)),
        (__vector unsigned char)vec_cmpeq(v, v_zero)
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vsx_first_match(m);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char v_cr   = vec_splats((unsigned char)'\r');
    const __vector unsigned char v_lf   = vec_splats((unsigned char)'\n');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmpeq(v, v_cr), (__vector unsigned char)vec_cmpeq(v, v_lf)),
        (__vector unsigned char)vec_cmpeq(v, v_zero)
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vx_first_match(m);
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = s[i];
    if (c == '\r' || c == '\n' || c == '\0') return i;
  }
  return len;
}

/* ============================================================================
 * 4. mt_simd_scan_json_plain: skips bytes >= 0x20 that are not '"', '\\', '/',
 *    '<', '>', or >= 0x80.
 * ============================================================================ */

static inline size_t mt_simd_scan_json_plain(const unsigned char *s, size_t len) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i v_sp = _mm256_set1_epi8(0x20);
    const __m256i v_qt = _mm256_set1_epi8('"');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_sl = _mm256_set1_epi8('/');
    const __m256i v_lt = _mm256_set1_epi8('<');
    const __m256i v_gt = _mm256_set1_epi8('>');
    for (; i + 32 <= len; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      /* Signed 0x20 > v matches unsigned v < 0x20 OR v >= 0x80 */
      __m256i m = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpgt_epi8(v_sp, v), _mm256_cmpeq_epi8(v, v_qt)),
        _mm256_or_si256(
          _mm256_or_si256(_mm256_cmpeq_epi8(v, v_bs), _mm256_cmpeq_epi8(v, v_sl)),
          _mm256_or_si256(_mm256_cmpeq_epi8(v, v_lt), _mm256_cmpeq_epi8(v, v_gt))
        )
      );
      unsigned int mask = (unsigned int)_mm256_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i v_sp = _mm_set1_epi8(0x20);
    const __m128i v_qt = _mm_set1_epi8('"');
    const __m128i v_bs = _mm_set1_epi8('\\');
    const __m128i v_sl = _mm_set1_epi8('/');
    const __m128i v_lt = _mm_set1_epi8('<');
    const __m128i v_gt = _mm_set1_epi8('>');
    for (; i + 16 <= len; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      __m128i m = _mm_or_si128(
        _mm_or_si128(_mm_cmplt_epi8(v, v_sp), _mm_cmpeq_epi8(v, v_qt)),
        _mm_or_si128(
          _mm_or_si128(_mm_cmpeq_epi8(v, v_bs), _mm_cmpeq_epi8(v, v_sl)),
          _mm_or_si128(_mm_cmpeq_epi8(v, v_lt), _mm_cmpeq_epi8(v, v_gt))
        )
      );
      unsigned int mask = (unsigned int)_mm_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_NEON)
  if (len >= 16) {
    const uint8x16_t v_sp = vdupq_n_u8(0x20);
    const uint8x16_t v_hi = vdupq_n_u8(0x80);
    const uint8x16_t v_qt = vdupq_n_u8('"');
    const uint8x16_t v_bs = vdupq_n_u8('\\');
    const uint8x16_t v_sl = vdupq_n_u8('/');
    const uint8x16_t v_lt = vdupq_n_u8('<');
    const uint8x16_t v_gt = vdupq_n_u8('>');
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(s + i);
      uint8x16_t m = vorrq_u8(
        vorrq_u8(vorrq_u8(vcltq_u8(v, v_sp), vcgeq_u8(v, v_hi)),
                 vorrq_u8(vceqq_u8(v, v_qt), vceqq_u8(v, v_bs))),
        vorrq_u8(vceqq_u8(v, v_sl), vorrq_u8(vceqq_u8(v, v_lt), vceqq_u8(v, v_gt)))
      );
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char v_sp   = vec_splats((unsigned char)0x20);
    const __vector unsigned char v_hi   = vec_splats((unsigned char)0x80);
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_bs   = vec_splats((unsigned char)'\\');
    const __vector unsigned char v_sl   = vec_splats((unsigned char)'/');
    const __vector unsigned char v_lt   = vec_splats((unsigned char)'<');
    const __vector unsigned char v_gt   = vec_splats((unsigned char)'>');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vsx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or(vec_or((__vector unsigned char)vec_cmplt(v, v_sp),
                      (__vector unsigned char)vec_cmpge(v, v_hi)),
               vec_or((__vector unsigned char)vec_cmpeq(v, v_qt),
                      (__vector unsigned char)vec_cmpeq(v, v_bs))),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_sl),
               vec_or((__vector unsigned char)vec_cmpeq(v, v_lt),
                      (__vector unsigned char)vec_cmpeq(v, v_gt)))
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vsx_first_match(m);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char v_sp   = vec_splats((unsigned char)0x20);
    const __vector unsigned char v_hi   = vec_splats((unsigned char)0x80);
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_bs   = vec_splats((unsigned char)'\\');
    const __vector unsigned char v_sl   = vec_splats((unsigned char)'/');
    const __vector unsigned char v_lt   = vec_splats((unsigned char)'<');
    const __vector unsigned char v_gt   = vec_splats((unsigned char)'>');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or(vec_or((__vector unsigned char)vec_cmplt(v, v_sp),
                      (__vector unsigned char)vec_cmpge(v, v_hi)),
               vec_or((__vector unsigned char)vec_cmpeq(v, v_qt),
                      (__vector unsigned char)vec_cmpeq(v, v_bs))),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_sl),
               vec_or((__vector unsigned char)vec_cmpeq(v, v_lt),
                      (__vector unsigned char)vec_cmpeq(v, v_gt)))
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vx_first_match(m);
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = s[i];
    if (c < 0x20 || c >= 0x80 || c == '"' || c == '\\' || c == '/' || c == '<' || c == '>') {
      return i;
    }
  }
  return len;
}

/* ============================================================================
 * 4b. mt_simd_scan_json_enc: Mojo::JSON string encoder scanner.
 *     Stops on < 0x20, '"', '\\', '/', and (when !is_utf8) >= 0x80.
 * ============================================================================ */

static inline size_t mt_simd_scan_json_enc(const unsigned char *s, size_t len, int is_utf8) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i v_sp   = _mm256_set1_epi8(0x20);
    const __m256i v_bias = _mm256_set1_epi8((char)0x80);
    const __m256i v_spb  = _mm256_set1_epi8((char)(0x20 ^ 0x80));
    const __m256i v_qt   = _mm256_set1_epi8('"');
    const __m256i v_bs   = _mm256_set1_epi8('\\');
    const __m256i v_sl   = _mm256_set1_epi8('/');
    for (; i + 32 <= len; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      __m256i ctrl = is_utf8
        ? _mm256_cmpgt_epi8(v_spb, _mm256_xor_si256(v, v_bias))
        : _mm256_cmpgt_epi8(v_sp, v);
      __m256i m = _mm256_or_si256(
        _mm256_or_si256(ctrl, _mm256_cmpeq_epi8(v, v_qt)),
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_bs), _mm256_cmpeq_epi8(v, v_sl))
      );
      unsigned int mask = (unsigned int)_mm256_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i v_sp   = _mm_set1_epi8(0x20);
    const __m128i v_bias = _mm_set1_epi8((char)0x80);
    const __m128i v_spb  = _mm_set1_epi8((char)(0x20 ^ 0x80));
    const __m128i v_qt   = _mm_set1_epi8('"');
    const __m128i v_bs   = _mm_set1_epi8('\\');
    const __m128i v_sl   = _mm_set1_epi8('/');
    for (; i + 16 <= len; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      __m128i ctrl = is_utf8
        ? _mm_cmplt_epi8(_mm_xor_si128(v, v_bias), v_spb)
        : _mm_cmplt_epi8(v, v_sp);
      __m128i m = _mm_or_si128(
        _mm_or_si128(ctrl, _mm_cmpeq_epi8(v, v_qt)),
        _mm_or_si128(_mm_cmpeq_epi8(v, v_bs), _mm_cmpeq_epi8(v, v_sl))
      );
      unsigned int mask = (unsigned int)_mm_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_NEON)
  if (len >= 16) {
    const uint8x16_t v_sp = vdupq_n_u8(0x20);
    const uint8x16_t v_qt = vdupq_n_u8('"');
    const uint8x16_t v_bs = vdupq_n_u8('\\');
    const uint8x16_t v_sl = vdupq_n_u8('/');
    const uint8x16_t v_hi = vdupq_n_u8(0x80);
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(s + i);
      uint8x16_t m = vorrq_u8(
        vorrq_u8(vcltq_u8(v, v_sp), vceqq_u8(v, v_qt)),
        vorrq_u8(vceqq_u8(v, v_bs), vceqq_u8(v, v_sl))
      );
      if (!is_utf8) m = vorrq_u8(m, vcgeq_u8(v, v_hi));
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char v_sp   = vec_splats((unsigned char)0x20);
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_bs   = vec_splats((unsigned char)'\\');
    const __vector unsigned char v_sl   = vec_splats((unsigned char)'/');
    const __vector unsigned char v_hi   = vec_splats((unsigned char)0x80);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vsx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmplt(v, v_sp),
               (__vector unsigned char)vec_cmpeq(v, v_qt)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_bs),
               (__vector unsigned char)vec_cmpeq(v, v_sl))
      );
      if (!is_utf8) m = vec_or(m, (__vector unsigned char)vec_cmpge(v, v_hi));
      if (vec_any_ne(m, v_zero)) return i + mt_vsx_first_match(m);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char v_sp   = vec_splats((unsigned char)0x20);
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_bs   = vec_splats((unsigned char)'\\');
    const __vector unsigned char v_sl   = vec_splats((unsigned char)'/');
    const __vector unsigned char v_hi   = vec_splats((unsigned char)0x80);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmplt(v, v_sp),
               (__vector unsigned char)vec_cmpeq(v, v_qt)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_bs),
               (__vector unsigned char)vec_cmpeq(v, v_sl))
      );
      if (!is_utf8) m = vec_or(m, (__vector unsigned char)vec_cmpge(v, v_hi));
      if (vec_any_ne(m, v_zero)) return i + mt_vx_first_match(m);
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = s[i];
    if (c < 0x20 || c == '"' || c == '\\' || c == '/' || (!is_utf8 && c >= 0x80)) {
      return i;
    }
  }
  return len;
}

/* ============================================================================
 * 4c. mt_simd_scan_json_dec: RFC 8259 JSON string decoder scanner.
 *     Stops on < 0x20, '"', '\\', or >= 0x80.
 * ============================================================================ */

static inline size_t mt_simd_scan_json_dec(const unsigned char *s, size_t len) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i v_sp = _mm256_set1_epi8(0x20);
    const __m256i v_qt = _mm256_set1_epi8('"');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    for (; i + 32 <= len; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      __m256i m = _mm256_or_si256(
        _mm256_cmpgt_epi8(v_sp, v),
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_qt), _mm256_cmpeq_epi8(v, v_bs))
      );
      unsigned int mask = (unsigned int)_mm256_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i v_sp = _mm_set1_epi8(0x20);
    const __m128i v_qt = _mm_set1_epi8('"');
    const __m128i v_bs = _mm_set1_epi8('\\');
    for (; i + 16 <= len; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      __m128i m = _mm_or_si128(
        _mm_cmplt_epi8(v, v_sp),
        _mm_or_si128(_mm_cmpeq_epi8(v, v_qt), _mm_cmpeq_epi8(v, v_bs))
      );
      unsigned int mask = (unsigned int)_mm_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_NEON)
  if (len >= 16) {
    const uint8x16_t v_sp = vdupq_n_u8(0x20);
    const uint8x16_t v_qt = vdupq_n_u8('"');
    const uint8x16_t v_bs = vdupq_n_u8('\\');
    const uint8x16_t v_hi = vdupq_n_u8(0x80);
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(s + i);
      uint8x16_t m = vorrq_u8(
        vorrq_u8(vcltq_u8(v, v_sp), vceqq_u8(v, v_qt)),
        vorrq_u8(vceqq_u8(v, v_bs), vcgeq_u8(v, v_hi))
      );
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char v_sp   = vec_splats((unsigned char)0x20);
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_bs   = vec_splats((unsigned char)'\\');
    const __vector unsigned char v_hi   = vec_splats((unsigned char)0x80);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vsx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmplt(v, v_sp),
               (__vector unsigned char)vec_cmpeq(v, v_qt)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_bs),
               (__vector unsigned char)vec_cmpge(v, v_hi))
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vsx_first_match(m);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char v_sp   = vec_splats((unsigned char)0x20);
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_bs   = vec_splats((unsigned char)'\\');
    const __vector unsigned char v_hi   = vec_splats((unsigned char)0x80);
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmplt(v, v_sp),
               (__vector unsigned char)vec_cmpeq(v, v_qt)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_bs),
               (__vector unsigned char)vec_cmpge(v, v_hi))
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vx_first_match(m);
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = s[i];
    if (c < 0x20 || c == '"' || c == '\\' || c >= 0x80) return i;
  }
  return len;
}

/* ============================================================================
 * 5. mt_simd_scan_xml_plain: skips bytes that are not '&', '<', '>', '"', '\''.
 * ============================================================================ */

static inline size_t mt_simd_scan_xml_plain(const unsigned char *s, size_t len) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i v_amp = _mm256_set1_epi8('&');
    const __m256i v_lt  = _mm256_set1_epi8('<');
    const __m256i v_gt  = _mm256_set1_epi8('>');
    const __m256i v_qt  = _mm256_set1_epi8('"');
    const __m256i v_ap  = _mm256_set1_epi8('\'');
    for (; i + 32 <= len; i += 32) {
      __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      __m256i m = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_amp), _mm256_cmpeq_epi8(v, v_lt)),
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_gt),
                        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_qt), _mm256_cmpeq_epi8(v, v_ap)))
      );
      unsigned int mask = (unsigned int)_mm256_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i v_amp = _mm_set1_epi8('&');
    const __m128i v_lt  = _mm_set1_epi8('<');
    const __m128i v_gt  = _mm_set1_epi8('>');
    const __m128i v_qt  = _mm_set1_epi8('"');
    const __m128i v_ap  = _mm_set1_epi8('\'');
    for (; i + 16 <= len; i += 16) {
      __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      __m128i m = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, v_amp), _mm_cmpeq_epi8(v, v_lt)),
        _mm_or_si128(_mm_cmpeq_epi8(v, v_gt),
                     _mm_or_si128(_mm_cmpeq_epi8(v, v_qt), _mm_cmpeq_epi8(v, v_ap)))
      );
      unsigned int mask = (unsigned int)_mm_movemask_epi8(m);
      if (mask) return i + (size_t)__builtin_ctz(mask);
    }
  }
#elif defined(MT_SIMD_NEON)
  if (len >= 16) {
    const uint8x16_t v_amp = vdupq_n_u8('&');
    const uint8x16_t v_lt  = vdupq_n_u8('<');
    const uint8x16_t v_gt  = vdupq_n_u8('>');
    const uint8x16_t v_qt  = vdupq_n_u8('"');
    const uint8x16_t v_ap  = vdupq_n_u8('\'');
    for (; i + 16 <= len; i += 16) {
      uint8x16_t v = vld1q_u8(s + i);
      uint8x16_t m = vorrq_u8(
        vorrq_u8(vceqq_u8(v, v_amp), vceqq_u8(v, v_lt)),
        vorrq_u8(vceqq_u8(v, v_gt), vorrq_u8(vceqq_u8(v, v_qt), vceqq_u8(v, v_ap)))
      );
      if (vmaxvq_u8(m) != 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char v_amp  = vec_splats((unsigned char)'&');
    const __vector unsigned char v_lt   = vec_splats((unsigned char)'<');
    const __vector unsigned char v_gt   = vec_splats((unsigned char)'>');
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_ap   = vec_splats((unsigned char)'\'');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vsx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmpeq(v, v_amp),
               (__vector unsigned char)vec_cmpeq(v, v_lt)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_gt),
               vec_or((__vector unsigned char)vec_cmpeq(v, v_qt),
                      (__vector unsigned char)vec_cmpeq(v, v_ap)))
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vsx_first_match(m);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char v_amp  = vec_splats((unsigned char)'&');
    const __vector unsigned char v_lt   = vec_splats((unsigned char)'<');
    const __vector unsigned char v_gt   = vec_splats((unsigned char)'>');
    const __vector unsigned char v_qt   = vec_splats((unsigned char)'"');
    const __vector unsigned char v_ap   = vec_splats((unsigned char)'\'');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v = mt_vx_loadu(s + i);
      __vector unsigned char m = vec_or(
        vec_or((__vector unsigned char)vec_cmpeq(v, v_amp),
               (__vector unsigned char)vec_cmpeq(v, v_lt)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_gt),
               vec_or((__vector unsigned char)vec_cmpeq(v, v_qt),
                      (__vector unsigned char)vec_cmpeq(v, v_ap)))
      );
      if (vec_any_ne(m, v_zero)) return i + mt_vx_first_match(m);
    }
  }
#endif
  for (; i < len; i++) {
    unsigned char c = s[i];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') return i;
  }
  return len;
}

/* ============================================================================
 * 6. mt_simd_scan_url_plain: skips RFC 3986 unreserved [A-Za-z0-9\-._~].
 * ============================================================================ */

static inline int mt_simd_is_url_unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
      || c == '-' || c == '.' || c == '_' || c == '~';
}

static inline size_t mt_simd_scan_url_plain(const unsigned char *s, size_t len) {
  size_t i = 0;
#if defined(MT_SIMD_AVX2)
  if (len >= 32) {
    const __m256i v_A    = _mm256_set1_epi8((char)((unsigned char)'A' + 128));
    const __m256i v_a    = _mm256_set1_epi8((char)((unsigned char)'a' + 128));
    const __m256i v_0    = _mm256_set1_epi8((char)((unsigned char)'0' + 128));
    const __m256i v_26   = _mm256_set1_epi8((char)(26 - 128));
    const __m256i v_10   = _mm256_set1_epi8((char)(10 - 128));
    const __m256i v_dash = _mm256_set1_epi8('-');
    const __m256i v_dot  = _mm256_set1_epi8('.');
    const __m256i v_und  = _mm256_set1_epi8('_');
    const __m256i v_til  = _mm256_set1_epi8('~');
    for (; i + 32 <= len; i += 32) {
      __m256i v        = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
      __m256i is_upper = _mm256_cmpgt_epi8(v_26, _mm256_sub_epi8(v, v_A));
      __m256i is_lower = _mm256_cmpgt_epi8(v_26, _mm256_sub_epi8(v, v_a));
      __m256i is_digit = _mm256_cmpgt_epi8(v_10, _mm256_sub_epi8(v, v_0));
      __m256i is_sym   = _mm256_or_si256(
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_dash), _mm256_cmpeq_epi8(v, v_dot)),
        _mm256_or_si256(_mm256_cmpeq_epi8(v, v_und),  _mm256_cmpeq_epi8(v, v_til))
      );
      __m256i ok = _mm256_or_si256(_mm256_or_si256(is_upper, is_lower),
                                   _mm256_or_si256(is_digit, is_sym));
      unsigned int bad_mask = ~(unsigned int)_mm256_movemask_epi8(ok);
      if (bad_mask) return i + (size_t)__builtin_ctz(bad_mask);
    }
  }
#elif defined(MT_SIMD_SSE2)
  if (len >= 16) {
    const __m128i v_A    = _mm_set1_epi8((char)((unsigned char)'A' + 128));
    const __m128i v_a    = _mm_set1_epi8((char)((unsigned char)'a' + 128));
    const __m128i v_0    = _mm_set1_epi8((char)((unsigned char)'0' + 128));
    const __m128i v_26   = _mm_set1_epi8((char)(26 - 128));
    const __m128i v_10   = _mm_set1_epi8((char)(10 - 128));
    const __m128i v_dash = _mm_set1_epi8('-');
    const __m128i v_dot  = _mm_set1_epi8('.');
    const __m128i v_und  = _mm_set1_epi8('_');
    const __m128i v_til  = _mm_set1_epi8('~');
    for (; i + 16 <= len; i += 16) {
      __m128i v        = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
      __m128i is_upper = _mm_cmplt_epi8(_mm_sub_epi8(v, v_A), v_26);
      __m128i is_lower = _mm_cmplt_epi8(_mm_sub_epi8(v, v_a), v_26);
      __m128i is_digit = _mm_cmplt_epi8(_mm_sub_epi8(v, v_0), v_10);
      __m128i is_sym   = _mm_or_si128(
        _mm_or_si128(_mm_cmpeq_epi8(v, v_dash), _mm_cmpeq_epi8(v, v_dot)),
        _mm_or_si128(_mm_cmpeq_epi8(v, v_und),  _mm_cmpeq_epi8(v, v_til))
      );
      __m128i ok = _mm_or_si128(_mm_or_si128(is_upper, is_lower),
                                _mm_or_si128(is_digit, is_sym));
      unsigned int bad_mask = (~(unsigned int)_mm_movemask_epi8(ok)) & 0xFFFFU;
      if (bad_mask) return i + (size_t)__builtin_ctz(bad_mask);
    }
  }
#elif defined(MT_SIMD_NEON)
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
      uint8x16_t v        = vld1q_u8(s + i);
      uint8x16_t is_upper = vcltq_u8(vsubq_u8(v, v_A), v_26);
      uint8x16_t is_lower = vcltq_u8(vsubq_u8(v, v_a), v_26);
      uint8x16_t is_digit = vcltq_u8(vsubq_u8(v, v_0), v_10);
      uint8x16_t is_sym   = vorrq_u8(
        vorrq_u8(vceqq_u8(v, v_dash), vceqq_u8(v, v_dot)),
        vorrq_u8(vceqq_u8(v, v_und),  vceqq_u8(v, v_til))
      );
      uint8x16_t ok = vorrq_u8(vorrq_u8(is_upper, is_lower), vorrq_u8(is_digit, is_sym));
      if (vminvq_u8(ok) == 0) break;
    }
  }
#elif defined(MT_SIMD_VSX)
  if (len >= 16) {
    const __vector unsigned char v_A    = vec_splats((unsigned char)'A');
    const __vector unsigned char v_a    = vec_splats((unsigned char)'a');
    const __vector unsigned char v_0    = vec_splats((unsigned char)'0');
    const __vector unsigned char v_26   = vec_splats((unsigned char)26);
    const __vector unsigned char v_10   = vec_splats((unsigned char)10);
    const __vector unsigned char v_dash = vec_splats((unsigned char)'-');
    const __vector unsigned char v_dot  = vec_splats((unsigned char)'.');
    const __vector unsigned char v_und  = vec_splats((unsigned char)'_');
    const __vector unsigned char v_til  = vec_splats((unsigned char)'~');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v        = mt_vsx_loadu(s + i);
      __vector unsigned char is_upper = (__vector unsigned char)vec_cmplt(vec_sub(v, v_A), v_26);
      __vector unsigned char is_lower = (__vector unsigned char)vec_cmplt(vec_sub(v, v_a), v_26);
      __vector unsigned char is_digit = (__vector unsigned char)vec_cmplt(vec_sub(v, v_0), v_10);
      __vector unsigned char is_sym   = vec_or(
        vec_or((__vector unsigned char)vec_cmpeq(v, v_dash), (__vector unsigned char)vec_cmpeq(v, v_dot)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_und),  (__vector unsigned char)vec_cmpeq(v, v_til))
      );
      __vector unsigned char ok  = vec_or(vec_or(is_upper, is_lower), vec_or(is_digit, is_sym));
      __vector unsigned char bad = vec_nor(ok, ok);
      if (vec_any_ne(bad, v_zero)) return i + mt_vsx_first_match(bad);
    }
  }
#elif defined(MT_SIMD_VX)
  if (len >= 16) {
    const __vector unsigned char v_A    = vec_splats((unsigned char)'A');
    const __vector unsigned char v_a    = vec_splats((unsigned char)'a');
    const __vector unsigned char v_0    = vec_splats((unsigned char)'0');
    const __vector unsigned char v_26   = vec_splats((unsigned char)26);
    const __vector unsigned char v_10   = vec_splats((unsigned char)10);
    const __vector unsigned char v_dash = vec_splats((unsigned char)'-');
    const __vector unsigned char v_dot  = vec_splats((unsigned char)'.');
    const __vector unsigned char v_und  = vec_splats((unsigned char)'_');
    const __vector unsigned char v_til  = vec_splats((unsigned char)'~');
    const __vector unsigned char v_zero = vec_splats((unsigned char)0);
    for (; i + 16 <= len; i += 16) {
      __vector unsigned char v        = mt_vx_loadu(s + i);
      __vector unsigned char is_upper = (__vector unsigned char)vec_cmplt(v - v_A, v_26);
      __vector unsigned char is_lower = (__vector unsigned char)vec_cmplt(v - v_a, v_26);
      __vector unsigned char is_digit = (__vector unsigned char)vec_cmplt(v - v_0, v_10);
      __vector unsigned char is_sym   = vec_or(
        vec_or((__vector unsigned char)vec_cmpeq(v, v_dash), (__vector unsigned char)vec_cmpeq(v, v_dot)),
        vec_or((__vector unsigned char)vec_cmpeq(v, v_und),  (__vector unsigned char)vec_cmpeq(v, v_til))
      );
      __vector unsigned char ok  = vec_or(vec_or(is_upper, is_lower), vec_or(is_digit, is_sym));
      __vector unsigned char bad = vec_nor(ok, ok);
      if (vec_any_ne(bad, v_zero)) return i + mt_vx_first_match(bad);
    }
  }
#endif
  for (; i < len; i++) {
    if (!mt_simd_is_url_unreserved(s[i])) return i;
  }
  return len;
}

#endif /* MOJO_MT_SIMD_H */
