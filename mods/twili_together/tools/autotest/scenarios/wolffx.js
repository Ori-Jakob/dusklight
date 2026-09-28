// A remote wolf's attack effects; wolfDome forces the tired light-world Midna's dome open.

const { STAGES, COMMON_CVARS, tt, warp, waitStage, barrier, meetIn, connect } = require("../lib");

const CVARS = [...COMMON_CVARS, "game.infiniteHearts=1"];
// Bokoblin type 0 without switches.
const BOKOBLIN = { name: "E_oc", param: 0xffffff00, dy: 30 };
const hideInCutscenes = [...CVARS, tt("hide_players_in_cutscene", 1)];
const B_BUTTON = 0x200;
// B held with the stick at rest: the wolf crouches into the charge and attacks when B lifts.
const holdB = (frames, async = false) => ({ op: "walk", frames, stickX: 0, stickY: 0, buttons: B_BUTTON, async });
const fx = (fields) => ({ op: "expectRemoteWolfFx", ...fields });
const patch = (p, packets) => ({ op: "patchPlayerUpdate", packets, patch: p });
// One empty patch ends the others; the packet after it puts every field back.
const unpatch = patch({}, 1);
// A stands in front of B's dummy, facing it.
const faceDummy = (dist = 300) => ({ op: "approachDummy", dist });
const hold = (what, frames = 45) => [{ op: "log", msg: `WOLFFX_HOLD ${what}` }, { op: "wait", frames }];
// The camera behind A, looking at B's dummy (only for the captures).
const cameraBehind = (view = {}) => [{ op: "viewDummy", ...view }, { op: "wait", frames: 10 }];

// A and B meet in `start`, B as a wolf, run their steps and finish together.
const duo = (name, description, { a, b, start = STAGES.southFaron, timeoutSec = 360, cvars = CVARS, bForm = "wolf" }) => ({
    name,
    description,
    timeoutSec,
    cvars,
    instances: [["A", "B", a], ["B", "A", b]].map(([self, other, steps]) => ({
        name: self,
        start,
        steps: [
            ...connect,
            ...meetIn(start, other),
            ...(self === "B" && bForm === "wolf" ? [{ op: "transform", form: "wolf", timeoutSec: 30 }] : []),
            ...barrier("ready", other),
            ...steps,
            ...barrier("done", other),
            { op: "quit" },
        ],
    })),
});

// md for a drawn Midna under the dome: WAITA, pointing upper clip and hair hand, hands 0/2.
const POINTING_MIDNA = {
    md: [1 | (2 << 2) | (0x8 << 4), 0x1dc, 0x1df, 0, 0x3f8, 0x399, 0 | (2 << 8)],
    mf: [0, 192, 0],
    ma: [0, 0, 0, 0, 0],
};

