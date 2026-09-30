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
    {
        name: "name-tag-health",
        description: "A sees B's health on B's tag in quarter hearts out of B's containers, following B's changes; Show Player Health off hides the bar live; 20 hearts keep the tag narrow",
        timeoutSec: 300,
        // Life stays where the steps put it.
        cvars: [...COMMON_CVARS, "game.infiniteHearts=false"],
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    ...barrier("b-hp-1", "B"),
                    // 5 containers, 2 3/4 hearts left.
                    { op: "expectNameTag", name: "B", life: 11, maxLife: 20, holdTicks: 10 },
                    { op: "showPlayerHealth", value: false },
                    { op: "expectNameTag", name: "B", health: false, holdTicks: 10 },
                    { op: "showPlayerHealth", value: true },
                    { op: "expectNameTag", name: "B", life: 11, maxLife: 20, holdTicks: 10 },
                    ...barrier("a-toggled", "B"),
                    ...barrier("b-hp-2", "B"),
                    { op: "expectNameTag", name: "B", life: 20, maxLife: 20, holdTicks: 10 },
                    ...barrier("b-hp-3", "B"),
                    // A one-letter name with 20 containers: bar, pads and dot stay under 110.
                    { op: "expectNameTag", name: "B", life: 3, maxLife: 80, maxWidth: 110, holdTicks: 10 },
                    { op: "mark", msg: "NAMETAG_HOLD health" },
                    { op: "wait", sec: 2 },
                    ...barrier("hp-done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "setLife", hearts: 5, value: 11 },
                    { op: "wait", frames: 10 },
                    ...barrier("b-hp-1", "A"),
                    ...barrier("a-toggled", "A"),
                    { op: "setLife", value: 20 },
                    { op: "wait", frames: 10 },
                    ...barrier("b-hp-2", "A"),
                    { op: "setLife", hearts: 20, value: 3 },
                    { op: "wait", frames: 10 },
                    ...barrier("b-hp-3", "A"),
                    ...barrier("hp-done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
