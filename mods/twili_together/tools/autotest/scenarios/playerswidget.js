// Players widget on the overlay and the Players tab rows (src/ui/PlayersWidget.cpp, TwiliWindow.cpp;
// steps in src/autotest/StepsPlayersWidget.cpp).

const { STAGES, COMMON_CVARS, waitStage, barrier, meetIn, connect } = require("../lib");

const other = (name) => (name === "A" ? "B" : "A");
const house = "Link's House";
const enableTeleport = [
    { op: "setRoomOption", name: "teleportMode", value: true },
    { op: "waitRoomOption", name: "teleportMode", value: true, timeoutSec: 20 },
];
const reconnect = (team) => [
    { op: "disconnect" },
    { op: "wait", sec: 2 },
    { op: "connect", team },
    { op: "waitConnected", timeoutSec: 20 },
    { op: "waitPeers", count: 1, timeoutSec: 60 },
];
// On the header while the widget sits in the top left corner (UI scales 1 to 1.5).
const header = [40, 30];
// The first row's teleport button with the widget in the top right corner of an 800x450 window
// (UI scale 1): 12 inset, 1 border, 6 padding, half the 18 wide button; rows start below the header.
const tpButton = [772, 45];
// Long names and three areas, for the widget's size; it shows 12 characters of a name and "...".
const TRIO = [
    { name: "A", peer: "Aurelia Stormwind", label: "Aurelia Stor...", start: STAGES.linksHouse, area: house },
    { name: "B", peer: "Émilie-Françoise Noé", label: "Émilie-Franç...", start: STAGES.southFaron, area: "South Faron Woods" },
    { name: "C", peer: "Cy", label: "Cy", start: STAGES.faronField, area: "Hyrule Field" },
];
// A real drag out of the top left corner works out the widget's size (UI scale 1, 800x450 window:
// 776x426 of free space); `grab` is on its header at that size setting.
const measure = (scale, more) => ({
    op: "measurePlayersWidget", grab: [12 + 0.2 * scale, 12 + 0.13 * scale], by: [100, 50], area: [776, 426], ...more,
});
const widgetScale = (value) => [
    { op: "playersWidgetOption", name: "players_widget_scale", value },
    { op: "expectPlayersWidget", scale: value, count: 2, holdTicks: 3 },
];
const trioBarrier = (signal, name, timeoutSec = 120) => [
    { op: "signal", name: signal },
    ...TRIO.filter((t) => t.name !== name).map((t) => ({ op: "waitSignal", name: signal, from: t.name, timeoutSec })),
];

