#pragma once
/*
 * Minimal <math.h> for TAPP builds — SINGLE PRECISION ONLY.
 *
 * The double entry points (sin, cos, pow, exp, log, sqrt, ...) are NOT exported by the firmware.
 * Calling one links fine but fails to load on device, so they are deliberately not declared:
 * you get a compile error naming the exact call site instead. Use the `f` variants.
 *
 * Two groups below:
 *   - extern  : exported by firmware (lib/tapp/api/tapp_api_table.c), resolved at load time.
 *   - inline  : NOT exported, but Cortex-M7 FPv5 has a single instruction for them, so the
 *               builtin expands in place and costs no import. Verified with llvm-nm: none of
 *               these produce a libcall (vsqrt / vabs / vminnm / vmaxnm / vrintX).
 *
 * Absent on purpose (no export AND no instruction): asinf, acosf, log2f, cbrtf, tanhf, ldexpf,
 * copysignf. logf is provided via the exported log10f.
 */

/* ---- exported by firmware ---- */
float sinf(float x);
float cosf(float x);
float tanf(float x);
float sinhf(float x);
float coshf(float x);
float asinhf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float exp2f(float x);
float expm1f(float x);
float log10f(float x);
float log1pf(float x);
float powf(float x, float y);
float pow10f(float x);
float fmodf(float x, float y);
float hypotf(float x, float y);
float frexpf(float x, int* exp);
float scalbnf(float x, int n);
long  lrintf(float x);
int   finitef(float x);
float nanf(const char* tag);

/* ---- single FPv5 instruction, no import needed ---- */

/*
 * sqrtf must NOT go through __builtin_sqrtf here. Without -fno-math-errno (implied by -ffast-math)
 * the builtin is allowed to lower to a *call* to sqrtf — i.e. to this very function — which at -O0
 * compiles to infinite recursion (verified: one `bl <sqrtf>` inside <sqrtf>). tapp-build always
 * passes -ffast-math so it happens to be safe, but a header must not depend on that. There is no
 * libm to fall back to on this target, so a libcall is always wrong: emit the instruction directly.
 */
#if defined(__ARM_FP) && (__ARM_FP & 4)   /* single-precision VFP present */
static inline float sqrtf(float x) {
    float r;
    __asm__("vsqrt.f32 %0, %1" : "=t"(r) : "t"(x));
    return r;
}
#else
static inline float sqrtf(float x)           { return __builtin_sqrtf(x); }
#endif

static inline float fabsf(float x)           { return __builtin_fabsf(x); }
static inline float fminf(float x, float y)  { return __builtin_fminf(x, y); }
static inline float fmaxf(float x, float y)  { return __builtin_fmaxf(x, y); }
static inline float ceilf(float x)           { return __builtin_ceilf(x); }
static inline float floorf(float x)          { return __builtin_floorf(x); }
static inline float roundf(float x)          { return __builtin_roundf(x); }
static inline float truncf(float x)          { return __builtin_truncf(x); }
static inline float rintf(float x)           { return __builtin_rintf(x); }

/* natural log via the exported base-10 one (ln(x) = log10(x) * ln(10)) */
static inline float logf(float x)            { return log10f(x) * 2.302585093f; }

/*
 * ---- classification: compiler builtins, no imports ----
 *
 * CAUTION: tapp-build compiles with -ffast-math, which implies -ffinite-math-only — the compiler is
 * then entitled to assume NaN and infinity never occur, so isnan()/isinf() fold to constant false
 * and INFINITY/NAN comparisons are undefined. clang warns (-Wnan-infinity-disabled) at each use.
 * These are provided for source compatibility, not as a working guard: to reject bad input, range-
 * check it (e.g. `x > 0.0f && x < 1e30f`) rather than testing for NaN.
 */
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)

#define INFINITY      __builtin_inff()
#define NAN           __builtin_nanf("")
#define HUGE_VALF     __builtin_inff()

/*
 * Constants are float-suffixed on purpose: tapp-build compiles with -Werror=double-promotion, so a
 * double-typed M_PI would make `x * M_PI` a hard error rather than the intended f32 multiply.
 */
#define M_PI      3.14159265358979323846f
#define M_PI_2    1.57079632679489661923f
#define M_PI_4    0.78539816339744830962f
#define M_1_PI    0.31830988618379067154f
#define M_2_PI    0.63661977236758134308f
#define M_TWOPI   6.28318530717958647692f   /* non-standard, kept for existing app code */
#define M_E       2.71828182845904523536f
#define M_LOG2E   1.44269504088896340736f
#define M_LOG10E  0.43429448190325182765f
#define M_LN2     0.69314718055994530942f
#define M_LN10    2.30258509299404568402f
#define M_SQRT2   1.41421356237309504880f
#define M_SQRT1_2 0.70710678118654752440f
