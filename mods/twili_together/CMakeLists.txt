cmake_minimum_required(VERSION 3.25)
project(twili_together CXX)
set(CMAKE_CXX_STANDARD 20)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DUSK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Path to dusk source root")
    option(DUSK_MOD_USE_FULL_TREE "Use full build instead of the minimal mod SDK" OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    if (DUSK_MOD_USE_FULL_TREE)
        add_subdirectory("${DUSK_DIR}" dusk EXCLUDE_FROM_ALL)
    else ()
        add_subdirectory("${DUSK_DIR}/sdk" dusk-sdk EXCLUDE_FROM_ALL)
    endif ()
endif ()

# Dormant unless its cvar is set; kept in release builds so tests run the shipped binary.
option(TWILI_ENABLE_AUTOTEST "Compile the scripted autotest into Twili-Together" ON)

# nlohmann_json: reuse the game's target in-tree, fetch the header-only release standalone.
if (NOT TARGET nlohmann_json::nlohmann_json)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
    FetchContent_MakeAvailable(nlohmann_json)
endif ()

set(TWILI_SOURCES
    src/mod.cpp
    src/actors/DummyActors.cpp
    src/actors/DummyItemFx.cpp
    src/actors/DummyMidna.cpp
    src/actors/DummyPlayer.cpp
    src/actors/DummyWolfFx.cpp
    src/actors/Profiles.cpp
    src/core/Base64.cpp
    src/core/ClientState.cpp
    src/core/Config.cpp
    src/core/GameAccess.cpp
    src/core/Host.cpp
    src/core/RoomStateSync.cpp
    src/core/SaveGate.cpp
    src/core/Session.cpp
    src/core/Visibility.cpp
    src/fx/ItemFx.cpp
    src/fx/PlayerRecolor.cpp
    src/fx/RemoteTransformFx.cpp
    src/fx/StatusFx.cpp
    src/fx/WolfFx.cpp
    src/hooks/Hooks.cpp
    src/hooks/hooks_actor.cpp
    src/hooks/hooks_player.cpp
    src/hooks/hooks_save.cpp
    src/horse/HorsePuppetStub.cpp
    src/horse/HorseSync.cpp
    src/net/Link.cpp
    src/net/TcpTransport.cpp
    src/net/WsTransport.cpp
    src/presence/LinkPuppetState.cpp
    src/presence/PlayerSfx.cpp
    src/presence/PlayerUpdate.cpp
    src/presence/RemotePose.cpp
    src/pvp/PvpStub.cpp
    src/ui/TwiliWindow.cpp
)

if (TWILI_ENABLE_AUTOTEST)
    list(APPEND TWILI_SOURCES
        src/autotest/AutoTest.cpp
        src/autotest/BootTakeover.cpp
        src/autotest/NetDriver.cpp
        src/autotest/PadOverride.cpp
        src/autotest/Platform.cpp
        src/autotest/StepsMotion.cpp
        src/hooks/hooks_autotest.cpp
    )
endif ()

add_mod(twili_together
    FEATURES game fmt webgpu
    SOURCES  ${TWILI_SOURCES}
    MOD_JSON mod.json
    RES_DIR  res
)

target_include_directories(twili_together PRIVATE src)
target_link_libraries(twili_together PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(twili_together PRIVATE
    TWILI_ENABLE_AUTOTEST=$<BOOL:${TWILI_ENABLE_AUTOTEST}>)
