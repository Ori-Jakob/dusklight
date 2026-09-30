// Smooth remote motion and hiding players in cutscenes.

const { STAGES, COMMON_CVARS, tt, waitStage, barrier, meetIn, connect } = require("../lib");

// B measures its own motion, A measures B's dummy: one sender tick per local tick.
const smoothMotion = (name, description, cvars, serverEnv = {}) => ({
    name,
    description,
    serverEnv,
    timeoutSec: 300,
    cvars: [...COMMON_CVARS, ...cvars],
    instances: [
        {
            name: "A",
            start: STAGES.faronField,
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, "B"),
                ...barrier("walk", "B"),
                { op: "measureDummyMotion", frames: 240, settleFrames: 20, minSpeed: 3, maxBadRatio: 0.03 },
                ...barrier("measured", "B"),
                { op: "quit" },
            ],
        },
        {
            name: "B",
            start: STAGES.faronField,
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, "A"),
                ...barrier("walk", "A"),
                { op: "walk", frames: 300, circle: true, async: true },
                { op: "measureDummyMotion", self: true, frames: 240, settleFrames: 20, minSpeed: 3 },
                ...barrier("measured", "A"),
                { op: "quit" },
            ],
        },
    ],
});

// Every instance sets the room option: whichever one owns the room pushes it.
const hideInCutscenes = [...COMMON_CVARS, tt("hide_players_in_cutscene", 1)];
const roomHidesInCutscenes = { op: "waitRoomState", key: "hidePlayersInCutscene", value: true, timeoutSec: 20 };

// The body's climb rate may change sharply on few ticks, for our Link and for the dummy alike.
const steadyBody = { frames: 240, settleFrames: 20, jerk: 5, maxJerkRatio: 0.05 };

// B stands across a slope (and falls from over it); A's dummy of B plants both feet like B's Link.
const feetOnSlope = (name, description, stage, wolf) => ({
    name,
    description,
    timeoutSec: 300,
    cvars: COMMON_CVARS,
    instances: [
        {
            name: "A",
            start: stage,
            steps: [
                ...connect,
                ...meetIn(stage, "B"),
                { op: "waitSignal", name: "on-slope", from: "B", timeoutSec: 90 },
                { op: "expectFeetOnGround", maxSpread: wolf ? 6 : 4, minBend: 3, frames: 30 },
                ...barrier("feet-checked", "B"),
                { op: "expectDummyAirborne", timeoutSec: 5 },
                ...barrier("fell", "B"),
                { op: "quit" },
            ],
        },
        {
            name: "B",
            start: stage,
            steps: [
                ...connect,
                ...meetIn(stage, "A"),
                ...(wolf ? [{ op: "transform", form: "wolf", timeoutSec: 30 }] : []),
                { op: "findSlope", minDeg: 12, maxDeg: 30 },
                { op: "wait", frames: 30 },
                // Our own Link on the same spot is the yardstick for the check on A
                { op: "expectFeetOnGround", self: true, maxSpread: wolf ? 6 : 4, minBend: 3, frames: 10 },
                { op: "signal", name: "on-slope" },
                ...barrier("feet-checked", "A"),
                { op: "teleportSelf", dy: 250 },
                { op: "wait", frames: 60 },
                ...barrier("fell", "A"),
                { op: "quit" },
            ],
        },
    ],
});

