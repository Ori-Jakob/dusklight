// Regression scenarios for engine interactions found in play, one per bug.

const { STAGES, COMMON_CVARS, connect, barrier, meetIn } = require("../lib");

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

// A reconnect's catch-up replays the whole queue: key deltas applied live before the drop were
// applied again (2 keys became 4). Queued packets are numbered and applied once; a key picked up
// while offline still arrives.
const reconnectKeysA = [
    ...connect,
    ...meetIn(STAGES.forestTemple, "B"),
    ...barrier("ready", "B"),
    { op: "expectKeys", count: 2, timeoutSec: 20 },
    ...barrier("got-2", "B"),
    { op: "signal", name: "going-offline" },
    { op: "wait", frames: 30 },
    { op: "disconnect" },
    { op: "wait", sec: 4 },
    ...connect,
    { op: "waitPeers", count: 1, timeoutSec: 60 },
    // Signals only reach connected clients: B waits for this one before its last barrier.
    { op: "signal", name: "back" },
    // 2 applied live + 1 missed while offline; the replay of the first 2 must not add again.
    { op: "expectKeys", count: 3, forSec: 8, timeoutSec: 30 },
    ...barrier("done", "B"),
    { op: "quit" },
];
const reconnectKeysB = [
    ...connect,
    ...meetIn(STAGES.forestTemple, "A"),
    ...barrier("ready", "A"),
    { op: "addKeys", count: 1 },
    { op: "wait", frames: 30 },
    { op: "addKeys", count: 1 },
    { op: "expectKeys", count: 2 },
    ...barrier("got-2", "A"),
    { op: "waitSignal", name: "going-offline", from: "A", timeoutSec: 60 },
    { op: "wait", sec: 2 },
    { op: "addKeys", count: 1 },
    { op: "expectKeys", count: 3 },
    { op: "waitSignal", name: "back", from: "A", timeoutSec: 120 },
    ...barrier("done", "A", 180),
    { op: "quit" },
];

// Wooden shields burn: personal while the room's shareWoodenShield is off, so a merge after a
// reconnect must not bring back a burnt one; shared (the default), it does.
const WOOD_SHIELD = 0x2a;
const NO_SHIELD = 255;
const woodShieldA = [
    ...connect,
    ...meetIn(STAGES.linksHouse, "B"),
    { op: "setRoomOption", name: "shareWoodenShield", value: false },
    { op: "waitRoomOption", name: "shareWoodenShield", value: false, timeoutSec: 20 },
    { op: "giveItem", item: WOOD_SHIELD },
    { op: "signal", name: "personal" },
    // One-way waits from here: B is offline part of the time.
    { op: "waitSignal", name: "b-back-1", from: "B", timeoutSec: 150 },
    { op: "setRoomOption", name: "shareWoodenShield", value: true },
    { op: "waitRoomOption", name: "shareWoodenShield", value: true, timeoutSec: 20 },
    { op: "signal", name: "shared-on" },
    { op: "waitSignal", name: "b-back-2", from: "B", timeoutSec: 150 },
    { op: "quit" },
];
const woodShieldB = [
    ...connect,
    ...meetIn(STAGES.linksHouse, "A"),
    { op: "waitSignal", name: "personal", from: "A", timeoutSec: 60 },
    { op: "waitRoomOption", name: "shareWoodenShield", value: false, timeoutSec: 20 },
    { op: "giveItem", item: WOOD_SHIELD },
    { op: "expectShield", item: WOOD_SHIELD, owned: true },
    { op: "burnShield" },
    { op: "expectShield", item: NO_SHIELD },
    { op: "disconnect" },
    { op: "wait", sec: 3 },
    ...connect,
    { op: "waitPeers", count: 1, timeoutSec: 60 },
    // The catch-up merge with A (who has the shield) must leave ours burnt.
    { op: "expectShield", item: NO_SHIELD, forSec: 8 },
    { op: "signal", name: "b-back-1" },
    { op: "waitSignal", name: "shared-on", from: "A", timeoutSec: 60 },
    { op: "waitRoomOption", name: "shareWoodenShield", value: true, timeoutSec: 20 },
    { op: "disconnect" },
    { op: "wait", sec: 3 },
    ...connect,
    { op: "waitPeers", count: 1, timeoutSec: 60 },
    // Shared: the merge brings A's shield.
    { op: "expectShield", item: WOOD_SHIELD, owned: true, timeoutSec: 30 },
    { op: "signal", name: "b-back-2" },
    { op: "quit" },
];

module.exports = [
    {
        name: "wood-shield-option",
        description: "with shareWoodenShield off a burnt wooden shield stays burnt through a reconnect merge; with it on, the merge brings a teammate's shield back",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            // A launches first, so it owns the room and can set the option.
            { name: "A", start: STAGES.linksHouse, launchDelayMs: 15000, steps: woodShieldA },
            { name: "B", start: STAGES.linksHouse, steps: woodShieldB },
        ],
    },
    {
        name: "reconnect-keys",
        description: "keys applied live are not applied again by the catch-up after a reconnect; a key picked up while offline still arrives",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            { name: "A", start: STAGES.forestTemple, steps: reconnectKeysA },
            { name: "B", start: STAGES.forestTemple, steps: reconnectKeysB },
        ],
    },
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
