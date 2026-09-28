// The remote player recolour (src/fx/PlayerRecolor.cpp): a dummy's tunic-coloured texture blocks
// (tunic and hat, Zora blue, Magic Armor red, Ordon tunic, wolf tail and ankle chain) shown in its
// player's colour. coloring-armor-power covers the Magic Armor's drained look (StatusFx).
// Scenarios log COLORING_HOLD <what> (on A) where a window capture is worth taking.

const { STAGES, COMMON_CVARS, tt, barrier, meetIn, connect } = require("../lib");

const RED = "#E53935";
const BLUE = "#1E88E5";
const colorCvars = (hex) => [tt("color", hex.slice(1).toLowerCase())];
// dItemNo_*.
const CLOTHES = { casual: 0x2e, kokiri: 0x2f, armor: 0x30, zora: 0x31 };
const SET = { casual: "ordon", kokiri: "kokiri", armor: "magic", zora: "zora", wolf: "wolf" };
// Ordon clothes first: their head has 6 joints instead of 8 (fidelity-clothes' order).
const ORDER = ["casual", "zora", "armor", "kokiri", "wolf"];
// Vanilla draws the wolf's dangling chain only while this event bit is set in the local save.
const M_011 = 0x0510;
// The dummy's recolour stores of every set (about 0x19C00) next to its 0xC00000 heap.
const MAX_STORE_BYTES = 0x20000;
// Close on the peer's dummy: from the front, or a wolf from behind on its left (tail, shackle).
const CAMERA = { dist: 150, height: 75, up: 25 };
const WOLF_CAMERA = { dist: -110, side: -130, height: 45, up: 50 };
const hold = (what, sec = 3, camera = CAMERA) => [
    { op: "recolorCamera", target: "dummy", ...camera },
    { op: "wait", frames: 10 },
    { op: "log", msg: `COLORING_HOLD ${what}` },
    { op: "wait", sec },
];
// meetIn leaves both on the same spot: B steps away so A's Link does not hide the dummy.
const stepAway = [{ op: "walk", frames: 40, stickY: 1 }, { op: "wait", frames: 30 }];
const wear = (n) => (n === "wolf"
    ? [{ op: "transform", form: "wolf", timeoutSec: 30 }]
    : [{ op: "setClothes", item: CLOTHES[n], timeoutSec: 30 }]);

// B wears each set in red, blue and exact white (as on disc); A checks its dummy of B and holds a
// close-up of each. A wears the same clothes (not the wolf) and sets M_011 for the wolf's chain.
const COLORS = [["red", RED], ["blue", BLUE], ["white", "#FFFFFF"]];
const setsA = ORDER.flatMap((n) => [
    ...(n === "wolf" ? [{ op: "setEventBit", no: M_011 }] : wear(n)),
    ...COLORS.flatMap(([c, rgb], i) => [
        { op: "waitSignal", name: `${c}-${n}`, from: "B", timeoutSec: 90 },
        c === "white"
            ? { op: "expectDummyRecolor", set: SET[n], vanilla: true, frames: 30, timeoutSec: 10 }
            : { op: "expectDummyRecolor", set: SET[n], rgb, maxBytes: MAX_STORE_BYTES, frames: 30,
                timeoutSec: i === 0 ? 30 : 10 },
        ...hold(`${n}-${c}`, 3, n === "wolf" ? WOLF_CAMERA : CAMERA),
        ...barrier(`saw-${c}-${n}`, "B"),
    ]),
]);
const setsB = ORDER.flatMap((n) => [
    ...wear(n),
    ...COLORS.flatMap(([c, rgb]) => [
        { op: "setColor", rgb },
        { op: "wait", frames: 30 },
        { op: "signal", name: `${c}-${n}` },
        ...barrier(`saw-${c}-${n}`, "A"),
    ]),
]);

// Black keeps the folds, grey has no hue, #F5F5F5 is a white tunic, exact white is as on disc.
const GREYS = ["#000000", "#808080", "#F5F5F5"];
const greysA = ["kokiri", "zora"].flatMap((n) => [
    ...GREYS.flatMap((g, i) => [
        { op: "waitSignal", name: `grey-${n}-${i}`, from: "B", timeoutSec: 60 },
        { op: "expectDummyRecolor", set: SET[n], rgb: g, frames: 15, timeoutSec: 10 },
        ...(i === 0 ? hold(`${n}-black`, 2) : []),
        ...barrier(`saw-grey-${n}-${i}`, "B"),
    ]),
    { op: "waitSignal", name: `white-${n}`, from: "B", timeoutSec: 60 },
    { op: "expectDummyRecolor", set: SET[n], vanilla: true, frames: 15, timeoutSec: 10 },
    ...barrier(`saw-white-${n}`, "B"),
]);
const greysB = ["kokiri", "zora"].flatMap((n) => [
    { op: "setClothes", item: CLOTHES[n], timeoutSec: 30 },
    ...GREYS.flatMap((g, i) => [
        { op: "setColor", rgb: g },
        { op: "signal", name: `grey-${n}-${i}` },
        ...barrier(`saw-grey-${n}-${i}`, "A"),
    ]),
    { op: "setColor", rgb: "#FFFFFF" },
    { op: "signal", name: `white-${n}` },
    ...barrier(`saw-white-${n}`, "A"),
]);

