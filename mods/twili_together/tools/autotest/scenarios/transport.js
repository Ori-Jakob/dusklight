// Transports and reconnecting (src/net/).

const { STAGES, COMMON_CVARS, meetIn, connect } = require("../lib");

// Both meet, the relay dies (the runner kills it on A's signal) and comes back on the same
// ports: the dummies go and come back without a connect step.
const afterRestart = (other) => [
    { op: "waitNoDummies", timeoutSec: 60 },
    { op: "waitConnected", timeoutSec: 90 },
    ...meetIn(STAGES.linksHouse, other),
    { op: "quit" },
];

module.exports = [
    {
        name: "tcp-transport",
        description: "same-stage over tcp:// (NetService, length-prefixed JSON) instead of WebSocket",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        transport: "tcp",
        expectLog: [/\[net\] connected to tcp:\/\/127\.0\.0\.1:\d+ over tcp/],
        instances: ["A", "B"].map((name, i, names) => ({
            name,
            start: STAGES.linksHouse,
            steps: [...connect, ...meetIn(STAGES.linksHouse, names[1 - i]), { op: "quit" }],
        })),
    },
    {
        name: "auto-reconnect",
        description: "the relay restarts mid-session; both players reconnect and see each other again on their own",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        restartServer: { afterSignal: "restart-relay", downMs: 3000 },
        expectLog: [/\[net\] connection lost/, /\[net\] reconnecting to/],
        instances: [
            {
                name: "A",
                start: STAGES.linksHouse,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "signal", name: "restart-relay" },
                    ...afterRestart("B"),
                ],
            },
            {
                name: "B",
                start: STAGES.linksHouse,
                steps: [...connect, ...meetIn(STAGES.linksHouse, "A"), ...afterRestart("A")],
            },
        ],
    },
];
