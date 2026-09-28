// How remote players look.

const { STAGES, ITEMS, COMMON_CVARS, tt, waitStage, barrier, meetIn, connect } = require("../lib");

// dItemNo_* (include/d/d_item_data.h).
const CLOTHES = { casual: 0x2e, kokiri: 0x2f, armor: 0x30, zora: 0x31 };
// The idle loop a dummy falls back to once it has held a missing clip for 20 ticks.
const WAITS = 0x26a;
const PAD_X = 0x400;
// Ordon clothes first: their head has 6 joints instead of 8 (setHatAngle).
const WEAR_ORDER = ["casual", "zora", "armor", "kokiri"];

// Clip ids a receiver cannot show: 0x1006 is in alSumou (sumo stages only), the others exist nowhere.
const BOGUS_CLIPS = {
    la: [0x1006, 0x2800, 0x6e00],
    ua: [0x3100, 0x6f01, 0x0fff],
    lr: [1024, 512, 512],
};

module.exports = [
    {
        name: "fidelity-clothes",
        description: "B changes into each set of clothes and puts iron boots on and off; A's dummy of B follows",
        timeoutSec: 480,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    {
                        op: "expectDummyLook",
                        clothes: CLOTHES.kokiri,
                        casualHead: false,
                        basePack: true,
                        ground: true,
                        timeoutSec: 20,
                    },
                    ...barrier("look-start", "B"),
                    ...WEAR_ORDER.flatMap((name) => [
                        { op: "waitSignal", name: `wear-${name}`, from: "B", timeoutSec: 60 },
                        // B walks meanwhile, so the hat swings on whichever head it has.
                        {
                            op: "expectDummyLook",
                            clothes: CLOTHES[name],
                            casualHead: name === "casual",
                            zoraMask: false,
                            basePack: true,
                            frames: 90,
                            timeoutSec: 30,
                        },
                        ...barrier(`saw-${name}`, "B"),
                    ]),
                    { op: "waitSignal", name: "boots-on", from: "B", timeoutSec: 60 },
                    { op: "expectDummyLook", heavyBoots: true, frames: 30, timeoutSec: 20 },
                    ...barrier("saw-boots-on", "B"),
                    { op: "waitSignal", name: "boots-off", from: "B", timeoutSec: 60 },
                    { op: "expectDummyLook", heavyBoots: false, frames: 30, timeoutSec: 20 },
                    ...barrier("saw-boots-off", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...barrier("look-start", "A"),
                    ...WEAR_ORDER.flatMap((name) => [
                        { op: "setClothes", item: CLOTHES[name], timeoutSec: 30 },
                        { op: "signal", name: `wear-${name}` },
                        { op: "walk", frames: 150, circle: true },
                        ...barrier(`saw-${name}`, "A"),
                    ]),
                    { op: "setBoots", on: true },
                    { op: "signal", name: "boots-on" },
                    { op: "walk", frames: 90, circle: true },
                    ...barrier("saw-boots-on", "A"),
                    { op: "setBoots", on: false },
                    { op: "signal", name: "boots-off" },
                    ...barrier("saw-boots-off", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "fidelity-anim-fuzz",
        description: "B sends clips A cannot load and garbage gear for 5 s; A's dummy keeps a pose on a stand-in clip, then returns to B's",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "waitSignal", name: "fuzz", from: "B", timeoutSec: 60 },
                    // The last good clip, then after 20 ticks the idle loop; the boots bit rides in "vf".
                    {
                        op: "expectDummyLook",
                        basePack: true,
                        basePackAnm: WAITS,
                        standIn: true,
                        heavyBoots: true,
                        clothes: CLOTHES.kokiri,
                        frames: 60,
                        timeoutSec: 10,
                    },
                    { op: "expectDummyLook", basePack: true, standIn: false, heavyBoots: false, timeoutSec: 20 },
                    { op: "checkDummies", maxDist: 400 },
                    ...barrier("fuzz-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "walk", frames: 240, circle: true, async: true },
                    {
                        op: "patchPlayerUpdate",
                        packets: 150,
                        patch: {
                            ...BOGUS_CLIPS,
                            vf: 1,
                            eq: [255, 255, 255, 65535],
                            hi: [255, 255, 255, 255, 255, 255],
                            ij: [999, 999],
                            ib: [65535, -16],
                        },
                    },
                    { op: "signal", name: "fuzz" },
                    ...barrier("fuzz-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "fidelity-boomerang",
        description: "B throws the boomerang twice while A's dummy of B plays the throw",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("throwing", "B"),
                    ...barrier("thrown", "B", 120),
                    { op: "checkDummies", maxDist: 400 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "giveItem", item: ITEMS.boomerang },
                    { op: "assignItemX", item: ITEMS.boomerang },
                    ...barrier("throwing", "A"),
                    // The equip item drops to none mid-throw while the throw clip plays on.
                    ...[1, 2].flatMap(() => [
                        { op: "walk", frames: 8, stickX: 0, stickY: 0, buttons: PAD_X },
                        { op: "wait", frames: 90 },
                    ]),
                    ...barrier("thrown", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        // Regression: Ordon clothes -> Zora armor crashed in initModel (freed archive heap).
        name: "clothes-solo",
        description: "the local player changes clothes repeatedly with no peer (port crash regression)",
        timeoutSec: 400,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                start: STAGES.southFaron,
                steps: [
                    waitStage(STAGES.southFaron),
                    ...[0, 1, 2].flatMap(() => WEAR_ORDER.flatMap((name) => [
                        { op: "setClothes", item: CLOTHES[name], timeoutSec: 30 },
                        { op: "wait", frames: 150 },
                    ])),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        // Captures (manual): both windows at each "shot-<what>" signal, sender B beside receiver A.
        name: "fidelity-showcase",
        description: "B in red shows a recoloured tunic, a boomerang gust, burning, a transformation and Midna on its wolf; A watches its dummy of B",
        manualOnly: true,
        timeoutSec: 400,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("apart", "B"),
                    { op: "recolorCamera", target: "dummy" },
                    ...barrier("shot-tunic", "B"),
                    { op: "wait", sec: 4 },
                    { op: "viewDummy", back: 120, side: 60, up: 260 },
                    ...barrier("shot-boomerang", "B"),
                    { op: "wait", sec: 4 },
                    { op: "recolorCamera", target: "dummy", dist: 220, up: 60 },
                    ...barrier("shot-burning", "B"),
                    { op: "wait", sec: 4 },
                    { op: "recolorCamera", target: "dummy", dist: 260, up: 60 },
                    ...barrier("shot-transform", "B"),
                    { op: "wait", sec: 6 },
                    { op: "recolorCamera", target: "dummy", dist: -160, side: -120, height: 60, up: 60 },
                    ...barrier("shot-midna", "B"),
                    { op: "wait", sec: 4 },
                    ...barrier("shots-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                cvars: [tt("color", "e53935")],
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "walk", frames: 40, stickY: 1 },
                    { op: "wait", frames: 30 },
                    { op: "giveItem", item: ITEMS.boomerang },
                    { op: "assignItemX", item: ITEMS.boomerang },
                    ...barrier("apart", "A"),
                    { op: "recolorCamera", target: "self" },
                    { op: "wait", frames: 20 },
                    ...barrier("shot-tunic", "A"),
                    { op: "wait", sec: 4 },
                    { op: "viewDummy", back: 200, up: 200 },
                    { op: "wait", frames: 20 },
                    { op: "walk", frames: 8, stickX: 0, stickY: 0, buttons: PAD_X },
                    { op: "wait", frames: 6 },
                    ...barrier("shot-boomerang", "A"),
                    { op: "wait", sec: 4 },
                    { op: "forceStatus", kind: "burn" },
                    { op: "recolorCamera", target: "self", dist: 220, up: 60 },
                    { op: "wait", frames: 20 },
                    ...barrier("shot-burning", "A"),
                    { op: "wait", sec: 4 },
                    { op: "forceStatus", kind: "extinguish" },
                    { op: "recolorCamera", target: "self", dist: 260, up: 60 },
                    ...barrier("shot-transform", "A"),
                    { op: "transform", form: "wolf", timeoutSec: 30 },
                    { op: "wait", sec: 2 },
                    { op: "setMidna", ride: true, visible: true },
                    { op: "recolorCamera", target: "self", dist: -160, side: -120, height: 60, up: 60 },
                    { op: "wait", frames: 30 },
                    ...barrier("shot-midna", "A"),
                    { op: "wait", sec: 4 },
                    ...barrier("shots-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
