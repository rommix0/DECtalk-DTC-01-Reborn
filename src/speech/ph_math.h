/* The ROM's fixed-point helpers, with their exact 68000 results (REFERENCE.md s15.17).
 * Arithmetic right shifts of negative values are assumed (true for MSVC, GCC and Clang). */
#ifndef PH_MATH_H
#define PH_MATH_H
#include <stdint.h>

/* frac_mul_q14 0x1d80a: muls.w, lsr.l #14, low word used. The logical shift only changes bits the callers drop. */
static inline int16_t q14(int a, int b)
{
    return (int16_t)(((int32_t)(int16_t)a * (int16_t)b) >> 14);
}

/* frac_mul_q12 0x1d862: muls.w of the two low words, lsl.l #4, swap, ext.l. */
static inline int16_t q12(int a, int b)
{
    return (int16_t)(((int32_t)(int16_t)a * (int16_t)b) >> 12);
}

/* muldiv_globals 0x1d81c: muls.w a*b, divs.w c. divs.w leaves D0 unchanged on overflow, so the result is then
 * the low word of the product. A zero divisor traps on the 68000; it cannot happen on this path. */
static inline int16_t muldiv(int a, int b, int c)
{
    int32_t p = (int32_t)(int16_t)a * (int16_t)b, q;
    if ((int16_t)c == 0) return (int16_t)p;
    q = p / (int16_t)c;
    if (q < -32768 || q > 32767) return (int16_t)p;
    return (int16_t)q;
}

/* long_divide 0x1d6ae: signed 32-bit divide, truncating toward zero. Exact for divisors below 0x10000, the only
 * ones on this path (the ROM's approximation for larger divisors is not reproduced). A zero divisor traps on
 * the 68000; here it returns 0. */
static inline int32_t long_divide(int32_t a, int32_t b)
{
    return b ? a / b : 0;
}

#endif
