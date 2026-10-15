include_guard(GLOBAL)
include(FetchContent)

# Native SDL3 dependencies, pinned to reviewed upstream releases. CMake's
# FETCHCONTENT_SOURCE_DIR_<NAME> overrides support offline source checkouts.
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP ON CACHE BOOL "" FORCE)
set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_PLUTOSVG OFF CACHE BOOL "" FORCE)
set(SDLMIXER_TESTS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDLMIXER_GME OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MIDI OFF CACHE BOOL "" FORCE)
set(SDLMIXER_FLAC_LIBFLAC OFF CACHE BOOL "" FORCE)
set(SDLMIXER_MP3_MPG123 OFF CACHE BOOL "" FORCE)
set(SDLMIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDLMIXER_VORBIS_VORBISFILE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(sdl3
    URL https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-3.4.18)
FetchContent_Declare(sdl3_image
    URL https://codeload.github.com/libsdl-org/SDL_image/tar.gz/refs/tags/release-3.4.8)
FetchContent_Declare(sdl3_ttf
    URL https://codeload.github.com/libsdl-org/SDL_ttf/tar.gz/refs/tags/release-3.2.2)
FetchContent_Declare(sdl3_mixer
    URL https://codeload.github.com/libsdl-org/SDL_mixer/tar.gz/refs/tags/release-3.2.4)
FetchContent_MakeAvailable(sdl3 sdl3_image sdl3_ttf sdl3_mixer)

add_library(game_engine_sdl_headers INTERFACE)
target_link_libraries(game_engine_sdl_headers INTERFACE
    SDL3::SDL3 SDL3_image::SDL3_image SDL3_ttf::SDL3_ttf SDL3_mixer::SDL3_mixer)
add_library(game_engine_sdl INTERFACE)
target_link_libraries(game_engine_sdl INTERFACE game_engine_sdl_headers)

function(game_engine_ensure_sdl)
    # Dependencies above are initialized once per configure.
endfunction()

function(game_engine_link_sdl target)
    target_link_libraries(${target} PRIVATE game_engine_sdl)
    if(WIN32 AND GAME_ENGINE_STAGE_SDL_RUNTIME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS VERBATIM)
    endif()
endfunction()

function(game_engine_mirror_windows_runtime_to_root target)
    if(WIN32 AND GAME_ENGINE_STAGE_SDL_RUNTIME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${target}> "${PROJECT_SOURCE_DIR}"
            COMMAND_EXPAND_LISTS VERBATIM)
    endif()
endfunction()
