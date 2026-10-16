include_guard(GLOBAL)
include(FetchContent)

# Box2D 3.1.1, pinned to the reviewed upstream release tag. Box2D's own CMake
# compiles its sources with -ffp-contract=off for cross-platform determinism;
# do not enable fast-math for it. FETCHCONTENT_SOURCE_DIR_BOX2D overrides
# support an offline checkout of the same release.
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(box2d
    URL https://codeload.github.com/erincatto/box2d/tar.gz/refs/tags/v3.1.1)
# Link Box2D statically even when other dependencies build shared libraries.
block()
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(box2d)
endblock()
