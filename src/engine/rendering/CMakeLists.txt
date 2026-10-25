target_sources(
    engine_runtime
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Renderer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SpriteRenderer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SpriteAnimation.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/FrameRateOverlay.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/FrameRateOverlay.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/Renderer.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/SpriteRenderer.h"
)

# The splash image is compiled into the runtime so shipped games need no
# .engine folder; reconfigure when tools/make_splash.py redraws it.
set(POCKET_SPLASH_PNG "${PROJECT_SOURCE_DIR}/.engine/system/splash.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${POCKET_SPLASH_PNG}")
file(READ "${POCKET_SPLASH_PNG}" POCKET_SPLASH_HEX HEX)
string(LENGTH "${POCKET_SPLASH_HEX}" POCKET_SPLASH_HEX_LENGTH)
math(EXPR POCKET_SPLASH_SIZE "${POCKET_SPLASH_HEX_LENGTH} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," POCKET_SPLASH_BYTES "${POCKET_SPLASH_HEX}")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SplashImage.cpp" CONTENT
"// Generated from .engine/system/splash.png; do not edit.
#include <cstddef>
extern const unsigned char kPocketSplashPng[] = {${POCKET_SPLASH_BYTES}};
extern const std::size_t kPocketSplashPngSize = ${POCKET_SPLASH_SIZE};
")
target_sources(
    engine_runtime
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/SplashScreen.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/SplashImage.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SplashScreen.h"
)
