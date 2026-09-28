// The cost of our hooks while a peer plays next to us (src/autotest/StepsPerf.cpp).

const { STAGES, COMMON_CVARS, barrier, meetIn, connect } = require("../lib");

// Held B: sword swings and a spin attack, so the effect taps see calls.
const PAD_B = 0x200;

module.exports = [
    {
        name: "hook-perf",
        description: "A measures what our hooks cost per tick while B runs and swings its sword next to A's dummy of it: under 0.1 ms",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("perf-start", "B"),
                    { op: "walk", frames: 150, circle: true },
                    { op: "measureHookCost", frames: 300, maxMs: 0.1, timeoutSec: 60 },
                    ...barrier("perf-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...barrier("perf-start", "A"),
                    { op: "walk", frames: 240, circle: true },
                    { op: "walk", frames: 240, circle: true, buttons: PAD_B },
                    ...barrier("perf-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
