// Teleport to a player: teleportTo and expectNear (StepsTeleport.cpp), room options (StepsWorld.cpp).

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
                    // Another stage: start point -1 reload.
                    { op: "teleportTo", target: "B", expect: "stage", timeoutSec: 90 },
                    ...stayIn(STAGES.southFaron),
                    { op: "expectNear", target: "B", maxDist: 250 },
                    ...barrier("a-in-faron", "B"),
                    ...barrier("b-moved", "B"),
                    // Same stage and layer, B's room loaded here: moved without a reload.
                    { op: "teleportTo", target: "B", expect: "local", timeoutSec: 30 },
                    { op: "expectNear", target: "B", maxDist: 150, timeoutSec: 10 },
                    ...barrier("a-moved", "B"),
                    ...barrier("b-in-temple", "B", 180),
                    { op: "teleportTo", target: "B", expect: "stage", timeoutSec: 90 },
                    ...stayIn(STAGES.forestTemple),
                    { op: "expectNear", target: "B", maxDist: 250 },
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
                    // Sent at once, while B is still online: B refuses it as loading, or the
                    // server (offline), our roster (offline) or our timeout ends it once B is gone.
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
];
