file(GLOB GAME_ENGINE_LUA_HEADERS CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/thirdparty/Lua/*.h"
    "${PROJECT_SOURCE_DIR}/thirdparty/Lua/*.hpp"
)
file(GLOB GAME_ENGINE_LUA_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/thirdparty/Lua/*.c"
)
list(FILTER GAME_ENGINE_LUA_SOURCES EXCLUDE REGEX "/lua\\.c$")
list(FILTER GAME_ENGINE_LUA_SOURCES EXCLUDE REGEX "/luac\\.c$")
list(SORT GAME_ENGINE_LUA_HEADERS)
list(SORT GAME_ENGINE_LUA_SOURCES)

add_library(lua_runtime STATIC)
target_sources(
    lua_runtime
    PRIVATE
        ${GAME_ENGINE_LUA_SOURCES}
        ${GAME_ENGINE_LUA_HEADERS}
)
target_include_directories(
    lua_runtime
    PUBLIC
        "${PROJECT_SOURCE_DIR}/thirdparty/Lua"
)
target_compile_definitions(
    lua_runtime
    PRIVATE
        $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
        # _setjmp/_longjmp skip the signal-mask syscalls of setjmp/longjmp on
        # every protected call; also selects POSIX popen, mkstemp, *_r time.
        $<$<NOT:$<PLATFORM_ID:Windows>>:LUA_USE_POSIX>
        # A fixed string-hash seed, and transcendental functions and ^ through
        # deterministic math (thirdparty/detmath/pocket_lua_user.h).
        "LUA_USER_H=\"pocket_lua_user.h\""
)
target_link_libraries(lua_runtime PRIVATE pocket_detmath)
target_compile_options(
    lua_runtime
    PRIVATE
        $<$<C_COMPILER_ID:MSVC>:/W3>
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wall>
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wextra>
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-Wno-deprecated-declarations>
        # Each float operation rounds on its own, as on every other platform.
        $<$<NOT:$<C_COMPILER_ID:MSVC>>:-ffp-contract=off>
)
set_target_properties(lua_runtime PROPERTIES FOLDER "thirdparty")

add_library(imgui_runtime STATIC)
target_sources(
    imgui_runtime
    PRIVATE
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/imgui.cpp"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/imgui_draw.cpp"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/imgui_tables.cpp"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/imgui_widgets.cpp"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdl3.cpp"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdlrenderer3.cpp"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/imgui.h"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/imgui_internal.h"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/imconfig.h"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdl3.h"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/backends/imgui_impl_sdlrenderer3.h"
)
target_include_directories(
    imgui_runtime
    PUBLIC
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui"
        "${PROJECT_SOURCE_DIR}/thirdparty/imgui/backends"
)
target_link_libraries(imgui_runtime PUBLIC game_engine_sdl)
set_target_properties(imgui_runtime PROPERTIES FOLDER "thirdparty")

# Dear ImGui Test Engine (its own license, thirdparty/imgui_test_engine/LICENSE.txt)
# drives editor UI tests. Its hooks are compiled into ImGui itself, so the
# engine's sources join the same static library.
option(POCKET_ENABLE_IMGUI_TEST_ENGINE "Build ImGui with Dear ImGui Test Engine hooks for editor UI tests" ON)
if(POCKET_ENABLE_IMGUI_TEST_ENGINE)
    file(GLOB POCKET_IMGUI_TEST_ENGINE_SOURCES "${PROJECT_SOURCE_DIR}/thirdparty/imgui_test_engine/*.cpp")
    target_sources(imgui_runtime PRIVATE ${POCKET_IMGUI_TEST_ENGINE_SOURCES})
    target_include_directories(imgui_runtime PUBLIC "${PROJECT_SOURCE_DIR}/thirdparty/imgui_test_engine")
    target_compile_definitions(imgui_runtime PUBLIC
        IMGUI_USER_CONFIG="imgui_te_imconfig.h"
        IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
        IMGUI_TEST_ENGINE_ENABLE_CAPTURE=0)
endif()

add_library(imguizmo_runtime STATIC)
target_sources(
    imguizmo_runtime
    PRIVATE
        "${PROJECT_SOURCE_DIR}/thirdparty/imguizmo/ImGuizmo.cpp"
        "${PROJECT_SOURCE_DIR}/thirdparty/imguizmo/ImGuizmo.h"
)
target_include_directories(
    imguizmo_runtime
    PUBLIC
        "${PROJECT_SOURCE_DIR}/thirdparty/imguizmo"
)
target_link_libraries(imguizmo_runtime PUBLIC imgui_runtime)
set_target_properties(imguizmo_runtime PROPERTIES FOLDER "thirdparty")
