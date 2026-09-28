// Name tags over remote players (src/ui/NameTags.cpp, steps in src/autotest/StepsNameTags.cpp).

const { STAGES, COMMON_CVARS, tt, barrier, meetIn, connect } = require("../lib");

const COLORS = { A: "13c76d", B: "fa059b" };
const other = (name) => (name === "A" ? "B" : "A");

module.exports = [
    {
        name: "name-tags",
        description: "each player sees the other's tag; a local cutscene hides it and it comes back; B looks at A's tag under its HUD for a capture",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.southFaron,
            cvars: [tt("color", COLORS[name])],
            steps: [
                ...connect,
                ...meetIn(STAGES.southFaron, other(name)),
                { op: "expectNameTag", name: other(name), holdTicks: 15 },
                { op: "forceCutscene", on: true },
                { op: "expectNameTag", name: other(name), shown: false, gate: "cutscene", holdTicks: 15 },
                { op: "forceCutscene", on: null },
                { op: "expectNameTag", name: other(name), holdTicks: 15 },
                ...barrier("tags-checked", other(name)),
                // A near B's camera on the left: its tag under the life gauge, which draws over it.
                ...(name === "B"
                    ? [{ op: "viewPeer", name: "A", yaw: -34, beyond: -180 }, { op: "wait", sec: 1 },
                       { op: "expectNameTag", name: "A" }, { op: "mark", msg: "NAMETAG_HOLD hud" },
                       { op: "wait", sec: 2 }]
                    : [{ op: "wait", sec: 3 }]),
                { op: "wait", sec: 4 },
                ...barrier("tags-done", other(name)),
                { op: "quit" },
            ],
        })),
    },
];
