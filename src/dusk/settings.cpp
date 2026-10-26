#include "dusk/settings.h"

#include "dusk/config.hpp"
#include "dusk/game_mode.hpp"
#include "dusk/render/render_bridge.hpp"
#include "dusk/texture_replacements.hpp"
#include "dusk/ui/ui.hpp"

#include <aurora/aurora.h>
#include <dolphin/vi.h>

namespace dusk {

// clang-format off
UserSettings g_userSettings = {
    .video = {
        .enableFullscreen {"video.enableFullscreen", false},
        .enableVsync {"video.enableVsync", true},
        .lockAspectRatio {"video.lockAspectRatio", false},
        .enableFpsOverlay {"game.enableFpsOverlay", false},
        .fpsOverlayCorner {"game.fpsOverlayCorner", 0},
        .maxFrameRate {"video.maxFrameRate", 240},
        .rememberWindowSize {"video.rememberWindowSize", false},
        .lastWindowWidth {"video.lastWindowWidth", 0},
        .lastWindowHeight {"video.lastWindowHeight", 0},
        .uiScale {"video.uiScale", 100},
    },

    .audio = {
        .outputMode {"audio.outputMode", AudioOutputMode::StereoSpeakers},
        .masterVolume {"audio.masterVolume", 60},
        .mainMusicVolume {"audio.mainMusicVolume", 100},
        .subMusicVolume {"audio.subMusicVolume", 100},
        .soundEffectsVolume {"audio.soundEffectsVolume", 100},
        .fanfareVolume {"audio.fanfareVolume", 100},
        .enableReverb {"audio.enableReverb", true},
        .menuSounds {"audio.menuSounds", true},
    },

    .game = {
        .language { "game.language", GameLanguage::English },

        // Quality of Life
        .enableQuickTransform {"game.enableQuickTransform", false},
        .hideTvSettingsScreen {"game.hideTvSettingsScreen", true},
        .biggerWallets {"game.biggerWallets", false},
        .noReturnRupees {"game.noReturnRupees", false},
        .disableRupeeCutscenes {"game.disableRupeeCutscenes", false},
        .fastTransitions {"game.fastTransitions", false},
        .noSwordRecoil {"game.noSwordRecoil", false},
        .damageMultiplier {"game.damageMultiplier", 1},
        .noHeartDrops {"game.noHeartDrops", false},
        .instantDeath {"game.instantDeath", false},
        .fastClimbing {"game.fastClimbing", false},
        .noMissClimbing {"game.noMissClimbing", false},
        .fastTears {"game.fastTears", false},
        .no2ndFishForCat {"game.no2ndFishForCat", false},
        .buttonFishing {"game.buttonFishing", false},
        .instantSaves {"game.instantSaves", false},
        .instantText {"game.instantText", false},
        .holdToMash {"game.holdToMash", false},
        .sunsSong {"game.sunsSong", false},
        .autoSave {"game.autoSave", false},
        .enhancedMapMenus {"game.enhancedMapMenus", false},
        .aimingReticle {"game.aimingReticle", false},
        .noHitStun {"game.noHitStun", false},

        // Preferences
        .enableMirrorMode {"game.enableMirrorMode", false},
        .minimalHUD {"game.minimalHUD", false},
        .hudScale {"game.hudScale", 1.0f},
        .pauseOnFocusLost {"game.pauseOnFocusLost", false},
        .enableLinkDollRotation {"game.enableLinkDollRotation", false},
        .enableAchievementToasts {"game.enableAchievementToasts", true},
        .enableControllerToasts {"game.enableControllerToasts", true},
        .enableDiscordPresence {"game.enableDiscordPresence", true},
        .menuScalingMode {"game.menuScalingMode", MenuScaling::Wii},

        // Graphics
        .bloomMode {"game.bloomMode", BloomMode::Dusk},
        .bloomMultiplier {"game.bloomMultiplier", 1.0f},
        .depthOfFieldMode{"game.depthOfFieldMode", DepthOfFieldMode::Dusk},
        .disableWaterRefraction {"game.disableWaterRefraction", false},
        .enableTextureReplacements {"game.enableTextureReplacements", true},
        .enableFrameInterpolation {"game.enableFrameInterpolation", FrameInterpMode::Off},
        .internalResolutionScale {"game.internalResolutionScale", 0},
        .shadowResolutionMultiplier {"game.shadowResolutionMultiplier", 1},
        .resampler {"game.resampler", Resampler::Bilinear},
        .enableMapBackground {"game.enableMapBackground", true},
        .disableCutscenePillarboxing {"game.disableCutscenePillarboxing", false},
        .disableLetterboxing {"game.disableLetterboxing", LetterboxMode::Off},
        .enableHighQualityMinimapTextures {"game.enableHighQualityMinimapTextures", true},

        // Audio
        .noLowHpSound {"game.noLowHpSound", false},
        .midnasLamentNonStop {"game.midnasLamentNonStop", false},

        // Input
        .enableGyroAim {"game.enableGyroAim", false},
        .enableGyroRollgoal {"game.enableGyroRollgoal", false},
        .gyroSensitivityX {"game.gyroSensitivityX", 1.0f},
        .gyroSensitivityY {"game.gyroSensitivityY", 1.0f},
        .gyroSensitivityRollgoal {"game.gyroSensitivityRollgoal", 1.0f},
        .gyroSmoothing {"game.gyroSmoothing", 0.65f},
        .gyroDeadband {"game.gyroDeadband", 0.04f},
        .gyroInvertPitch {"game.gyroInvertPitch", false},
        .gyroInvertYaw {"game.gyroInvertYaw", false},
        .enableMouseCamera {"game.enableMouseCamera", false},
        .enableMouseAim {"game.enableMouseAim", false},
        .mouseAimSensitivity {"game.mouseAimSensitivity", 1.0f},
        .mouseCameraSensitivity {"game.mouseCameraSensitivity", 1.0f},
        .invertMouseY {"game.invertMouseY", false},
        .freeCamera {"game.freeCamera", false},
        .enableTouchControls {"game.enableTouchControls", false},
        .touchTargeting {"game.touchTargeting", TouchTargeting::Hybrid},
        .enableMenuPointer {"game.enableMenuPointer", true},
        .touchControlsLayout {"game.touchControlsLayout", ui::ControlLayout{}},
        .invertCameraXAxis {"game.invertCameraXAxis", false},
        .invertCameraYAxis {"game.invertCameraYAxis", false},
        .invertFirstPersonXAxis {"game.invertFirstPersonXAxis", false},
        .invertFirstPersonYAxis {"game.invertFirstPersonYAxis", false},
        .invertAirSwimX {"game.invertAirSwimX", false},
        .invertAirSwimY {"game.invertAirSwimY", false},
        .freeCameraXSensitivity {"game.freeCameraXSensitivity", 1.0f},
        .freeCameraYSensitivity {"game.freeCameraYSensitivity", 1.0f},
        .touchCameraXSensitivity {"game.touchCameraXSensitivity", 1.0f},
        .touchCameraYSensitivity {"game.touchCameraYSensitivity", 1.0f},
        .debugFlyCam {"game.debugFlyCam", false},
        .debugFlyCamLockEvents {"game.debugFlyCamLockEvents", true},
        .allowBackgroundInput {"game.allowBackgroundInput", true},
        .enableLED {
            ConfigVar<bool>{"game.enableLED_port0", true},
            ConfigVar<bool>{"game.enableLED_port1", true},
            ConfigVar<bool>{"game.enableLED_port2", true},
            ConfigVar<bool>{"game.enableLED_port3", true},
        },
        .swapDirectSelect {"game.swapDirectSelect", false},

        // Cheats
        .infiniteHearts {"game.infiniteHearts", false},
        .infiniteArrows {"game.infiniteArrows", false},
        .infiniteSeeds {"game.infiniteSeeds", false},
        .infiniteBombs {"game.infiniteBombs", false},
        .infiniteOil {"game.infiniteOil", false},
        .infiniteOxygen {"game.infiniteOxygen", false},
        .infiniteRupees {"game.infiniteRupees", false},
        .enableIndefiniteItemDrops {"game.enableIndefiniteItemDrops", false},
        .moonJump {"game.moonJump", false},
        .superClawshot {"game.superClawshot", false},
        .alwaysGreatspin {"game.alwaysGreatspin", AlwaysGreatspinMode::OFF},
        .enableFastIronBoots {"game.enableFastIronBoots", false},
        .canTransformAnywhere {"game.canTransformAnywhere", false},
        .fastRoll {"game.fastRoll", false},
        .fastSpinner {"game.fastSpinner", false},
        .armorRupeeDrain {"game.armorRupeeDrain", MagicArmorMode::NORMAL},
        .invincibleEnemies {"game.invincibleEnemies", false},
        .easyQuickSpin {"game.easyQuickSpin", false},
        .infiniteEponaStamina {"game.infiniteEponaStamina", false},

        // Technical
        .restoreWiiGlitches {"game.restoreWiiGlitches", false},

        // Controls
        .enableTurboKeybind {"game.enableTurboKeybind", false},
        .enableResetKeybind {"game.enableResetKeybind", false},

        // Tools
        .speedrunMode {"game.speedrunMode", false},
        .liveSplitEnabled {"game.liveSplitEnabled", false},
        .showSpeedrunRTATimer {"game.showSpeedrunRTATimer", true},
        .recordingMode {"game.recordingMode", false},
        .removeQuestMapMarkers {"game.removeQuestMapMarkers", false},
        .showInputViewer {"game.showInputViewer", false},
        .showInputViewerGyro {"game.showInputViewerGyro", false},
        .enableMoveLinkCombo {"game.enableMoveLinkCombo", false},
        .enableTeleportCombo {"game.enableTeleportCombo", false},
        .lastSelectedGameModeId {"game.lastSelectedGameModeId", gamemode::kVanillaGameModeId}
    },

    .graphics = {
        .preset {"graphics.preset", GraphicsPreset::Original},
        .textureMaterials {"graphics.textureMaterials", true},
        .stochasticTiling {"graphics.stochasticTiling", false},
        .legacyMatMaps {"graphics.legacyMatMaps", false},
        .generateMissingMips {"graphics.generateMissingMips", false},
        .pbr {"graphics.pbr", false},
        .legacyBlend {"graphics.legacyBlend", 0.0f},
        .exposure {"graphics.exposure", GraphicsExposure::MatchLegacy},
        .exposureBias {"graphics.exposureBias", 0.0f},
        .tonemap {"graphics.tonemap", GraphicsTonemap::PbrNeutral},
        .taa {"graphics.taa", false},
        .taaSharpness {"graphics.taaSharpness", 0.35f},
        .ao {"graphics.ao", GraphicsQuality::Off},
        .ssil {"graphics.ssil", false},
        .shadowQuality {"graphics.shadowQuality", GraphicsQuality::Off},
        .shadowPages {"graphics.shadowPages", 1024},
        .shadowPageBudget {"graphics.shadowPageBudget", 64},
        .gameShadows {"graphics.gameShadows", GraphicsGameShadows::Auto},
        .legacyShadowGain {"graphics.legacyShadowGain", 1.0f},
        .keepShadowCues {"graphics.keepShadowCues", false},
        .gi {"graphics.gi", GraphicsGi::Off},
        .rt {"graphics.rt", GraphicsRtMode::Off},
        .rtBackend {"graphics.rtBackend", GraphicsRtBackend::Auto},
        .reflections {"graphics.reflections", GraphicsReflections::Off},
        .fogVolumetric {"graphics.fogVolumetric", GraphicsQuality::Off},
        .godRays {"graphics.godRays", GraphicsGodRays::Off},
        .fogScattering {"graphics.fogScattering", 1.0f},
        .shaftStrength {"graphics.shaftStrength", 1.0f},
        .fogLegacyMatch {"graphics.fogLegacyMatch", false},
        .fogTranslucents {"graphics.fogTranslucents", false},
        .clouds {"graphics.clouds", GraphicsQuality::Off},
        .cloudStyle {"graphics.cloudStyle", -1.0f},
        .cloudShadows {"graphics.cloudShadows", false},
        .cloudShadowStrength {"graphics.cloudShadowStrength", 1.0f},
        .keepCloudDome {"graphics.keepCloudDome", false},
        .cloudCoverage {"graphics.cloudCoverage", 1.0f},
        .cloudDensity {"graphics.cloudDensity", 1.0f},
        .cloudAltitude {"graphics.cloudAltitude", 1.0f},
        .cloudWindSpeed {"graphics.cloudWindSpeed", 1.0f},
        .cloudSilverLining {"graphics.cloudSilverLining", 1.0f},
        .cloudPowder {"graphics.cloudPowder", 1.0f},
        .grassQuality {"graphics.grassQuality", GraphicsQuality::Off},
        .grassReplaceGame {"graphics.grassReplaceGame", true},
        .grassTerrain {"graphics.grassTerrain", true},
        .grassDistanceM {"graphics.grassDistanceM", 0},
        .grassDensity {"graphics.grassDensity", 1.0f},
        .grassDistantDensity {"graphics.grassDistantDensity", 0.0f},
        .grassShadows {"graphics.grassShadows", GraphicsGrassShadows::Contact},
        .grassLegacyShadows {"graphics.grassLegacyShadows", true},
        .grassInteraction {"graphics.grassInteraction", true},
        .grassWind {"graphics.grassWind", 1.0f},
        .terrainDetail {"graphics.terrainDetail", GraphicsTerrainDetail::Off},
        .terrainQuality {"graphics.terrainQuality", GraphicsQuality::High},
        .terrainStrength {"graphics.terrainStrength", 1.0f},
        .terrainDistanceM {"graphics.terrainDistanceM", 0},
        .terrainFlatten {"graphics.terrainFlatten", false},
        .gpuFoliage {"graphics.gpuFoliage", false},
        .gpuWeather {"graphics.gpuWeather", false},
        .gpuParticles {"graphics.gpuParticles", false},
        .gpuDeform {"graphics.gpuDeform", false},
        .meshReplacement {"graphics.meshReplacement", false},
        .staticWorld {"graphics.staticWorld", GraphicsStaticWorld::Off},
        .debugView {"graphics.debugView", 0},
    },

    .backend = {
        .isoPath {"backend.isoPath", ""},
        .isoVerification {"backend.isoVerification", DiscVerificationState::Unknown},
        .graphicsBackend {"backend.graphicsBackend", "auto"},
        .skipPreLaunchUI {"backend.skipPreLaunchUI", false},
        .showPipelineCompilation {"backend.showPipelineCompilation", true},
        .wasPresetChosen {"backend.wasPresetChosen", false},
        .checkForUpdates {"backend.checkForUpdates", true},
        .checkForModUpdates {"backend.checkForModUpdates", true},
        .cardFileType {"backend.cardFileType", static_cast<int>(CARD_GCIFOLDER)},
        .enableAdvancedSettings {"backend.enableAdvancedSettings", false},
    },

    // Not sure if there's a better way to declare this
    .actionBindings = {
        .firstPersonCamera {
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.firstPersonCamera_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .callMidna {
            ActionBindConfigVar{"actionBindings.callMidna_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.callMidna_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openMapScreen {
            ActionBindConfigVar{"actionBindings.openMapScreen_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openMapScreen_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .toggleMinimap {
            ActionBindConfigVar{"actionBindings.toggleMinimap_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.toggleMinimap_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .openDusklightMenu {
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.openDusklightMenu_port3", PAD_NATIVE_BUTTON_INVALID},
        },
        .turboSpeedButton {
            ActionBindConfigVar{"actionBindings.turboButton_port0", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port1", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port2", PAD_NATIVE_BUTTON_INVALID},
            ActionBindConfigVar{"actionBindings.turboButton_port3", PAD_NATIVE_BUTTON_INVALID},
        },
    }
};
// clang-format on

UserSettings& getSettings() {
    return g_userSettings;
}

void applyInternalResolutionScale(int scale) {
    VISetFrameBufferScale(static_cast<float>(scale));
}

void applyResampler(Resampler resampler) {
    switch (resampler) {
    case Resampler::Area:
        aurora_set_resampler(SAMPLER_AREA);
        break;
    case Resampler::Bilinear:
    default:
        aurora_set_resampler(SAMPLER_BILINEAR);
        break;
    }
}

namespace {
template <typename T>
void RegisterGraphics(ConfigVar<T>& var) {
    Register(var, [](const T&, const T&) { render::mark_dirty(); });
}
}  // namespace

void registerSettings() {
    // Video
    Register(g_userSettings.video.enableFullscreen);
    Register(g_userSettings.video.enableVsync);
    Register(g_userSettings.video.lockAspectRatio);
    Register(g_userSettings.video.enableFpsOverlay);
    Register(g_userSettings.video.fpsOverlayCorner);
    Register(g_userSettings.video.maxFrameRate);
    Register(g_userSettings.video.rememberWindowSize);
    Register(g_userSettings.video.lastWindowWidth);
    Register(g_userSettings.video.lastWindowHeight);
    Register(g_userSettings.video.uiScale,
        [](const int&, const int&) { dusk::ui::apply_scale(); });

    // Audio
    Register(g_userSettings.audio.outputMode);
    Register(g_userSettings.audio.masterVolume);
    Register(g_userSettings.audio.mainMusicVolume);
    Register(g_userSettings.audio.subMusicVolume);
    Register(g_userSettings.audio.soundEffectsVolume);
    Register(g_userSettings.audio.fanfareVolume);
    Register(g_userSettings.audio.enableReverb);
    Register(g_userSettings.audio.menuSounds);

    // Game
    Register(g_userSettings.game.language);
    Register(g_userSettings.game.enableQuickTransform);
    Register(g_userSettings.game.hideTvSettingsScreen);
    Register(g_userSettings.game.biggerWallets);
    Register(g_userSettings.game.noReturnRupees);
    Register(g_userSettings.game.disableRupeeCutscenes);
    Register(g_userSettings.game.fastTransitions);
    Register(g_userSettings.game.noSwordRecoil);
    Register(g_userSettings.game.damageMultiplier);
    Register(g_userSettings.game.noHeartDrops);
    Register(g_userSettings.game.instantDeath);
    Register(g_userSettings.game.fastClimbing);
    Register(g_userSettings.game.fastTears);
    Register(g_userSettings.game.no2ndFishForCat);
    Register(g_userSettings.game.buttonFishing);
    Register(g_userSettings.game.instantSaves);
    Register(g_userSettings.game.instantText);
    Register(g_userSettings.game.holdToMash);
    Register(g_userSettings.game.sunsSong);
    Register(g_userSettings.game.autoSave);
    Register(g_userSettings.game.enhancedMapMenus);
    Register(g_userSettings.game.aimingReticle);
    Register(g_userSettings.game.noHitStun);
    Register(g_userSettings.game.enableMirrorMode);
    Register(g_userSettings.game.invertCameraXAxis);
    Register(g_userSettings.game.invertCameraYAxis);
    Register(g_userSettings.game.invertFirstPersonXAxis);
    Register(g_userSettings.game.invertFirstPersonYAxis);
    Register(g_userSettings.game.invertAirSwimX);
    Register(g_userSettings.game.invertAirSwimY);
    Register(g_userSettings.game.freeCameraXSensitivity);
    Register(g_userSettings.game.freeCameraYSensitivity);
    Register(g_userSettings.game.touchCameraXSensitivity);
    Register(g_userSettings.game.touchCameraYSensitivity);
    Register(g_userSettings.game.minimalHUD);
    Register(g_userSettings.game.hudScale);
    Register(g_userSettings.game.pauseOnFocusLost,
        [](const bool& value, const bool&) { aurora_set_pause_on_focus_lost(value); });
    Register(g_userSettings.game.enableDiscordPresence);
    Register(g_userSettings.game.bloomMode);
    Register(g_userSettings.game.bloomMultiplier);
    Register(g_userSettings.game.depthOfFieldMode);
    Register(g_userSettings.game.disableWaterRefraction);
    Register(g_userSettings.game.enableTextureReplacements,
        [](const bool&, const bool&) { texture_replacements::reload(); });
    Register(g_userSettings.game.internalResolutionScale,
        [](const int& value, const int&) { applyInternalResolutionScale(value); });
    Register(g_userSettings.game.resampler,
        [](const Resampler& value, const Resampler&) { applyResampler(value); });
    Register(g_userSettings.game.shadowResolutionMultiplier);
    Register(g_userSettings.game.enableMapBackground);
    Register(g_userSettings.game.disableCutscenePillarboxing);
    Register(g_userSettings.game.disableLetterboxing);
    Register(g_userSettings.game.enableHighQualityMinimapTextures);
    Register(g_userSettings.game.enableFastIronBoots);
    Register(g_userSettings.game.canTransformAnywhere);
    Register(g_userSettings.game.fastRoll);
    Register(g_userSettings.game.armorRupeeDrain);
    Register(g_userSettings.game.restoreWiiGlitches);
    Register(g_userSettings.game.enableLinkDollRotation);
    Register(g_userSettings.game.enableAchievementToasts);
    Register(g_userSettings.game.enableControllerToasts);
    Register(g_userSettings.game.noMissClimbing);
    Register(g_userSettings.game.noLowHpSound);
    Register(g_userSettings.game.midnasLamentNonStop);
    Register(g_userSettings.game.enableTurboKeybind);
    Register(g_userSettings.game.enableResetKeybind);
    Register(g_userSettings.game.speedrunMode);
    Register(g_userSettings.game.liveSplitEnabled);
    Register(g_userSettings.game.showSpeedrunRTATimer);
    Register(g_userSettings.game.recordingMode);
    Register(g_userSettings.game.menuScalingMode);
    Register(g_userSettings.game.removeQuestMapMarkers);
    Register(g_userSettings.game.showInputViewer);
    Register(g_userSettings.game.showInputViewerGyro);
    Register(g_userSettings.game.enableMoveLinkCombo);
    Register(g_userSettings.game.enableTeleportCombo);
    Register(g_userSettings.game.lastSelectedGameModeId);
    Register(g_userSettings.game.fastSpinner);
    Register(g_userSettings.game.infiniteHearts);
    Register(g_userSettings.game.infiniteArrows);
    Register(g_userSettings.game.infiniteSeeds);
    Register(g_userSettings.game.infiniteBombs);
    Register(g_userSettings.game.infiniteOil);
    Register(g_userSettings.game.infiniteOxygen);
    Register(g_userSettings.game.infiniteRupees);
    Register(g_userSettings.game.enableIndefiniteItemDrops);
    Register(g_userSettings.game.moonJump);
    Register(g_userSettings.game.superClawshot);
    Register(g_userSettings.game.alwaysGreatspin);
    Register(g_userSettings.game.invincibleEnemies);
    Register(g_userSettings.game.easyQuickSpin);
    Register(g_userSettings.game.infiniteEponaStamina);

    Register(g_userSettings.game.enableFrameInterpolation);
    Register(g_userSettings.game.enableGyroAim);
    Register(g_userSettings.game.enableGyroRollgoal);
    Register(g_userSettings.game.gyroSensitivityX);
    Register(g_userSettings.game.gyroSensitivityY);
    Register(g_userSettings.game.gyroSensitivityRollgoal);
    Register(g_userSettings.game.gyroDeadband);
    Register(g_userSettings.game.gyroSmoothing);
    Register(g_userSettings.game.gyroInvertPitch);
    Register(g_userSettings.game.gyroInvertYaw);
    Register(g_userSettings.game.enableMouseCamera);
    Register(g_userSettings.game.enableMouseAim);
    Register(g_userSettings.game.mouseAimSensitivity);
    Register(g_userSettings.game.mouseCameraSensitivity);
    Register(g_userSettings.game.invertMouseY);
    Register(g_userSettings.game.freeCamera);
    Register(g_userSettings.game.enableTouchControls);
    Register(g_userSettings.game.touchTargeting);
    Register(g_userSettings.game.enableMenuPointer);
    Register(g_userSettings.game.touchControlsLayout);
    Register(g_userSettings.game.debugFlyCam);
    Register(g_userSettings.game.debugFlyCamLockEvents);
    Register(g_userSettings.game.allowBackgroundInput);
    Register(g_userSettings.game.enableLED[0]);
    Register(g_userSettings.game.enableLED[1]);
    Register(g_userSettings.game.enableLED[2]);
    Register(g_userSettings.game.enableLED[3]);
    Register(g_userSettings.game.swapDirectSelect);

    RegisterGraphics(g_userSettings.graphics.preset);
    RegisterGraphics(g_userSettings.graphics.textureMaterials);
    RegisterGraphics(g_userSettings.graphics.stochasticTiling);
    RegisterGraphics(g_userSettings.graphics.legacyMatMaps);
    RegisterGraphics(g_userSettings.graphics.generateMissingMips);
    RegisterGraphics(g_userSettings.graphics.pbr);
    RegisterGraphics(g_userSettings.graphics.legacyBlend);
    RegisterGraphics(g_userSettings.graphics.exposure);
    RegisterGraphics(g_userSettings.graphics.exposureBias);
    RegisterGraphics(g_userSettings.graphics.tonemap);
    RegisterGraphics(g_userSettings.graphics.taa);
    RegisterGraphics(g_userSettings.graphics.taaSharpness);
    RegisterGraphics(g_userSettings.graphics.ao);
    RegisterGraphics(g_userSettings.graphics.ssil);
    RegisterGraphics(g_userSettings.graphics.shadowQuality);
    RegisterGraphics(g_userSettings.graphics.shadowPages);
    RegisterGraphics(g_userSettings.graphics.shadowPageBudget);
    RegisterGraphics(g_userSettings.graphics.gameShadows);
    RegisterGraphics(g_userSettings.graphics.legacyShadowGain);
    RegisterGraphics(g_userSettings.graphics.keepShadowCues);
    RegisterGraphics(g_userSettings.graphics.gi);
    RegisterGraphics(g_userSettings.graphics.rt);
    RegisterGraphics(g_userSettings.graphics.rtBackend);
    RegisterGraphics(g_userSettings.graphics.reflections);
    RegisterGraphics(g_userSettings.graphics.fogVolumetric);
    RegisterGraphics(g_userSettings.graphics.godRays);
    RegisterGraphics(g_userSettings.graphics.fogScattering);
    RegisterGraphics(g_userSettings.graphics.shaftStrength);
    RegisterGraphics(g_userSettings.graphics.fogLegacyMatch);
    RegisterGraphics(g_userSettings.graphics.fogTranslucents);
    RegisterGraphics(g_userSettings.graphics.clouds);
    RegisterGraphics(g_userSettings.graphics.cloudStyle);
    RegisterGraphics(g_userSettings.graphics.cloudShadows);
    RegisterGraphics(g_userSettings.graphics.cloudShadowStrength);
    RegisterGraphics(g_userSettings.graphics.keepCloudDome);
    RegisterGraphics(g_userSettings.graphics.cloudCoverage);
    RegisterGraphics(g_userSettings.graphics.cloudDensity);
    RegisterGraphics(g_userSettings.graphics.cloudAltitude);
    RegisterGraphics(g_userSettings.graphics.cloudWindSpeed);
    RegisterGraphics(g_userSettings.graphics.cloudSilverLining);
    RegisterGraphics(g_userSettings.graphics.cloudPowder);
    RegisterGraphics(g_userSettings.graphics.grassQuality);
    RegisterGraphics(g_userSettings.graphics.grassReplaceGame);
    RegisterGraphics(g_userSettings.graphics.grassTerrain);
    RegisterGraphics(g_userSettings.graphics.grassDistanceM);
    RegisterGraphics(g_userSettings.graphics.grassDensity);
    RegisterGraphics(g_userSettings.graphics.grassDistantDensity);
    RegisterGraphics(g_userSettings.graphics.grassShadows);
    RegisterGraphics(g_userSettings.graphics.grassLegacyShadows);
    RegisterGraphics(g_userSettings.graphics.grassInteraction);
    RegisterGraphics(g_userSettings.graphics.grassWind);
    RegisterGraphics(g_userSettings.graphics.terrainDetail);
    RegisterGraphics(g_userSettings.graphics.terrainQuality);
    RegisterGraphics(g_userSettings.graphics.terrainStrength);
    RegisterGraphics(g_userSettings.graphics.terrainDistanceM);
    RegisterGraphics(g_userSettings.graphics.terrainFlatten);
    RegisterGraphics(g_userSettings.graphics.gpuFoliage);
    RegisterGraphics(g_userSettings.graphics.gpuWeather);
    RegisterGraphics(g_userSettings.graphics.gpuParticles);
    RegisterGraphics(g_userSettings.graphics.gpuDeform);
    RegisterGraphics(g_userSettings.graphics.meshReplacement);
    RegisterGraphics(g_userSettings.graphics.staticWorld);
    RegisterGraphics(g_userSettings.graphics.debugView);

    Register(g_userSettings.backend.isoPath);
    Register(g_userSettings.backend.isoVerification);
    Register(g_userSettings.backend.graphicsBackend);
    Register(g_userSettings.backend.skipPreLaunchUI);
    Register(g_userSettings.backend.showPipelineCompilation);
    Register(g_userSettings.backend.wasPresetChosen);
    Register(g_userSettings.backend.checkForUpdates);
    Register(g_userSettings.backend.checkForModUpdates);
    Register(g_userSettings.backend.cardFileType);
    Register(g_userSettings.backend.enableAdvancedSettings);

    Register(g_userSettings.actionBindings.firstPersonCamera[0]);
    Register(g_userSettings.actionBindings.firstPersonCamera[1]);
    Register(g_userSettings.actionBindings.firstPersonCamera[2]);
    Register(g_userSettings.actionBindings.firstPersonCamera[3]);
    Register(g_userSettings.actionBindings.callMidna[0]);
    Register(g_userSettings.actionBindings.callMidna[1]);
    Register(g_userSettings.actionBindings.callMidna[2]);
    Register(g_userSettings.actionBindings.callMidna[3]);
    Register(g_userSettings.actionBindings.openMapScreen[0]);
    Register(g_userSettings.actionBindings.openMapScreen[1]);
    Register(g_userSettings.actionBindings.openMapScreen[2]);
    Register(g_userSettings.actionBindings.openMapScreen[3]);
    Register(g_userSettings.actionBindings.toggleMinimap[0]);
    Register(g_userSettings.actionBindings.toggleMinimap[1]);
    Register(g_userSettings.actionBindings.toggleMinimap[2]);
    Register(g_userSettings.actionBindings.toggleMinimap[3]);
    Register(g_userSettings.actionBindings.openDusklightMenu[0]);
    Register(g_userSettings.actionBindings.openDusklightMenu[1]);
    Register(g_userSettings.actionBindings.openDusklightMenu[2]);
    Register(g_userSettings.actionBindings.openDusklightMenu[3]);
    Register(g_userSettings.actionBindings.turboSpeedButton[0]);
    Register(g_userSettings.actionBindings.turboSpeedButton[1]);
    Register(g_userSettings.actionBindings.turboSpeedButton[2]);
    Register(g_userSettings.actionBindings.turboSpeedButton[3]);
}

// Transient settings

static TransientSettings g_transientSettings = {
    .collisionView = {
        .enableTerrainView = false,
        .enableWireframe = false,
        .enableAtView = false,
        .enableTgView = false,
        .enableCoView = false,
        .terrainViewOpacity = 50.0f,
        .colliderViewOpacity = 50.0f,
        .drawRange = 100.0f,
    },
    .triggerView = {
        .loadZones = false,
        .eventAreas = false,
        .switchAreas = false,
        .eventTags = false,
        .midnaStops = false,
        .twilightGates = false,
        .checkpoints = false,
        .paths = false,
        .transformDists = false,
        .attentionDists = false,
        .purpleMistAvoid = false,
        .leevers = false,
        .opacity = 75.0f,
    },
    .turboMode = false,
};

TransientSettings& getTransientSettings() {
    return g_transientSettings;
}

}
