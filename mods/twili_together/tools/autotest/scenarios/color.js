// Player colour and its live sync; COLOR_HOLD <what> marks a moment worth a window capture.

const { STAGES, COMMON_CVARS, tt, barrier, meetIn, connect, waitStage } = require("../lib");

// The minimap has to show for expectMapCursor.
const CVARS = [...COMMON_CVARS, "game.minimalHUD=false", "game.recordingMode=false",
               "game.debugFlyCam=false", "game.enableMirrorMode=false"];
const COLORS = { A: "#13C76D", B: "#FA059B" };
const colorCvars = (hex) => [tt("color", hex.slice(1).toLowerCase())];
const other = (name) => (name === "A" ? "B" : "A");
const hold = (what, sec = 3) => [{ op: "mark", msg: `COLOR_HOLD ${what}` }, { op: "wait", sec }];
const key = (k, count = 1) => ({ op: "pickerKey", key: k, count });
// What a peer shows for `name` in colour `rgb`: client state, dummy tint and map marker.
const seesColor = (name, rgb, timeoutSec = 5) => [
    { op: "expectPeerColor", name, rgb, timeoutSec },
    { op: "expectDummyColor", name, rgb, timeoutSec },
    { op: "expectMapCursor", target: name, color: rgb, timeoutSec },
];

// Picker layout: square, hue bar, presets (8 a row), recent colours, then Hex / Default.
const PRESET = "#7CB342";
const LATE = "#0A64C8";
const bPicks = [
    { op: "openColorPicker" },
    key("down", 2), key("right", 3),
    key("confirm"),
    { op: "expectColor", rgb: PRESET },
    ...barrier("b-preset", "A"),
    ...barrier("a-saw-preset", "A"),
    // A held hue change then a cancel: the last value goes out despite the 250 ms throttle.
    { op: "resetColorPushes" },
    key("up"), key("confirm"),
    { op: "pickerKey", key: "right", holdMs: 3000 },
    { op: "expectColorHsv", hMin: 100, hMax: 200, s: 0.63, v: 0.7, tol: 0.03 },
    key("cancel"),
    { op: "expectColor", rgb: PRESET },
    { op: "wait", frames: 30 },
    // 3 s at <= 4 a second and the cancel.
    { op: "expectColorPushes", min: 3, max: 16 },
    { op: "closeColorPicker" },
    // Our own colour again, from the colours used before.
    { op: "openColorPicker" },
    key("down", 4), key("right"), key("confirm"),
    { op: "expectColor", rgb: COLORS.B },
    { op: "closeColorPicker" },
    { op: "hideWindow" },
    ...barrier("b-reverted", "A"),
];
const aWatches = [
    ...barrier("b-preset", "B"),
    ...seesColor("B", PRESET, 3),
    ...hold("live-preset", 4),
    ...barrier("a-saw-preset", "B"),
    ...barrier("b-reverted", "B"),
    ...seesColor("B", COLORS.B, 3),
];

// B leaves, A changes colour, B comes back: the server's ALL_CLIENT_STATE has the new one.
const bLateJoin = [
    { op: "signal", name: "b-off" },
    { op: "wait", sec: 1 },
    { op: "disconnect" },
    { op: "wait", sec: 4 },
    ...connect,
    { op: "expectPeerColor", name: "A", rgb: LATE, timeoutSec: 5 },
    { op: "waitDummies", count: 1, timeoutSec: 60 },
    ...seesColor("A", LATE, 20),
    { op: "expectSelfRow", saveLoaded: true, stage: STAGES.forestTemple.stage, rgb: COLORS.B },
    { op: "signal", name: "b-back" },
    // The Players tab: the colour squares and our own row, for a capture.
    { op: "showWindow", tab: 2 },
    ...barrier("players-tab", "A"),
    ...barrier("players-shot", "A"),
    { op: "hideWindow" },
];
const aLateJoin = [
    { op: "waitSignal", name: "b-off", from: "B", timeoutSec: 60 },
    { op: "wait", sec: 2 },
    { op: "setColor", rgb: LATE },
    { op: "expectSelfRow", rgb: LATE },
    { op: "waitSignal", name: "b-back", from: "B", timeoutSec: 120 },
    ...barrier("players-tab", "B"),
    ...hold("players-tab", 4),
    ...barrier("players-shot", "B"),
];

