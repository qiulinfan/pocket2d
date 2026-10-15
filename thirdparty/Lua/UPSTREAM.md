# Upstream Lua source

This directory vendors the source files from the official **Lua 5.4.9** release:

- [Official source archive](https://www.lua.org/ftp/lua-5.4.9.tar.gz)
- [Release history](https://www.lua.org/versions.html)
- [Reference manual](https://www.lua.org/manual/5.4/)
- [Known bugs and fixes](https://www.lua.org/bugs.html)

The upstream C/C++ source files are retained without local patches. Copyright
and the Lua license remain embedded in `lua.h` and the original source headers.
`thirdparty/CMakeLists.txt` builds the embedded library as C and excludes the
standalone `lua.c` and `luac.c` entry points. The host remains C++26 and uses the
LuaBridge3 bindings documented separately in `../LuaBridge/UPSTREAM.md`.

This replaces the previously vendored 5.4.6 source as a patch-level maintenance
update. The Lua source update itself does not introduce Lua 5.5, LuaJIT or Luau.
The subsequent checked-binding migration is recorded in ADR 007.
Development rationale and validation gates are in
[`docs/development/scripting-options.md`](../../docs/development/scripting-options.md).
Runtime logs, download verification output and local rollback copies belong in
ignored build storage, not this vendor directory.
