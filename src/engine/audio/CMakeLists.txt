target_sources(
    engine_runtime
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/AudioManager.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SoundLedger.cpp"
        "${PROJECT_SOURCE_DIR}/include/engine/audio/AudioManager.h"
        "${PROJECT_SOURCE_DIR}/include/engine/audio/SoundLedger.h"
)
