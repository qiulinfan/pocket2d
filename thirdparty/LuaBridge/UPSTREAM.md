# LuaBridge3 source

Source: https://github.com/kunitoki/LuaBridge3
Release: 3.0-rc12 (prerelease)
Commit: 53e031b7df6a14d43f92a54fd792b76dbadcc970
Imported directory: Source/LuaBridge
License: MIT, retained in LICENSE.txt and individual source headers.

Local patches (upstream license and remaining source retained):

1. `detail/LuaHelpers.h`: `equalstates` returns immediately for identical non-null
   lua_State pointers. Distinct pointers still use the upstream registry
   comparison, preserving coroutine-versus-independent-VM behavior. This
   addresses the profiled Release assertion path without disabling checks. See
   `lua_vm_test.cpp` and `binding-runtime-performance.md`.
2. `detail/CFunctions.h` and the instance fallback registrations in
   `detail/Namespace.h`: class instance receivers must be non-null. Typed member,
   const member, member-C-function and direct-property entry points decode once,
   finish the non-trivial result object's lifetime, then reject null before
   invoking native code. Class-bound free proxies/functors reject nil/missing
   self before constructing argument tuples. Class-bound raw C callbacks reuse
   one C closure with class-receiver checking and the configured exception guard;
   no extra Lua call layer is introduced. Generic index/newindex entry points
   turn invalid receiver shapes, missing metatables or missing property lookup
   tables into Lua errors rather than assertion failures/invalid table access.
   The existing lookup order and registered namespace/other-userdata behavior
   are retained. Namespace/static functions and
   ordinary nullable pointer arguments retain their prior conversion rules;
   `Userdata::get` is not globally changed. This is receiver validation, not a
   sandbox for arbitrary metatable/upvalue tampering. Coverage is provided by
   the generic receiver and real-agent regression tests in Pocket2D.

Pocket2D uses C++26, Lua 5.4.9 compiled as C, and
LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING=1. Explicit engine conversion/call
policies live in scripting/LuaBoundary.h; source/migration evidence is in
docs/development/adr-007-checked-lua-binding.md and the current status.