module.exports = [
    {
        name: "window-tour",
        description: "the Twili-Together window's three tabs while connected, for captures (manual)",
        manualOnly: true,
        timeoutSec: 180,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                start: STAGES.linksHouse,
                cvars: colorCvars(COLORS.B),
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    ...[0, 1, 2].flatMap((tab) => [{ op: "showWindow", tab }, ...hold(`tab${tab}`, 2)]),
                    { op: "hideWindow" },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "color-picker",
        description: "solo: colour maths over all 2^24 colours, then the host colour picker by keys (grab, undo, keep, hue bar, held key), our twelve presets, Default, the colours used before, and the saved setting",
        timeoutSec: 240,
        cvars: COMMON_CVARS,
        instances: [
            {
                name: "solo",
                start: STAGES.linksHouse,
                steps: [
                    waitStage(STAGES.linksHouse),
                    { op: "colorMathSelfTest" },
                    { op: "setColor", rgb: "#FFFFFF" },
                    { op: "openColorPicker" },
                    ...hold("white"),
                    // Grab the square: saturation right, value down.
                    key("confirm"),
                    key("right", 51), key("down", 51),
                    { op: "expectColor", rgb: "#CCA3A3" },
                    { op: "expectColorHsv", h: 0, s: 0.2, v: 0.8 },
                    ...hold("grabbed"),
                    key("cancel"),  // undo to the grab's start
                    { op: "expectColor", rgb: "#FFFFFF" },
                    key("confirm"), key("right", 255), key("confirm"),  // keep
                    { op: "expectColor", rgb: "#FF0000" },
                    key("left"),  // not grabbed: no change
                    { op: "expectColor", rgb: "#FF0000" },
                    // The hue bar, 0 degrees on the left.
                    key("down"), key("confirm"),
                    key("right", 60),
                    { op: "expectColor", rgb: "#FFFF00" },
                    { op: "pickerKey", key: "right", holdMs: 2500 },
                    { op: "expectColorHsv", hMin: 100, hMax: 200, s: 1, v: 1 },
                    key("confirm"),
                    // Presets: ours, eight a row.
                    key("down"), key("confirm"),
                    { op: "expectColor", rgb: "#E53935" },
                    ...hold("preset"),
                    key("right", 3), key("confirm"),
                    { op: "expectColor", rgb: "#7CB342" },
                    // Row two, then Default in the footer, which is white for us.
                    key("down"), key("down"), key("right"), key("confirm"),
                    { op: "expectColor", rgb: "#FFFFFF" },
                    key("cancel"),  // closes, keeping the colour
                    { op: "expectColor", rgb: "#FFFFFF" },
                    // The colours used before, from the second opening on.
                    { op: "setColor", rgb: "#3FA34D" },
                    { op: "openColorPicker" },
                    key("down", 2), key("confirm"),
                    { op: "expectColor", rgb: "#E53935" },
                    { op: "closeColorPicker" },
                    { op: "openColorPicker" },
                    key("down", 4), key("right"),
                    ...hold("recent"),
                    key("confirm"),
                    { op: "expectColor", rgb: "#3FA34D" },
                    { op: "closeColorPicker" },
                    // The full-size window, for a capture.
                    { op: "resizeWindow", width: 1280, height: 720 },
                    { op: "openColorPicker" },
                    ...hold("large"),
                    { op: "closeColorPicker" },
                    ...hold("tag", 2),
                    { op: "hideWindow" },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "color-live",
        description: "dungeon: B changes its colour in the picker mid-session and A's client entry, dummy tint and map marker follow within 3 s; the updates are throttled and the last one still goes out; a rejoining player gets the colour from the server; own row shows our save and stage",
        timeoutSec: 420,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.forestTemple,
            cvars: colorCvars(COLORS[name]),
            steps: [
                { op: "waitStage", stage: STAGES.forestTemple.stage, timeoutSec: 90 },
                { op: "showMinimap" },
                ...connect,
                ...meetIn(STAGES.forestTemple, other(name)),
                // Our own row (Players tab): the save and stage we announced, our colour.
                { op: "expectSelfRow", saveLoaded: true, stage: STAGES.forestTemple.stage, rgb: COLORS[name] },
                ...seesColor(other(name), COLORS[other(name)]),
                ...barrier("start-colors", other(name)),
                ...(name === "B" ? bPicks : aWatches),
                ...(name === "B" ? bLateJoin : aLateJoin),
                ...barrier("color-done", other(name)),
                { op: "quit" },
            ],
        })),
    },
];
