target_sources(
    engine_runtime
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Input.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ActionMap.cpp"
        "${PROJECT_SOURCE_DIR}/include/engine/input/ActionMap.h"
        "${PROJECT_SOURCE_DIR}/include/engine/input/Input.h"
)
