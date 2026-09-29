// Map markers, checked against each instance's literal colour (low bits set to catch RGB555).

const { STAGES, COMMON_CVARS, tt, barrier, meetIn, connect } = require("../lib");

// Nothing in the user's config may hide the HUD or mirror the map behind a test's back.
const CVARS = [...COMMON_CVARS, "game.minimalHUD=false", "game.recordingMode=false",
               "game.debugFlyCam=false"];
const NORMAL = [...CVARS, "game.enableMirrorMode=false"];
const MIRROR = [...CVARS, "game.enableMirrorMode=true"];

const COLORS = { A: "#13C76D", B: "#FA059B" };
const colorCvars = (hex) => [tt("color", hex.slice(1).toLowerCase())];
const other = (name) => (name === "A" ? "B" : "A");
// Every instance sets it: whichever one owns the room pushes it.
const room = (name, value) => [
    { op: "setRoomOption", name, value },
    { op: "waitRoomOption", name, value, timeoutSec: 20 },
];
const seesPeer = (name, extra = {}) =>
    ({ op: "expectMapCursor", target: other(name), color: COLORS[other(name)], ...extra });

// Twelve injected players with ids equal mod 6 in a ring, plus a far one pinned to the edge.
const RING = Array.from({ length: 12 }, (_, i) => ({
    id: 600 + 6 * i,
    color: "#" + [0x17 + 19 * i, 0xEF - 17 * i, 0x2B + 23 * i]
        .map((v) => ((v & 0xFF) | 7).toString(16).padStart(2, "0").toUpperCase()).join(""),
    dx: Math.round(22 * Math.cos((i * Math.PI) / 6)),
    dz: Math.round(22 * Math.sin((i * Math.PI) / 6)),
    angle: (i * 0x1555) & 0xFFFF,
}));
const FAR = { id: 996, color: "#3F07F7", dx: 900, dz: -40, angle: 0 };
// Facing on screen: angle 0x4000 is +X (right, left when mirrored), 0 is +Z (down).
const EAST = { id: 998, color: "#F7F707", dx: 0, dz: -30, angle: 0x4000 };
const SOUTH = { id: 999, color: "#07F7F7", dx: -30, dz: 0, angle: 0 };
const INJECTED = [...RING, FAR, EAST, SOUTH];
// Light and dark colours a few texels off our arrow, for the outline.
const CONTRAST = [
    { id: 700, color: "#FFFFFF", dx: 10, dz: 0, angle: 0x4000 },
    { id: 701, color: "#070707", dx: -10, dz: 0, angle: 0xC000 },
    { id: 702, color: "#5E3DFA", dx: 0, dz: -10, angle: 0x8000 },
];
// The pause map is drawn at a much smaller scale than the minimap.
const CONTRAST_FAR = CONTRAST.map((c) => ({ ...c, dx: c.dx * 5, dz: c.dz * 5 }));
const injected = (mirror) => [
    { op: "injectMapCursorClients", unit: "texel", clients: INJECTED },
    {
        op: "expectMapCursor",
        targets: ["B", ...INJECTED.map((c) => `#${c.id}`)],
        colors: [COLORS.B, ...INJECTED.map((c) => c.color)],
        count: 1 + INJECTED.length,
        holdTicks: 30,
    },
    { op: "expectMapCursor", target: `#${FAR.id}`, color: FAR.color, pinned: true },
    { op: "expectMapCursor", target: `#${EAST.id}`, pinned: false, facing: [mirror ? -1 : 1, 0] },
    { op: "expectMapCursor", target: `#${SOUTH.id}`, pinned: false, facing: [0, 1] },
    { op: "checkMapCursorTransform" },
    { op: "dumpMapCursors" },
    // Held for window captures.
    { op: "mark", msg: "MAPCURSOR_HOLD injected" },
    { op: "wait", frames: 150 },
    { op: "clearMapCursorClients" },
];

