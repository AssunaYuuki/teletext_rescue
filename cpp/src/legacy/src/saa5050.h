// Знакогенератор SAA5050: клетка 12 × 20 со сглаживанием, как в микросхеме.
// (the original engine's copy, compiled inside namespace legacy; kept different from ours for #pragma once)
#pragma once
#include <array>
#include <cstdint>
using Glyph20 = std::array<std::array<bool, 12>, 20>;
const Glyph20 *saa_rounded(char32_t cp);   // nullptr — знака нет
