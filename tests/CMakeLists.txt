function(pocket_runtime_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE GameEngine::Runtime)
    game_engine_link_sdl(${name})
    game_engine_apply_cxx_warnings(${name})
endfunction()

pocket_runtime_test(runtime_step_test runtime_step_test.cpp)
add_test(NAME runtime_step COMMAND runtime_step_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
set_tests_properties(runtime_step PROPERTIES TIMEOUT 30)
pocket_runtime_test(log_test log_test.cpp)
add_test(NAME log COMMAND log_test)
set_tests_properties(log PROPERTIES TIMEOUT 30)
pocket_runtime_test(world_isolation_test world_isolation_test.cpp)
add_test(NAME world_isolation_defense COMMAND world_isolation_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense" defense)
add_test(NAME world_isolation_village COMMAND world_isolation_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/two_world_village" village)
set_tests_properties(world_isolation_defense world_isolation_village PROPERTIES TIMEOUT 120)
pocket_runtime_test(runtime_actor_lifetime_test runtime_actor_lifetime_test.cpp)
add_test(NAME runtime_actor_lifetime COMMAND runtime_actor_lifetime_test)
set_tests_properties(runtime_actor_lifetime PROPERTIES TIMEOUT 30)
pocket_runtime_test(runtime_uid_test runtime_uid_test.cpp)
add_test(NAME runtime_uid COMMAND runtime_uid_test)
set_tests_properties(runtime_uid PROPERTIES TIMEOUT 30)
pocket_runtime_test(input_state_test input_state_test.cpp)
add_test(NAME input_state COMMAND input_state_test)
set_tests_properties(input_state PROPERTIES TIMEOUT 30)
pocket_runtime_test(component_isolation_test component_isolation_test.cpp)
add_test(NAME component_isolation COMMAND component_isolation_test)
set_tests_properties(component_isolation PROPERTIES TIMEOUT 30)
pocket_runtime_test(lua_vm_test lua_vm_test.cpp)
add_test(NAME lua_vm COMMAND lua_vm_test)
set_tests_properties(lua_vm PROPERTIES TIMEOUT 30)

pocket_runtime_test(hierarchy_queries_test hierarchy_queries_test.cpp)
add_test(NAME hierarchy_queries COMMAND hierarchy_queries_test)
set_tests_properties(hierarchy_queries PROPERTIES TIMEOUT 60)
pocket_runtime_test(hierarchy_benchmark hierarchy_benchmark.cpp)
pocket_runtime_test(particle_benchmark particle_benchmark.cpp)

pocket_runtime_test(task_executor_test task_executor_test.cpp)
add_test(NAME task_executor COMMAND task_executor_test)
set_tests_properties(task_executor PROPERTIES TIMEOUT 30)

pocket_runtime_test(render_backend_test render_backend_test.cpp)
add_test(NAME render_software COMMAND render_backend_test software)
set_tests_properties(render_software PROPERTIES TIMEOUT 30)

if(APPLE)
    set(POCKET_GPU_RENDER_TESTS_DEFAULT ON)
else()
    set(POCKET_GPU_RENDER_TESTS_DEFAULT OFF)
endif()
option(POCKET_ENABLE_GPU_RENDER_TESTS "Run pixel tests on SDL's GPU renderer (needs a GPU driver)"
    ${POCKET_GPU_RENDER_TESTS_DEFAULT})
if(POCKET_ENABLE_GPU_RENDER_TESTS)
    add_test(NAME render_gpu COMMAND render_backend_test gpu)
    set_tests_properties(render_gpu PROPERTIES TIMEOUT 30)
endif()

pocket_runtime_test(audio_runtime_test audio_runtime_test.cpp)
add_test(NAME audio_runtime COMMAND audio_runtime_test)
set_tests_properties(audio_runtime PROPERTIES TIMEOUT 30)

pocket_runtime_test(event_bus_test event_bus_test.cpp)
add_test(NAME event_bus COMMAND event_bus_test)
set_tests_properties(event_bus PROPERTIES TIMEOUT 30)

pocket_runtime_test(scene_validation_test scene_validation_test.cpp)
target_link_libraries(scene_validation_test PRIVATE GameEngine::Editor)
add_test(NAME scene_validation COMMAND scene_validation_test)
set_tests_properties(scene_validation PROPERTIES TIMEOUT 30)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME agent_protocol COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_protocol_test.py"
    $<TARGET_FILE:game_engine> "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
set_tests_properties(agent_protocol PROPERTIES TIMEOUT 90)
add_test(NAME agent_errors COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_error_test.py"
    $<TARGET_FILE:game_engine> "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
set_tests_properties(agent_errors PROPERTIES TIMEOUT 90)
add_test(NAME agent_run_until COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_run_until_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_run_until PROPERTIES TIMEOUT 60)
add_test(NAME agent_replay COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_replay_test.py"
    $<TARGET_FILE:game_engine> "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
set_tests_properties(agent_replay PROPERTIES TIMEOUT 120)
add_test(NAME agent_mcp COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_mcp_test.py"
    $<TARGET_FILE:game_engine> "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
set_tests_properties(agent_mcp PROPERTIES TIMEOUT 90)
add_test(NAME agent_actions COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_actions_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_actions PROPERTIES TIMEOUT 60)
add_test(NAME scenario_env COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/scenario_env_test.py"
    $<TARGET_FILE:game_engine> "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/two_world_village")
set_tests_properties(scenario_env PROPERTIES TIMEOUT 180)
add_test(NAME agent_session COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_session_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_session PROPERTIES TIMEOUT 60)
add_test(NAME agent_image COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_image_test.py"
    $<TARGET_FILE:game_engine> "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/two_world_village")
set_tests_properties(agent_image PROPERTIES TIMEOUT 120)
add_test(NAME agent_hot_reload COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_hot_reload_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_hot_reload PROPERTIES TIMEOUT 60)
add_test(NAME agent_batch_update COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_batch_update_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_batch_update PROPERTIES TIMEOUT 60)
add_test(NAME agent_checkpoint COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_checkpoint_test.py"
    $<TARGET_FILE:game_engine> "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
set_tests_properties(agent_checkpoint PROPERTIES TIMEOUT 120)
add_test(NAME asset_import COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/asset_import_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(asset_import PROPERTIES TIMEOUT 180)
add_test(NAME pocket_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/pocket_cli_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(pocket_cli PROPERTIES TIMEOUT 300)
add_test(NAME asset_schemas COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/validate_schemas.py" --root "${PROJECT_SOURCE_DIR}")
set_tests_properties(asset_schemas PROPERTIES TIMEOUT 60)
add_test(NAME agent_protocol_generated COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/generate_agent_protocol.py" --check)
set_tests_properties(agent_protocol_generated PROPERTIES TIMEOUT 30)
add_test(NAME agent_mouse COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_mouse_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_mouse PROPERTIES TIMEOUT 30)
add_test(NAME agent_awake COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_awake_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_awake PROPERTIES TIMEOUT 30)
add_test(NAME agent_audio_events COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_audio_events_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_audio_events PROPERTIES TIMEOUT 30)
add_test(NAME agent_any_key COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_any_key_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_any_key PROPERTIES TIMEOUT 30)
add_test(NAME agent_start_scene COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_start_scene_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_start_scene PROPERTIES TIMEOUT 60)
add_test(NAME agent_camera COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_camera_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_camera PROPERTIES TIMEOUT 30)

pocket_runtime_test(lua_budget_test lua_budget_test.cpp)
add_test(NAME lua_budget COMMAND lua_budget_test)
set_tests_properties(lua_budget PROPERTIES TIMEOUT 30)
add_test(NAME agent_budget COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_budget_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_budget PROPERTIES TIMEOUT 90)

pocket_runtime_test(tilemap_test tilemap_test.cpp)
add_test(NAME tilemap_native COMMAND tilemap_test)
set_tests_properties(tilemap_native PROPERTIES TIMEOUT 30)
pocket_runtime_test(tilemap_runtime_test tilemap_runtime_test.cpp)
target_link_libraries(tilemap_runtime_test PRIVATE GameEngine::Editor)
add_test(NAME tilemap_runtime COMMAND tilemap_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tilemap_farm")
set_tests_properties(tilemap_runtime PROPERTIES TIMEOUT 30)
add_test(NAME agent_tilemap COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_tilemap_test.py" $<TARGET_FILE:game_engine>
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tilemap_farm")
set_tests_properties(agent_tilemap PROPERTIES TIMEOUT 90)
pocket_runtime_test(tilemap_editor_test tilemap_editor_test.cpp)
target_link_libraries(tilemap_editor_test PRIVATE GameEngine::Editor)
add_test(NAME tilemap_editor COMMAND tilemap_editor_test "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tilemap_farm")
set_tests_properties(tilemap_editor PROPERTIES TIMEOUT 30)
pocket_runtime_test(tilemap_render_test tilemap_render_test.cpp)
add_test(NAME tilemap_render_software COMMAND tilemap_render_test software)
set_tests_properties(tilemap_render_software PROPERTIES TIMEOUT 30)
pocket_runtime_test(tilemap_benchmark tilemap_benchmark.cpp)
pocket_runtime_test(text_benchmark text_benchmark.cpp)
pocket_runtime_test(physics_benchmark physics_benchmark.cpp)
pocket_runtime_test(path_benchmark path_benchmark.cpp)
pocket_runtime_test(bunnymark_benchmark bunnymark_benchmark.cpp)
add_test(NAME physics_threads COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/physics_threads_test.py" $<TARGET_FILE:physics_benchmark>)
set_tests_properties(physics_threads PROPERTIES TIMEOUT 600)
target_compile_definitions(text_benchmark PRIVATE POCKET_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
pocket_runtime_test(physics_callback_lifecycle_test physics_callback_lifecycle_test.cpp)
add_test(NAME physics_callback_lifecycle COMMAND physics_callback_lifecycle_test)
set_tests_properties(physics_callback_lifecycle PROPERTIES TIMEOUT 30)
pocket_runtime_test(tilemap_overlay_test tilemap_overlay_test.cpp)
target_link_libraries(tilemap_overlay_test PRIVATE GameEngine::Editor)
add_test(NAME tilemap_overlay COMMAND tilemap_overlay_test "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tilemap_farm")
set_tests_properties(tilemap_overlay PROPERTIES TIMEOUT 30)

pocket_runtime_test(sprite_animation_asset_test sprite_animation_asset_test.cpp)
add_test(NAME sprite_animation_asset COMMAND sprite_animation_asset_test)
set_tests_properties(sprite_animation_asset PROPERTIES TIMEOUT 30)
pocket_runtime_test(sprite_animation_test sprite_animation_test.cpp)
add_test(NAME sprite_animation_native COMMAND sprite_animation_test)
set_tests_properties(sprite_animation_native PROPERTIES TIMEOUT 30)
pocket_runtime_test(sprite_animation_render_test sprite_animation_render_test.cpp)
add_test(NAME sprite_animation_render_software COMMAND sprite_animation_render_test software)
set_tests_properties(sprite_animation_render_software PROPERTIES TIMEOUT 30)
pocket_runtime_test(sprite_animation_runtime_test sprite_animation_runtime_test.cpp)
target_link_libraries(sprite_animation_runtime_test PRIVATE GameEngine::Editor)
add_test(NAME sprite_animation_runtime COMMAND sprite_animation_runtime_test "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sprite_animation")
set_tests_properties(sprite_animation_runtime PROPERTIES TIMEOUT 30)
add_test(NAME agent_animation COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_animation_test.py" $<TARGET_FILE:game_engine>
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sprite_animation")
set_tests_properties(agent_animation PROPERTIES TIMEOUT 90)
pocket_runtime_test(sprite_animation_benchmark sprite_animation_benchmark.cpp)
add_test(NAME animation_getter_reentry COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/animation_getter_reentry_test.py" $<TARGET_FILE:game_engine>
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sprite_animation")
set_tests_properties(animation_getter_reentry PROPERTIES TIMEOUT 90)
pocket_runtime_test(save_store_test save_store_test.cpp)
add_test(NAME save_store COMMAND save_store_test)
set_tests_properties(save_store PROPERTIES TIMEOUT 30)
pocket_runtime_test(save_lua_test save_lua_test.cpp)
add_test(NAME save_lua COMMAND save_lua_test)
set_tests_properties(save_lua PROPERTIES TIMEOUT 30)
add_test(NAME agent_savegame COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_savegame_test.py" $<TARGET_FILE:game_engine>
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/persistent_village")
set_tests_properties(agent_savegame PROPERTIES TIMEOUT 90)
pocket_runtime_test(lua_host_reentry_test lua_host_reentry_test.cpp)
add_test(NAME lua_host_reentry COMMAND lua_host_reentry_test)
set_tests_properties(lua_host_reentry PROPERTIES TIMEOUT 30)
pocket_runtime_test(script_api_contract_test script_api_contract_test.cpp)
add_test(NAME script_api_contract COMMAND script_api_contract_test)
set_tests_properties(script_api_contract PROPERTIES TIMEOUT 30)
add_test(NAME script_api_generated COMMAND "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/tools/generate_script_api.py" --check)
set_tests_properties(script_api_generated PROPERTIES TIMEOUT 30)

option(POCKET_ENABLE_TYPESCRIPT_TESTS "Require pinned optional TypeScript authoring tests" OFF)
if(POCKET_ENABLE_TYPESCRIPT_TESTS)
    find_program(POCKET_NODE_EXECUTABLE NAMES node REQUIRED)
    execute_process(COMMAND "${POCKET_NODE_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/script-authoring/compile.mjs" --check-toolchain
        RESULT_VARIABLE POCKET_TYPESCRIPT_CHECK_RESULT
        OUTPUT_VARIABLE POCKET_TYPESCRIPT_CHECK_OUTPUT
        ERROR_VARIABLE POCKET_TYPESCRIPT_CHECK_ERROR)
    if(NOT POCKET_TYPESCRIPT_CHECK_RESULT EQUAL 0)
        message(FATAL_ERROR "Pinned TypeScript authoring dependencies are unavailable. Run npm ci --ignore-scripts in tools/script-authoring first. ${POCKET_TYPESCRIPT_CHECK_OUTPUT} ${POCKET_TYPESCRIPT_CHECK_ERROR}")
    endif()
    add_test(NAME script_authoring_tools COMMAND "${POCKET_NODE_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/script-authoring/test.mjs")
    set_tests_properties(script_authoring_tools PROPERTIES TIMEOUT 90)
    add_test(NAME agent_typed_gameplay COMMAND "${POCKET_NODE_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tools/script-authoring/replay.mjs"
        --runtime $<TARGET_FILE:game_engine> --python "${Python3_EXECUTABLE}")
    set_tests_properties(agent_typed_gameplay PROPERTIES TIMEOUT 90)
endif()

pocket_runtime_test(item_catalog_test item_catalog_test.cpp)
add_test(NAME item_catalog COMMAND item_catalog_test)
set_tests_properties(item_catalog PROPERTIES TIMEOUT 30)
pocket_runtime_test(inventory_model_test inventory_model_test.cpp)
add_test(NAME inventory_model COMMAND inventory_model_test)
set_tests_properties(inventory_model PROPERTIES TIMEOUT 30)
pocket_runtime_test(inventory_lua_test inventory_lua_test.cpp)
add_test(NAME inventory_lua COMMAND inventory_lua_test)
set_tests_properties(inventory_lua PROPERTIES TIMEOUT 30)
pocket_runtime_test(inventory_render_test inventory_render_test.cpp)
add_test(NAME inventory_render_software COMMAND inventory_render_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/inventory_village" software)
set_tests_properties(inventory_render_software PROPERTIES TIMEOUT 30)
if(POCKET_ENABLE_GPU_RENDER_TESTS)
    add_test(NAME inventory_render_gpu COMMAND inventory_render_test
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/inventory_village" gpu)
    set_tests_properties(inventory_render_gpu PROPERTIES TIMEOUT 30)
endif()
add_test(NAME agent_inventory COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_inventory_test.py" $<TARGET_FILE:game_engine>
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/inventory_village")
set_tests_properties(agent_inventory PROPERTIES TIMEOUT 90)
pocket_runtime_test(script_reload_candidate_test script_reload_candidate_test.cpp)
add_test(NAME script_reload_candidate COMMAND script_reload_candidate_test)
set_tests_properties(script_reload_candidate PROPERTIES TIMEOUT 30)
pocket_runtime_test(script_reload_runtime_test script_reload_runtime_test.cpp)
add_test(NAME script_reload_runtime COMMAND script_reload_runtime_test)
set_tests_properties(script_reload_runtime PROPERTIES TIMEOUT 30)
add_test(NAME agent_reload COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_reload_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_reload PROPERTIES TIMEOUT 90)
pocket_runtime_test(script_reload_editor_test script_reload_editor_test.cpp)
target_link_libraries(script_reload_editor_test PRIVATE GameEngine::Editor)
add_test(NAME script_reload_editor COMMAND script_reload_editor_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/tilemap_farm")
set_tests_properties(script_reload_editor PROPERTIES TIMEOUT 45)

pocket_runtime_test(scene_scripts_test scene_scripts_test.cpp)
add_test(NAME scene_scripts COMMAND scene_scripts_test)
set_tests_properties(scene_scripts PROPERTIES TIMEOUT 30)
pocket_runtime_test(scene_component_preflight_test scene_component_preflight_test.cpp)
add_test(NAME scene_component_preflight COMMAND scene_component_preflight_test)
set_tests_properties(scene_component_preflight PROPERTIES TIMEOUT 30)
pocket_runtime_test(scene_transition_test scene_transition_test.cpp)
target_link_libraries(scene_transition_test PRIVATE GameEngine::Editor)
add_test(NAME scene_transition COMMAND scene_transition_test)
set_tests_properties(scene_transition PROPERTIES TIMEOUT 30)
add_test(NAME agent_scene_transition COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_scene_transition_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_scene_transition PROPERTIES TIMEOUT 90)

pocket_runtime_test(scene_persistence_test scene_persistence_test.cpp)
add_test(NAME scene_persistence COMMAND scene_persistence_test)
set_tests_properties(scene_persistence PROPERTIES TIMEOUT 30)

# Native runtime mutation domains share validation with scene preparation.
pocket_runtime_test(rigidbody_validation_test rigidbody_validation_test.cpp)
add_test(NAME rigidbody_validation COMMAND rigidbody_validation_test)
set_tests_properties(rigidbody_validation PROPERTIES TIMEOUT 30)
pocket_runtime_test(particle_validation_test particle_validation_test.cpp)
add_test(NAME particle_validation COMMAND particle_validation_test)
set_tests_properties(particle_validation PROPERTIES TIMEOUT 60)
add_test(NAME agent_native_validation COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_native_validation_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_native_validation PROPERTIES TIMEOUT 90)

# Whole runtime actors prepare before becoming observable.
pocket_runtime_test(runtime_actor_preparation_test runtime_actor_preparation_test.cpp)
add_test(NAME runtime_actor_preparation COMMAND runtime_actor_preparation_test)
set_tests_properties(runtime_actor_preparation PROPERTIES TIMEOUT 30)
pocket_runtime_test(runtime_actor_assets_test runtime_actor_assets_test.cpp)
add_test(NAME runtime_actor_assets COMMAND runtime_actor_assets_test)
set_tests_properties(runtime_actor_assets PROPERTIES TIMEOUT 30)
pocket_runtime_test(runtime_actor_spawn_test runtime_actor_spawn_test.cpp)
add_test(NAME runtime_actor_spawn COMMAND runtime_actor_spawn_test)
set_tests_properties(runtime_actor_spawn PROPERTIES TIMEOUT 30)
add_test(NAME agent_actor_spawn COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_actor_spawn_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_actor_spawn PROPERTIES TIMEOUT 90)

# Prepared editor commands preserve document/runtime agreement on rejection.
pocket_runtime_test(scene_edit_runtime_test scene_edit_runtime_test.cpp)
add_test(NAME scene_edit_runtime COMMAND scene_edit_runtime_test)
set_tests_properties(scene_edit_runtime PROPERTIES TIMEOUT 60)
pocket_runtime_test(scene_document_transaction_test scene_document_transaction_test.cpp)
target_link_libraries(scene_document_transaction_test PRIVATE GameEngine::Editor)
add_test(NAME scene_document_transaction COMMAND scene_document_transaction_test)
set_tests_properties(scene_document_transaction PROPERTIES TIMEOUT 30)
pocket_runtime_test(editor_command_transaction_test editor_command_transaction_test.cpp)
target_link_libraries(editor_command_transaction_test PRIVATE GameEngine::Editor)
add_test(NAME editor_command_transaction COMMAND editor_command_transaction_test)
set_tests_properties(editor_command_transaction PROPERTIES TIMEOUT 60)
pocket_runtime_test(prepared_native_adoption_test prepared_native_adoption_test.cpp)
add_test(NAME prepared_native_adoption COMMAND prepared_native_adoption_test)
set_tests_properties(prepared_native_adoption PROPERTIES TIMEOUT 30)

pocket_runtime_test(editor_edit_status_test editor_edit_status_test.cpp)
target_link_libraries(editor_edit_status_test PRIVATE GameEngine::Editor)
add_test(NAME editor_edit_status COMMAND editor_edit_status_test)
set_tests_properties(editor_edit_status PROPERTIES TIMEOUT 30)

# Manual Release workload; timings are measurements, never pass/fail thresholds.
pocket_runtime_test(scene_edit_benchmark scene_edit_benchmark.cpp)

pocket_runtime_test(component_key_rename_test component_key_rename_test.cpp)
add_test(NAME component_key_rename COMMAND component_key_rename_test)
set_tests_properties(component_key_rename PROPERTIES TIMEOUT 30)

pocket_runtime_test(lua_boundary_test lua_boundary_test.cpp)
add_test(NAME lua_boundary COMMAND lua_boundary_test)
set_tests_properties(lua_boundary PROPERTIES TIMEOUT 30)

# Invalid native self is a recoverable Lua error; ordinary nullable arguments remain valid.
pocket_runtime_test(lua_receiver_test lua_receiver_test.cpp)
add_test(NAME lua_receiver COMMAND lua_receiver_test)
set_tests_properties(lua_receiver PROPERTIES TIMEOUT 30)
add_test(NAME agent_receiver COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_receiver_test.py" $<TARGET_FILE:game_engine>)
set_tests_properties(agent_receiver PROPERTIES TIMEOUT 100)

# One real gameplay loop combines native inventory, farming, combat and world persistence.
pocket_runtime_test(two_world_render_test two_world_render_test.cpp)
add_test(NAME two_world_render_software COMMAND two_world_render_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/two_world_village" software)
set_tests_properties(two_world_render_software PROPERTIES TIMEOUT 45)
if(POCKET_ENABLE_GPU_RENDER_TESTS)
    add_test(NAME two_world_render_gpu COMMAND two_world_render_test
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/two_world_village" gpu)
    set_tests_properties(two_world_render_gpu PROPERTIES TIMEOUT 45)
endif()
add_test(NAME agent_two_world_village COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent_two_world_village_test.py" $<TARGET_FILE:game_engine>
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/two_world_village")
set_tests_properties(agent_two_world_village PROPERTIES TIMEOUT 180)

pocket_runtime_test(rigidbody_activation_test rigidbody_activation_test.cpp)
add_test(NAME rigidbody_activation COMMAND rigidbody_activation_test)
set_tests_properties(rigidbody_activation PROPERTIES TIMEOUT 30)

pocket_runtime_test(tilemap_activation_test tilemap_activation_test.cpp)
add_test(NAME tilemap_activation COMMAND tilemap_activation_test)
set_tests_properties(tilemap_activation PROPERTIES TIMEOUT 30)

pocket_runtime_test(editor_play_world_test editor_play_world_test.cpp)
target_link_libraries(editor_play_world_test PRIVATE GameEngine::Editor)
add_test(NAME editor_play_world COMMAND editor_play_world_test
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
set_tests_properties(editor_play_world PROPERTIES TIMEOUT 60)
if(POCKET_ENABLE_IMGUI_TEST_ENGINE)
    pocket_runtime_test(editor_ui_test editor_ui_test.cpp)
    target_link_libraries(editor_ui_test PRIVATE GameEngine::Editor)
    add_test(NAME editor_ui COMMAND editor_ui_test "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/village_defense")
    set_tests_properties(editor_ui PROPERTIES TIMEOUT 120)
endif()

pocket_runtime_test(physics_api_test physics_api_test.cpp)
add_test(NAME physics_api COMMAND physics_api_test)
set_tests_properties(physics_api PROPERTIES TIMEOUT 60)
