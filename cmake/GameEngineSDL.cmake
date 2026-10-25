include_guard(GLOBAL)
include(FetchContent)

# Native SDL3 dependencies, pinned to reviewed upstream releases. CMake's
# FETCHCONTENT_SOURCE_DIR_<NAME> overrides support offline source checkouts.
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
if(EMSCRIPTEN)   # the web runtime links everything into one module
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
endif()
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
set(SDLMIXER_WAVPACK OFF CACHE BOOL "" FORCE)
# SDL_mixer defaults to vendored codec sources with MSVC, which its release
# archives do not include; every codec left on is built in (WAV, stb_vorbis, dr_mp3).
set(SDLMIXER_VENDORED OFF CACHE BOOL "" FORCE)

# Distribution runtimes (tools/build_runtime.py, for `pocket package`) carry
# no dependency on libraries a player may lack: SDL_ttf builds its own
# FreeType (without HarfBuzz) and SDL_image decodes with its built-in stb
# loaders instead of system libpng and libwebp. Windows builds always do so:
# Windows has no system FreeType or image libraries to find.
option(POCKET_DISTRIBUTION "Build a self-contained runtime for packaged games" OFF)
set(sdl3_ttf_patch "")
if(POCKET_DISTRIBUTION OR EMSCRIPTEN OR WIN32)
    set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
    set(SDLTTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_VENDORED OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
    set(SDLIMAGE_BACKEND_IMAGEIO OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_JPG_LIBJPEG OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
    set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
    set(sdl3_ttf_patch PATCH_COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/FetchFreetypeIntoSdlTtf.cmake")
endif()

FetchContent_Declare(sdl3
    URL https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-3.4.18)
FetchContent_Declare(sdl3_image
    URL https://codeload.github.com/libsdl-org/SDL_image/tar.gz/refs/tags/release-3.4.8)
FetchContent_Declare(sdl3_ttf
    URL https://codeload.github.com/libsdl-org/SDL_ttf/tar.gz/refs/tags/release-3.2.2
    ${sdl3_ttf_patch})
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
