target_sources(
    engine_runtime
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/EventBus.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/LuaBoundary.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/LuaJson.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/APIRegistration.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/ComponentManager.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/EventBus.h"
)

add_subdirectory(luaapi)
add_subdirectory(manager)
