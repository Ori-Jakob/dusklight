// Teleport to a player: teleportTo, expectNear and expectBeside (StepsTeleport.cpp), room options
// (StepsWorld.cpp).

const { STAGES, COMMON_CVARS, warp, waitStage, barrier, meetIn, connect } = require("../lib");

// Every instance sets it: whichever one owns the room pushes it.
const enableTeleport = [
    { op: "setRoomOption", name: "teleportMode", value: true },
    { op: "waitRoomOption", name: "teleportMode", value: true, timeoutSec: 20 },
];
// So the spot the peer is sent to is where we stand.
const standStill = { op: "wait", frames: 45 };
// Still there a few seconds later: map event 0 of South Faron would load another stage.
const stayIn = (s) => [waitStage(s), { op: "wait", sec: 5 }, waitStage(s)];

module.exports = [
    {
        name: "teleport",
        description: "teleport is refused while the room disables it, then crosses stages, moves within a stage and enters a dungeon",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    waitStage(STAGES.linksHouse),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    { op: "teleportTo", target: "B", expect: "refused", reason: "disabled" },
                    ...barrier("disabled-checked", "B"),
                    ...enableTeleport,
                    ...barrier("b-in-faron", "B", 180),
                    // Another stage: start point -1 reload, then beside B once its room is loaded.
                    { op: "teleportTo", target: "B", expect: "stage", placement: "exact", timeoutSec: 90 },
                    ...stayIn(STAGES.southFaron),
                    { op: "expectBeside", target: "B" },
                    ...barrier("a-in-faron", "B"),
                    ...barrier("b-moved", "B"),
                    // Same stage and layer, B's room loaded here: moved without a reload.
                    { op: "teleportTo", target: "B", expect: "local", placement: "exact", timeoutSec: 30 },
                    { op: "expectBeside", target: "B" },
                    ...barrier("a-moved", "B"),
                    ...barrier("b-in-temple", "B", 180),
                    { op: "teleportTo", target: "B", expect: "stage", placement: "exact", timeoutSec: 90 },
                    ...stayIn(STAGES.forestTemple),
                    { op: "expectBeside", target: "B" },
                    { op: "waitDummies", count: 1, timeoutSec: 60 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    waitStage(STAGES.linksHouse),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...barrier("disabled-checked", "A"),
                    ...enableTeleport,
                    warp(STAGES.southFaron), waitStage(STAGES.southFaron), standStill,
                    ...barrier("b-in-faron", "A", 180),
                    ...barrier("a-in-faron", "A", 180),
                    { op: "walk", frames: 150, circle: true }, standStill,
                    ...barrier("b-moved", "A"),
                    ...barrier("a-moved", "A"),
                    warp(STAGES.forestTemple), waitStage(STAGES.forestTemple), standStill,
                    ...barrier("b-in-temple", "A", 180),
                    ...barrier("done", "A", 180),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "teleport-mutual",
        description: "both players ask to teleport to each other at once; exactly one moves and they end in one stage",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [["A", STAGES.linksHouse], ["B", STAGES.southFaron]].map(([name, start], i, all) => {
            const other = all[1 - i][0];
            return {
                name,
                start,
                steps: [
                    ...connect,
                    waitStage(start),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...enableTeleport,
                    standStill,
                    ...barrier("go", other),
                    // The lower client id stays and answers; the other one moves.
                    { op: "teleportTo", target: other, expect: ["arrived", "refused"], timeoutSec: 90 },
                    { op: "wait", sec: 3 },
                    ...barrier("settled", other, 180),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 60 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "teleport-target-vanishes",
        description: "the target starts a stage load and its game exits moments after the request; the requester gets a refusal or timeout and keeps running",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    ...enableTeleport,
                    { op: "waitSignal", name: "leaving", from: "B", timeoutSec: 120 },
                    // Sent while B is still online: refused as loading, or ended once B is gone.
                    { op: "teleportTo", target: "B", expect: ["refused", "timeout"], timeoutSec: 30 },
                    { op: "waitNoDummies", timeoutSec: 60 },
                    { op: "walk", frames: 120, circle: true },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...enableTeleport,
                    // Loading until the exit, so a request that still reaches us is refused.
                    warp(STAGES.southFaron),
                    { op: "signal", name: "leaving" },
                    // Lets the signal leave the socket before the process dies.
                    { op: "wait", sec: 0.3 },
                    { op: "exitNow" },
                ],
            },
        ],
    },
    {
        name: "teleport-cutscene",
        description: "teleporting to a player in a cutscene: across stages and within one, the requester lands beside them and runs no event itself while theirs plays on",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    waitStage(STAGES.linksHouse),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...enableTeleport,
                    ...barrier("b-in-event", "B", 180),
                    { op: "teleportTo", target: "B", expect: "stage", placement: "exact", timeoutSec: 90 },
                    ...stayIn(STAGES.southFaron),
                    { op: "expectBeside", target: "B" },
                    { op: "expectOwnEvent", running: false, holdTicks: 90 },
                    ...barrier("a-beside", "B"),
                    ...barrier("b-ended", "B"),
                    // Away from B in the same room; B starts another event.
                    { op: "walk", frames: 150, circle: true }, standStill,
                    ...barrier("a-away", "B"),
                    ...barrier("b-in-event-again", "B"),
                    { op: "teleportTo", target: "B", expect: "local", placement: "exact", timeoutSec: 30 },
                    { op: "expectBeside", target: "B" },
                    { op: "expectOwnEvent", running: false, holdTicks: 90 },
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
                    ...enableTeleport,
                    standStill,
                    { op: "beginEvent", kind: "compulsory" },
                    ...barrier("b-in-event", "A", 180),
                    ...barrier("a-beside", "A", 180),
                    // Ours kept running through A's arrival.
                    { op: "expectOwnEvent", running: true, holdTicks: 1 },
                    { op: "endEvent" },
                    { op: "expectOwnEvent", running: false, holdTicks: 30 },
                    ...barrier("b-ended", "A"),
                    ...barrier("a-away", "A"),
                    { op: "beginEvent", kind: "compulsory" },
                    ...barrier("b-in-event-again", "A"),
                    ...barrier("done", "A", 180),
                    { op: "endEvent" },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "teleport-fallback",
        description: "when the ground beside the target is not standable (here: the pose shifted high into the air), the requester lands on the spot the target sent, after a stage load and without one",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    waitStage(STAGES.linksHouse),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...enableTeleport,
                    ...barrier("b-standing", "B", 180),
                    { op: "teleportTo", target: "B", offset: [0, 2000, 0], expect: "stage", placement: "fallback", timeoutSec: 90 },
                    ...stayIn(STAGES.southFaron),
                    { op: "expectNear", target: "B", maxDist: 150 },
                    { op: "walk", frames: 150, circle: true }, standStill,
                    // B's room is loaded here: straight onto its spot, no reload.
                    { op: "teleportTo", target: "B", offset: [0, 2000, 0], expect: "local", placement: "fallback", timeoutSec: 30 },
                    { op: "expectNear", target: "B", maxDist: 150 },
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
                    ...enableTeleport,
                    standStill,
                    ...barrier("b-standing", "A", 180),
                    ...barrier("done", "A", 240),
                    { op: "quit" },
                ],
            },
        ],
    },
];
