// Core scenarios; feature scenarios in scenarios/*.js. Reference in README.md.

const fs = require("node:fs");
const path = require("node:path");
const { MODS, STAGES, COMMON_CVARS, waitStage } = require("./lib");

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
