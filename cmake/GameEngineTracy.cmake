include_guard(GLOBAL)
include(FetchContent)

# Optional Tracy profiling (off by default). Declared before Box2D so Box2D's
# BOX2D_PROFILE reuses the same TracyClient instead of fetching its own copy.
option(POCKET_ENABLE_TRACY "Instrument the engine and Box2D with Tracy zones" OFF)
if(POCKET_ENABLE_TRACY)
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    FetchContent_Declare(tracy
        URL https://codeload.github.com/wolfpld/tracy/tar.gz/refs/tags/v0.14.1)
    block()
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_MakeAvailable(tracy)
    endblock()
    set(BOX2D_PROFILE ON CACHE BOOL "" FORCE)
endif()
