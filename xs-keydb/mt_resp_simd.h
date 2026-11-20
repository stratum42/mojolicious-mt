/* mt_resp_simd.h - Multi-architecture bounds-safe byte scanner for RESP2/RESP3.
 *
 * Supports:
 *   - AMD64 AVX2 (32-byte) and SSE2 (16-byte)
 *   - ARM64 NEON (16-byte)
 *   - POWER VSX / AltiVec (16-byte, little-endian and big-endian)
 *   - IBM Z s390x VX (16-byte, z13+ vector facility)
 *   - Portable scalar fallback (always selected under PERL_FORCE_SCALAR or EBCDIC)
 *
 * No vector path reads outside [s, s + len): vector loops only run while a
 * complete vector width fits inside the remaining buffer, and any trailing
 * bytes are scanned by the scalar twin.
 */

#ifndef MT_RESP_SIMD_H
#define MT_RESP_SIMD_H

#include <stddef.h>
#include <stdint.h>

#if defined(PERL_FORCE_SCALAR) || defined(EBCDIC)
#  define MT_RESP_SIMD_SCALAR 1
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) && defined(__AVX2__)
#  define MT_RESP_SIMD_AVX2 1
#  include <immintrin.h>
#elif (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)) && defined(__SSE2__)
#  define MT_RESP_SIMD_SSE2 1
#  include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define MT_RESP_SIMD_NEON 1
#  include <arm_neon.h>
#elif (defined(__powerpc64__) || defined(__PPC64__)) && (defined(__VSX__) || defined(__ALTIVEC__))
#  define MT_RESP_SIMD_VSX 1
#  include <altivec.h>
#  undef vector
#  undef pixel
#  undef bool
#elif (defined(__s390x__) || defined(__zarch__)) && defined(__VEC__)
#  define MT_RESP_SIMD_S390X_VX 1
#  include <vecintrin.h>
#else
#  define MT_RESP_SIMD_SCALAR 1
#endif

static inline const char *mt_resp_simd_tier_name(void) {
#if defined(MT_RESP_SIMD_AVX2)
  return "avx2";
#elif defined(MT_RESP_SIMD_SSE2)
  return "sse2";
#elif defined(MT_RESP_SIMD_NEON)
  return "neon";
#elif defined(MT_RESP_SIMD_VSX)
  return "vsx";
#elif defined(MT_RESP_SIMD_S390X_VX)
  return "s390x_vx";
#else
  return "scalar";
#endif
}

/* Portable scalar twin: never reads outside [s, s + len). */
static inline const unsigned char *
mt_resp_find_byte_scalar(const unsigned char *s, size_t len, unsigned char c) {
  for (size_t i = 0; i < len; i++) {
    if (s[i] == c) return s + i;
  }
  return NULL;
}

#if defined(MT_RESP_SIMD_AVX2) || defined(MT_RESP_SIMD_SSE2)
static inline size_t mt_resp_ctz32(unsigned int mask) {
#if defined(_MSC_VER)
  unsigned long idx = 0;
  _BitScanForward(&idx, (unsigned long)mask);
  return (size_t)idx;
#else
  return (size_t)__builtin_ctz(mask);
#endif
}
#endif

#if defined(MT_RESP_SIMD_NEON) && !defined(__AARCH64EB__)
static inline size_t mt_resp_ctz64(uint64_t mask) {
#if defined(_MSC_VER)
  unsigned long idx = 0;
  _BitScanForward64(&idx, mask);
  return (size_t)idx;
#else
  return (size_t)__builtin_ctzll(mask);
#endif
}
#endif

/* Find first occurrence of byte c in [s, s + len), or NULL if not present. */
static inline const unsigned char *
mt_resp_find_byte(const unsigned char *s, size_t len, unsigned char c) {
#if defined(MT_RESP_SIMD_AVX2)
  const __m256i vc = _mm256_set1_epi8((char)c);
  size_t i = 0;
  while (i + 32 <= len) {
    __m256i chunk = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
    unsigned int mask = (unsigned int)_mm256_movemask_epi8(_mm256_cmpeq_epi8(chunk, vc));
    if (mask != 0) {
      return s + i + mt_resp_ctz32(mask);
    }
    i += 32;
  }
  if (i + 16 <= len) {
    const __m128i vc16 = _mm_set1_epi8((char)c);
    __m128i chunk = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
    unsigned int mask = (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vc16));
    if (mask != 0) {
      return s + i + mt_resp_ctz32(mask);
    }
    i += 16;
  }
  return mt_resp_find_byte_scalar(s + i, len - i, c);

#elif defined(MT_RESP_SIMD_SSE2)
  const __m128i vc = _mm_set1_epi8((char)c);
  size_t i = 0;
  while (i + 16 <= len) {
    __m128i chunk = _mm_loadu_si128((const __m128i *)(const void *)(s + i));
    unsigned int mask = (unsigned int)_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, vc));
    if (mask != 0) {
      return s + i + mt_resp_ctz32(mask);
    }
    i += 16;
  }
  return mt_resp_find_byte_scalar(s + i, len - i, c);

#elif defined(MT_RESP_SIMD_NEON)
  const uint8x16_t vc = vdupq_n_u8(c);
  size_t i = 0;
  while (i + 16 <= len) {
    uint8x16_t chunk = vld1q_u8(s + i);
    uint8x16_t eq    = vceqq_u8(chunk, vc);
    if (vmaxvq_u8(eq) != 0) {
#if !defined(__AARCH64EB__)
      uint8x8_t narrowed = vshrn_n_u16(vreinterpretq_u16_u8(eq), 4);
      uint64_t bits      = vget_lane_u64(vreinterpret_u64_u8(narrowed), 0);
      return s + i + (mt_resp_ctz64(bits) >> 2);
#else
      return mt_resp_find_byte_scalar(s + i, 16, c);
#endif
    }
    i += 16;
  }
  return mt_resp_find_byte_scalar(s + i, len - i, c);

#elif defined(MT_RESP_SIMD_VSX)
  const __vector unsigned char vc = vec_splats(c);
  size_t i = 0;
  while (i + 16 <= len) {
    __vector unsigned char chunk;
#if defined(__VSX__)
    chunk = vec_xl(0, s + i);
#else
    __builtin_memcpy(&chunk, s + i, 16);
#endif
    if (vec_any_eq(chunk, vc)) {
      return mt_resp_find_byte_scalar(s + i, 16, c);
    }
    i += 16;
  }
  return mt_resp_find_byte_scalar(s + i, len - i, c);

#elif defined(MT_RESP_SIMD_S390X_VX)
  const __vector unsigned char vc = vec_splats(c);
  size_t i = 0;
  while (i + 16 <= len) {
    __vector unsigned char chunk = vec_xl(0, s + i);
    __vector unsigned char res   = vec_find_any_eq_idx(chunk, vc);
    unsigned int idx             = (unsigned int)vec_extract(res, 7);
    if (idx < 16) {
      return s + i + (size_t)idx;
    }
    i += 16;
  }
  return mt_resp_find_byte_scalar(s + i, len - i, c);

#else
  return mt_resp_find_byte_scalar(s, len, c);
#endif
}

#endif /* MT_RESP_SIMD_H */