module.exports = [
    duo("wolf-spin-fx", "B spins right, left without a trail and from a released charge: A's dummy of B shows each spin's emitters on B's ticks and stops them after", {
        a: [
            // Where meetIn left A: the way B faces may lead into the void here.
            ...cameraBehind({ side: 150 }),
            ...barrier("spin-right", "B"),
            { op: "expectPeerWolfFx", spin: "right", timeoutSec: 5 },
            fx({ minSpinTicks: 20, lastSpin: "right", spinEmitters: 2, dome: false }),
            fx({ spin: "none", timeoutSec: 5 }),
            ...barrier("spin-left", "B"),
            fx({ minSpinTicks: 20, lastSpin: "left", spinEmitters: 1 }),
            fx({ spin: "none", timeoutSec: 5 }),
            ...barrier("spin-input", "B"),
            fx({ minSpinTicks: 20, lastSpin: "right", spinEmitters: 2, timeoutSec: 10 }),
            fx({ spin: "none", timeoutSec: 5 }),
            ...barrier("spin-shots", "B"),
            ...hold("spin", 150),
            ...barrier("spin-shots-done", "B"),
            { op: "expectCleanAnims", frames: 60 },
        ],
        b: [
            ...barrier("spin-right", "A"),
            { op: "wolfSpin", dir: "right" },
            { op: "wait", frames: 60 },
            ...barrier("spin-left", "A"),
            { op: "wolfSpin", dir: "left", trail: false },
            { op: "wait", frames: 60 },
            ...barrier("spin-input", "A"),
            // A real charge released after 40 ticks: no Midna, so no dome and a right spin.
            holdB(40),
            { op: "expectLocalWolfFx", spin: "right", timeoutSec: 3 },
            { op: "wait", frames: 60 },
            ...barrier("spin-shots", "A"),
            ...[0, 1, 2, 3].flatMap(() => [{ op: "wolfSpin", dir: "right" }, { op: "wait", frames: 36 }]),
            ...barrier("spin-shots-done", "A"),
        ],
    }),
    duo("wolf-dome-fx", "B holds B with Midna on its back: A's dummy of B shows her dome grow to full size and Midna pointing, then the release spin without the dome", {
        a: [
            ...barrier("midna-on", "B"),
            // Where meetIn left A: the way B faces may lead into the void here.
            ...cameraBehind({ back: 350, side: 200, up: 300 }),
            ...barrier("dome-up", "B"),
            fx({ dome: true, domeShown: true, minRadius: 540, maxRadius: 551, frames: 10, timeoutSec: 5 }),
            ...hold("dome", 15),
            { op: "expectRemoteMidna", mode: "drawn", upper: 0x1df, hairHand: 1, timeoutSec: 3 },
            fx({ dome: false, minSpinTicks: 10, lastSpin: "right", timeoutSec: 10 }),
        ],
        b: [
            { op: "setMidna", ride: true, visible: true },
            ...barrier("midna-on", "A"),
            holdB(260, true),
            { op: "wolfDome", force: true, radius: 550 },
            { op: "expectLocalWolfFx", dome: true, charge: true },
            { op: "signal", name: "dome-up" },
            // The dome releases on its own after 120 ticks, with no target into the spin.
            { op: "expectLocalWolfFx", spin: "right", dome: false, timeoutSec: 8 },
            { op: "wait", frames: 150 },
        ],
    }),
    // The Bokoblin comes at once and a hit ends the charge: spawn it inside the full dome.
    duo("wolf-lock-real", "B's dome locks onto a Bokoblin and B lets go of B: A's dummy of B shows Midna pointing her hair hand at it, then the lock-on jump's burst and tail blur", {
        start: STAGES.forestTemple,
        a: [
            faceDummy(),
            ...cameraBehind({ side: 200 }),
            ...barrier("armed", "B"),
            { op: "waitSignal", name: "lock-dome", from: "B", timeoutSec: 30 },
            fx({ dome: true, domeShown: true, hairAim: true, timeoutSec: 2 }),
            { op: "expectRemoteMidna", mode: "drawn", upper: 0x1df, hairHand: 2, timeoutSec: 2 },
            { op: "log", msg: "WOLFFX_HOLD lock" },
            fx({ minLockDashes: 1, maxLockDashes: 1, lockBlur: true, timeoutSec: 8 }),
            fx({ dome: false, lockBlur: false, hairAim: false, frames: 10, timeoutSec: 8 }),
        ],
        b: [
            { op: "setMidna", ride: true, visible: true },
            ...barrier("armed", "A"),
            holdB(300, true),
            { op: "wolfDome", force: true, radius: 550 },
            // Inside the full dome: the search catches it on its first tick.
            { op: "spawnEnemy", ...BOKOBLIN, dz: 300, tag: "boko" },
            { op: "expectEnemyHealth", tag: "boko" },
            { op: "wolfDome", force: true, radius: 0, minLocks: 1, timeoutSec: 3 },
            { op: "signal", name: "lock-dome" },
            { op: "wait", frames: 30 },
            // Lets go of B: with a target the wolf jumps at it.
            { op: "walk", frames: 1, stickX: 0, stickY: 0, buttons: 0 },
            { op: "wait", frames: 90 },
        ],
    }),
    duo("wolf-fx-synthetic", "B feeds A wolf fx its game does not make here: a spin and dome on a human body, three lock-on jumps, Midna's hair aim, a dome alone and junk; A's dummy shows them and stops when they stop", {
        bForm: "human",
        a: [
            // Nothing on a human body.
            ...barrier("syn-human", "B"),
            fx({ dome: false, spin: "none", maxSpinTicks: 0, maxLockDashes: 0, frames: 60, timeoutSec: 5 }),
            ...barrier("syn-wolf", "B"),
            { op: "expectRemoteForm", form: "wolf", body: "wolf", timeoutSec: 30 },
            // Three jumps: one burst each, the blur throughout, none on the step back to 0.
            ...barrier("syn-lock", "B"),
            fx({ minLockDashes: 3, maxLockDashes: 3, lockBlur: true, timeoutSec: 8 }),
            ...barrier("syn-lock-reset", "B"),
            fx({ lockBlur: false, maxLockDashes: 0, frames: 30, timeoutSec: 5 }),
            // Midna's hair hand turned toward the sender's lock target.
            ...barrier("syn-aim", "B"),
            { op: "expectPeerWolfFx", hairAim: 0x2000, timeoutSec: 5 },
            { op: "expectRemoteMidna", mode: "drawn", upper: 0x1df, hairHand: 2, timeoutSec: 8 },
            fx({ hairAim: 0x2000, frames: 30, timeoutSec: 8 }),
            ...barrier("syn-dome", "B"),
            fx({ dome: true, domeShown: true, minRadius: 399, maxRadius: 401, hairAim: false, frames: 30, timeoutSec: 8 }),
            // Junk: flags masked, radius clamped, no burst for a sequence jump of 127.
            ...barrier("syn-junk", "B"),
            fx({ dome: true, minRadius: 1000, maxRadius: 1000, maxLockDashes: 0, spin: "right", frames: 30, timeoutSec: 8 }),
            ...barrier("syn-back", "B"),
            fx({ dome: false, spin: "none", lockBlur: false, maxLockDashes: 0, frames: 30, timeoutSec: 8 }),
        ],
        b: [
            ...barrier("syn-human", "A"),
            patch({ wx: [0x11, 300, 5, 0] }, 150),
            { op: "wait", frames: 150 },
            { op: "transform", form: "wolf", timeoutSec: 30 },
            ...barrier("syn-wolf", "A"),
            ...barrier("syn-lock", "A"),
            { op: "wait", frames: 10 },
            patch({ wx: [0x20, 0, 1 | (2 << 8), 0] }, 20),
            { op: "wait", frames: 20 },
            patch({ wx: [0x20, 0, 2 | (1 << 8), 0] }, 20),
            { op: "wait", frames: 20 },
            patch({ wx: [0x20, 0, 3, 0] }, 30),
            { op: "wait", frames: 30 },
            unpatch,
            ...barrier("syn-lock-reset", "A"),
            ...barrier("syn-aim", "A"),
            patch({ ...POINTING_MIDNA, wx: [0x40, 0, 0, 0x2000] }, 300),
            ...barrier("syn-dome", "A"),
            patch({ wx: [0x10, 400, 0, 0] }, 300),
            ...barrier("syn-junk", "A"),
            patch({ wx: [0xff, 99999, 0xff7f, 0x7fff] }, 300),
            ...barrier("syn-back", "A"),
            unpatch,
        ],
    }),
    duo("wolf-fx-hidden-hop", "A's cutscene hides B's dummy mid-spin and mid-dome (no emitters, no dome drawn); then A leaves and comes back, and its new dummy of B shows the dome at once without replaying a burst", {
        cvars: hideInCutscenes,
        a: [
            { op: "waitRoomState", key: "hidePlayersInCutscene", value: true, timeoutSec: 20 },
            ...barrier("hid-go", "B"),
            fx({ dome: true, domeShown: true, lastSpin: "left", minSpinTicks: 5, timeoutSec: 8 }),
            { op: "forceCutscene", on: true },
            { op: "expectDummyHidden", timeoutSec: 3 },
            fx({ dome: true, domeShown: false, spin: "none", maxSpinTicks: 0, frames: 30, timeoutSec: 5 }),
            { op: "forceCutscene", on: null },
            fx({ domeShown: true, lastSpin: "left", minSpinTicks: 5, timeoutSec: 8 }),
            ...barrier("hop", "B"),
            warp(STAGES.faronField),
            waitStage(STAGES.faronField),
            warp(STAGES.southFaron),
            waitStage(STAGES.southFaron),
            { op: "waitDummies", count: 1, timeoutSec: 60 },
            fx({ dome: true, domeShown: true, minRadius: 299, maxRadius: 301, maxLockDashes: 0, frames: 30, timeoutSec: 20 }),
            ...barrier("hop-done", "B"),
        ],
        b: [
            { op: "waitRoomState", key: "hidePlayersInCutscene", value: true, timeoutSec: 20 },
            ...barrier("hid-go", "A"),
            patch({ wx: [0x11, 300, 0, 0] }, 900),
            ...barrier("hop", "A"),
            // A dome and a sequence of 7 that A's new dummy has never seen go up.
            patch({ wx: [0x10, 300, 7, 0] }, 2400),
            ...barrier("hop-done", "A"),
            unpatch,
        ],
    }),
];
