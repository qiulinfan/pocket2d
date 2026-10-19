cmake_minimum_required(VERSION 3.30)

project(pocket2d VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(WIN32)   # windows.h must not define min/max macros over std::min/std::max
    add_compile_definitions(NOMINMAX)
endif()
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # CMake knows no C++26 flag for MSVC; its newest mode is /std:c++latest.
    set(CMAKE_CXX_STANDARD 23)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/std:c++latest> $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
                        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
endif()
if(EMSCRIPTEN)   # Emscripten's EM_ASM (SDL uses it) needs the GNU dialects
    set(CMAKE_C_EXTENSIONS ON)
    set(CMAKE_CXX_EXTENSIONS ON)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
find_package(Threads REQUIRED)

option(
    GAME_ENGINE_STAGE_RESOURCES
    "Copy the engine's .engine/system and .engine/templates next to the executable after each build."
    ON
)
option(
    GAME_ENGINE_STAGE_SDL_RUNTIME
    "Stage vendored SDL runtime files next to the executable when needed."
    ON
)

if(CMAKE_GENERATOR STREQUAL "Xcode")
    set(CMAKE_XCODE_GENERATE_SCHEME ON)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GameEngineSDL)
game_engine_ensure_sdl()
include(GameEngineTracy)
include(GameEngineBox2D)
include(GameEngineDetMath)

add_library(game_engine_vendor_headers INTERFACE)
target_compile_definitions(game_engine_vendor_headers INTERFACE
    LUABRIDGE_SAFE_LUA_C_EXCEPTION_HANDLING=1)
target_include_directories(
    game_engine_vendor_headers
    SYSTEM
    INTERFACE
        "${PROJECT_SOURCE_DIR}/thirdparty"
        "${PROJECT_SOURCE_DIR}/thirdparty/Lua"
)

# game and pocket build into the same folder, so one target stages it for both:
# separate per-executable copies collide when the two link in parallel.
function(game_engine_stage_resources target)
    if(GAME_ENGINE_STAGE_RESOURCES)
        if(NOT TARGET game_engine_resources)
            add_custom_target(
                game_engine_resources
                COMMAND
                    ${CMAKE_COMMAND} -E copy_directory_if_different
                    "${PROJECT_SOURCE_DIR}/.engine/system"
                    "$<TARGET_FILE_DIR:${target}>/.engine/system"
                COMMAND
                    ${CMAKE_COMMAND} -E copy_directory_if_different
                    "${PROJECT_SOURCE_DIR}/.engine/templates"
                    "$<TARGET_FILE_DIR:${target}>/.engine/templates"
                COMMENT "Staging engine resources and project templates"
            )
        endif()
        add_dependencies(${target} game_engine_resources)
    endif()
endfunction()

function(game_engine_apply_cxx_warnings target)
    target_compile_definitions(
        ${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
            $<$<CXX_COMPILER_ID:MSVC>:_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING>
            $<$<CXX_COMPILER_ID:MSVC>:_SILENCE_CXX17_ITERATOR_DEPRECATION_WARNING>
    )
    target_compile_options(
        ${target}
        PRIVATE
            $<$<CXX_COMPILER_ID:MSVC>:/W4>
            $<$<CXX_COMPILER_ID:MSVC>:/wd4702>
            $<$<CXX_COMPILER_ID:MSVC>:/permissive->
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-deprecated-declarations>
    )
endfunction()

add_subdirectory(thirdparty)

add_library(engine_runtime STATIC)
target_include_directories(
    engine_runtime
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/include/engine"
)
target_link_libraries(
    engine_runtime
    PUBLIC
        box2d::box2d
        game_engine_sdl_headers
        game_engine_vendor_headers
        Threads::Threads
        $<$<PLATFORM_ID:Windows>:ws2_32>
        $<$<PLATFORM_ID:Windows>:iphlpapi>
    PRIVATE
        lua_runtime
        pocket_detmath
)
game_engine_apply_cxx_warnings(engine_runtime)
if(POCKET_ENABLE_TRACY)
    target_link_libraries(engine_runtime PUBLIC TracyClient)
    target_compile_definitions(engine_runtime PUBLIC POCKET_PROFILE)
endif()
set_target_properties(engine_runtime PROPERTIES FOLDER "engine")
add_library(GameEngine::Runtime ALIAS engine_runtime)

add_library(editor_core STATIC)
target_include_directories(
    editor_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(
    editor_core
    PUBLIC
        engine_runtime
        imgui_runtime
        imguizmo_runtime
)
game_engine_apply_cxx_warnings(editor_core)
set_target_properties(editor_core PROPERTIES FOLDER "editor")
add_library(GameEngine::Editor ALIAS editor_core)

add_subdirectory(src/shared)
add_subdirectory(src/engine)
add_subdirectory(src/editor)
add_subdirectory(src/app/runtime)
add_subdirectory(src/app/editor)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(MSVC)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT game_engine)
    set_target_properties(
        game_engine
        PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:game_engine>"
    )
endif()
