// Story sync after the capture: later beats from synthetic checkpoints (lib.js STORY).

const { COMMON_CVARS, STORY, STORY_BITS: B } = require("../lib");

const settle = (timeoutSec = 240) => ({ op: "waitStorySettled", pressA: true, timeoutSec });
const at = (s) => ({ op: "waitStage", stage: s, timeoutSec: 120 });

// Phase 0: play one beat from a checkpoint and log what the tracker saw (depart, arrive, settle).
const observe = (name, description, start, steps) => ({
    name: `story-observe-${name}`,
    description: `observe: ${description}`,
    manualOnly: true,
    timeoutSec: 420,
    cvars: COMMON_CVARS,
    instances: [{
        name: "solo",
        start,
        steps: [at(start.stage), { op: "dumpStory" }, ...steps, settle(), { op: "dumpStory" }, { op: "quit" }],
    }],
});

const bossExit = (key, label) =>
    observe(`boss-${STORY[key].stage}`, `${label} boss warp exit`, STORY[key], [{ op: "triggerStory", via: "bossWarp" }, { op: "wait", sec: 5 }]);

const observeScenarios = [
    observe("eldin-spring", "Eldin tears complete: kytag04's warp from Kakariko (V17)", STORY.eldinTwilight,
        [{ op: "triggerStory", via: "tearsFull", area: 1 }, { op: "wait", sec: 5 }, settle(), { op: "expectDarkClear", level: 1 }]),
    observe("lanayru-spring", "Lanayru tears complete: kytag04's warp from Lake Hylia (V26)", STORY.lanayruTwilight,
        [{ op: "triggerStory", via: "tearsFull", area: 2 }, { op: "wait", sec: 5 }, settle(), { op: "expectDarkClear", level: 2 }]),
    observe("eldin-gate", "Eldin twilight arrival at Hyrule Field room 2 point 10 (V14)", STORY.eldinGate,
        [{ op: "expectSwitch", no: 12, set: false }, { op: "triggerStory", via: "arrival", room: 2, point: 10, layer: 14 },
            { op: "wait", sec: 5 }, settle(), { op: "expectTransformLevel", level: 1 }]),
    observe("lanayru-gate", "Lanayru twilight arrival at Hyrule Field room 9 point 10 (V21)", STORY.lanayruGate,
        [{ op: "expectSwitch", no: 13, set: false }, { op: "triggerStory", via: "arrival", room: 9, point: 10, layer: 14 },
            { op: "wait", sec: 5 }, settle(), { op: "expectTransformLevel", level: 2 }]),
    observe("master-sword", "Sacred Grove point 99: the Master Sword arrival (V33)", STORY.sacredGrove,
        [{ op: "triggerStory", via: "arrival", room: 1, point: 99 }, { op: "wait", sec: 5 }, settle(), { op: "expectDarkClear", level: 3 }]),
    observe("zant-l9", "the throne room point 25 on cutscene layer 9, after Zant (V42)", STORY.palaceZant,
        [{ op: "setEventBit", no: B.zantDefeated }, { op: "triggerStory", via: "arrival", room: 10, point: 25, layer: 9 }, { op: "wait", sec: 5 }]),
    observe("cell-l11", "the cell point 24 on layer 11 with the wake-up bit already set (V45)",
        { stage: "R_SP107", room: 0, point: 0, eventBits: [B.day2Done, B.cellWakeUp], levels: { transform: [0] } },
        [{ op: "expectSwitch", no: 27, set: false }, { op: "triggerStory", via: "arrival", room: 0, point: 24, layer: 11 }, { op: "wait", sec: 5 }]),
    bossExit("forestBoss", "Forest Temple (V13)"),
    bossExit("goronMinesBoss", "Goron Mines (V19)"),
    bossExit("lakebedBoss", "Lakebed Temple and the Zant chain (V30)"),
    bossExit("agBoss", "Arbiter's Grounds (V34)"),
    bossExit("snowpeakBoss", "Snowpeak Ruins (V35)"),
    bossExit("totBoss", "Temple of Time (V36)"),
    bossExit("cityBoss", "City in the Sky (V40)"),
];

module.exports = [...observeScenarios];
