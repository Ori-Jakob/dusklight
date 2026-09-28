// Scenario helpers; stage triples from include/dusk/map_loader_definitions.h.

// Same escaping as the loader's escape_mod_id_for_config.
const MOD_ID = "dev.n0ted.twili_together";
const escapeModId = (id) => id.replace(/_/g, "__").replace(/\./g, "_");
const CONFIG_PREFIX = `mod.${escapeModId(MOD_ID)}.`;

// tt("hide_players_in_cutscene", 1) -> a --cvar for a mod setting.
const tt = (name, value) => `${CONFIG_PREFIX}${name}=${value}`;

// For a scenario's enableMods; every other mod next to the exe is disabled.
const MODS = { randomizer: "dev.twilitrealm.randomizer" };

// Load without a forced cutscene on a fresh save.
const STAGES = {
    linksHouse: { stage: "R_SP01", room: 4, point: 0 },
    southFaron: { stage: "F_SP108", room: 0, point: 0 },
    faronField: { stage: "F_SP121", room: 6, point: 0 },
    forestTemple: { stage: "D_MN05", room: 0, point: 0 },
    goronMines: { stage: "D_MN04", room: 1, point: 0 },
    gerudoDesert: { stage: "F_SP124", room: 0, point: 0 },
    // Day 1 village: NPC trigger areas can start dialogue.
    ordonVillage: { stage: "F_SP103", room: 0, point: 0 },
};

// dItemNo_* values and event bits.
const ITEMS = {
    boomerang: 0x40, spinner: 0x41, ironball: 0x42, bow: 0x43, hookshot: 0x44, copyRod: 0x46,
    wHookshot: 0x47, lantern: 0x48, slingshot: 0x4b, horseFlute: 0x84,
};
const EVENTS = { shieldAttack: 0x2908, copyRodPower: 0x2580 };

// Extra --cvar overrides for every instance.
const COMMON_CVARS = [];

// Prelaunch (forced open over the game by these two) restores the last game mode: the randomizer's.
const RANDOMIZER_MODE_CVARS = [
    "backend.skipPreLaunchUI=true",
    `game.lastSelectedGameModeId=randomizer_${MODS.randomizer}`,
];

const warp = (s) => ({ op: "warp", ...s });
const waitStage = (s, timeoutSec = 90) => ({ op: "waitStage", stage: s.stage, timeoutSec });
const barrier = (name, from, timeoutSec = 120) => [
    { op: "signal", name },
    { op: "waitSignal", name, from, timeoutSec },
];

const meetIn = (s, other) => [
    waitStage(s),
    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 90 },
    { op: "waitDummies", count: 1, timeoutSec: 60 },
    { op: "walk", frames: 180, circle: true },
    { op: "wait", frames: 30 },
    { op: "checkDummies", maxDist: 400 },
    ...barrier(`met-${s.stage}-${s.room}`, other),
];

const connect = [
    { op: "connect" },
    { op: "waitConnected", timeoutSec: 20 },
];

module.exports = {
    MOD_ID, CONFIG_PREFIX, escapeModId, tt, MODS, STAGES, ITEMS, EVENTS, COMMON_CVARS, RANDOMIZER_MODE_CVARS,
    warp, waitStage, barrier, meetIn, connect,
};
