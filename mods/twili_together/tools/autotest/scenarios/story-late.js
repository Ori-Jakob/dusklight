// Story sync after the capture: later beats from synthetic checkpoints (lib.js STORY).

const { COMMON_CVARS, STAGES, STORY, STORY_BITS: B, barrier, connect } = require("../lib");

// MoveRecord qual bits (StoryTypes.hpp).
const QUAL = { curated: 1, side: 2, form: 4, levels: 8, oneShot: 16, bits: 32, boss: 64 };

const settle = (timeoutSec = 240) => ({ op: "waitStorySettled", pressA: true, timeoutSec });
// Dungeon exits and some arrivals end at the save prompt: answer it with no.
const noSave = { op: "dismissSaveRequest", optional: true, timeoutSec: 120 };
const at = (s) => ({ op: "waitStage", stage: s, timeoutSec: 120 });

// Phase 0: play one beat from a checkpoint and log what the tracker saw (depart, arrive, settle).
const observe = (name, description, start, steps) => ({
    name: `story-observe-${name}`,
    description: `observe: ${description}`,
    manualOnly: true,
    timeoutSec: 600,
    cvars: COMMON_CVARS,
    instances: [{
        name: "solo",
        start,
        steps: [at(start.stage), { op: "dumpStory" }, ...steps, settle(400), { op: "dumpStory" }, { op: "quit" }],
    }],
});

const bossExit = (key, label) =>
    observe(`boss-${STORY[key].stage}`, `${label} boss warp exit`, STORY[key], [{ op: "triggerStory", via: "bossWarp" }, noSave]);

const observeScenarios = [
    observe("eldin-spring", "Eldin tears complete: kytag04's warp from Kakariko (V17)", STORY.eldinTwilight,
        [{ op: "triggerStory", via: "tearsFull", area: 1 }, { op: "wait", sec: 5 }, settle(), { op: "expectDarkClear", level: 1 }]),
    observe("lanayru-spring", "Lanayru tears complete: kytag04's warp from Lake Hylia (V26)", STORY.lanayruTwilight,
        [{ op: "triggerStory", via: "tearsFull", area: 2 }, { op: "wait", sec: 5 }, settle(), { op: "expectDarkClear", level: 2 }]),
    observe("eldin-gate", "Eldin twilight arrival at Hyrule Field room 2 point 10 (V14)", { ...STORY.eldinGate, point: 10, layer: 14 },
        [settle(), { op: "expectTransformLevel", level: 1 }, { op: "expectSwitch", no: 12 }]),
    observe("lanayru-gate", "Lanayru twilight arrival at Hyrule Field room 9 point 10 (V21)", { ...STORY.lanayruGate, point: 10, layer: 14 },
        [settle(), { op: "expectTransformLevel", level: 2 }, { op: "expectSwitch", no: 13 }]),
    observe("master-sword", "Sacred Grove point 99: the Master Sword arrival (V33)", STORY.sacredGrove,
        [{ op: "triggerStory", via: "arrival", room: 1, point: 99 }, { op: "wait", sec: 5 }, settle(400), noSave, { op: "expectDarkClear", level: 3 }]),
    observe("zant-l9", "the throne room point 25 on cutscene layer 9, after Zant (V42)", STORY.palaceZant,
        [{ op: "setEventBit", no: B.zantDefeated }, { op: "triggerStory", via: "arrival", room: 10, point: 25, layer: 9 }, { op: "wait", sec: 5 }]),
    observe("cell-l11", "the cell point 24 on layer 11 with the wake-up bit already set (V45)",
        { stage: "R_SP107", room: 0, point: 0, eventBits: [B.day2Done, B.cellWakeUp], levels: { transform: [0] } },
        [{ op: "expectSwitch", no: 27, set: false }, { op: "triggerStory", via: "arrival", room: 0, point: 24, layer: 11 }, { op: "wait", sec: 5 }]),
    observe("tear", "one Eldin tear picked up in Kakariko", { ...STORY.eldinTwilight, lightDrops: { 1: 10 } },
        [{ op: "countTears" }, { op: "collectTear", nth: 0 }, { op: "expectLightDrops", area: 1, num: 11 }]),
    bossExit("forestBoss", "Forest Temple (V13)"),
    bossExit("goronMinesBoss", "Goron Mines (V19)"),
    bossExit("lakebedBoss", "Lakebed Temple and the Zant chain (V30)"),
    bossExit("agBoss", "Arbiter's Grounds (V34)"),
    bossExit("snowpeakBoss", "Snowpeak Ruins (V35)"),
    bossExit("totBoss", "Temple of Time (V36)"),
    bossExit("cityBoss", "City in the Sky (V40)"),
];

