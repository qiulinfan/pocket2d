# Run in script mode as SDL_ttf's FetchContent PATCH_COMMAND (working
# directory: SDL_ttf's sources): puts FreeType's pinned sources where SDL_ttf's
# vendored build expects them, external/freetype, since SDL_ttf's release
# archives leave that folder empty.
set(freetype_url "https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz")
set(freetype_sha256 "bc5c898e4756d373e0d991bab053036c5eb2aa7c0d5c67e8662ddc6da40c4103")
if(EXISTS "external/freetype/CMakeLists.txt")
    return()
endif()
file(DOWNLOAD "${freetype_url}" "external/freetype.tar.gz" EXPECTED_HASH SHA256=${freetype_sha256} TLS_VERIFY ON)
file(ARCHIVE_EXTRACT INPUT "external/freetype.tar.gz" DESTINATION "external/freetype-extract")
file(REMOVE_RECURSE "external/freetype")
file(RENAME "external/freetype-extract/freetype-VER-2-13-3" "external/freetype")
file(REMOVE_RECURSE "external/freetype-extract")
file(REMOVE "external/freetype.tar.gz")
