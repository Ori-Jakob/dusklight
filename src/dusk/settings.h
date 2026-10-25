#pragma once

#include <array>

#include "dusk/config_var.hpp"
#include "dusk/ui/controls.hpp"

namespace dusk {

using config::ConfigVar;
using config::ActionBindConfigVar;

enum class BloomMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class DepthOfFieldMode : int {
    Off = 0,
    Classic = 1,
    Dusk = 2,
};

enum class Resampler : int {
    Bilinear = 0,
    Area = 1,
};

enum class GameLanguage : u8 {
    English = OS_LANGUAGE_ENGLISH,
    German = OS_LANGUAGE_GERMAN,
    French = OS_LANGUAGE_FRENCH,
    Spanish = OS_LANGUAGE_SPANISH,
    Italian = OS_LANGUAGE_ITALIAN,
    Japanese = 6,
};

enum class DiscVerificationState : u8 {
    Unknown = 0,
    Success,
    HashMismatch,
};

enum class FrameInterpMode : u8 {
    Off = 0,
    Capped = 1,
    Unlimited = 2,
};

enum class LetterboxMode : u8 {
    Off = 0,
    On = 1,
    GameplayOnly = 2,
    CutsceneOnly = 3,
};

enum class TouchTargeting : u8 {
    Hybrid = 0,
    Hold = 1,
    Switch = 2,
};

enum class MenuScaling : u8 {
    GameCube = 0,
    Wii = 1,
    Dusklight = 2,
};

enum class AlwaysGreatspinMode : u8 {
    OFF = 0,
    AFTER_SKILL = 1,
    ALWAYS = 2,
};

enum class MagicArmorMode : u8 {
    NORMAL = 0,
    ON_DAMAGE = 1,
    DOUBLE_DEFENSE = 2,
    INVINCIBLE = 3,
    COSMETIC = 4,
};

enum class AudioOutputMode : u8 {
    StereoSpeakers = 0,
    StereoHeadphones = 1,   // spatial audio
    Surround6ch = 2,        // discrete 5.1
    Surround8ch = 3,        // discrete 7.1
};

enum class GraphicsPreset : u8 {
    Custom = 0,
    Original = 1,
    Low = 2,
    Medium = 3,
    High = 4,
    Ultra = 5,
};

enum class GraphicsQuality : u8 {
    Off = 0,
    Low = 1,
    Medium = 2,
    High = 3,
    Ultra = 4,
};

enum class GraphicsExposure : u8 {
    MatchLegacy = 0,
    Auto = 1,
    Manual = 2,
};

enum class GraphicsTonemap : u8 {
    PbrNeutral = 0,
    AgX = 1,
};

enum class GraphicsGi : u8 {
    Off = 0,
    Voxel = 1,
    Ddgi = 2,
};

enum class GraphicsRtMode : u8 {
    Off = 0,
    Shadows = 1,
    ShadowsAo = 2,
    ShadowsAoReflections = 3,
    PathTraceReference = 4,
};

enum class GraphicsRtBackend : u8 {
    Auto = 0,
    Compute = 1,
    Dxr = 2,
};

enum class GraphicsReflections : u8 {
    Off = 0,
    Ssr = 1,
    SsrRt = 2,
};

enum class GraphicsGameShadows : u8 {
    Auto = 0,
    Keep = 1,
    Remove = 2,
};

enum class GraphicsGodRays : u8 {
    Off = 0,
    Auto = 1,
    ScreenSpace = 2,
    Volumetric = 3,
};

enum class GraphicsTerrainDetail : u8 {
    Off = 0,
    Parallax = 1,
    Tessellated = 2,
};

enum class GraphicsGrassShadows : u8 {
    Off = 0,
    Contact = 1,
    Shell = 2,
    Full = 3,
};

enum class GraphicsStaticWorld : u8 {
    Off = 0,
    Validate = 1,
    On = 2,
};

namespace config {
template <>
struct ConfigEnumRange<BloomMode> {
    static constexpr auto min = BloomMode::Off;
    static constexpr auto max = BloomMode::Dusk;
};

template <>
struct ConfigEnumRange<DepthOfFieldMode> {
    static constexpr auto min = DepthOfFieldMode::Off;
    static constexpr auto max = DepthOfFieldMode::Dusk;
};

template <>
struct ConfigEnumRange<Resampler> {
    static constexpr auto min = Resampler::Bilinear;
    static constexpr auto max = Resampler::Area;
};

template <>
struct ConfigEnumRange<GameLanguage> {
    static constexpr auto min = GameLanguage::English;
    static constexpr auto max = GameLanguage::Japanese;
};

template <>
struct ConfigEnumRange<DiscVerificationState> {
    static constexpr auto min = DiscVerificationState::Unknown;
    static constexpr auto max = DiscVerificationState::HashMismatch;
};

template <>
struct ConfigEnumRange<FrameInterpMode> {
    static constexpr auto min = FrameInterpMode::Off;
    static constexpr auto max = FrameInterpMode::Unlimited;
};

template <>
struct ConfigEnumRange<LetterboxMode> {
    static constexpr auto min = LetterboxMode::Off;
    static constexpr auto max = LetterboxMode::CutsceneOnly;
};

template <>
struct ConfigEnumRange<TouchTargeting> {
    static constexpr auto min = TouchTargeting::Hybrid;
    static constexpr auto max = TouchTargeting::Switch;
};

template <>
struct ConfigEnumRange<MenuScaling> {
    static constexpr auto min = MenuScaling::GameCube;
    static constexpr auto max = MenuScaling::Dusklight;
};

template <>
struct ConfigEnumRange<AlwaysGreatspinMode> {
    static constexpr auto min = AlwaysGreatspinMode::OFF;
    static constexpr auto max = AlwaysGreatspinMode::ALWAYS;
};

template <>
struct ConfigEnumRange<MagicArmorMode> {
    static constexpr auto min = MagicArmorMode::NORMAL;
    static constexpr auto max = MagicArmorMode::COSMETIC;
};

template <>
struct ConfigEnumRange<AudioOutputMode> {
    static constexpr auto min = AudioOutputMode::StereoSpeakers;
    static constexpr auto max = AudioOutputMode::Surround8ch;
};

// Custom is derived from the other keys and never stored, so a stored 0 loads as Original
template <>
struct ConfigEnumRange<GraphicsPreset> {
    static constexpr auto min = GraphicsPreset::Original;
    static constexpr auto max = GraphicsPreset::Ultra;
};

template <>
struct ConfigEnumRange<GraphicsQuality> {
    static constexpr auto min = GraphicsQuality::Off;
    static constexpr auto max = GraphicsQuality::Ultra;
};

template <>
struct ConfigEnumRange<GraphicsExposure> {
    static constexpr auto min = GraphicsExposure::MatchLegacy;
    static constexpr auto max = GraphicsExposure::Manual;
};

template <>
struct ConfigEnumRange<GraphicsTonemap> {
    static constexpr auto min = GraphicsTonemap::PbrNeutral;
    static constexpr auto max = GraphicsTonemap::AgX;
};

template <>
struct ConfigEnumRange<GraphicsGi> {
    static constexpr auto min = GraphicsGi::Off;
    static constexpr auto max = GraphicsGi::Ddgi;
};

template <>
struct ConfigEnumRange<GraphicsRtMode> {
    static constexpr auto min = GraphicsRtMode::Off;
    static constexpr auto max = GraphicsRtMode::PathTraceReference;
};

template <>
struct ConfigEnumRange<GraphicsRtBackend> {
    static constexpr auto min = GraphicsRtBackend::Auto;
    static constexpr auto max = GraphicsRtBackend::Dxr;
};

template <>
struct ConfigEnumRange<GraphicsReflections> {
    static constexpr auto min = GraphicsReflections::Off;
    static constexpr auto max = GraphicsReflections::SsrRt;
};

template <>
struct ConfigEnumRange<GraphicsGameShadows> {
    static constexpr auto min = GraphicsGameShadows::Auto;
    static constexpr auto max = GraphicsGameShadows::Remove;
};

template <>
struct ConfigEnumRange<GraphicsGodRays> {
    static constexpr auto min = GraphicsGodRays::Off;
    static constexpr auto max = GraphicsGodRays::Volumetric;
};

template <>
struct ConfigEnumRange<GraphicsTerrainDetail> {
    static constexpr auto min = GraphicsTerrainDetail::Off;
    static constexpr auto max = GraphicsTerrainDetail::Tessellated;
};

template <>
struct ConfigEnumRange<GraphicsGrassShadows> {
    static constexpr auto min = GraphicsGrassShadows::Off;
    static constexpr auto max = GraphicsGrassShadows::Full;
};

template <>
struct ConfigEnumRange<GraphicsStaticWorld> {
    static constexpr auto min = GraphicsStaticWorld::Off;
    static constexpr auto max = GraphicsStaticWorld::On;
};

template <>
struct ConfigValueTraits<ui::ControlLayout> {
    static constexpr bool enabled = true;
};
}  // namespace config

// Persistent user settings

struct UserSettings {
    // Program settings

