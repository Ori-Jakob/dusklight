// World sync: flags, event bits, items and the server catch-up.

const { MODS, STAGES, ITEMS, EVENTS, COMMON_CVARS, RANDOMIZER_MODE_CVARS, waitStage, barrier, meetIn, connect } =
    require("../lib");

const WRONG_LAYOUT = "00000000deadbeef";

// Both run the randomizer without a seed: equal probe keys, which sync only once confirmed.
const confirmUnverified = [
    waitStage(STAGES.linksHouse),
    { op: "expectLocalGame", kind: "randomizer", keyPrefix: "rando-probe/", verified: false },
    { op: "expectSyncState", state: "unverified" },
    { op: "confirmUnverified" },
    { op: "expectSyncState", state: "ok" },
];

module.exports = [
    {
        name: "world-sync",
        description: "items, a memory switch (set and cleared) and an event bit reach the teammate",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "giveItem", item: ITEMS.boomerang },
                    ...barrier("gave-boomerang", "B"),
                    { op: "expectItem", item: ITEMS.slingshot },
                    { op: "setSwitch", no: 20 },
                    ...barrier("set-sw20", "B"),
                    // Clearing before B saw it set would let both land in one update there.
                    { op: "waitSignal", name: "saw-sw20", from: "B", timeoutSec: 60 },
                    { op: "unsetSwitch", no: 20 },
                    ...barrier("cleared-sw20", "B"),
                    { op: "setEventBit", no: EVENTS.shieldAttack },
                    ...barrier("set-event", "B"),
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("gave-boomerang", "A"),
                    { op: "expectItem", item: ITEMS.boomerang },
                    { op: "giveItem", item: ITEMS.slingshot },
                    ...barrier("set-sw20", "A"),
                    { op: "expectSwitch", no: 20 },
                    { op: "signal", name: "saw-sw20" },
                    ...barrier("cleared-sw20", "A"),
                    { op: "expectSwitch", no: 20, set: false },
                    ...barrier("set-event", "A"),
                    { op: "expectEventBit", no: EVENTS.shieldAttack },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "offline-catchup",
        description: "progress made before a teammate connects reaches them through the server catch-up",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                // Quits before B is launched: B can only learn from the server's cache and queue.
                name: "A",
                launchDelayMs: 45000,
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "wait", sec: 3 },
                    { op: "giveItem", item: ITEMS.hookshot },
                    { op: "setSwitch", no: 21 },
                    { op: "setEventBit", no: EVENTS.shieldAttack },
                    { op: "wait", sec: 3 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "expectItem", item: ITEMS.hookshot, timeoutSec: 60 },
                    { op: "expectSwitch", no: 21 },
                    { op: "expectEventBit", no: EVENTS.shieldAttack },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "layout-mismatch",
        description: "a teammate announcing another save layout gets a warning toast and no world data either way, cached or live",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        expectLog: [
            { instance: "A", pattern: /\[sync\] B \(client \d+\) uses save layout 00000000deadbeef/ },
            { instance: "B", pattern: /\[sync\] A \(client \d+\) uses save layout [0-9a-f]{16}, we use 00000000deadbeef/ },
        ],
        rejectLog: [/merged world state/],
        instances: [
            {
                // Plays first, so the server holds A's cached state and queue when B asks.
                name: "A",
                launchDelayMs: 15000,
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "wait", sec: 3 },
                    { op: "giveItem", item: ITEMS.boomerang },
                    { op: "setEventBit", no: EVENTS.shieldAttack },
                    { op: "waitPeers", count: 1, timeoutSec: 120 },
                    { op: "waitLayoutMismatch", peer: "B" },
                    { op: "waitSignal", name: "b-gave", from: "B", timeoutSec: 120 },
                    { op: "expectNoItem", item: ITEMS.slingshot, forSec: 6 },
                    { op: "expectMerges", max: 0 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    { op: "forceLayout", layout: WRONG_LAYOUT },
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "waitPeers", count: 1, timeoutSec: 60 },
                    { op: "waitLayoutMismatch", peer: "A" },
                    { op: "expectNoItem", item: ITEMS.boomerang, forSec: 8 },
                    { op: "expectEventBit", no: EVENTS.shieldAttack, set: false },
                    { op: "giveItem", item: ITEMS.slingshot },
                    { op: "signal", name: "b-gave" },
                    { op: "expectMerges", max: 0 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "randomizer-active-sync",
        description: "with the randomizer's game mode active (its hooks installed, no seed) the match is unverified until confirmed; then flags, event bits and items sync both ways",
        timeoutSec: 300,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        expectLog: [/randomizer game mode activated/, /\[core\] Twili-Together initialized/,
            /\[game\] local game: randomizer rando-probe\/[0-9a-f]{16} "no seed loaded", unverified/,
            /\[game\] unverified match prompt \(not shown under autotest\)/],
        rejectLog: [/conflict/i],
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...confirmUnverified,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "setEventBit", no: EVENTS.shieldAttack },
                    { op: "giveItem", item: ITEMS.boomerang },
                    ...barrier("a-done", "B"),
                    { op: "expectSwitch", no: 20 },
                    { op: "expectItem", item: ITEMS.slingshot },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...confirmUnverified,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("a-done", "A"),
                    { op: "expectEventBit", no: EVENTS.shieldAttack },
                    { op: "expectItem", item: ITEMS.boomerang },
                    { op: "setSwitch", no: 20 },
                    { op: "giveItem", item: ITEMS.slingshot },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