module.exports = [
    {
        name: "map-cursor-color",
        description: "dungeon minimap: each player sees the other's marker in its exact configured colour at the right place, the map palette stays untouched, a wolf keeps its marker",
        timeoutSec: 360,
        cvars: NORMAL,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.forestTemple,
            cvars: colorCvars(COLORS[name]),
            steps: [
                { op: "waitStage", stage: STAGES.forestTemple.stage, timeoutSec: 90 },
                { op: "showMinimap" },
                { op: "mapCursorSelfTest" },
                { op: "expectMapPaletteClean", holdTicks: 10 },
                ...connect,
                ...meetIn(STAGES.forestTemple, other(name)),
                seesPeer(name, { holdTicks: 30 }),
                { op: "checkMapCursorTransform", target: other(name) },
                { op: "injectMapCursorClients", unit: "texel", clients: CONTRAST },
                {
                    op: "expectMapCursor",
                    targets: [other(name), ...CONTRAST.map((c) => `#${c.id}`)],
                    colors: [COLORS[other(name)], ...CONTRAST.map((c) => c.color)],
                    holdTicks: 15,
                },
                { op: "checkMapCursorTransform" },
                { op: "expectMapPaletteClean", holdTicks: 60 },
                { op: "dumpMapCursors" },
                { op: "mark", msg: "MAPCURSOR_HOLD color" },
                { op: "wait", frames: 150 },
                { op: "clearMapCursorClients" },
                ...barrier("checked-color", other(name)),
                ...(name === "B"
                    ? [{ op: "transform", form: "wolf", timeoutSec: 30 }]
                    : [{ op: "expectPeerFlag", flag: "wolf", set: true, timeoutSec: 30 },
                       seesPeer(name, { holdTicks: 30 })]),
                ...barrier("checked-wolf", other(name)),
                { op: "quit" },
            ],
        })),
    },
    {
        name: "map-cursor-many",
        description: "field minimap: a peer plus 15 injected players (ids equal mod 6) all in their exact colours in one frame, a far one pinned, facings and transform checked",
        timeoutSec: 300,
        cvars: NORMAL,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.southFaron,
            cvars: colorCvars(COLORS[name]),
            steps: [
                ...connect,
                ...meetIn(STAGES.southFaron, other(name)),
                { op: "showMinimap" },
                ...(name === "A" ? injected(false) : [seesPeer(name, { holdTicks: 30 })]),
                ...barrier("checked-many", other(name)),
                { op: "quit" },
            ],
        })),
    },
    {
        name: "map-cursor-lowres",
        description: "map-cursor-many with the high-quality minimap textures off",
        timeoutSec: 300,
        cvars: [...NORMAL, "game.enableHighQualityMinimapTextures=false"],
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.southFaron,
            cvars: colorCvars(COLORS[name]),
            steps: [
                ...connect,
                ...meetIn(STAGES.southFaron, other(name)),
                { op: "showMinimap" },
                ...(name === "A" ? injected(false) : [seesPeer(name, { holdTicks: 30 })]),
                ...barrier("checked-lowres", other(name)),
                { op: "quit" },
            ],
        })),
    },
    {
        name: "map-cursor-mirror",
        description: "mirror mode: the same checks with the minimap mirrored; facings flip",
        timeoutSec: 300,
        cvars: MIRROR,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.southFaron,
            cvars: colorCvars(COLORS[name]),
            steps: [
                ...connect,
                ...meetIn(STAGES.southFaron, other(name)),
                { op: "showMinimap" },
                { op: "mapCursorSelfTest" },
                ...(name === "A" ? injected(true) : [seesPeer(name, { holdTicks: 30 })]),
                ...barrier("checked-mirror", other(name)),
                { op: "quit" },
            ],
        })),
    },
    {
        name: "map-cursor-visibility",
        description: "markers follow Show Locations, the local cutscene rule (the remote's own cutscene keeps it), disconnect and reconnect, and stage changes, each for the stated reason",
        timeoutSec: 480,
        cvars: NORMAL,
        instances: [
            {
                name: "A",
                start: STAGES.southFaron,
                cvars: colorCvars(COLORS.A),
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "B"),
                    { op: "showMinimap" },
                    seesPeer("A"),
                    ...room("showLocationsMode", false),
                    { op: "expectNoMapCursor", target: "B", reason: "locationsOff" },
                    ...room("showLocationsMode", true),
                    seesPeer("A", { holdTicks: 15 }),
                    ...room("hidePlayersInCutscene", true),
                    { op: "forceCutscene", on: true },
                    { op: "expectNoMapCursor", target: "B", reason: "cutscene" },
                    { op: "forceCutscene", on: null },
                    seesPeer("A", { holdTicks: 15 }),
                    ...barrier("b-cutscene", "B"),
                    // B's own cutscene: its marker stays.
                    { op: "expectPeerFlag", flag: "inCutscene", set: true, timeoutSec: 10 },
                    seesPeer("A", { holdTicks: 30 }),
                    ...barrier("b-cutscene-checked", "B"),
                    // B is offline for five seconds (it cannot take part in a barrier then).
                    { op: "expectNoMapCursor", target: "B", reason: "absent", timeoutSec: 30 },
                    // B is back: its colour comes with its new client state.
                    seesPeer("A", { holdTicks: 15, timeoutSec: 60 }),
                    ...barrier("b-back-checked", "B"),
                    { op: "expectNoMapCursor", target: "B", reason: "otherStage", timeoutSec: 60 },
                    // Nothing overlaps our arrow now: a capture shows the map's own one.
                    { op: "dumpMapCursors" },
                    { op: "mark", msg: "MAPCURSOR_HOLD alone" },
                    { op: "wait", frames: 120 },
                    ...barrier("b-away-checked", "B"),
                    seesPeer("A", { holdTicks: 15, timeoutSec: 90 }),
                    ...barrier("b-home-checked", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                cvars: colorCvars(COLORS.B),
                steps: [
                    ...connect,
                    ...meetIn(STAGES.southFaron, "A"),
                    { op: "showMinimap" },
                    seesPeer("B"),
                    ...room("showLocationsMode", false),
                    ...room("showLocationsMode", true),
                    ...room("hidePlayersInCutscene", true),
                    { op: "forceCutscene", on: true },
                    ...barrier("b-cutscene", "A"),
                    ...barrier("b-cutscene-checked", "A"),
                    { op: "forceCutscene", on: null },
                    { op: "disconnect" },
                    { op: "wait", sec: 5 },
                    ...connect,
                    // A's colour here comes from ALL_CLIENT_STATE, as for a late joiner.
                    seesPeer("B", { holdTicks: 15, timeoutSec: 60 }),
                    ...barrier("b-back-checked", "A"),
                    { op: "warp", ...STAGES.linksHouse },
                    { op: "waitStage", stage: STAGES.linksHouse.stage, timeoutSec: 90 },
                    ...barrier("b-away-checked", "A"),
                    { op: "warp", ...STAGES.southFaron },
                    { op: "waitStage", stage: STAGES.southFaron.stage, timeoutSec: 90 },
                    ...barrier("b-home-checked", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "map-cursor-dmap",
        description: "dungeon pause map: the peer's marker in its exact colour where the Link icon would be",
        timeoutSec: 300,
        cvars: NORMAL,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.forestTemple,
            cvars: colorCvars(COLORS[name]),
            steps: [
                ...connect,
                ...meetIn(STAGES.forestTemple, other(name)),
                { op: "showMinimap" },
                ...(name === "A"
                    ? [
                        { op: "injectMapCursorClients", unit: "texel", clients: CONTRAST_FAR },
                        { op: "openPauseMap" },
                        seesPeer(name, { surface: "dmap", holdTicks: 30 }),
                        {
                            op: "expectMapCursor",
                            surface: "dmap",
                            targets: CONTRAST_FAR.map((c) => `#${c.id}`),
                            colors: CONTRAST_FAR.map((c) => c.color),
                        },
                        { op: "dumpMapCursors", surface: "dmap" },
                        { op: "mark", msg: "MAPCURSOR_HOLD dmap" },
                        { op: "wait", frames: 150 },
                        // A B press closes the map (and slides the minimap out).
                        { op: "walk", frames: 1, stickX: 0, stickY: 0, buttons: 0x200 },
                        { op: "wait", frames: 90 },
                        { op: "clearMapCursorClients" },
                    ]
                    : [seesPeer(name, { holdTicks: 30 })]),
                ...barrier("checked-dmap", other(name)),
                { op: "quit" },
            ],
        })),
    },
    {
        name: "map-cursor-fmap",
        description: "field pause map: the peer and injected players in their exact colours, placed like the Link icon (ours lands under it), facings right, hidden with Show Locations off and in a local cutscene",
        timeoutSec: 360,
        cvars: NORMAL,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.faronField,
            cvars: colorCvars(COLORS[name]),
            steps: [
                ...connect,
                ...meetIn(STAGES.faronField, other(name)),
                { op: "showMinimap" },
                ...(name === "A"
                    ? [
                        { op: "injectMapCursorClients", unit: "texel", clients: [...CONTRAST_FAR, EAST, SOUTH] },
                        { op: "openPauseMap", surface: "fmap" },
                        seesPeer(name, { surface: "fmap", holdTicks: 30 }),
                        {
                            op: "expectMapCursor",
                            surface: "fmap",
                            targets: CONTRAST_FAR.map((c) => `#${c.id}`),
                            colors: CONTRAST_FAR.map((c) => c.color),
                        },
                        { op: "expectMapCursor", surface: "fmap", target: `#${EAST.id}`, facing: [1, 0] },
                        { op: "expectMapCursor", surface: "fmap", target: `#${SOUTH.id}`, facing: [0, 1] },
                        { op: "checkFmapLinkIcon" },
                        { op: "dumpMapCursors", surface: "fmap" },
                        { op: "mark", msg: "MAPCURSOR_HOLD fmap" },
                        { op: "wait", frames: 150 },
                    ]
                    : []),
                ...barrier("fmap-open", other(name)),
                ...room("showLocationsMode", false),
                ...(name === "A" ? [{ op: "expectNoMapCursor", surface: "fmap", target: "B", reason: "locationsOff" }] : []),
                ...barrier("fmap-locations", other(name)),
                ...room("showLocationsMode", true),
                ...room("hidePlayersInCutscene", true),
                ...(name === "A"
                    ? [
                        seesPeer(name, { surface: "fmap", holdTicks: 15 }),
                        { op: "forceCutscene", on: true },
                        { op: "expectNoMapCursor", surface: "fmap", target: "B", reason: "cutscene" },
                        { op: "forceCutscene", on: null },
                        seesPeer(name, { surface: "fmap", holdTicks: 15 }),
                        // A B press closes the map.
                        { op: "walk", frames: 1, stickX: 0, stickY: 0, buttons: 0x200 },
                        { op: "wait", frames: 90 },
                        { op: "clearMapCursorClients" },
                    ]
                    : []),
                ...barrier("checked-fmap", other(name)),
                { op: "quit" },
            ],
        })),
    },
];