    struct {
        // Video
        ConfigVar<bool> enableFullscreen;
        ConfigVar<bool> enableVsync;
        ConfigVar<bool> lockAspectRatio;
        ConfigVar<bool> enableFpsOverlay;
        ConfigVar<int> fpsOverlayCorner;
        ConfigVar<int> maxFrameRate;
        ConfigVar<bool> rememberWindowSize;
        ConfigVar<int> lastWindowWidth;
        ConfigVar<int> lastWindowHeight;
        ConfigVar<int> uiScale;
    } video;

    struct {
        // Audio
        ConfigVar<AudioOutputMode> outputMode;
        ConfigVar<int> masterVolume;
        ConfigVar<int> mainMusicVolume;
        ConfigVar<int> subMusicVolume;
        ConfigVar<int> soundEffectsVolume;
        ConfigVar<int> fanfareVolume;
        ConfigVar<bool> enableReverb;
        ConfigVar<bool> menuSounds;
    } audio;

    // Game settings

    struct {
        ConfigVar<GameLanguage> language;

        // QoL
        ConfigVar<bool> enableQuickTransform;
        ConfigVar<bool> hideTvSettingsScreen;
        ConfigVar<bool> biggerWallets;
        ConfigVar<bool> noReturnRupees;
        ConfigVar<bool> disableRupeeCutscenes;
        ConfigVar<bool> fastTransitions;
        ConfigVar<bool> noSwordRecoil;
        ConfigVar<int> damageMultiplier;
        ConfigVar<bool> noHeartDrops;
        ConfigVar<bool> instantDeath;
        ConfigVar<bool> fastClimbing;
        ConfigVar<bool> noMissClimbing;
        ConfigVar<bool> fastTears;
        ConfigVar<bool> no2ndFishForCat;
        ConfigVar<bool> buttonFishing;
        ConfigVar<bool> instantSaves;
        ConfigVar<bool> instantText;
        ConfigVar<bool> holdToMash;
        ConfigVar<bool> sunsSong;
        ConfigVar<bool> autoSave;
        ConfigVar<bool> enhancedMapMenus;
        ConfigVar<bool> aimingReticle;
        ConfigVar<bool> noHitStun;

