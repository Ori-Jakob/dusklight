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
    wHookshot: 0x47, lantern: 0x48, fishingRod: 0x4a, slingshot: 0x4b, horseFlute: 0x84,
};
const EVENTS = { shieldAttack: 0x2908, copyRodPower: 0x2580 };

// Story event bits (include/d/d_save_bit_labels.inc).
const STORY_BITS = {
    day2Done: 0x4510, castleEscape: 0x0502, cellWakeUp: 0x4d08, forestClear: 0x0602,
    faronSpiritTalk: 0x0c40, eldinSpirit: 0x0708, minesClear: 0x0701, meteorWarped: 0x0880,
    lanayruSpirit: 0x0c02, lakebedClear: 0x0904, zantAppears: 0x0c01, zeldaHealsMidna: 0x1e08,
    masterSword: 0x2020, shadowCrystal: 0x0d04, midnaRiding: 0x0c10, arbiterClear: 0x2010,
    snowpeakClear: 0x2008, timeClear: 0x2004, cityClear: 0x2002, mirrorRestored: 0x2b08,
    zantDefeated: 0x5410, barrierShattered: 0x4208,
};
const B = STORY_BITS;
const FARON_DONE = [B.day2Done, B.castleEscape, B.cellWakeUp, B.forestClear, B.faronSpiritTalk];
const ELDIN_DONE = [...FARON_DONE, B.eldinSpirit, B.minesClear];
const LANAYRU_DONE = [...ELDIN_DONE, B.meteorWarped, B.lanayruSpirit, B.lakebedClear];
const MDH_DONE = [...LANAYRU_DONE, B.zantAppears, B.zeldaHealsMidna];
const SWORD_DONE = [...MDH_DONE, B.masterSword, B.shadowCrystal];

// Synthetic story checkpoints: start fields for one area each (a fixture replaces them when present).
const bossRoom = (stage, eventBits, levels) => ({ stage, room: 50, point: 0, eventBits, levels, stageBoss: [stage] });
const STORY = {
    eldinGate: { stage: "F_SP121", room: 2, point: 0, eventBits: FARON_DONE, levels: { transform: [0], darkClear: [0] } },
    eldinTwilight: { stage: "F_SP109", room: 0, point: 0, eventBits: FARON_DONE, levels: { transform: [0, 1], darkClear: [0] }, vessels: [1] },
    goronMinesBoss: bossRoom("D_MN04A", FARON_DONE.concat([B.eldinSpirit]), { transform: [0, 1], darkClear: [0, 1] }),
    lanayruGate: { stage: "F_SP121", room: 9, point: 0, eventBits: ELDIN_DONE, levels: { transform: [0, 1], darkClear: [0, 1] } },
    lanayruTwilight: { stage: "F_SP115", room: 0, point: 0, eventBits: ELDIN_DONE.concat([B.meteorWarped]), levels: { transform: [0, 1, 2], darkClear: [0, 1] }, vessels: [2] },
    lakebedBoss: bossRoom("D_MN01A", LANAYRU_DONE, { transform: [0, 1, 2], darkClear: [0, 1, 2] }),
    zeldaTower: { stage: "R_SP107", room: 3, point: 0, eventBits: LANAYRU_DONE.concat([B.zantAppears]), levels: { transform: [0, 1, 2, 3], darkClear: [0, 1, 2] } },
    sacredGrove: { stage: "F_SP117", room: 1, point: 1, eventBits: MDH_DONE.concat([B.midnaRiding]), levels: { transform: [0, 1, 2, 3], darkClear: [0, 1, 2] } },
    forestBoss: bossRoom("D_MN05A", [B.day2Done, B.castleEscape, B.cellWakeUp], { transform: [0], darkClear: [0] }),
    agBoss: bossRoom("D_MN10A", SWORD_DONE, { transform: [0, 1, 2, 3], darkClear: [0, 1, 2, 3] }),
    snowpeakBoss: bossRoom("D_MN11A", SWORD_DONE.concat([B.arbiterClear]), { transform: [0, 1, 2, 3], darkClear: [0, 1, 2, 3] }),
    totBoss: bossRoom("D_MN06A", SWORD_DONE.concat([B.arbiterClear, B.snowpeakClear]), { transform: [0, 1, 2, 3], darkClear: [0, 1, 2, 3] }),
    cityBoss: bossRoom("D_MN07A", SWORD_DONE.concat([B.arbiterClear, B.snowpeakClear, B.timeClear]), { transform: [0, 1, 2, 3], darkClear: [0, 1, 2, 3] }),
    palaceZant: { stage: "D_MN08A", room: 10, point: 0, eventBits: SWORD_DONE.concat([B.arbiterClear, B.snowpeakClear, B.timeClear, B.cityClear, B.mirrorRestored]), levels: { transform: [0, 1, 2, 3], darkClear: [0, 1, 2, 3] } },
    kakariko: { stage: "F_SP109", room: 0, point: 0, eventBits: ELDIN_DONE, levels: { transform: [0, 1], darkClear: [0, 1] } },
};

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
    STORY_BITS, STORY, warp, waitStage, barrier, meetIn, connect,
};
