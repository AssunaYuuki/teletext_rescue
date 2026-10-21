// 128-bit unsigned words for the code syndromes of Japanese teletext: unsigned __int128 where the compiler has it
// (64-bit GCC / Clang), otherwise two 64-bit halves with the operations used (32-bit x86 builds).
// No #pragma once and no includes: the engine of the original program includes it inside namespace legacy.
#ifndef TR_U128_DEFINED_HERE
#if defined(__SIZEOF_INT128__) && !defined(TR_NO_INT128)   // TR_NO_INT128: check the 32-bit form on a 64-bit compiler
typedef unsigned __int128 u128;
#else
struct u128 {
    uint64_t lo = 0, hi = 0;
    u128() = default;
    u128(uint64_t v) : lo(v) {}
    u128(unsigned v) : lo(v) {}
    u128(int v) : lo((uint64_t)(int64_t)v), hi(v < 0 ? ~0ULL : 0) {}
    u128 operator<<(int n) const
    {
        u128 r;
        if (n >= 128) return r;
        if (n >= 64) r.hi = lo << (n - 64);
        else if (n > 0) { r.hi = hi << n | lo >> (64 - n); r.lo = lo << n; }
        else r = *this;
        return r;
    }
    u128 operator>>(int n) const
    {
        u128 r;
        if (n >= 128) return r;
        if (n >= 64) r.lo = hi >> (n - 64);
        else if (n > 0) { r.lo = lo >> n | hi << (64 - n); r.hi = hi >> n; }
        else r = *this;
        return r;
    }
    u128 operator|(const u128& o) const { u128 r; r.lo = lo | o.lo; r.hi = hi | o.hi; return r; }
    u128 operator^(const u128& o) const { u128 r; r.lo = lo ^ o.lo; r.hi = hi ^ o.hi; return r; }
    u128 operator&(const u128& o) const { u128 r; r.lo = lo & o.lo; r.hi = hi & o.hi; return r; }
    u128 operator-(const u128& o) const { u128 r; r.lo = lo - o.lo; r.hi = hi - o.hi - (lo < o.lo); return r; }
    u128& operator|=(const u128& o) { lo |= o.lo; hi |= o.hi; return *this; }
    u128& operator^=(const u128& o) { lo ^= o.lo; hi ^= o.hi; return *this; }
    u128& operator&=(const u128& o) { lo &= o.lo; hi &= o.hi; return *this; }
    bool operator==(const u128& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const u128& o) const { return !(*this == o); }
    explicit operator bool() const { return lo || hi; }
    explicit operator uint64_t() const { return lo; }
    explicit operator uint32_t() const { return (uint32_t)lo; }
    explicit operator int() const { return (int)lo; }
};
#endif
#endif