// B's Magic Armor runs out of rupees and gets some back; A wears it powered beside B's dummy.
const armorA = [
    ...barrier("apart", "B"),
    { op: "setRupees", value: 300 },
    { op: "setClothes", item: CLOTHES.armor, timeoutSec: 30 },
    { op: "waitSignal", name: "armor-on", from: "B", timeoutSec: 90 },
    { op: "expectDummyLook", clothes: CLOTHES.armor, armorDrained: false, armorSettled: true,
      frames: 15, timeoutSec: 20 },
    { op: "expectDummyRecolor", set: "magic", rgb: RED, frames: 15 },
    ...hold("armor-powered"),
    ...barrier("saw-powered", "B"),
    { op: "waitSignal", name: "drained", from: "B", timeoutSec: 60 },
    // power_down fades the gold over 30 ticks; the red cloth stays recoloured under it.
    { op: "expectDummyLook", armorDrained: true, armorSettled: true, frames: 15, timeoutSec: 10 },
    { op: "expectDummyRecolor", set: "magic", rgb: RED, c1Min: 10, c1Max: 40, frames: 15 },
    ...hold("armor-drained"),
    ...barrier("saw-drained", "B"),
    // A dummy created while the armor is drained starts drained, without the fade.
    { op: "disconnect" },
    { op: "wait", sec: 2 },
    ...connect,
    { op: "waitDummies", count: 1, timeoutSec: 60 },
    { op: "expectDummyLook", armorDrained: true, armorSettled: true, frames: 5, timeoutSec: 20 },
    ...barrier("rejoined", "B"),
    { op: "waitSignal", name: "powered", from: "B", timeoutSec: 60 },
    // power_up_a flashes for 70 ticks and ends at no offset.
    { op: "expectDummyLook", armorDrained: false, armorSettled: true, frames: 15, timeoutSec: 10 },
    { op: "expectDummyRecolor", set: "magic", rgb: RED, frames: 15, timeoutSec: 10 },
    ...hold("armor-repowered"),
    ...barrier("power-done", "B"),
];
// B's own window: its Link from behind, A's dummy ahead (the sender's side of each hold).
const senderView = { op: "viewDummy", back: 160, up: 120 };
const armorB = [
    ...stepAway,
    ...barrier("apart", "A"),
    { op: "setRupees", value: 300 },
    { op: "setClothes", item: CLOTHES.armor, timeoutSec: 30 },
    { op: "expectLocalStatus", armorDrained: false },
    { op: "signal", name: "armor-on" },
    senderView,
    ...barrier("saw-powered", "A"),
    { op: "setRupees", value: 0 },
    { op: "wait", frames: 2 },
    { op: "expectLocalStatus", armorDrained: true, timeoutSec: 2 },
    { op: "signal", name: "drained" },
    senderView,
    ...barrier("saw-drained", "A"),
    ...barrier("rejoined", "A"),
    { op: "setRupees", value: 300 },
    { op: "wait", frames: 2 },
    { op: "expectLocalStatus", armorDrained: false, timeoutSec: 2 },
    { op: "signal", name: "powered" },
    senderView,
    ...barrier("power-done", "A"),
];

// Six changes in two seconds: the rate limit coalesces but never drops the newest.
const BURST = ["#FDD835", "#8E24AA", "#43A047", "#FB8C00", "#00ACC1", "#D81B60"];

