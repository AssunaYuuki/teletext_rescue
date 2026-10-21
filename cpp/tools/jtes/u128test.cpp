// the two-halves u128 of src/u128.h against unsigned __int128: the JTES syndrome and the engine's shift register
#include <cstdint>
#include <cstdio>
#include <random>
namespace nat {
#include "u128.h"
}
namespace two {
#define TR_NO_INT128
#undef TR_U128_DEFINED_HERE
#include "u128.h"
}
template <class U> U syn(const uint8_t* bits)
{
    static const int e[] = {82, 77, 76, 71, 67, 66, 56, 52, 48, 40, 36, 34, 24, 22, 18, 10, 4, 0};
    U g = 0;
    for (int x : e) g |= (U)1 << x;
    U r = 0;
    for (int i = 0; i < 272; i++) {
        r = (r << 1) | (U)bits[i];
        if ((r >> 82) & (U)1) r ^= g;
    }
    return r;
}
template <class U> U reg(const uint8_t* bits)
{
    const U MASK82 = (((U)1) << 82) - (U)1;
    U r = 0;
    for (int i = 0; i < 272; i++) r = ((r << 1) & MASK82) | (U)(int)(bits[i] ^ (int)(uint64_t)(r >> 81 & (U)1));
    return r;
}
int main()
{
    std::mt19937 rng(7);
    int bad = 0;
    for (int t = 0; t < 20000; t++) {
        uint8_t b[272];
        for (auto& x : b) x = rng() & 1;
        nat::u128 a = syn<nat::u128>(b), ra = reg<nat::u128>(b);
        two::u128 c = syn<two::u128>(b), rc = reg<two::u128>(b);
        if ((uint64_t)a != c.lo || (uint64_t)(a >> 64) != c.hi || (uint64_t)ra != rc.lo || (uint64_t)(ra >> 64) != rc.hi) bad++;
    }
    printf("20000 lines, %d differ\n", bad);
    return bad != 0;
}