module.exports = [
    {
        name: "presence-showcase",
        description: "two players stand near each other in Faron Field for a screenshot (manual)",
        manualOnly: true,
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name, i, names) => ({
            name,
            start: STAGES.faronField,
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, names[1 - i]),
                { op: "log", msg: "showcase ready" },
                { op: "wait", sec: 20 },
                { op: "quit" },
            ],
        })),
    },
    {
        name: "pose-selftest",
        description: "interpolation buffer and cutscene classifier unit checks",
        timeoutSec: 120,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                steps: [waitStage(STAGES.linksHouse), { op: "poseSelfTest" }, { op: "quit" }],
            },
        ],
    },
    // With frame interpolation on, a frame runs zero to two ticks.
    smoothMotion("smooth-motion", "a remote walking in circles moves every tick, with mixed frame pacing",
        ["game.enableFrameInterpolation=2"]),
    smoothMotion("smooth-motion-clean", "a remote walking in circles moves every tick, one tick per frame",
        ["game.enableFrameInterpolation=0"]),
    // The relay delays every packet by 0-90 ms (order kept): the playout delay must absorb it.
    smoothMotion("smooth-motion-jitter", "a remote walking in circles moves every tick with 0-90 ms network jitter",
        [], { TT_TEST_JITTER_MS: "90" }),
    {
        name: "puppet-ground-steady",
        description: "B walks in circles in Faron Field and then on a slope: the height of A's dummy of B changes as smoothly as B's own body (no hop at every step or slope change)",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "B"),
                    ...barrier("walk", "B"),
                    { op: "measureBodyHeight", ...steadyBody },
                    ...barrier("slope", "B"),
                    { op: "measureBodyHeight", ...steadyBody },
                    ...barrier("measured", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "A"),
                    ...barrier("walk", "A"),
                    { op: "walk", frames: 300, circle: true, async: true },
                    { op: "measureBodyHeight", self: true, ...steadyBody },
                    { op: "wait", frames: 60 },
                    { op: "findSlope", minDeg: 10, maxDeg: 30 },
                    { op: "wait", frames: 20 },
                    ...barrier("slope", "A"),
                    { op: "walk", frames: 300, circle: true, async: true },
                    { op: "measureBodyHeight", self: true, ...steadyBody },
                    ...barrier("measured", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    feetOnSlope("puppet-feet-slope",
        "B stands across a slope in Faron Field: A's dummy of B bends its legs and drops to the lower foot like B's Link, both feet on A's ground; in B's fall the dummy's legs let go",
        STAGES.faronField, false),
    feetOnSlope("puppet-feet-slope-wolf",
        "the same for B as a wolf in South Faron: all four paws of A's dummy on the slope",
        STAGES.southFaron, true),
    {
        name: "teleport-snap",
        description: "a peer that jumps 1600 units shows up there at once instead of sliding",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "B"),
                    ...barrier("arm", "B"),
                    // B stands still until it jumps: a slide would show up as a partial step.
                    { op: "expectDummyTeleport", minDist: 1500, maxPreStep: 50, timeoutSec: 20 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.faronField,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.faronField, "A"),
                    ...barrier("arm", "A"),
                    { op: "wait", frames: 30 },
                    // Straight up: open sky is above the start point, and the fall is ordinary movement.
                    { op: "teleportSelf", dy: 1600 },
                    { op: "wait", frames: 60 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "cutscene-hide",
        description: "hide_players_in_cutscene: a local cutscene hides everyone, a remote in a cutscene is hidden, a compulsory event hides nobody",
        timeoutSec: 300,
        cvars: hideInCutscenes,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    roomHidesInCutscenes,
                    ...barrier("room-ready", "B"),
                    ...barrier("b-in-cutscene", "B"),
                    // B's cutscene reaches us as its PLAYER_UPDATE flag and hides its dummy.
                    { op: "expectPeerFlag", flag: "inCutscene", set: true, timeoutSec: 10 },
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 10 },
                    ...barrier("a-saw-hidden", "B"),
                    ...barrier("b-out", "B"),
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 10 },
                    // A player-ordered event (like drinking from a bottle) is no cutscene.
                    { op: "beginEvent", kind: "compulsory", timeoutSec: 10 },
                    ...barrier("a-compulsory", "B"),
                    { op: "expectDummyHidden", hidden: false },
                    { op: "endEvent", timeoutSec: 10 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    roomHidesInCutscenes,
                    ...barrier("room-ready", "A"),
                    // Our own cutscene hides A's dummy here.
                    { op: "forceCutscene", on: true },
                    { op: "expectDummyHidden", hidden: true, timeoutSec: 5 },
                    ...barrier("b-in-cutscene", "A"),
                    ...barrier("a-saw-hidden", "A"),
                    // Shown again once the release delay after the cutscene ran out.
                    { op: "forceCutscene", on: null },
                    { op: "wait", frames: 15 },
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 5 },
                    ...barrier("b-out", "A"),
                    ...barrier("a-compulsory", "A"),
                    { op: "expectPeerFlag", flag: "inCutscene", set: false },
                    { op: "expectDummyHidden", hidden: false },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