module.exports = [
    {
        name: "coloring-armor-power",
        description: "B in red Magic Armor runs out of rupees: A's dummy of B fades to the drained gold with the red cloth still recoloured (StatusFx kStatusArmorDrained, not A's rupees), a dummy created meanwhile starts drained, and rupees power it back up",
        timeoutSec: 480,
        cvars: [...COMMON_CVARS, "game.armorRupeeDrain=0"],
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [...connect, ...meetIn(STAGES.southFaron, "B"), ...armorA, { op: "quit" }],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                cvars: colorCvars(RED),
                steps: [...connect, ...meetIn(STAGES.southFaron, "A"), ...armorB, { op: "quit" }],
            },
        ],
    },
    {
        name: "coloring-sets",
        description: "B wears every set of clothes and turns wolf, in red, blue and exact white; A's dummy of B shows the colour only in the tunic-coloured blocks (tunic and hat, Zora blue, Magic Armor red, Ordon tunic, wolf tail and ankle chain), the rest byte-identical, no C1 tint, and white puts the bytes back as on disc; B sees white A as vanilla",
        timeoutSec: 720,
        // INVINCIBLE: the armor stays powered with no rupees (C1 holds no drained-gold offset).
        cvars: [...COMMON_CVARS, "game.armorRupeeDrain=3"],
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    { op: "recolorSelfTest" },
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "expectDummyRecolor", set: "kokiri", rgb: RED, maxBytes: MAX_STORE_BYTES,
                      frames: 30 },
                    ...barrier("colors-start", "B"),
                    ...setsA,
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                cvars: colorCvars(RED),
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "expectDummyRecolor", set: "kokiri", vanilla: true, frames: 30 },
                    ...stepAway,
                    ...barrier("colors-start", "A"),
                    ...setsB,
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "coloring-grey",
        description: "B turns black, mid grey and #F5F5F5, then exact white, in hero's clothes and the Zora armour; A's dummy of B follows the grey rule and returns to the bytes as on disc for white",
        timeoutSec: 480,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("apart", "B"),
                    ...greysA,
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                cvars: colorCvars(BLUE),
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...stepAway,
                    ...barrier("apart", "A"),
                    ...greysB,
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "coloring-live",
        description: "B changes colour mid-session as the picker sends it: A's dummy textures follow at once, a burst of six coalesces to the last one, and 200 changes in a row leave A's GPU and private memory flat",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "expectDummyRecolor", set: "kokiri", rgb: BLUE, frames: 15 },
                    ...barrier("apart", "B"),
                    ...hold("live-before", 2),
                    ...barrier("live-start", "B"),
                    { op: "waitSignal", name: "live-red", from: "B", timeoutSec: 30 },
                    { op: "expectDummyRecolor", set: "kokiri", rgb: RED, timeoutSec: 3 },
                    ...hold("live-after", 2),
                    { op: "markRecolor" },
                    ...barrier("burst-start", "B"),
                    { op: "waitSignal", name: "burst-done", from: "B", timeoutSec: 30 },
                    { op: "expectDummyRecolor", set: "kokiri", rgb: BURST[BURST.length - 1],
                      timeoutSec: 3 },
                    { op: "expectRecolorDelta", minApplies: 1, maxApplies: 7 },
                    // 200 changes with the dummy on screen, so aurora decodes each one again.
                    ...barrier("warmup-start", "B"),
                    { op: "waitSignal", name: "warmup-done", from: "B", timeoutSec: 60 },
                    { op: "wait", sec: 1 },
                    { op: "markRecolor" },
                    ...barrier("cycle-start", "B"),
                    // Keep the camera on the dummy while B cycles (about 53 s).
                    ...[0, 1, 2, 3, 4].flatMap((i) => [
                        { op: "recolorCamera", target: "dummy" },
                        { op: "wait", sec: 8 },
                        { op: "log", msg: `COLORING_HOLD cycle-${i}` },
                    ]),
                    { op: "waitSignal", name: "cycle-done", from: "B", timeoutSec: 180 },
                    { op: "expectDummyRecolor", set: "kokiri", peerColor: true, timeoutSec: 3 },
                    { op: "wait", sec: 2 },
                    // aurora caches up to 128 MiB of decoded textures, a new entry per colour.
                    { op: "expectRecolorDelta", minApplies: 150, maxApplies: 201, minDraws: 300,
                      maxGpuGrowthMB: 192, maxPrivateGrowthMB: 192 },
                    ...barrier("live-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                cvars: colorCvars(BLUE),
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...stepAway,
                    ...barrier("apart", "A"),
                    ...barrier("live-start", "A"),
                    { op: "setColor", rgb: RED },
                    { op: "signal", name: "live-red" },
                    ...barrier("burst-start", "A"),
                    ...BURST.flatMap((c) => [{ op: "setColor", rgb: c }, { op: "wait", frames: 10 }]),
                    { op: "signal", name: "burst-done" },
                    ...barrier("warmup-start", "A"),
                    { op: "colorCycle", count: 20, everyTicks: 8 },
                    { op: "signal", name: "warmup-done" },
                    ...barrier("cycle-start", "A"),
                    { op: "colorCycle", count: 200, everyTicks: 8 },
                    { op: "signal", name: "cycle-done" },
                    ...barrier("live-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
