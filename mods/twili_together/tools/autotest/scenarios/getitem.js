// The item a player holds up (chest, heart piece, randomizer item) on its puppet for the others.

const { MODS, STAGES, COMMON_CVARS, RANDOMIZER_MODE_CVARS, tt, warp, waitStage, barrier, meetIn, connect } =
    require("../lib");

const RED_RUPEE = 0x04;
const HEART_PIECE = 0x21;
const POE_SOUL = 0xe0;
// A TBOX number no chest of South Faron's stage table uses.
const TBOX = 62;
// A memory item bit of South Faron (stage table 2) for the spawned heart piece.
const HEART_BIT = 0x9d;
// daDitem_c's l_player_offset: 115 above the feet, 54 ahead.
const HELD_UP = [0, 115, 54];

// Pickups are off so the chest's rupees do not reach the other player.
const noPickups = [
    { op: "setRoomOption", name: "sharePickups", value: false },
    { op: "waitRoomOption", name: "sharePickups", value: false },
];

const confirmUnverified = [
    waitStage(STAGES.linksHouse),
    { op: "expectSyncState", state: "unverified" },
    { op: "confirmUnverified" },
    { op: "expectSyncState", state: "ok" },
];

// A opens a chest holding `item`; B sees A's puppet hold it up, then let it go.
const chestPair = (item, extraB = []) => [
    {
        name: "A",
        start: STAGES.southFaron,
        steps: [
            ...connect,
            ...noPickups,
            ...meetIn(STAGES.southFaron, "B"),
            { op: "setInventory", rupees: 0 },
            { op: "spawnChest", tag: "c", tbox: TBOX, item },
            { op: "wait", sec: 2 },
            ...barrier("chest", "B"),
            { op: "openChest", tag: "c" },
            { op: "expectLocalGetItem", item, real: item, shows: 1, ticks: 20 },
            ...barrier("opened", "B"),
            ...barrier("done", "B"),
            { op: "quit" },
        ],
    },
    {
        name: "B",
        start: STAGES.southFaron,
        steps: [
            ...connect,
            ...noPickups,
            ...meetIn(STAGES.southFaron, "A"),
            ...barrier("chest", "A"),
            { op: "expectRemoteGetItem", item, near: HELD_UP, tol: 40, minScale: 0.9, frames: 15, timeoutSec: 60 },
            ...extraB,
            { op: "expectRemoteGetItem", none: true, timeoutSec: 60 },
            ...barrier("opened", "A"),
            ...barrier("done", "A"),
            { op: "quit" },
        ],
    },
];

module.exports = [
    {
        name: "getitem-chest",
        description: "A opens a chest; B sees A's puppet hold the rupee up where the game puts it, full size, and the model goes when A's presentation ends",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: chestPair(RED_RUPEE),
    },
    {
        name: "getitem-heart-piece",
        description: "A takes a freestanding heart piece (item get event); B sees it held up with its sparkle effects, then gone",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...noPickups,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: HEART_BIT, dist: 150 },
                    { op: "wait", sec: 2 },
                    { op: "expectFieldItem", tag: "h" },
                    ...barrier("spawned", "B"),
                    { op: "takeFieldItem", tag: "h" },
                    { op: "expectItemBit", bit: HEART_BIT },
                    { op: "expectLocalGetItem", item: HEART_PIECE, real: HEART_PIECE, shows: 1, ticks: 20 },
                    ...barrier("taken", "B"),
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...noPickups,
                    ...meetIn(STAGES.southFaron, "A"),
                    ...barrier("spawned", "A"),
                    { op: "expectRemoteGetItem", item: HEART_PIECE, near: HELD_UP, tol: 40, emitters: 1, frames: 10, timeoutSec: 60 },
                    { op: "expectRemoteGetItem", none: true, timeoutSec: 60 },
                    ...barrier("taken", "A"),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "getitem-hidden",
        description: "with hidePlayersInCutscene, A's heart piece event hides A's puppet on B and the item it holds up with it",
        timeoutSec: 300,
        cvars: [...COMMON_CVARS, tt("hide_players_in_cutscene", 1)],
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...noPickups,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "waitRoomState", key: "hidePlayersInCutscene", value: true, timeoutSec: 20 },
                    { op: "spawnFieldItem", tag: "h", item: HEART_PIECE, bit: HEART_BIT, dist: 150 },
                    { op: "wait", sec: 2 },
                    ...barrier("spawned", "B"),
                    { op: "takeFieldItem", tag: "h" },
                    { op: "expectLocalGetItem", item: HEART_PIECE, shows: 1, ticks: 20 },
                    ...barrier("taken", "B"),
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...noPickups,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "waitRoomState", key: "hidePlayersInCutscene", value: true, timeoutSec: 20 },
                    ...barrier("spawned", "A"),
                    { op: "expectRemoteGetItem", item: HEART_PIECE, drawn: false, timeoutSec: 60 },
                    { op: "expectDummyHidden", hidden: true },
                    { op: "expectRemoteGetItem", none: true, timeoutSec: 60 },
                    { op: "expectDummyHidden", hidden: false, timeoutSec: 10 },
                    ...barrier("taken", "A"),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "getitem-rando",
        description: "randomizer game mode: A's chest holds a Poe soul; B's puppet of A holds up the model from the randomizer's item table, with its clips, and no hook conflict",
        timeoutSec: 420,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        rejectLog: [/conflict/i],
        instances: chestPair(POE_SOUL).map((inst) => ({
            ...inst,
            start: undefined,
            steps: [
                ...connect,
                ...confirmUnverified,
                warp(STAGES.southFaron),
                ...inst.steps.slice(connect.length),
            ],
        })),
    },
];