        // Preferences
        ConfigVar<bool> enableMirrorMode;
        ConfigVar<bool> minimalHUD;
        ConfigVar<float> hudScale;
        ConfigVar<bool> pauseOnFocusLost;
        ConfigVar<bool> enableLinkDollRotation;
        ConfigVar<bool> enableAchievementToasts;
        ConfigVar<bool> enableControllerToasts;
        ConfigVar<bool> enableDiscordPresence;
        ConfigVar<MenuScaling> menuScalingMode;

        // Graphics
        ConfigVar<BloomMode> bloomMode;
        ConfigVar<float> bloomMultiplier;
        ConfigVar<DepthOfFieldMode> depthOfFieldMode;
        ConfigVar<bool> disableWaterRefraction;
        ConfigVar<bool> enableTextureReplacements;
        ConfigVar<FrameInterpMode> enableFrameInterpolation;
        ConfigVar<int> internalResolutionScale;
        ConfigVar<int> shadowResolutionMultiplier;
        ConfigVar<Resampler> resampler;
        ConfigVar<bool> enableMapBackground;
        ConfigVar<bool> disableCutscenePillarboxing;
        ConfigVar<LetterboxMode> disableLetterboxing;
        ConfigVar<bool> enableHighQualityMinimapTextures;

        // Audio
        ConfigVar<bool> noLowHpSound;
        ConfigVar<bool> midnasLamentNonStop;

