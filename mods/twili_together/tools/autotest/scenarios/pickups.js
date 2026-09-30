// Chest sync, shared pickups, randomizer items and traps, and toast batching.

const { MODS, STAGES, ITEMS, COMMON_CVARS, RANDOMIZER_MODE_CVARS, warp, waitStage, barrier, meetIn, connect } =
    require("../lib");

// dItemNo_* values (include/d/d_item_data.h) and randomizer ids (mods/randomizer/src/item_ids.h).
const I = {
    ...ITEMS,
    heart: 0x00, redRupee: 0x04, silverRupee: 0x07, arrows10: 0x0e, halfMilkBottle: 0x65, poeSoul: 0xe0,
    randoTrap: 0x13, randoOrdonPortal: 0x14, randoForestKey: 0x85,
};
// A TBOX number no chest of the stage table uses; South Faron is stage table 2.
const TBOX = 62;
const FARON_TBL = 2;
const FOREST_TBL = 0x10;

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

module.exports = [
    {
        name: "chest-sync",
        description: "A opens a chest; B's copy of it (B stands right at it) opens by itself with no event or item, cannot be opened again, and the bit is in B's save after leaving",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                steps: [
                    ...connect,
                    ...noPickups,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "spawnChest", tag: "c", tbox: TBOX, item: I.redRupee },
                    { op: "wait", sec: 2 },
                    ...barrier("chests", "B"),
                    { op: "openChest", tag: "c" },
                    { op: "expectChestOpen", tag: "c" },
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
                    { op: "setInventory", rupees: 0 },
                    { op: "spawnChest", tag: "c", tbox: TBOX, item: I.redRupee },
                    { op: "wait", sec: 2 },
                    { op: "expectChestOpen", tag: "c", open: false, forSec: 0 },
                    ...barrier("chests", "A"),
                    ...barrier("opened", "A"),
                    { op: "expectTbox", no: TBOX },
                    { op: "expectChestOpen", tag: "c", timeoutSec: 10 },
                    { op: "openChest", tag: "c", refused: true, frames: 90 },
                    { op: "expectInventory", rupees: 0, forSec: 3 },
                    warp(STAGES.linksHouse),
                    waitStage(STAGES.linksHouse),
                    { op: "expectTbox", no: TBOX, tbl: FARON_TBL },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "chest-sync-away",
        description: "A opens a chest while B is in another stage; B's chest with that bit is open when B gets there",
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
                    { op: "spawnChest", tag: "c", tbox: TBOX, item: I.redRupee },
                    { op: "wait", sec: 2 },
                    { op: "openChest", tag: "c" },
                    ...barrier("opened", "B"),
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
                    { op: "waitPeers", count: 1, timeoutSec: 90 },
                    ...barrier("ready", "A"),
                    ...barrier("opened", "A"),
                    { op: "expectTbox", no: TBOX, tbl: FARON_TBL },
                    warp(STAGES.southFaron),
                    waitStage(STAGES.southFaron),
                    { op: "expectTbox", no: TBOX },
                    { op: "spawnChest", tag: "c", tbox: TBOX, item: I.redRupee },
                    { op: "expectChestOpen", tag: "c", timeoutSec: 10 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "pickup-share",
        description: "A's rupees, heart and arrows are added for B within B's limits (wallet, hearts, a bow), never echoed back, and not with Share Pickups off",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "setInventory", rupees: 0 },
                    ...barrier("set", "B"),
                    { op: "giveItem", item: I.redRupee },
                    { op: "giveItem", item: I.heart },
                    { op: "giveItem", item: I.arrows10 },
                    ...barrier("round1", "B"),
                    // Our own pickup counts once: no echo from B.
                    { op: "expectInventory", rupees: 20, forSec: 3 },
                    { op: "expectPickups", sent: 3, applied: 0 },
                    ...barrier("b-has-bow", "B"),
                    { op: "giveItem", item: I.arrows10 },
                    { op: "giveItem", item: I.silverRupee },
                    ...barrier("round2", "B"),
                    { op: "setRoomOption", name: "sharePickups", value: false },
                    { op: "waitRoomOption", name: "sharePickups", value: false },
                    ...barrier("off", "B"),
                    { op: "giveItem", item: I.redRupee },
                    ...barrier("round3", "B"),
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    { op: "setInventory", rupees: 0, life: 8, arrows: 0 },
                    ...barrier("set", "A"),
                    ...barrier("round1", "A"),
                    // No bow yet: the arrows are skipped.
                    { op: "expectInventory", rupees: 20, life: 12, arrows: 0 },
                    { op: "expectPickups", applied: 2, skipped: 1, rupees: 20, life: 4, arrows: 0 },
                    // The bow comes with a full quiver.
                    { op: "giveItem", item: I.bow },
                    { op: "wait", sec: 1 },
                    { op: "setInventory", rupees: 290, arrows: 0 },
                    ...barrier("b-has-bow", "A"),
                    ...barrier("round2", "A"),
                    // 290 + 200 stops at the 300 wallet.
                    { op: "expectInventory", arrows: 10, rupees: 300 },
                    ...barrier("off", "A"),
                    ...barrier("round3", "A"),
                    { op: "expectInventory", rupees: 300, forSec: 3 },
                    { op: "expectPickups", applied: 4, forSec: 0 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "pickup-no-replay",
        description: "pickups made before a teammate connects are not handed out on its catch-up",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                launchDelayMs: 45000,
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "wait", sec: 3 },
                    { op: "giveItem", item: I.silverRupee },
                    { op: "giveItem", item: ITEMS.hookshot },
                    { op: "wait", sec: 3 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    waitStage(STAGES.linksHouse),
                    { op: "setInventory", rupees: 0 },
                    ...connect,
                    { op: "expectItem", item: ITEMS.hookshot, timeoutSec: 60 },
                    { op: "expectInventory", rupees: 0, forSec: 5 },
                    { op: "expectPickups", applied: 0, rupees: 0 },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "rando-item-sync",
        description: "randomizer game mode: a portal, two dungeon small keys, two Poe souls and a filled bottle reach the teammate exactly once, with no echo",
        timeoutSec: 360,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        rejectLog: [/conflict/i],
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...confirmUnverified,
                    ...meetIn(STAGES.linksHouse, "B"),
                    ...barrier("ready", "B"),
                    { op: "giveItem", item: I.randoOrdonPortal },
                    { op: "giveItem", item: I.randoForestKey },
                    { op: "giveItem", item: I.randoForestKey },
                    { op: "giveItem", item: I.poeSoul },
                    { op: "giveItem", item: I.poeSoul },
                    { op: "giveItem", item: I.halfMilkBottle },
                    ...barrier("given", "B"),
                    { op: "expectInventory", tbl: FOREST_TBL, keys: 2, poes: 2, bottles: 1, forSec: 5 },
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
                    ...barrier("ready", "A"),
                    ...barrier("given", "A"),
                    { op: "expectItem", item: I.randoOrdonPortal, timeoutSec: 30 },
                    { op: "expectInventory", tbl: FOREST_TBL, keys: 2, poes: 2, bottles: 1, timeoutSec: 30 },
                    { op: "expectInventory", tbl: FOREST_TBL, keys: 2, poes: 2, bottles: 1, forSec: 5 },
                    { op: "expectPickups", min: { grantsApplied: 6 } },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "rando-trap-spread",
        description: "randomizer game mode: a trap A picks up goes off for B through B's randomizer, once, and never comes back to A",
        timeoutSec: 300,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        rejectLog: [/conflict/i],
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...confirmUnverified,
                    ...meetIn(STAGES.linksHouse, "B"),
                    ...barrier("ready", "B"),
                    { op: "giveItem", item: I.randoTrap },
                    { op: "expectPickups", trapsSent: 1 },
                    ...barrier("sprung", "B"),
                    { op: "expectPickups", trapsSent: 1, trapsReceived: 0, trapsApplied: 0, forSec: 5 },
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
                    ...barrier("ready", "A"),
                    ...barrier("sprung", "A"),
                    { op: "expectPickups", trapsReceived: 1, trapsApplied: 1, trapsSent: 0, timeoutSec: 30 },
                    { op: "expectPickups", trapsReceived: 1, trapsApplied: 1, trapsSent: 0, forSec: 5 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-toast-burst",
        description: "three item toasts in one tick go out together at once, not one after another",
        timeoutSec: 120,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                steps: [
                    waitStage(STAGES.linksHouse),
                    { op: "clearItemToasts" },
                    { op: "testItemToast", item: ITEMS.boomerang, name: "Dad" },
                    { op: "wait", frames: 2 },
                    { op: "testItemToast", item: ITEMS.hookshot, name: "Mom" },
                    { op: "testItemToast", item: ITEMS.lantern, name: "Dad" },
                    { op: "testItemToast", item: ITEMS.spinner, name: "Mom" },
                    // Behind the first, all three wait one toast at most (3.5 s plus the fade).
                    { op: "expectItemToast", item: ITEMS.spinner, pushed: true, timeoutSec: 5 },
                    { op: "expectItemToast", item: ITEMS.hookshot, pushed: true, timeoutSec: 0 },
                    { op: "expectItemToast", item: ITEMS.lantern, pushed: true, timeoutSec: 0 },
                    { op: "dumpItemToasts" },
                    { op: "quit" },
                ],
            },
        ],
    },
];
