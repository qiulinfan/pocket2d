/* LUA_USER_H for lua_runtime, included by lua.h after luaconf.h.

   A fixed string-hash seed (stock Lua mixes in the time and addresses), so
   table iteration order is the same in every run and on every machine.

   Transcendental functions and ^ go through the deterministic functions
   (pocket_detmath.h), so scripts compute the same bits on every platform.
   Functions IEEE 754 defines exactly (floor, sqrt, fmod...) stay the
   platform's. Replays, lockstep and rollback netplay depend on both. */
#ifndef POCKET_LUA_USER_H
#define POCKET_LUA_USER_H

#define luai_makeseed(L) ((unsigned int)0x2D504B32u)

#include <math.h>
#include "pocket_detmath.h"

#define sin pocket_det_sin
#define cos pocket_det_cos
#define tan pocket_det_tan
#define asin pocket_det_asin
#define acos pocket_det_acos
#define atan2 pocket_det_atan2
#define exp pocket_det_exp
#define log pocket_det_log
#define log2 pocket_det_log2
#define log10 pocket_det_log10
#define pow pocket_det_pow

#endif
