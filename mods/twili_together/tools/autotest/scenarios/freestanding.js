// Freestanding items with a save bit (field items, heart pieces): taken by one player, gone for the
// others, and each check reaches everyone once. Also the item name audit.

const { MODS, STAGES, COMMON_CVARS, RANDOMIZER_MODE_CVARS, warp, waitStage, barrier, meetIn, connect } =
    require("../lib");

const GREEN_RUPEE = 0x01;
const HEART_PIECE = 0x21;
// Memory item bits of South Faron (stage table 2) for the spawned items.
const RUPEE_BIT = 0x9c;
const HEART_BIT = 0x9d;
const FARON_TBL = 2;
// Three hearts; one piece makes 16.
const BASE_LIFE = 15;

// Pickups are off so a taken rupee does not reach the other player as a shared pickup.
const noPickups = [
    { op: "setRoomOption", name: "sharePickups", value: false },
    { op: "waitRoomOption", name: "sharePickups", value: false },
];

const inFaron = (other) => [
    ...connect,
    ...noPickups,
    ...meetIn(STAGES.southFaron, other),
    { op: "setInventory", rupees: 0, maxLife: BASE_LIFE },
];

const spawnBoth = [
    { op: "spawnFieldItem", tag: "r", item: GREEN_RUPEE, bit: RUPEE_BIT, dist: 150 },
    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: HEART_BIT, dist: 260 },
    { op: "wait", sec: 2 },
    { op: "expectFieldItem", tag: "r" },
    { op: "expectFieldItem", tag: "h" },
];

module.exports = [
    {
        name: "freestanding-sync",
        description: "A takes a flagged rupee and a heart piece; B's copies vanish with no item or event, B gets the heart piece once and both bits",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...inFaron("B"),
                    ...spawnBoth,
                    ...barrier("spawned", "B"),
                    { op: "takeFieldItem", tag: "r" },
                    { op: "expectItemBit", bit: RUPEE_BIT },
                    ...barrier("rupee", "B"),
                    { op: "takeFieldItem", tag: "h" },
                    { op: "expectItemBit", bit: HEART_BIT },
                    { op: "expectInventory", maxLife: BASE_LIFE + 1 },
                    ...barrier("heart", "B"),
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, forSec: 5 },
                    { op: "expectPickups", deduped: 0 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...inFaron("A"),
                    ...spawnBoth,
                    ...barrier("spawned", "A"),
                    ...barrier("rupee", "A"),
                    { op: "expectFieldItem", tag: "r", present: false, timeoutSec: 10 },
                    { op: "expectItemBit", bit: RUPEE_BIT },
                    { op: "expectInventory", rupees: 0, forSec: 2 },
                    ...barrier("heart", "A"),
                    { op: "expectFieldItem", tag: "h", present: false, timeoutSec: 15 },
                    { op: "expectItemBit", bit: HEART_BIT },
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, timeoutSec: 30 },
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, forSec: 5 },
                    { op: "expectPickups", removed: 2, blocked: 0, deduped: 0 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "freestanding-overlap",
        description: "A holds its heart piece message while B takes its own copy of that heart piece; each ends with exactly one piece",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...inFaron("B"),
                    ...spawnBoth,
                    ...barrier("spawned", "B"),
                    // B takes its copy during these 12 s.
                    { op: "takeFieldItem", tag: "h", holdSec: 12 },
                    ...barrier("taken", "B"),
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, forSec: 6 },
                    { op: "expectPickups", deduped: 1 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...inFaron("A"),
                    ...spawnBoth,
                    ...barrier("spawned", "A"),
                    { op: "wait", sec: 2 },
                    { op: "takeFieldItem", tag: "h" },
                    ...barrier("taken", "A"),
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, forSec: 6 },
                    { op: "expectPickups", deduped: 1 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "freestanding-busy",
        description: "B's copies are pulled by the clawshot when A takes them: B's pickup of the rupee gives nothing, the let-go heart piece vanishes",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...inFaron("B"),
                    ...spawnBoth,
                    ...barrier("spawned", "B"),
                    { op: "takeFieldItem", tag: "r" },
                    { op: "takeFieldItem", tag: "h" },
                    ...barrier("taken", "B"),
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...inFaron("A"),
                    ...spawnBoth,
                    { op: "holdFieldItem", tag: "r" },
                    { op: "holdFieldItem", tag: "h" },
                    ...barrier("spawned", "A"),
                    ...barrier("taken", "A"),
                    { op: "expectItemBit", bit: HEART_BIT, timeoutSec: 30 },
                    // Busy when the bits came: still there.
                    { op: "expectFieldItem", tag: "r", forSec: 2 },
                    { op: "expectFieldItem", tag: "h", forSec: 0 },
                    { op: "takeFieldItem", tag: "r" },
                    { op: "expectInventory", rupees: 0, forSec: 2 },
                    { op: "expectPickups", blocked: 1 },
                    { op: "holdFieldItem", tag: "h", hold: false },
                    { op: "expectFieldItem", tag: "h", present: false, timeoutSec: 5 },
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, forSec: 5 },
                    { op: "expectPickups", removed: 1, blocked: 1 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "freestanding-away",
        description: "A takes a heart piece while B is in another stage; the bit is in B's save and B's copy never appears when B gets there",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...noPickups,
                    waitStage(STAGES.southFaron),
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...barrier("ready", "B"),
                    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: HEART_BIT, dist: 150 },
                    { op: "wait", sec: 2 },
                    { op: "takeFieldItem", tag: "h" },
                    ...barrier("taken", "B"),
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...noPickups,
                    waitStage(STAGES.linksHouse),
                    { op: "setInventory", maxLife: BASE_LIFE },
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...barrier("ready", "A"),
                    ...barrier("taken", "A"),
                    { op: "expectItemBit", bit: HEART_BIT, tbl: FARON_TBL },
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, timeoutSec: 30 },
                    warp(STAGES.southFaron),
                    waitStage(STAGES.southFaron),
                    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: HEART_BIT, dist: 150 },
                    { op: "expectFieldItem", tag: "h", present: false, forSec: 3 },
                    { op: "expectInventory", maxLife: BASE_LIFE + 1, forSec: 2 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-name-audit",
        description: "every item a toast can name in the unmodded game has a proper name (no placeholder, number or lower-case bug)",
        timeoutSec: 120,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                steps: [waitStage(STAGES.linksHouse), { op: "itemNameAudit" }, { op: "quit" }],
            },
        ],
    },
    {
        name: "item-name-audit-rando",
        description: "randomizer game mode: every item the randomizer hands out has its randomizer name in toasts",
        timeoutSec: 150,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        rejectLog: [/conflict/i],
        instances: [
            {
                name: "solo",
                steps: [
                    waitStage(STAGES.linksHouse),
                    { op: "expectLocalGame", kind: "randomizer" },
                    { op: "itemNameAudit" },
                    { op: "quit" },
                ],
            },
        ],
    },
];
