/* Deterministic double math: openlibm's (FreeBSD msun / FDLIBM) functions,
   built from source with plain IEEE arithmetic and no FMA contraction, so
   every platform computes the same bits. Lua's math library and ^ use them
   (pocket_lua_user.h); cmake/GameEngineDetMath.cmake builds them. */
#ifndef POCKET_DETMATH_H
#define POCKET_DETMATH_H

#ifdef __cplusplus
extern "C" {
#endif

double pocket_det_sin(double x);
double pocket_det_cos(double x);
double pocket_det_tan(double x);
double pocket_det_asin(double x);
double pocket_det_acos(double x);
double pocket_det_atan(double x);
double pocket_det_atan2(double y, double x);
double pocket_det_exp(double x);
double pocket_det_log(double x);
double pocket_det_log2(double x);
double pocket_det_log10(double x);
double pocket_det_pow(double x, double y);

#ifdef __cplusplus
}
#endif

#endif
