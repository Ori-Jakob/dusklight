// Regression scenarios for engine interactions found in play, one per bug.

const { COMMON_CVARS, connect, barrier } = require("../lib");

// Snowpeak's arrival point 1 lies in a suspend region (daSus_c): a dummy spawned there was
// suspended like any NPC-group actor and stayed frozen at its spawn point.
const SNOWPEAK_TOP = { stage: "F_SP114", room: 1, point: 1 };
const suspendSteps = (other) => [
    ...connect,
    { op: "waitStage", stage: SNOWPEAK_TOP.stage, timeoutSec: 90 },
    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 90 },
    { op: "waitDummies", count: 1, timeoutSec: 60 },
    { op: "wait", frames: 90 },
    { op: "checkDummies", maxDist: 400 },
    { op: "walk", frames: 180, circle: true },
    { op: "wait", frames: 30 },
    { op: "checkDummies", maxDist: 400 },
    ...barrier("done", other),
    { op: "quit" },
];

module.exports = [
    {
        name: "dummy-suspend-region",
        description: "a dummy spawned inside a suspend region (Snowpeak point 1) still follows its player",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            { name: "A", start: SNOWPEAK_TOP, steps: suspendSteps("B") },
            { name: "B", start: SNOWPEAK_TOP, steps: suspendSteps("A") },
        ],
    },
];