        // Input
        ConfigVar<bool> enableGyroAim;
        ConfigVar<bool> enableGyroRollgoal;
        ConfigVar<float> gyroSensitivityX;
        ConfigVar<float> gyroSensitivityY;
        ConfigVar<float> gyroSensitivityRollgoal;
        ConfigVar<float> gyroSmoothing;
        ConfigVar<float> gyroDeadband;
        ConfigVar<bool> gyroInvertPitch;
        ConfigVar<bool> gyroInvertYaw;
        ConfigVar<bool> enableMouseCamera;
        ConfigVar<bool> enableMouseAim;
        ConfigVar<float> mouseAimSensitivity;
        ConfigVar<float> mouseCameraSensitivity;
        ConfigVar<bool> invertMouseY;
        ConfigVar<bool> freeCamera;
        ConfigVar<bool> enableTouchControls;
        ConfigVar<TouchTargeting> touchTargeting;
        ConfigVar<bool> enableMenuPointer;
        ConfigVar<ui::ControlLayout> touchControlsLayout;
        ConfigVar<bool> invertCameraXAxis;
        ConfigVar<bool> invertCameraYAxis;
        ConfigVar<bool> invertFirstPersonXAxis;
        ConfigVar<bool> invertFirstPersonYAxis;
        ConfigVar<bool> invertAirSwimX;
        ConfigVar<bool> invertAirSwimY;
        ConfigVar<float> freeCameraXSensitivity;
        ConfigVar<float> freeCameraYSensitivity;
        ConfigVar<float> touchCameraXSensitivity;
        ConfigVar<float> touchCameraYSensitivity;
        ConfigVar<bool> debugFlyCam;
        ConfigVar<bool> debugFlyCamLockEvents;
        ConfigVar<bool> allowBackgroundInput;
        std::array<ConfigVar<bool>, 4> enableLED;
        ConfigVar<bool> swapDirectSelect;

        // Cheats
        ConfigVar<bool> infiniteHearts;
        ConfigVar<bool> infiniteArrows;
        ConfigVar<bool> infiniteSeeds;
        ConfigVar<bool> infiniteBombs;
        ConfigVar<bool> infiniteOil;
        ConfigVar<bool> infiniteOxygen;
        ConfigVar<bool> infiniteRupees;
        ConfigVar<bool> enableIndefiniteItemDrops;
        ConfigVar<bool> moonJump;
        ConfigVar<bool> superClawshot;
        ConfigVar<AlwaysGreatspinMode> alwaysGreatspin;
        ConfigVar<bool> enableFastIronBoots;
        ConfigVar<bool> canTransformAnywhere;
        ConfigVar<bool> fastRoll;
        ConfigVar<bool> fastSpinner;
        ConfigVar<MagicArmorMode> armorRupeeDrain;
        ConfigVar<bool> invincibleEnemies;
        ConfigVar<bool> easyQuickSpin;
        ConfigVar<bool> infiniteEponaStamina;

        // Technical
        ConfigVar<bool> restoreWiiGlitches;

        // Controls
        ConfigVar<bool> enableTurboKeybind;
        ConfigVar<bool> enableResetKeybind;

        // Tools
        ConfigVar<bool> speedrunMode;
        ConfigVar<bool> liveSplitEnabled;
        ConfigVar<bool> showSpeedrunRTATimer;
        ConfigVar<bool> recordingMode;
        ConfigVar<bool> removeQuestMapMarkers;
        ConfigVar<bool> showInputViewer;
        ConfigVar<bool> showInputViewerGyro;
        ConfigVar<bool> enableMoveLinkCombo;
        ConfigVar<bool> enableTeleportCombo;

        ConfigVar<std::string> lastSelectedGameModeId;
    } game;