// A plays a beat, B elsewhere on the same team reacts; both meet at "ready" and "done".
const pair = ({ name, description, a, b, timeoutSec = 600 }) => ({
    name,
    description,
    timeoutSec,
    cvars: COMMON_CVARS,
    instances: [
        { name: "A", start: a.start, steps: [...connect, at(a.start.stage), { op: "waitPeers", count: 1, timeoutSec: 90 },
            ...barrier("ready", "B", 180), ...a.steps, ...barrier("done", "B", 480), { op: "quit" }] },
        { name: "B", start: b.start, steps: [...connect, at(b.start.stage), { op: "waitPeers", count: 1, timeoutSec: 90 },
            ...barrier("ready", "A", 180), ...b.steps, ...barrier("done", "A", 480), { op: "quit" }] },
    ],
});

const followTo = (stage, titleHas = "") => [
    { op: "expectPrompt", kind: "move", titleHas, timeoutSec: 360 },
    { op: "answerPrompt", answer: "follow" },
    { op: "expectStoryLoad", state: "arrived", timeoutSec: 150 },
    at(stage),
];

const scenarios = [
    pair({
        name: "story-eldin-spring",
        description: "A completes the Eldin tears: kytag04's potential-event warp is one strong move; B in Faron follows and ends human in Kakariko",
        a: {
            start: STORY.eldinTwilight,
            steps: [
                { op: "triggerStory", via: "tearsFull", area: 1 },
                { op: "wait", sec: 5 },
                settle(300),
                { op: "expectStoryMove", role: "sent", curated: "eldin-light", qualHas: QUAL.levels | QUAL.form, toStage: "F_SP109" },
                { op: "expectDarkClear", level: 1 },
                { op: "expectLocalForm", form: "human" },
            ],
        },
        b: {
            // Faron: no kytag04 shares the Eldin save table, so only the prompt brings B along.
            start: { ...STORY.eldinTwilight, stage: "F_SP108", room: 0, point: 0 },
            steps: [
                { op: "expectSegment", id: "eldin-twilight", state: "behind" },
                ...followTo("F_SP109", "Eldin Spring"),
                settle(),
                { op: "expectLocalForm", form: "human" },
                { op: "expectDarkClear", level: 1 },
                { op: "expectLayer", natural: true },
                { op: "expectStoryMove", role: "none" },
            ],
        },
    }),
    pair({
        name: "story-eldin-spring-sync",
        description: "A completes the Eldin tears; B, a wolf on Death Mountain, gets kytag04's switch by world sync and the game warps B to the spring too: B ends human in Kakariko without a prompt",
        a: {
            start: STORY.eldinTwilight,
            steps: [
                { op: "triggerStory", via: "tearsFull", area: 1 },
                { op: "wait", sec: 5 },
                settle(300),
                { op: "expectStoryMove", role: "sent", curated: "eldin-light" },
            ],
        },
        b: {
            start: { ...STORY.eldinTwilight, stage: "F_SP110", room: 0, point: 0 },
            steps: [
                { op: "expectLocalForm", form: "wolf" },
                { op: "waitStage", stage: "F_SP109", timeoutSec: 300 },
                settle(300),
                { op: "expectLocalForm", form: "human" },
                { op: "expectDarkClear", level: 1 },
                { op: "expectStoryMove", role: "sent", curated: "eldin-light" },
                { op: "expectNoPrompt", sec: 10 },
            ],
        },
    }),
    pair({
        name: "story-dungeon-exit",
        description: "A leaves the Forest Temple through the boss warp: a strong boss-clear move whose prompt names the dungeon; B in South Faron follows",
        a: {
            start: STORY.forestBoss,
            steps: [
                { op: "triggerStory", via: "bossWarp" },
                noSave,
                settle(300),
                { op: "expectStoryMove", role: "sent", qualHas: QUAL.boss, toStage: "F_SP108", toRoom: 1 },
            ],
        },
        b: {
            start: { stage: "F_SP108", room: 0, point: 0, eventBits: [B.day2Done, B.castleEscape, B.cellWakeUp], levels: { transform: [0], darkClear: [0] } },
            steps: [
                ...followTo("F_SP108", "cleared the Forest Temple"),
                noSave,
                settle(),
                { op: "expectStoryMove", role: "none" },
            ],
        },
    }),
    pair({
        name: "story-transient",
        description: "A's story move ends on a cutscene layer A stays on: B gets no prompt while A is there, then the prompt for A's next strong move",
        a: {
            start: STORY.kakariko,
            steps: [
                // Kakariko's layer 9 hosts the spring scenes; the flags pick another one.
                { op: "triggerStory", via: "forcedMove", stage: "F_SP109", room: 0, point: 0, layer: 9, bit: 0x0180 },
                settle(),
                { op: "expectStoryMove", role: "sent", qualHas: QUAL.bits, toStage: "F_SP109" },
                { op: "signal", name: "a-moved" },
                { op: "waitSignal", name: "b-checked", from: "B", timeoutSec: 120 },
                { op: "triggerStory", via: "forcedMove", stage: "F_SP121", room: 2, point: 10, layer: -1 },
                settle(),
                { op: "expectStoryMove", role: "sent", curated: "eldin-gate" },
            ],
        },
        b: {
            start: { ...STORY.kakariko, stage: "F_SP110" },
            steps: [
                { op: "waitSignal", name: "a-moved", from: "A", timeoutSec: 240 },
                { op: "expectStoryMove", role: "received", toStage: "F_SP109" },
                { op: "expectTransient", value: true },
                { op: "expectNoPrompt", sec: 20 },
                { op: "catchUp", expectKind: "none", start: false },
                { op: "signal", name: "b-checked" },
                { op: "expectPrompt", kind: "move", titleHas: "Eldin twilight wall", timeoutSec: 240 },
                { op: "expectTransient", value: false },
                { op: "answerPrompt", answer: "decline" },
            ],
        },
    }),
    pair({
        name: "story-master-sword",
        description: "A draws the Master Sword (a potential event into the Sacred Grove point 99): B, a wolf in Faron, follows and ends human; after M_077 A's later move as a wolf leaves B human",
        timeoutSec: 780,
        a: {
            start: STORY.sacredGrove,
            steps: [
                { op: "triggerStory", via: "forcedMove", stage: "F_SP117", room: 1, point: 99, layer: -1, bit: B.masterSword },
                settle(400),
                { op: "expectStoryMove", role: "sent", curated: "master-sword", qualHas: QUAL.levels | QUAL.form, toStage: "F_SP117" },
                { op: "expectDarkClear", level: 3 },
                { op: "expectLocalForm", form: "human" },
                // The Shadow Crystal: transforming is each player's own choice from here on.
                { op: "setEventBit", no: B.shadowCrystal },
                { op: "waitSignal", name: "b-human", from: "B", timeoutSec: 360 },
                // Past B's own-arrival grace, so the next move is a pop-up again.
                { op: "wait", sec: 12 },
                { op: "transform", form: "wolf", timeoutSec: 60 },
                // Faron Spring point 3 has a phase_1 side effect: a strong move no row names.
                { op: "triggerStory", via: "forcedMove", stage: "F_SP108", room: 1, point: 3, layer: -1 },
                settle(),
                { op: "expectStoryMove", role: "sent", qualHas: QUAL.side, toStage: "F_SP108", toRoom: 1 },
                { op: "expectLocalForm", form: "wolf" },
            ],
        },
        b: {
            start: { stage: "F_SP108", room: 0, point: 0, eventBits: STORY.sacredGrove.eventBits, levels: STORY.sacredGrove.levels },
            steps: [
                { op: "expectLocalForm", form: "wolf" },
                ...followTo("F_SP117", "drew the Master Sword"),
                settle(),
                { op: "expectLocalForm", form: "human" },
                { op: "expectEventBit", no: B.shadowCrystal, timeoutSec: 60 },
                { op: "signal", name: "b-human" },
                ...followTo("F_SP108"),
                settle(),
                { op: "expectLocalForm", form: "human" },
            ],
        },
    }),
    (() => {
        const kakariko = { ...STORY.eldinTwilight, lightDrops: { 1: 10 } };
        const both = (other, collect, after) => [
            ...connect, at("F_SP109"), { op: "waitPeers", count: 1, timeoutSec: 90 },
            { op: "countTears" },
            ...barrier("ready", other, 180),
            collect,
            { op: "expectLightDrops", area: 1, num: 12, timeoutSec: 60 },
            { op: "expectTears", delta: -2, timeoutSec: 30 },
            ...barrier("both-collected", other, 120),
            ...after,
            ...barrier("done", other, 300),
            { op: "quit" },
        ];
        return {
            name: "story-tears-union",
            description: "A and B collect different Eldin tears at once: both counts rise by two and neither tear respawns; a pickup racing a teammate's is not counted twice",
            timeoutSec: 600,
            cvars: COMMON_CVARS,
            instances: [
                { name: "A", start: kakariko, steps: both("B", { op: "collectTear", nth: 0 }, [
                    ...barrier("keeping", "B", 60),
                    { op: "collectTear", nth: 0 },
                    { op: "expectLightDrops", area: 1, num: 13 },
                    { op: "signal", name: "a-third" },
                    { op: "waitSignal", name: "b-raced", from: "B", timeoutSec: 120 },
                    { op: "expectLightDrops", area: 1, num: 13 },
                ]) },
                { name: "B", start: kakariko, steps: both("A", { op: "collectTear", nth: 1 }, [
                    { op: "warp", stage: "F_SP109", room: 0, point: 0 },
                    { op: "wait", sec: 3 },
                    at("F_SP109"),
                    { op: "expectTears", delta: -2 },
                    { op: "expectLightDrops", area: 1, num: 12 },
                    { op: "keepTakenTears", value: true },
                    ...barrier("keeping", "A", 60),
                    { op: "waitSignal", name: "a-third", from: "A", timeoutSec: 120 },
                    { op: "expectLightDrops", area: 1, num: 13, timeoutSec: 30 },
                    { op: "collectTear", nth: 0 },
                    { op: "expectLightDrops", area: 1, num: 13 },
                    { op: "keepTakenTears", value: false },
                    { op: "signal", name: "b-raced" },
                ]) },
            ],
        };
    })(),
    pair({
        name: "story-mdh-start",
        description: "A leaves the Lakebed: boss warp, Zant's scene and the curse to Hyrule Field; B in Kakariko follows mdh-start and ends a wolf where the mdh segment is consistent",
        timeoutSec: 900,
        a: {
            start: STORY.lakebedBoss,
            steps: [
                { op: "triggerStory", via: "bossWarp" },
                noSave,
                settle(500),
                { op: "expectStoryMove", role: "sent", curated: "mdh-start", qualHas: QUAL.levels, toStage: "F_SP121", toRoom: 10 },
                { op: "expectLocalForm", form: "wolf" },
                { op: "expectSegment", id: "mdh", state: "consistent" },
            ],
        },
        b: {
            start: { stage: "F_SP109", room: 0, point: 0, eventBits: STORY.lakebedBoss.eventBits, levels: STORY.lakebedBoss.levels },
            steps: [
                { op: "expectLocalForm", form: "human" },
                ...followTo("F_SP121", "cursed by Zant"),
                settle(),
                { op: "expectLocalForm", form: "wolf" },
                { op: "expectSegment", id: "mdh", state: "consistent" },
                { op: "expectStoryMove", role: "none" },
            ],
        },
    }),
    {
        name: "story-mdh-repair",
        description: "offline, a save with Zant's curse (M_071, transform level 3) in Kakariko is inconsistent; Catch up to story loads Hyrule Field room 10 and a wolf arrives",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [{
            name: "solo",
            start: { stage: "F_SP109", room: 0, point: 0, eventBits: [...STORY.lakebedBoss.eventBits, B.zantAppears], levels: { transform: [0, 1, 2, 3], darkClear: [0, 1, 2] } },
            steps: [
                at("F_SP109"),
                { op: "storyPrompts", value: false },
                { op: "expectSegment", id: "mdh", state: "inconsistent" },
                { op: "catchUp", expectKind: "entrance", toStage: "F_SP121" },
                { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                at("F_SP121"),
                settle(),
                { op: "expectLocalForm", form: "wolf" },
                { op: "expectSegment", id: "mdh", state: "consistent" },
                { op: "quit" },
            ],
        }],
    },
    pair({
        name: "story-learned-catchup",
        description: "A makes a strong move, then a weak one; B declined the first: Catch up to story picks the strong move, not the newer weak one",
        timeoutSec: 780,
        a: {
            start: STORY.kakariko,
            steps: [
                // Faron Spring point 3 has a phase_1 side effect: strong, and no row names it.
                { op: "triggerStory", via: "forcedMove", stage: "F_SP108", room: 1, point: 3, layer: -1 },
                settle(),
                { op: "expectStoryMove", role: "sent", qualHas: QUAL.side, toStage: "F_SP108" },
                { op: "waitSignal", name: "b-declined", from: "B", timeoutSec: 360 },
                { op: "triggerStory", via: "forcedMove", stage: "F_SP109", room: 0, point: 0, layer: -1, bit: 0x0180 },
                settle(),
                { op: "expectStoryMove", role: "sent", qualHas: QUAL.bits, toStage: "F_SP109" },
                { op: "signal", name: "a-second" },
            ],
        },
        b: {
            start: { ...STORY.kakariko, stage: "F_SP110" },
            steps: [
                { op: "expectPrompt", kind: "move", timeoutSec: 300 },
                { op: "answerPrompt", answer: "decline" },
                { op: "signal", name: "b-declined" },
                { op: "waitSignal", name: "a-second", from: "A", timeoutSec: 360 },
                { op: "expectStoryMove", role: "received", toStage: "F_SP109" },
                { op: "catchUp", expectKind: "entrance", toStage: "F_SP108" },
                { op: "expectStoryLoad", state: "arrived", timeoutSec: 150 },
                at("F_SP108"),
                { op: "expectLearned", count: 2 },
            ],
        },
    }),
    (() => {
        // A teammate's Forest Temple exit, learned in an earlier session (fnv64("vanilla")).
        const move = {
            mid: "0000000000000001", from: { stage: "D_MN05A", room: 50, point: 0, layerArg: -1, layer: 0 },
            to: { stage: "F_SP108", room: 1, point: 1, layerArg: -1, layer: 0 },
            event: { name: "BOSS_WARPIN", ev: 299, m: 255, lt: 1, type: 255, sw: 255, req: 355, rk: 3, mode: 2, arrival: false },
            wolf: [false, false], tlv: [1, 1], dcl: [1, 1], arrivalEvent: { m: 10, name: "SAVEREQ" },
            curated: "", qual: QUAL.boss, hops: 1, hl: [], bits: [], key: "actor|BOSS_WARPIN|D_MN05A/50|F_SP108/1", th: false, boss: true,
        };
        const learned = { version: 1, identity: "vanilla", lastOwn: 0, entries: [{ move, seen: 1, lastSeen: Date.now() - 60000, own: false }] };
        return {
            name: "story-learned-offline",
            description: "after a restart and offline, a teammate's strong move learned earlier is still offered by Catch up to story",
            timeoutSec: 300,
            cvars: COMMON_CVARS,
            files: { "mod_data/dev.n0ted.twili_together/story-learned/0bd5832498c9c0d0.json": JSON.stringify(learned) },
            instances: [{
                name: "solo",
                start: { ...STAGES.linksHouse, eventBits: [B.day2Done, B.castleEscape, B.cellWakeUp], levels: { transform: [0], darkClear: [0] } },
                steps: [
                    at("R_SP01"),
                    { op: "storyPrompts", value: false },
                    { op: "expectLearned", key: move.key },
                    { op: "catchUp", expectKind: "entrance", toStage: "F_SP108" },
                    { op: "expectStoryLoad", state: "arrived", timeoutSec: 120 },
                    at("F_SP108"),
                    noSave,
                    settle(),
                    { op: "catchUp", expectKind: "none", start: false },
                    { op: "quit" },
                ],
            }],
        };
    })(),
    {
        name: "story-cinematic-repair",
        description: "the stuck-capture repair loads the cell on layer 11 while the wake-up's switch is off, so demo04_02 plays and ends at point 0 on layer 14",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        expectLog: [/\[story\] accept .*'demo04_02'.*R_SP107 room 0 layer 11/],
        rejectLog: [/demo data load error/i],
        instances: [{
            name: "solo",
            start: { stage: "F_SP108", room: 0, point: 0, eventBits: [B.day2Done] },
            steps: [
                at("F_SP108"),
                { op: "storyPrompts", value: false },
                { op: "setEventBit", no: B.cellWakeUp },
                { op: "setTransformLevel", level: 0 },
                { op: "expectSegment", id: "captured", state: "inconsistent" },
                { op: "catchUp", expectKind: "entrance", toStage: "R_SP107" },
                { op: "expectStoryLoad", state: "arrived", layerArg: 11, timeoutSec: 120 },
                at("R_SP107"),
                settle(),
                { op: "expectLocalForm", form: "wolf" },
                { op: "expectLayer", layer: 14 },
                { op: "expectSegment", id: "captured", state: "consistent" },
                { op: "quit" },
            ],
        }],
    },
    {
        name: "story-entrance-sweep",
        description: "loads every segment entrance, curated follow point and learned destination: each is a known point and spawns the predicted form",
        timeoutSec: 1200,
        cvars: COMMON_CVARS,
        instances: [{
            name: "solo",
            start: STORY.kakariko,
            steps: [at("F_SP109"), { op: "storyPrompts", value: false }, { op: "entranceSweep", timeoutSec: 1100 }, { op: "quit" }],
        }],
    },
    (() => {
        const teleportOn = [
            { op: "setRoomOption", name: "teleportMode", value: true },
            { op: "waitRoomOption", name: "teleportMode", value: true, timeoutSec: 20 },
        ];
        return pair({
            name: "story-rando-bypass",
            description: "a stand-in entrance shuffle on B sends every load into South Faron to Kakariko: B's follow and teleport still land in South Faron, a plain exit there is shuffled",
            timeoutSec: 780,
            a: {
                start: STORY.kakariko,
                steps: [
                    ...teleportOn,
                    { op: "triggerStory", via: "forcedMove", stage: "F_SP108", room: 1, point: 3, layer: -1 },
                    settle(),
                    { op: "expectStoryMove", role: "sent", qualHas: QUAL.side, toStage: "F_SP108" },
                    { op: "waitSignal", name: "b-teleported", from: "B", timeoutSec: 480 },
                ],
            },
            b: {
                start: { ...STORY.kakariko, stage: "F_SP110", point: 0 },
                steps: [
                    ...teleportOn,
                    { op: "testRemap", from: { stage: "F_SP108" }, to: { stage: "F_SP109", room: 0, point: 33 } },
                    ...followTo("F_SP108"),
                    settle(),
                    { op: "warp", stage: "F_SP110", room: 0, point: 0 },
                    { op: "wait", sec: 3 },
                    at("F_SP110"),
                    { op: "teleportTo", target: "A", expect: "stage", timeoutSec: 90 },
                    at("F_SP108"),
                    { op: "signal", name: "b-teleported" },
                    // A game exit into South Faron is still shuffled.
                    { op: "warp", stage: "F_SP108", room: 0, point: 0 },
                    { op: "wait", sec: 3 },
                    at("F_SP109"),
                    { op: "testRemap" },
                ],
            },
        });
    })(),
];

module.exports = [...observeScenarios, ...scenarios];
