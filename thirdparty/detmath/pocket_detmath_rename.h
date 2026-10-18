/* Force-included into each openlibm source file built by
   cmake/GameEngineDetMath.cmake: every symbol it defines (and every alias it
   would create) gets a pocket_det_ name, so nothing replaces the platform's
   own libm. */
#define sin pocket_det_sin
#define cos pocket_det_cos
#define tan pocket_det_tan
#define asin pocket_det_asin
#define acos pocket_det_acos
#define atan pocket_det_atan
#define atan2 pocket_det_atan2
#define exp pocket_det_exp
#define log pocket_det_log
#define log2 pocket_det_log2
#define log10 pocket_det_log10
#define pow pocket_det_pow
#define scalbn pocket_det_scalbn
#define floor pocket_det_floor
#define copysign pocket_det_copysign
#define ldexp pocket_det_ldexp
#define __kernel_sin pocket_det_kernel_sin
#define __kernel_cos pocket_det_kernel_cos
#define __kernel_tan pocket_det_kernel_tan
#define __ieee754_rem_pio2 pocket_det_rem_pio2
#define __kernel_rem_pio2 pocket_det_kernel_rem_pio2
#define sinl pocket_det_sinl
#define cosl pocket_det_cosl
#define tanl pocket_det_tanl
#define asinl pocket_det_asinl
#define acosl pocket_det_acosl
#define atanl pocket_det_atanl
#define atan2l pocket_det_atan2l
#define expl pocket_det_expl
#define logl pocket_det_logl
#define log2l pocket_det_log2l
#define log10l pocket_det_log10l
#define powl pocket_det_powl
#define scalbnl pocket_det_scalbnl
#define ldexpl pocket_det_ldexpl
#define floorl pocket_det_floorl
#define copysignl pocket_det_copysignl

/* A static library inside the engine: no dllexport (MSVC also rejects the
   kernels' plain declarations against their exported definitions). */
#define OPENLIBM_DEFS_H_
#define OLM_DLLEXPORT

/* MSVC's C compiler has no C99 complex type: skip openlibm's complex header
   (only the real functions are built; their complex declarations go unused). */
#if defined(_MSC_VER) && !defined(__clang__)
#define OPENLIBM_COMPLEX_H
#define complex
/* MSVC emits no symbol for an extern __inline function (C99 inline), but
   s_sin/s_cos/s_tan call __ieee754_rem_pio2 as an ordinary function. */
#define __inline
#endif

/* The aliases (sinl for sin, ldexp for scalbn...) are not wanted, and their
   names would be stringified before renaming: define them away, after the
   header that defines them (its include guard keeps this). */
#include "cdefs-compat.h"
#undef openlibm_weak_reference
#define openlibm_weak_reference(sym, alias)
#undef openlibm_strong_reference
#define openlibm_strong_reference(sym, alias)
