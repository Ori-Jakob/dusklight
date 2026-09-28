// Core scenarios; feature scenarios in scenarios/*.js. Reference in README.md.

const fs = require("node:fs");
const path = require("node:path");
const { MODS, STAGES, COMMON_CVARS, warp, waitStage, barrier, meetIn, connect } = require("./lib");

// Covers the boot takeover, the pad override and the request_quit exit.
const soloWalk = [
    waitStage(STAGES.linksHouse),
    { op: "markPos" },
    { op: "walk", frames: 180, circle: true },
    { op: "expectPos", minMoved: 50 },
    { op: "quit" },
];

const all = [
    {
        name: "smoke",
        description: "one instance boots into Link's house, walks, checks it moved, quits (the harness itself)",
        timeoutSec: 120,
        cvars: COMMON_CVARS,
        expectLog: [/\[core\] Twili-Together initialized/],
        instances: [{ name: "solo", steps: soloWalk }],
    },
    {
        name: "dormant",
        description: "without a script the mod loads, installs no autotest hooks and plays along (killed after 25s)",
        timeoutSec: 60,
        cvars: COMMON_CVARS,
        expectLog: [/\[core\] Twili-Together initialized/],
        rejectLog: [/group 'autotest' installed/, /\[autotest\]/],
        instances: [{ name: "solo", script: false, runSec: 25, steps: [] }],
    },
    {
        name: "randomizer-coexist",
        description: "smoke with the randomizer mod enabled: both load, no hook conflict",
        timeoutSec: 150,
        cvars: COMMON_CVARS,
        enableMods: [MODS.randomizer],
        expectLog: [/\[core\] Twili-Together initialized/, /randomizer .* initialized/],
        rejectLog: [/conflict/i],
        instances: [{ name: "solo", steps: soloWalk }],
    },
    {
        name: "same-stage",
        description: "two players in Link's house see each other's dummy",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name, i, names) => ({
            name,
            start: STAGES.linksHouse,
            steps: [...connect, ...meetIn(STAGES.linksHouse, names[1 - i]), { op: "quit" }],
        })),
    },
    {
        name: "stage-hop",
        description: "two players travel together through forest, field, dungeon and desert stages",
        timeoutSec: 600,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name, i, names) => {
            const other = names[1 - i];
            const route = [STAGES.southFaron, STAGES.faronField, STAGES.forestTemple,
                STAGES.goronMines, STAGES.gerudoDesert, STAGES.linksHouse];
            return {
                name,
                start: STAGES.linksHouse,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, other),
                    ...route.flatMap((s) => [warp(s), ...meetIn(s, other)]),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "split-and-rejoin",
        description: "one player leaves the stage and comes back; the other's dummy despawns and respawns",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.linksHouse,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "waitNoDummies", timeoutSec: 60 },
                    ...barrier("a-saw-b-leave", "B"),
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.linksHouse,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    warp(STAGES.southFaron),
                    waitStage(STAGES.southFaron),
                    { op: "expectPeers", count: 0, sameStage: true },
                    ...barrier("a-saw-b-leave", "A"),
                    warp(STAGES.linksHouse),
                    ...meetIn(STAGES.linksHouse, "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "reconnect",
        description: "one player disconnects and reconnects; the other drops and recreates the dummy",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.linksHouse,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "waitNoDummies", timeoutSec: 60 },
                    { op: "waitDummies", count: 1, timeoutSec: 90 },
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.linksHouse,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    { op: "disconnect" },
                    { op: "wait", sec: 3 },
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "trio",
        description: "three players in one stage each see both others",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: ["A", "B", "C"].map((name) => ({
            name,
            steps: [
                ...connect,
                waitStage(STAGES.linksHouse),
                { op: "waitPeers", count: 2, sameStage: true, timeoutSec: 120 },
                { op: "waitDummies", count: 2, timeoutSec: 90 },
                { op: "walk", frames: 240, circle: true },
                { op: "checkDummies", maxDist: 400 },
                { op: "signal", name: `ready-${name}` },
                ...["A", "B", "C"].filter((n) => n !== name)
                    .map((n) => ({ op: "waitSignal", name: `ready-${n}`, from: n, timeoutSec: 120 })),
                { op: "wait", sec: 2 },
                { op: "quit" },
            ],
        })),
    },
    {
        name: "rapid-hop",
        description: "one player bounces between two stages quickly while the other watches the dummy come and go",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    ...barrier("hopping", "B"),
                    ...barrier("hops-done", "B", 300),
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("hopping", "A"),
                    ...[1, 2, 3, 4, 5].flatMap(() => [
                        warp(STAGES.southFaron),
                        waitStage(STAGES.southFaron),
                        { op: "wait", frames: 20 },
                        warp(STAGES.linksHouse),
                        waitStage(STAGES.linksHouse),
                        { op: "wait", frames: 20 },
                    ]),
                    ...barrier("hops-done", "A"),
                    ...meetIn(STAGES.linksHouse, "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "peer-vanishes",
        description: "a player's game exits abruptly mid-session; the survivor drops the dummy and keeps running",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    // B exits without disconnecting; the relay notices the socket close.
                    { op: "waitNoDummies", timeoutSec: 60 },
                    { op: "expectPeers", count: 0 },
                    { op: "walk", frames: 120, circle: true },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [...connect, ...meetIn(STAGES.linksHouse, "A"), { op: "exitNow" }],
            },
        ],
    },
    {
        name: "late-join",
        description: "the second player boots and connects well after the first is playing",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.faronField,
                launchDelayMs: 25000,
                steps: [
                    waitStage(STAGES.faronField),
                    ...connect,
                    { op: "walk", frames: 600, circle: true },
                    ...meetIn(STAGES.faronField, "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.faronField,
                steps: [
                    waitStage(STAGES.faronField),
                    ...connect,
                    ...meetIn(STAGES.faronField, "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];

const featureDir = path.join(__dirname, "scenarios");
if (fs.existsSync(featureDir)) {
    for (const file of fs.readdirSync(featureDir).filter((f) => f.endsWith(".js")).sort()) {
        all.push(...require(path.join(featureDir, file)));
    }
}

for (const s of all) {
    for (const inst of s.instances) {
        inst.start ??= STAGES.linksHouse;
    }
}

module.exports = { all, STAGES };