module.exports = [
    {
        name: "players-widget",
        description: "the overlay players widget and the Players tab rows: teleport buttons blocked then ready, drag only with the menu open, a click teleports with the menu closed and open, teams list only teammates",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: ["A", "B"].map((name) => ({
            name,
            start: STAGES.linksHouse,
            steps: [
                ...connect,
                ...meetIn(STAGES.linksHouse, other(name)),
                { op: "expectPlayersWidget", header: "Players", count: 1, holdTicks: 10,
                  rows: [{ name: other(name), area: house, teleport: "blocked", tooltip: "teleporting is off in this room" }] },
                { op: "showWindow", tab: 2 },
                { op: "expectPlayersTab", count: 2,
                  rows: [{ name: other(name), teleport: "blocked", tooltip: "teleporting is off in this room" }] },
                { op: "hideWindow" },
                ...enableTeleport,
                { op: "expectPlayersWidget", count: 1, holdTicks: 10,
                  rows: [{ name: other(name), teleport: "ready", tooltip: `Teleport to ${other(name)}` }] },
                ...barrier("widget-listed", other(name)),
                ...(name === "A"
                    ? [
                        { op: "mark", msg: "PW_SHOT closed" },
                        { op: "wait", sec: 3 },
                        // Passive without the menu: a drag on the header does nothing.
                        { op: "movePlayersWidget", x: 0, y: 0 },
                        { op: "expectPlayersWidget", x: 0, y: 0 },
                        { op: "mouseDrag", from: header, to: [300, 220], frames: 12 },
                        { op: "wait", frames: 10 },
                        { op: "expectPlayersWidget", x: 0, y: 0, holdTicks: 5 },
                        // Still closed: the teleport button takes a real click (UiService 2.5).
                        { op: "expectPlayersWidget", clickableInGame: true },
                        { op: "movePlayersWidget", x: 1, y: 0 },
                        { op: "wait", frames: 5 },
                        { op: "mouseMove", x: tpButton[0], y: tpButton[1] },
                        { op: "wait", frames: 10 },
                        { op: "mark", msg: "PW_SHOT closed-tip" },
                        { op: "wait", sec: 2 },
                        { op: "clickPlayersWidget", target: "B", at: tpButton, expect: "local", timeoutSec: 30 },
                        { op: "expectBeside", target: "B", timeoutSec: 10 },
                        // Past the teleport cooldown, and back where the drag below grabs it.
                        { op: "wait", sec: 3 },
                        { op: "movePlayersWidget", x: 0, y: 0 },
                        { op: "showWindow", tab: 2 },
                        { op: "expectPlayersTab", count: 2, rows: [{ name: "B", teleport: "ready", tooltip: "Teleport to B" }] },
                        { op: "mouseDrag", from: header, to: [300, 220], frames: 12 },
                        { op: "expectPlayersWidget", minX: 100, minY: 100 },
                        { op: "mark", msg: "PW_SHOT open" },
                        { op: "wait", sec: 3 },
                        // Top right: B's button is the last in the first row, the pointer on it shows the tooltip.
                        { op: "movePlayersWidget", x: 1, y: 0 },
                        { op: "wait", frames: 5 },
                        { op: "mouseMove", x: tpButton[0], y: tpButton[1] },
                        { op: "wait", frames: 10 },
                        { op: "mark", msg: "PW_SHOT tip" },
                        { op: "wait", sec: 2 },
                        { op: "clickPlayersWidget", target: "B", at: tpButton, expect: "local", timeoutSec: 30 },
                        { op: "expectNear", target: "B", maxDist: 150, timeoutSec: 10 },
                        { op: "hideWindow" },
                    ]
                    : [{ op: "wait", sec: 2 }]),
                ...barrier("click-done", other(name), 180),
                // Different teams: nobody to list, and no button for the other team's player.
                ...reconnect(name === "A" ? "red" : "blue"),
                { op: "expectPlayersWidget", shown: false, gate: "empty", holdTicks: 10 },
                { op: "showWindow", tab: 2 },
                { op: "expectPlayersTab", count: 2, rows: [{ name: other(name), teleport: "none" }] },
                { op: "hideWindow" },
                ...barrier("teams-apart", other(name)),
                ...reconnect("red"),
                { op: "expectPlayersWidget", header: "Team red", count: 1, holdTicks: 10,
                  rows: [{ name: other(name), area: house, teleport: "ready" }] },
                ...(name === "A" ? [{ op: "mark", msg: "PW_SHOT team" }, { op: "wait", sec: 3 }] : []),
                ...barrier("done", other(name)),
                { op: "quit" },
            ],
        })),
    },
    {
        name: "players-widget-trio",
        description: "three players with long names in three areas: the widget sizes to its rows and shortens long names, the opacity, size and toggle settings apply live, then a team lists only its members",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        instances: TRIO.map(({ name, peer, start, area }) => {
            const others = TRIO.filter((t) => t.name !== name);
            return {
                name,
                start,
                steps: [
                    { op: "connect", name: peer },
                    { op: "waitConnected", timeoutSec: 20 },
                    waitStage(start),
                    { op: "waitPeers", count: 2, timeoutSec: 90 },
                    ...enableTeleport,
                    { op: "expectPlayersWidget", header: "Players", count: 2, opacity: 80, holdTicks: 10,
                      rows: others.map((o) => ({ name: o.peer, label: o.label, area: o.area, teleport: "ready" })) },
                    ...trioBarrier("trio-listed", name),
                    ...(name === "A"
                        ? [
                            { op: "mark", msg: "PW_SHOT trio" },
                            { op: "wait", sec: 3 },
                            { op: "movePlayersWidget", x: 0, y: 0.3 },
                            { op: "mark", msg: "PW_SHOT trio-left" },
                            { op: "wait", sec: 3 },
                            { op: "movePlayersWidget", x: 1, y: 0.5 },
                            { op: "playersWidgetOption", name: "players_widget_opacity", value: 35 },
                            { op: "expectPlayersWidget", opacity: 35, count: 2 },
                            { op: "mark", msg: "PW_SHOT trio-35" },
                            { op: "wait", sec: 3 },
                            { op: "playersWidgetOption", name: "players_widget_opacity", value: 100 },
                            { op: "expectPlayersWidget", opacity: 100, count: 2 },
                            { op: "showWindow", tab: 2 },
                            { op: "expectPlayersTab", count: 3,
                              rows: others.map((o) => ({ name: o.peer, teleport: "ready" })) },
                            { op: "mark", msg: "PW_SHOT trio-open" },
                            { op: "wait", sec: 3 },
                            // The size setting zooms the whole widget; its spot on screen stays put.
                            measure(100, { as: "base" }),
                            ...widgetScale(200),
                            { op: "expectPlayersWidget", x: 1000, y: 500 },
                            measure(200, { of: "base", ratio: [1.8, 2.2] }),
                            ...widgetScale(50),
                            measure(50, { of: "base", ratio: [0.4, 0.6] }),
                            { op: "hideWindow" },
                            { op: "mark", msg: "PW_SHOT trio-50" },
                            { op: "wait", sec: 3 },
                            ...widgetScale(200),
                            { op: "mark", msg: "PW_SHOT trio-200" },
                            { op: "wait", sec: 3 },
                            { op: "movePlayersWidget", x: 1, y: 1 },
                            { op: "mark", msg: "PW_SHOT trio-200-corner" },
                            { op: "wait", sec: 3 },
                            ...widgetScale(100),
                            { op: "movePlayersWidget", x: 1, y: 0.5 },
                            { op: "playersWidgetOption", name: "players_widget", value: false },
                            { op: "expectPlayersWidget", shown: false, gate: "off", holdTicks: 5 },
                            { op: "playersWidgetOption", name: "players_widget", value: true },
                            { op: "playersWidgetOption", name: "players_widget_opacity", value: 80 },
                            { op: "expectPlayersWidget", count: 2, opacity: 80 },
                        ]
                        : [{ op: "wait", sec: 2 }]),
                    ...trioBarrier("trio-shots", name, 180),
                    // B and C form a team: they list only each other; A without a team lists both
                    // but gets no teleport button for another team's players.
                    ...(name === "A"
                        ? [{ op: "wait", sec: 6 },
                           { op: "expectPlayersWidget", header: "Players", count: 2, holdTicks: 10,
                             rows: others.map((o) => ({ name: o.peer, teleport: "none" })) }]
                        : [{ op: "disconnect" }, { op: "wait", sec: 2 }, { op: "connect", name: peer, team: "red" },
                           { op: "waitConnected", timeoutSec: 20 }, { op: "waitPeers", count: 2, timeoutSec: 60 },
                           { op: "expectPlayersWidget", header: "Team red", count: 1, holdTicks: 10,
                             rows: [{ name: others.find((o) => o.name !== "A").peer, teleport: "ready" }],
                             absent: [TRIO[0].peer] }]),
                    ...(name === "B" ? [{ op: "mark", msg: "PW_SHOT trio-team" }, { op: "wait", sec: 3 }] : []),
                    ...trioBarrier("trio-done", name),
                    { op: "quit" },
                ],
            };
        }),
    },
];
