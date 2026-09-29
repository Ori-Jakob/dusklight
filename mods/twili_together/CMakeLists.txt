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
    src/actors/DummyHorse.cpp
    src/actors/DummyItemFx.cpp
    src/actors/DummyMidna.cpp
    src/actors/DummyPlayer.cpp
    src/actors/DummyWolfFx.cpp
    src/actors/PrivateArchives.cpp
    src/actors/Profiles.cpp
    src/core/Base64.cpp
    src/core/ClientState.cpp
    src/core/Config.cpp
    src/core/GameAccess.cpp
    src/core/Host.cpp
    src/core/LocalPlayer.cpp
    src/core/Maps.cpp
    src/core/RoomStateSync.cpp
    src/core/SaveGate.cpp
    src/core/Session.cpp
    src/core/Visibility.cpp
    src/enemy/EnemyDefeated.cpp
    src/enemy/EnemyDamage.cpp
    src/enemy/EnemyDamagePacket.cpp
    src/enemy/EnemyScaling.cpp
    src/enemy/EnemySync.cpp
    src/fx/ItemFx.cpp
    src/fx/PlayerRecolor.cpp
    src/fx/RemoteTransformFx.cpp
    src/fx/StatusFx.cpp
    src/fx/WolfFx.cpp
    src/game/GameIdentity.cpp
    src/game/RandoSeeds.cpp
    src/game/TeamGame.cpp
    src/hooks/Hooks.cpp
    src/hooks/hooks_actor.cpp
    src/hooks/hooks_enemy.cpp
    src/hooks/hooks_fx.cpp
    src/hooks/hooks_map.cpp
    src/hooks/hooks_player.cpp
    src/hooks/hooks_pvp.cpp
    src/hooks/hooks_save.cpp
    src/hooks/hooks_story.cpp
    src/hooks/hooks_sync.cpp
    src/horse/HorsePuppet.cpp
    src/horse/HorseSync.cpp
    src/net/Link.cpp
    src/net/TcpTransport.cpp
    src/net/WsTransport.cpp
    src/presence/LinkPuppetState.cpp
    src/presence/PlayerSfx.cpp
    src/presence/PlayerUpdate.cpp
    src/presence/RemotePose.cpp
    src/pvp/DamagePlayer.cpp
    src/pvp/PlantedHit.cpp
    src/pvp/Pvp.cpp
    src/story/StoryEvent.cpp
    src/story/StoryMove.cpp
    src/story/StoryMoves.cpp
    src/story/StorySegments.cpp
    src/story/StoryTracker.cpp
    src/story/StoryUi.cpp
    src/sync/DungeonItems.cpp
    src/sync/Flags.cpp
    src/sync/GiveItem.cpp
    src/sync/WorldState.cpp
    src/sync/WorldSync.cpp
    src/teleport/Teleport.cpp
    src/ui/ColorMath.cpp
    src/ui/ItemNames.cpp
    src/ui/ItemToasts.cpp
    src/ui/MapCursors.cpp
    src/ui/NameTags.cpp
    src/ui/StoryPrompt.cpp
    src/ui/Toasts.cpp
    src/ui/TwiliWindow.cpp
    src/ui/Ui.cpp
)

if (TWILI_ENABLE_AUTOTEST)
    list(APPEND TWILI_SOURCES
        src/autotest/AutoTest.cpp
        src/autotest/BootTakeover.cpp
        src/autotest/Input.cpp
        src/autotest/NetDriver.cpp
        src/autotest/PadOverride.cpp
        src/autotest/Platform.cpp
        src/autotest/StepsColor.cpp
        src/autotest/StepsEnemy.cpp
        src/autotest/StepsFidelity.cpp
        src/autotest/StepsHorse.cpp
        src/autotest/StepsItemFx.cpp
        src/autotest/StepsItemToast.cpp
        src/autotest/StepsMapCursor.cpp
        src/autotest/StepsMotion.cpp
        src/autotest/StepsNameTags.cpp
        src/autotest/StepsPerf.cpp
        src/autotest/StepsPvp.cpp
        src/autotest/StepsRecolor.cpp
        src/autotest/StepsStatusFx.cpp
        src/autotest/StepsStory.cpp
        src/autotest/StepsTeamGame.cpp
        src/autotest/StepsTeleport.cpp
        src/autotest/StepsWolfFx.cpp
        src/autotest/StepsWolf.cpp
        src/autotest/StepsWorld.cpp
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
