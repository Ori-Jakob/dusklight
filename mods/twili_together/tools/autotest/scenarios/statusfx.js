// Status effects on remote players; South Faron lacks the ice block and sparks (ifAvailable).

const { STAGES, COMMON_CVARS, warp, waitStage, barrier, meetIn, connect } = require("../lib");

// The freeze costs a quarter heart every 45 ticks: keep the difficulty options from making it lethal.
const CVARS = [...COMMON_CVARS, "game.damageMultiplier=1", "game.instantDeath=false",
               "game.infiniteHearts=false"];
// dItemNo_WOOD_SHIELD_e, the armed start's shield, and dItemNo_NONE_e.
const WOOD_SHIELD = 0x2a;
const NO_SHIELD = 0xff;
// One press of B draws the sword, which puts the shield on the arm.
const drawSword = [{ op: "walk", frames: 1, stickX: 0, stickY: 0, buttons: 0x200 }, { op: "wait", frames: 45 }];
// Packs with the ice block (Snowpeak) and the sparks (Lakebed, room 0 starts under water).
const SNOWPEAK = { stage: "F_SP114", room: 1, point: 5 };
const LAKEBED = { stage: "D_MN01", room: 1, point: 0 };
const room = (name, value) => [
    { op: "setRoomOption", name, value },
    { op: "waitRoomOption", name, value, timeoutSec: 20 },
];
const spawnAs = (form, s) => [
    { op: "setForm", form },
    warp(s),
    waitStage(s),
    { op: "expectLocalForm", form },
];
const dummy = (fields) => ({ op: "expectDummyStatus", ...fields });
// A faces B's dummy; hold() marks where a window capture is worth taking.
const faceDummy = { op: "approachDummy", dist: 160 };
const hold = (what, frames = 40) => [{ op: "log", msg: `STATUSFX_HOLD ${what}` }, { op: "wait", frames }];

// meetIn without its walk, for a start the players must not walk away from.
const meetInPlace = (s, other) => [
    waitStage(s),
    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 90 },
    { op: "waitDummies", count: 1, timeoutSec: 60 },
    { op: "wait", frames: 30 },
    { op: "checkDummies", maxDist: 400 },
    ...barrier(`met-${s.stage}-${s.room}`, other),
];

// A and B meet in `start` and finish together; `pre(name)` runs before connecting.
const duo = (name, description, { a, b, start = STAGES.southFaron, timeoutSec = 300, pre = () => [], before = [], meet = meetIn }) => ({
    name,
    description,
    timeoutSec,
    cvars: CVARS,
    instances: [["A", "B", a], ["B", "A", b]].map(([self, other, steps]) => ({
        name: self,
        start,
        steps: [...pre(self), ...connect, ...before, ...meet(start, other), ...steps,
                ...barrier("done", other), { op: "quit" }],
    })),
});

