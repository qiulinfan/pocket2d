# Deterministic math for Lua (thirdparty/detmath): openlibm's double functions
# built from source under pocket_det_* names, with no FMA contraction, so
# scripts compute the same bits on every platform (replays and netplay).
include(FetchContent)
FetchContent_Declare(openlibm
    URL https://codeload.github.com/JuliaMath/openlibm/tar.gz/refs/tags/v0.8.8
    SOURCE_SUBDIR do-not-build-the-whole-library)
FetchContent_MakeAvailable(openlibm)

set(POCKET_DETMATH_SOURCES
    k_sin k_cos k_tan e_rem_pio2 k_rem_pio2 s_sin s_cos s_tan
    e_asin e_acos s_atan e_atan2 e_exp e_log e_log2 e_log10 e_pow
    s_scalbn s_floor s_copysign)
list(TRANSFORM POCKET_DETMATH_SOURCES PREPEND "${openlibm_SOURCE_DIR}/src/")
list(TRANSFORM POCKET_DETMATH_SOURCES APPEND ".c")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$" OR CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
    set(POCKET_DETMATH_ARCH aarch64)
elseif(EMSCRIPTEN)
    set(POCKET_DETMATH_ARCH wasm32)
else()
    set(POCKET_DETMATH_ARCH amd64)
endif()

add_library(pocket_detmath STATIC ${POCKET_DETMATH_SOURCES})
target_include_directories(pocket_detmath
    PUBLIC "${PROJECT_SOURCE_DIR}/thirdparty/detmath"
    PRIVATE
        "${openlibm_SOURCE_DIR}/src"
        "${openlibm_SOURCE_DIR}/include"
        "${openlibm_SOURCE_DIR}/${POCKET_DETMATH_ARCH}")
set(POCKET_DETMATH_RENAME "${PROJECT_SOURCE_DIR}/thirdparty/detmath/pocket_detmath_rename.h")
target_compile_options(pocket_detmath PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/FI${POCKET_DETMATH_RENAME} /fp:strict /w>
    $<$<NOT:$<C_COMPILER_ID:MSVC>>:-include ${POCKET_DETMATH_RENAME} -ffp-contract=off -fno-fast-math -w>)
set_target_properties(pocket_detmath PROPERTIES FOLDER "thirdparty" POSITION_INDEPENDENT_CODE ON)
