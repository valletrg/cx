#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Returns pointer to first occurrence of needle in [data, data+size),
// or nullptr. Inlined so the compiler can elide call overhead and combine
// with surrounding loop in scan_lines().
//
// Uses AVX2 (32-byte) when available, SSE2 (16-byte) as fallback,
// or string_view::find as scalar baseline.

#if defined(__AVX2__)
#include <immintrin.h>

[[gnu::always_inline]] inline
const char* simd_find(const char* data, size_t size,
                      const char* needle, size_t needle_len) {
    if (needle_len == 0) return data;
    if (needle_len > size) return nullptr;
    if (needle_len == 1)
        return static_cast<const char*>(memchr(data, needle[0], size));

    const __m256i first = _mm256_set1_epi8(needle[0]);
    const __m256i last  = _mm256_set1_epi8(static_cast<char>(needle[needle_len - 1]));
    const char*   end   = data + size - needle_len + 1;
    const char*   p     = data;

    for (; p + 32 <= end; p += 32) {
        const __m256i bf = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        const __m256i bl = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(p + needle_len - 1));
        uint32_t mask = static_cast<uint32_t>(
            _mm256_movemask_epi8(_mm256_and_si256(
                _mm256_cmpeq_epi8(bf, first), _mm256_cmpeq_epi8(bl, last))));
        while (mask) {
            int bit = __builtin_ctz(mask);
            if (needle_len <= 2 ||
                memcmp(p + bit + 1, needle + 1, needle_len - 2) == 0)
                return p + bit;
            mask &= mask - 1;
        }
    }
    for (; p < end; ++p)
        if (*p == needle[0] && memcmp(p + 1, needle + 1, needle_len - 1) == 0)
            return p;
    return nullptr;
}

#elif defined(__SSE2__)
#include <emmintrin.h>

[[gnu::always_inline]] inline
const char* simd_find(const char* data, size_t size,
                      const char* needle, size_t needle_len) {
    if (needle_len == 0) return data;
    if (needle_len > size) return nullptr;
    if (needle_len == 1)
        return static_cast<const char*>(memchr(data, needle[0], size));

    const __m128i first = _mm_set1_epi8(needle[0]);
    const __m128i last  = _mm_set1_epi8(static_cast<char>(needle[needle_len - 1]));
    const char*   end   = data + size - needle_len + 1;
    const char*   p     = data;

    for (; p + 16 <= end; p += 16) {
        const __m128i bf = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
        const __m128i bl = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(p + needle_len - 1));
        uint32_t mask = static_cast<uint32_t>(
            _mm_movemask_epi8(_mm_and_si128(
                _mm_cmpeq_epi8(bf, first), _mm_cmpeq_epi8(bl, last))));
        while (mask) {
            int bit = __builtin_ctz(mask);
            if (needle_len <= 2 ||
                memcmp(p + bit + 1, needle + 1, needle_len - 2) == 0)
                return p + bit;
            mask &= mask - 1;
        }
    }
    for (; p < end; ++p)
        if (*p == needle[0] && memcmp(p + 1, needle + 1, needle_len - 1) == 0)
            return p;
    return nullptr;
}

#else

[[gnu::always_inline]] inline
const char* simd_find(const char* data, size_t size,
                      const char* needle, size_t needle_len) {
    if (needle_len == 0) return data;
    if (needle_len > size) return nullptr;
    std::string_view hay(data, size);
    auto pos = hay.find(std::string_view(needle, needle_len));
    return pos == std::string_view::npos ? nullptr : data + pos;
}

#endif