module.exports = [
    duo("status-freeze", "B is frozen, then thaws: A's dummy is tinted (and iced where the pack has it), then shatters once; then B's damage flash", {
        a: [
            faceDummy,
            ...barrier("go", "B"),
            dummy({ frozen: true, iceBlock: "ifAvailable", statusFlags: 3, timeoutSec: 5 }),
            ...hold("frozen"),
            ...barrier("a-saw-frozen", "B"),
            dummy({ frozen: false, thaws: 1, iceBlock: false, timeoutSec: 15 }),
            ...barrier("b-thawed", "B"),
            dummy({ thaws: 1, frozen: false, frames: 30 }),
            ...barrier("hurt", "B"),
            dummy({ damageTimerMin: 10, flashesMin: 1, timeoutSec: 3 }),
            dummy({ damageTimerMax: 0, timeoutSec: 3 }),
        ],
        b: [
            { op: "setLife", value: 12 },
            ...barrier("go", "A"),
            { op: "forceStatus", kind: "freeze" },
            { op: "expectLocalStatus", frozen: true, iceBlock: true },
            ...barrier("a-saw-frozen", "A"),
            { op: "expectLocalStatus", frozen: false, timeoutSec: 10 },
            ...barrier("b-thawed", "A"),
            { op: "wait", frames: 30 },
            ...barrier("hurt", "A"),
            { op: "forceStatus", kind: "hurt" },
            { op: "expectLocalStatus", damageTimerMin: 20 },
            { op: "wait", frames: 60 },
        ],
    }),
    duo("status-burn", "B burns at four points: A's dummy burns at four; B's fire is put out, then burns and runs out on its own, and a local cutscene hides the flames", {
        before: room("hidePlayersInCutscene", true),
        a: [
            faceDummy,
            ...barrier("burn", "B"),
            dummy({ fireMin: 4, fireEmittersMin: 4, fireReceived: 4, timeoutSec: 5 }),
            ...hold("burning"),
            ...barrier("a-saw-fire", "B"),
            dummy({ fireMax: 0, fireReceived: 0, fireEmittersMax: 0, timeoutSec: 5 }),
            ...barrier("burn-2", "B"),
            dummy({ fireMin: 1, timeoutSec: 5 }),
            // Our cutscene hides the dummy and its flames, not B's fire.
            { op: "forceCutscene", on: true },
            dummy({ fireEmittersMax: 0, fireMax: 0, fireReceived: 4, timeoutSec: 2 }),
            { op: "forceCutscene", on: null },
            dummy({ fireMin: 1, fireEmittersMin: 1, timeoutSec: 3 }),
            // Out when B's flames run out, and not lit again every tick after ours did.
            dummy({ fireMax: 0, fireReceived: 0, timeoutSec: 20 }),
            dummy({ fireMax: 0, frames: 30 }),
        ],
        b: [
            ...barrier("burn", "A"),
            { op: "forceStatus", kind: "burn" },
            { op: "expectLocalStatus", firePoints: 4 },
            ...barrier("a-saw-fire", "A"),
            { op: "forceStatus", kind: "extinguish" },
            { op: "expectLocalStatus", firePoints: 0, timeoutSec: 1 },
            { op: "wait", frames: 15 },
            ...barrier("burn-2", "A"),
            { op: "forceStatus", kind: "burn" },
            { op: "expectLocalStatus", firePoints: 0, timeoutSec: 20 },
        ],
    }),
    duo("status-shield", "B's wooden shield burns in hand until it burns up: A's dummy chars it, then bursts once and loses it", {
        a: [
            faceDummy,
            ...barrier("burning", "B"),
            dummy({ shieldBurnMin: 1, shieldBurnFx: true, shieldItem: WOOD_SHIELD, timeoutSec: 5 }),
            { op: "log", msg: "STATUSFX_HOLD shield burning" },
            dummy({ shieldBurnOuts: 1, shieldItem: NO_SHIELD, shieldBurn: 0, shieldBurnFx: false, timeoutSec: 10 }),
            ...barrier("b-burnt", "B"),
            dummy({ shieldBurnOuts: 1, frames: 30 }),
        ],
        b: [
            ...drawSword,
            { op: "expectLocalStatus", shieldInHand: true, shield: WOOD_SHIELD },
            { op: "forceStatus", kind: "shieldBurn", value: 120 },
            ...barrier("burning", "A"),
            { op: "expectLocalStatus", shieldBurnMin: 1, timeoutSec: 1 },
            { op: "expectLocalStatus", shield: NO_SHIELD, timeoutSec: 8 },
            ...barrier("b-burnt", "A"),
        ],
    }),
    duo("status-shield-douse", "B's burning shield is put out before it burns up: no burst, A's dummy keeps the shield", {
        a: [
            ...barrier("burning", "B"),
            dummy({ shieldBurnMin: 1, shieldBurnFx: true, timeoutSec: 5 }),
            ...barrier("doused", "B"),
            dummy({ shieldBurn: 0, shieldBurnFx: false, timeoutSec: 3 }),
            dummy({ shieldBurn: 0, shieldBurnOuts: 0, shieldItem: WOOD_SHIELD, frames: 60 }),
        ],
        b: [
            ...drawSword,
            { op: "forceStatus", kind: "shieldBurn", value: 120 },
            ...barrier("burning", "A"),
            { op: "wait", frames: 20 },
            { op: "forceStatus", kind: "douse" },
            { op: "expectLocalStatus", shieldBurnMax: 0, shield: WOOD_SHIELD, timeoutSec: 1 },
            ...barrier("doused", "A"),
        ],
    }),
    duo("status-ice", "Snowpeak: B wades through snow, then is frozen: A's dummy sinks into the snow with it, is encased in the ice block, and shatters it once", {
        start: SNOWPEAK,
        meet: meetInPlace,
        a: [
            { op: "statusFxPack", iceBlock: true },
            // Link stays sunk in the snow once he stands still.
            ...barrier("b-sunk", "B"),
            dummy({ sinkMax: -10, timeoutSec: 3 }),
            ...barrier("a-saw-sink", "B"),
            faceDummy,
            // A step aside, so our Link does not stand between the camera and the dummy.
            { op: "walk", frames: 24, stickX: 1, stickY: 0 },
            ...barrier("go", "B"),
            dummy({ frozen: true, iceBlock: true, statusFlags: 3, timeoutSec: 5 }),
            { op: "log", msg: "STATUSFX_HOLD iced" },
            dummy({ frozen: true, iceBlock: true, frames: 45 }),
            dummy({ frozen: false, thaws: 1, iceBlock: false, timeoutSec: 15 }),
        ],
        b: [
            { op: "setLife", value: 12 },
            { op: "walk", frames: 90, circle: true },
            { op: "expectLocalStatus", sinkMax: -10 },
            ...barrier("b-sunk", "A"),
            ...barrier("a-saw-sink", "A"),
            { op: "wait", frames: 30 },
            ...barrier("go", "A"),
            { op: "forceStatus", kind: "freeze" },
            { op: "expectLocalStatus", frozen: true, iceBlock: true },
            { op: "expectLocalStatus", frozen: false, timeoutSec: 10 },
        ],
    }),
    duo("status-elec", "Lakebed: B is electrocuted (a compulsory event): A's dummy shows the sparks and is not hidden as in a cutscene", {
        start: LAKEBED,
        before: room("hidePlayersInCutscene", true),
        a: [
            { op: "statusFxPack", elec: true },
            ...barrier("go", "B"),
            dummy({ elec: true, elecFx: true, statusFlags: 4, timeoutSec: 3 }),
            { op: "expectDummyHidden", hidden: false },
            dummy({ elec: false, statusFlags: 0, timeoutSec: 5 }),
        ],
        b: [
            ...barrier("go", "A"),
            { op: "forceStatus", kind: "elec" },
            { op: "expectLocalStatus", elec: true },
            { op: "expectLocalStatus", elec: false, timeoutSec: 5 },
        ],
    }),
    duo("status-synthetic", "patched sx/sf: every status field decodes and shows, a joint the body lacks is refused, junk is clamped, and a burn-out needs a burning tick before it", {
        a: [
            ...barrier("patched", "B"),
            // Frozen + ice + elec, shield burn 60, flash 20, chill 10, sink -25; fire on joints 7 and 63.
            dummy({ frozen: true, iceBlock: "ifAvailable", elec: true, shieldBurnMin: 60, shieldBurnMax: 60,
                    damageTimerMin: 20, damageTimerMax: 20, iceWait: 10, sinkMin: -25.5, sinkMax: -24.5,
                    fireReceived: 2, fireMin: 1, fireMax: 1, statusFlags: 7, timeoutSec: 5 }),
            ...barrier("a-saw-patch", "B"),
            dummy({ frozen: false, elec: false, shieldBurn: 0, damageTimerMax: 0, iceWait: 0, sinkMin: 0, sinkMax: 0,
                    fireReceived: 0, fireMax: 0, statusFlags: 0, shieldBurnOuts: 0, timeoutSec: 5 }),
            ...barrier("fuzz", "B"),
            // 255 keeps the four wire bits: frozen, ice block, elec and the drained Magic Armor.
            dummy({ statusFlags: 15, shieldBurnMax: 120, shieldBurnMin: 120, sinkMin: -256, sinkMax: 256, iceWait: 255, timeoutSec: 5 }),
            ...barrier("a-saw-fuzz", "B"),
            dummy({ statusFlags: 0, shieldBurn: 0, timeoutSec: 5 }),
            ...barrier("burnout", "B"),
            dummy({ shieldBurnMin: 60, timeoutSec: 5 }),
            dummy({ shieldBurnOuts: 1, shieldBurn: 0, timeoutSec: 5 }),
            ...barrier("a-saw-burnout", "B"),
            // The sequence going back to 0 without a burning tick first bursts nothing.
            dummy({ shieldBurnOuts: 1, frames: 60 }),
        ],
        b: [
            ...barrier("patched", "A"),
            { op: "patchPlayerUpdate", packets: 90, patch: { sx: [7, 60, 2068, 10, -200], sf: [15, 62981, 127, 0, 0, 0, 0, 0] } },
            ...barrier("a-saw-patch", "A"),
            { op: "wait", frames: 90 },
            ...barrier("fuzz", "A"),
            { op: "patchPlayerUpdate", packets: 60, patch: { sx: [255, 999, -1, 70000, 5000], sf: [-1, -1, 3, 2147483647, 0, 0, 1, 0] } },
            ...barrier("a-saw-fuzz", "A"),
            { op: "wait", frames: 60 },
            ...barrier("burnout", "A"),
            // The burn-out patch takes over while the burning one still runs.
            { op: "patchPlayerUpdate", packets: 30, patch: { sx: [0, 60, 0, 0, 0] } },
            { op: "wait", frames: 20 },
            { op: "patchPlayerUpdate", packets: 30, patch: { sx: [256, 0, 0, 0, 0] } },
            { op: "wait", frames: 30 },
            ...barrier("a-saw-burnout", "A"),
            { op: "wait", frames: 60 },
        ],
    }),
    {
        name: "status-leave",
        description: "B leaves the stage while it burns: A's dummy goes mid-burn without a crash",
        timeoutSec: 300,
        cvars: CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("burn", "B"),
                    dummy({ fireMin: 4, fireEmittersMin: 4, timeoutSec: 5 }),
                    ...barrier("a-saw-fire", "B"),
                    { op: "waitNoDummies", timeoutSec: 60 },
                    { op: "wait", frames: 90 },
                    { op: "walk", frames: 60, circle: true },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...barrier("burn", "A"),
                    { op: "forceStatus", kind: "burn" },
                    ...barrier("a-saw-fire", "A"),
                    warp(STAGES.linksHouse),
                    waitStage(STAGES.linksHouse),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "status-latejoin",
        description: "A's dummy of B is created while B is frozen and past a shield burn-out: frozen at once, no stale burst, then one shatter",
        timeoutSec: 360,
        cvars: CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.linksHouse,
                steps: [
                    ...connect,
                    waitStage(STAGES.linksHouse),
                    ...barrier("b-frozen", "B"),
                    warp(STAGES.southFaron),
                    waitStage(STAGES.southFaron),
                    { op: "waitDummies", count: 1, timeoutSec: 60 },
                    dummy({ frozen: true, thaws: 0, shieldBurnOuts: 0, statusFlags: 1, timeoutSec: 10 }),
                    dummy({ frozen: false, thaws: 1, shieldBurnOuts: 0, timeoutSec: 30 }),
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    waitStage(STAGES.southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    // Frozen (no ice block) and burn-out sequence 1 for 15 s.
                    { op: "patchPlayerUpdate", packets: 450, patch: { sx: [257, 0, 0, 0, 0] } },
                    ...barrier("b-frozen", "A"),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    duo("status-wolf", "B as a wolf is frozen, then burns: A's wolf dummy is tinted, and burns at the wolf's four points", {
        pre: (self) => [waitStage(STAGES.southFaron), ...spawnAs(self === "B" ? "wolf" : "human", STAGES.southFaron)],
        a: [
            { op: "expectRemoteForm", form: "wolf", timeoutSec: 10 },
            ...barrier("go", "B"),
            dummy({ frozen: true, iceBlock: "ifAvailable", statusFlags: 3, timeoutSec: 5 }),
            dummy({ frozen: false, thaws: 1, timeoutSec: 15 }),
            ...barrier("burn", "B"),
            dummy({ fireMin: 4, fireEmittersMin: 4, timeoutSec: 5 }),
        ],
        b: [
            { op: "setLife", value: 12 },
            ...barrier("go", "A"),
            { op: "forceStatus", kind: "freeze" },
            { op: "expectLocalStatus", frozen: true, iceBlock: true },
            { op: "expectLocalStatus", frozen: false, timeoutSec: 10 },
            { op: "wait", frames: 30 },
            ...barrier("burn", "A"),
            { op: "forceStatus", kind: "burn" },
            { op: "expectLocalStatus", firePoints: 4 },
            { op: "wait", frames: 30 },
        ],
    }),
];
