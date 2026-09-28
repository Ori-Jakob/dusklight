// Item pickup toasts (src/ui/ItemToasts.cpp) with the item's icon from the host's item:// provider.
// Steps in src/autotest/StepsItemToast.cpp. The mod logs "[toast] shown" / "[toast] summary" when
// a toast goes on screen: capture triggers.

const { STAGES, ITEMS, COMMON_CVARS, tt, warp, waitStage, barrier, meetIn, connect } = require("../lib");

// dItemNo_* values (include/d/d_item_data.h) beyond lib.js ITEMS.
const I = {
    ...ITEMS,
    spinner: 0x41,
    ironBall: 0x42,
    bow: 0x43,
    ironBoots: 0x45,
    dominionRod: 0x46,
    lantern: 0x48,
    smallKey: 0x20,
    heartPiece: 0x21,
    map: 0x23,
    sword: 0x28,
    bottle: 0x60,
};

// B picks things up in a colour of its own, so a capture shows the name coloured.
const B_COLOR = [tt("color", "e05a2b")];

module.exports = [
    {
        name: "item-toast-selftest",
        description: "item icons and names from the game files, then three toasts drawn with their icons",
        timeoutSec: 180,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                steps: [
                    waitStage(STAGES.linksHouse),
                    { op: "itemToastSelfTest" },
                    { op: "testItemToast", item: I.hookshot, name: "Dad", r: 40, g: 110, b: 230 },
                    { op: "expectItemToast", kind: "item", from: "Dad", item: I.hookshot, textContains: "Dad got Clawshot", rendered: true, timeoutSec: 20 },
                    { op: "mark", msg: "ITEMTOAST_HOLD clawshot" },
                    { op: "wait", sec: 4 },
                    // Black is lightened to read on the toast; the bottle is two layers.
                    { op: "testItemToast", item: I.bottle, name: "Shade", r: 0, g: 0, b: 0 },
                    { op: "expectItemToast", kind: "item", from: "Shade", item: I.bottle, rendered: true, timeoutSec: 20 },
                    { op: "mark", msg: "ITEMTOAST_HOLD bottle" },
                    { op: "wait", sec: 4 },
                    // Full HD: the toast scales with the UI; a count and another dungeon's name.
                    { op: "resizeWindow", width: 1920, height: 1080 },
                    { op: "testItemToast", item: I.smallKey, name: "Dad", r: 40, g: 110, b: 230, count: 2, saveTbl: 17 },
                    { op: "expectItemToast", kind: "item", from: "Dad", item: I.smallKey, textContains: "Small Key ×2 (Goron Mines)", rendered: true, timeoutSec: 20 },
                    { op: "mark", msg: "ITEMTOAST_HOLD full-hd" },
                    { op: "wait", sec: 4 },
                    { op: "dumpItemToasts" },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-toast-live",
        description: "a teammate's pickups toast with icon and name: an item, a key and a map in another dungeon, and one we already had",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "clearItemToasts" },
                    ...barrier("ready", "B"),
                    { op: "expectItem", item: I.hookshot },
                    { op: "expectItemToast", kind: "item", from: "B", item: I.hookshot, textContains: "B got Clawshot", rendered: true },
                    { op: "wait", sec: 2 },
                    ...barrier("saw-hookshot", "B"),
                    // B picks them up in the Forest Temple; we stay in Link's house.
                    { op: "expectItemToast", kind: "item", from: "B", item: I.smallKey, textContains: "Small Key (Forest Temple)", timeoutSec: 120 },
                    { op: "expectItemToast", kind: "item", from: "B", item: I.map, textContains: "(Forest Temple)", timeoutSec: 60 },
                    ...barrier("saw-dungeon", "B", 180),
                    // Already ours since the start (armed): still news about the teammate.
                    { op: "expectItemToast", kind: "item", from: "B", item: I.sword, textContains: "B got" },
                    { op: "expectItemToastCount", kind: "item", from: "B", count: 4 },
                    { op: "expectNoItemToast", kind: "summary", forSec: 0 },
                    { op: "dumpItemToasts" },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                cvars: B_COLOR,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("ready", "A"),
                    { op: "giveItem", item: I.hookshot },
                    ...barrier("saw-hookshot", "A"),
                    warp(STAGES.forestTemple),
                    waitStage(STAGES.forestTemple),
                    // Past the post-load world-state publish.
                    { op: "wait", sec: 3 },
                    { op: "giveItem", item: I.smallKey },
                    // The HUD adds a picked-up key over a few frames.
                    { op: "wait", sec: 2 },
                    { op: "giveItem", item: I.map },
                    ...barrier("saw-dungeon", "A", 180),
                    { op: "giveItem", item: I.sword },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-toast-replay",
        description: "items a teammate found before we joined arrive from the server queue as one summary naming them, though they left",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                // Plays alone and quits before A starts: A only learns of it from the server.
                name: "B",
                launchDelayMs: 45000,
                cvars: B_COLOR,
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    // Past our own-state publish, so the items go to the team queue.
                    { op: "wait", sec: 3 },
                    ...[I.boomerang, I.slingshot, I.hookshot, I.spinner, I.ironBoots, I.lantern]
                        .map((item) => ({ op: "giveItem", item })),
                    { op: "wait", sec: 3 },
                    { op: "quit" },
                ],
            },
            {
                name: "A",
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    ...[I.boomerang, I.slingshot, I.hookshot, I.spinner, I.ironBoots, I.lantern]
                        .map((item) => ({ op: "expectItem", item, timeoutSec: 60 })),
                    { op: "expectItemToast", kind: "summary", from: "B", fromPacket: true, minCount: 6, textContains: "Synced 6 items from B", rendered: true },
                    { op: "wait", sec: 2 },
                    { op: "expectNoItemToast", kind: "item", forSec: 6 },
                    { op: "expectItemToastCount", kind: "summary", count: 1 },
                    { op: "dumpItemToasts" },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-toast-reconnect",
        description: "after a reconnect the server replays items we already have: no summary",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    ...barrier("ready", "B"),
                    ...[I.boomerang, I.slingshot, I.spinner].map((item) => ({ op: "expectItem", item })),
                    { op: "expectItemToast", kind: "item", from: "B", item: I.spinner, timeoutSec: 60 },
                    ...barrier("got-all", "B"),
                    { op: "disconnect" },
                    { op: "wait", sec: 2 },
                    ...connect,
                    { op: "waitPeers", count: 1, timeoutSec: 60 },
                    // Signals only reach connected clients: B waits for this one.
                    { op: "signal", name: "back" },
                    { op: "expectNoItemToast", kind: "summary", forSec: 10 },
                    { op: "expectItemToastCount", kind: "item", count: 3 },
                    { op: "dumpItemToasts" },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("ready", "A"),
                    { op: "giveItem", item: I.boomerang },
                    { op: "giveItem", item: I.slingshot },
                    { op: "giveItem", item: I.spinner },
                    ...barrier("got-all", "A"),
                    { op: "waitSignal", name: "back", from: "A", timeoutSec: 120 },
                    ...barrier("done", "A", 180),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-toast-hold",
        description: "a teammate's toast waits while a cutscene runs for us and shows once it ends",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "clearItemToasts" },
                    { op: "beginEvent" },
                    ...barrier("in-event", "B"),
                    { op: "expectItemToast", item: I.boomerang, pushed: false, timeoutSec: 30 },
                    { op: "wait", sec: 3 },
                    { op: "expectItemToast", item: I.boomerang, pushed: false, hold: "cutscene", timeoutSec: 0 },
                    { op: "expectNoItemToast", pushed: true, forSec: 0 },
                    { op: "endEvent" },
                    { op: "expectItemToast", item: I.boomerang, pushed: true, timeoutSec: 10 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("in-event", "A"),
                    { op: "giveItem", item: I.boomerang },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-toast-own",
        description: "our own pickups toast only with item_toasts_own",
        timeoutSec: 180,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "wait", sec: 3 },
                    { op: "clearItemToasts" },
                    { op: "giveItem", item: I.boomerang },
                    { op: "expectNoItemToast", from: "self", forSec: 3 },
                    { op: "itemToastOption", name: "item_toasts_own", value: true },
                    { op: "giveItem", item: I.spinner },
                    { op: "expectItemToast", kind: "own", from: "self", item: I.spinner, textContains: "You got", rendered: true, timeoutSec: 20 },
                    { op: "expectItemToastCount", from: "self", count: 1 },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "item-toast-flood",
        description: "nine pickups in nine ticks: at most five single toasts and one merged toast, every item counted",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "A",
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "clearItemToasts" },
                    ...barrier("ready", "B"),
                    { op: "expectItem", item: I.slingshot, timeoutSec: 30 },
                    { op: "wait", sec: 1 },
                    { op: "expectItemToastCount", kind: "merged", from: "B", count: 1 },
                    // The singles that show; the rest went into the merged toast.
                    { op: "expectItemToastCount", kind: "item", from: "B", mergedAway: false, max: 5 },
                    { op: "expectItemToastItems", from: "B", count: 9 },
                    { op: "expectItemToast", kind: "merged", from: "B", textContains: "B got", timeoutSec: 30 },
                    { op: "dumpItemToasts" },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                cvars: B_COLOR,
                steps: [
                    ...connect,
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("ready", "A"),
                    // One per tick: each step runs on its own main-loop iteration.
                    ...[I.boomerang, I.spinner, I.ironBall, I.bow, I.hookshot, I.ironBoots,
                        I.dominionRod, I.lantern, I.slingshot].map((item) => ({ op: "giveItem", item })),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
