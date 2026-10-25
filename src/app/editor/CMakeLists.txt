add_executable(
    game_editor
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/editor_bootstrap.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/EditorApp.h"
)
target_link_libraries(game_editor PRIVATE editor_core)
if(WIN32)
    target_link_libraries(game_editor PRIVATE ole32 shell32)
endif()
game_engine_apply_cxx_warnings(game_editor)
game_engine_link_sdl(game_editor)
game_engine_stage_resources(game_editor)
set_target_properties(
    game_editor
    PROPERTIES
        OUTPUT_NAME "pocket"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
