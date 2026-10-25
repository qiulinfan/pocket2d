add_executable(
    game_engine
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime_bootstrap.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/AgentSession.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/AgentSupervisor.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/AgentReplay.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/AgentMcpServer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeApp.h"
)
target_link_libraries(game_engine PRIVATE engine_runtime)
target_compile_definitions(game_engine PRIVATE POCKET2D_VERSION="${PROJECT_VERSION}")
game_engine_apply_cxx_warnings(game_engine)
game_engine_link_sdl(game_engine)
game_engine_stage_resources(game_engine)
set_target_properties(
    game_engine
    PROPERTIES
        OUTPUT_NAME "game"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# The runtime kit for `pocket package`: `cmake --install BUILD --prefix KIT
# --component runtime_kit` lays out the runtime with its SDL libraries (in
# lib/ on macOS and Linux, beside it on Windows) and the engine's system
# resources; the installed runtime finds its libraries relative to itself.
if(WIN32 AND POCKET_DISTRIBUTION)   # a packaged game opens no console window beside it
    set_target_properties(game_engine PROPERTIES WIN32_EXECUTABLE ON)
endif()
if(APPLE)
    set_target_properties(game_engine PROPERTIES INSTALL_RPATH "@executable_path/lib")
elseif(UNIX AND NOT EMSCRIPTEN)
    set_target_properties(game_engine PROPERTIES INSTALL_RPATH "$ORIGIN/lib")
endif()
if(EMSCRIPTEN)
    # The web runtime: game.html/.js/.wasm. The page (web_shell.html) loads the
    # packaged project into /project before main; ASYNCIFY lets the game loop
    # yield to the browser every frame; saves live in IndexedDB.
    set_target_properties(game_engine PROPERTIES SUFFIX ".html")
    target_link_options(game_engine PRIVATE
        "-sASYNCIFY" "-sASYNCIFY_STACK_SIZE=1048576" "-sSTACK_SIZE=8388608" "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456" "-sFORCE_FILESYSTEM=1" "-lidbfs.js"
        "-sEXPORTED_RUNTIME_METHODS=FS,addRunDependency,removeRunDependency,callMain"
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web_shell.html")
    set_property(TARGET game_engine APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web_shell.html")
    install(FILES "$<TARGET_FILE_DIR:game_engine>/game.html" "$<TARGET_FILE_DIR:game_engine>/game.js"
        "$<TARGET_FILE_DIR:game_engine>/game.wasm" DESTINATION . COMPONENT runtime_kit)
else()
    install(TARGETS game_engine RUNTIME DESTINATION . COMPONENT runtime_kit)
    foreach(sdl_target SDL3-shared SDL3_image-shared SDL3_ttf-shared SDL3_mixer-shared)
        if(TARGET ${sdl_target})
            install(TARGETS ${sdl_target} RUNTIME DESTINATION . COMPONENT runtime_kit
                LIBRARY DESTINATION lib COMPONENT runtime_kit NAMELINK_SKIP)
        endif()
    endforeach()
endif()
install(DIRECTORY "${PROJECT_SOURCE_DIR}/.engine/system" DESTINATION .engine COMPONENT runtime_kit)