    struct {
        ConfigVar<GraphicsPreset> preset;
        ConfigVar<bool> textureMaterials;
        ConfigVar<bool> stochasticTiling;
        ConfigVar<bool> legacyMatMaps;
        ConfigVar<bool> generateMissingMips;
        ConfigVar<bool> pbr;
        ConfigVar<float> legacyBlend;
        ConfigVar<GraphicsExposure> exposure;
        ConfigVar<float> exposureBias;
        ConfigVar<GraphicsTonemap> tonemap;
        ConfigVar<bool> taa;
        ConfigVar<float> taaSharpness;
        ConfigVar<GraphicsQuality> ao;
        ConfigVar<bool> ssil;
        ConfigVar<GraphicsQuality> shadowQuality;
        ConfigVar<int> shadowPages;
        ConfigVar<int> shadowPageBudget;
        ConfigVar<GraphicsGameShadows> gameShadows;
        ConfigVar<float> legacyShadowGain;
        ConfigVar<bool> keepShadowCues;
        ConfigVar<GraphicsGi> gi;
        ConfigVar<GraphicsRtMode> rt;
        ConfigVar<GraphicsRtBackend> rtBackend;
        ConfigVar<GraphicsReflections> reflections;
        ConfigVar<GraphicsQuality> fogVolumetric;
        ConfigVar<GraphicsGodRays> godRays;
        ConfigVar<float> fogScattering;
        ConfigVar<float> shaftStrength;
        ConfigVar<bool> fogLegacyMatch;
        ConfigVar<bool> fogTranslucents;
        ConfigVar<GraphicsQuality> clouds;
        ConfigVar<float> cloudStyle;
        ConfigVar<bool> cloudShadows;
        ConfigVar<float> cloudShadowStrength;
        ConfigVar<bool> keepCloudDome;
        ConfigVar<float> cloudCoverage;
        ConfigVar<float> cloudDensity;
        ConfigVar<float> cloudAltitude;
        ConfigVar<float> cloudWindSpeed;
        ConfigVar<float> cloudSilverLining;
        ConfigVar<float> cloudPowder;
        ConfigVar<GraphicsQuality> grassQuality;
        ConfigVar<bool> grassReplaceGame;
        ConfigVar<bool> grassTerrain;
        ConfigVar<int> grassDistanceM;
        ConfigVar<float> grassDensity;
        ConfigVar<float> grassDistantDensity;
        ConfigVar<GraphicsGrassShadows> grassShadows;
        ConfigVar<bool> grassLegacyShadows;
        ConfigVar<bool> grassInteraction;
        ConfigVar<float> grassWind;
        ConfigVar<GraphicsTerrainDetail> terrainDetail;
        ConfigVar<GraphicsQuality> terrainQuality;
        ConfigVar<float> terrainStrength;
        ConfigVar<int> terrainDistanceM;
        ConfigVar<bool> terrainFlatten;
        ConfigVar<bool> gpuFoliage;
        ConfigVar<bool> gpuWeather;
        ConfigVar<bool> gpuParticles;
        ConfigVar<bool> gpuDeform;
        ConfigVar<bool> meshReplacement;
        ConfigVar<GraphicsStaticWorld> staticWorld;
        ConfigVar<int> debugView;
    } graphics;

    struct {
        ConfigVar<std::string> isoPath;
        ConfigVar<DiscVerificationState> isoVerification;
        ConfigVar<std::string> graphicsBackend;
        ConfigVar<bool> skipPreLaunchUI;
        ConfigVar<bool> showPipelineCompilation;
        ConfigVar<bool> wasPresetChosen;
        ConfigVar<bool> checkForUpdates;
        ConfigVar<bool> checkForModUpdates;
        ConfigVar<int> cardFileType;
        ConfigVar<bool> enableAdvancedSettings;
    } backend;

    // Arrays of size 4 for 4 ports
    struct {
        std::array<ActionBindConfigVar, 4> firstPersonCamera;
        std::array<ActionBindConfigVar, 4> callMidna;
        std::array<ActionBindConfigVar, 4> openMapScreen;
        std::array<ActionBindConfigVar, 4> toggleMinimap;
        std::array<ActionBindConfigVar, 4> openDusklightMenu;
        std::array<ActionBindConfigVar, 4> turboSpeedButton;
    } actionBindings;
};

UserSettings& getSettings();

void registerSettings();

void applyInternalResolutionScale(int scale);
void applyResampler(Resampler resampler);

inline bool isLetterboxingDisabled(bool inCutscene) {
    const auto mode = getSettings().game.disableLetterboxing.getValue();
    return mode == LetterboxMode::On ||
           (mode == LetterboxMode::CutsceneOnly && inCutscene) ||
           (mode == LetterboxMode::GameplayOnly && !inCutscene);
}

// Transient settings

struct CollisionViewSettings {
    bool enableTerrainView;
    bool enableWireframe;
    bool enableAtView;
    bool enableTgView;
    bool enableCoView;
    float terrainViewOpacity;
    float colliderViewOpacity;
    float drawRange;
};

struct TriggerViewSettings {
    bool loadZones;
    bool eventAreas;
    bool switchAreas;
    bool eventTags;
    bool midnaStops;
    bool twilightGates;
    bool checkpoints;
    bool paths;
    bool transformDists;
    bool attentionDists;
    bool purpleMistAvoid;
    bool leevers;
    float opacity;
};

struct TransientSettings {
    CollisionViewSettings collisionView;
    TriggerViewSettings triggerView;
    bool turboMode;
    bool moveLinkActive;
    bool stateShareLoadActive;
};

TransientSettings& getTransientSettings();

}  // namespace dusk
