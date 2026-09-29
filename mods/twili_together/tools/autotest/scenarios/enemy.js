// Enemy health multiplier and enemy-death sync; steps in StepsEnemy.cpp.

const { STAGES, COMMON_CVARS, waitStage, barrier, connect } = require("../lib");

// Bokoblin type 0 without switches: 40 health (d_a_e_oc.cpp daE_OC_c::create).
const BOKOBLIN = { name: "E_oc", param: 0xffffff00, dy: 30 };
// Tektite, not on the health-scaling allowlist: keeps its 80 whatever the room sets.
const TEKTITE = { name: "E_tt", param: 0xffffffff, dy: 30 };
// Mini Freezard, not on the kill-sync allowlist (its death has no puff and no mark).
const FREEZARD = { name: "E_fz", param: 0xffffffff, dy: 30 };
// Tile Worm with defeated switch 0x13 (its low param byte).
const TILE_WORM = { name: "E_hz", param: 0xffffff13, dy: 0 };
// Shadow Bulblin variant 12 with a club, the Twilit Kargorok's rider; it spawns its E_YC 5000 up.
const KARGOROK_RIDER = { name: "E_rdy", param: 0xffffc1ff, dy: 30 };
// The spawned enemies attack the idle players; keep those alive for the whole run.
const CVARS = [...COMMON_CVARS, "game.infiniteHearts=1"];

// Kill sync matches spawn data: every instance spawns relative to the shared start point.
const HOME = { anchor: "playerHome" };
// Forest Temple entrance: elsewhere Bokoblins drown or fall, or a demo holds kills back.

// Spawns `enemies` ([tag, template, dx, dz, extra]) and waits for any that falls off a ledge.
const spawnAndSettle = (enemies) => [
    ...enemies.map(([tag, t, dx, dz, extra]) => ({ op: "spawnEnemy", ...t, ...HOME, dx, dz, tag, ...extra })),
    ...enemies.map(([tag]) => ({ op: "expectEnemyHealth", tag })),
    { op: "wait", frames: 150 },
    ...enemies.map(([tag]) => ({ op: "expectEnemyHealth", tag, timeoutSec: 0 })),
];

// barrier() for more than two instances.
const barrierAll = (name, others, timeoutSec = 120) => [
    { op: "signal", name },
    ...others.map((from) => ({ op: "waitSignal", name, from, timeoutSec })),
];

// enemy-kill-sync: one of each way to die, plus a plain delete and an enemy off the allowlist.
const KILL_SYNC_ENEMIES = [
    ["real", BOKOBLIN, 0, 250],
    ["za", BOKOBLIN, 250, 250, { setId: 0x20 }],
    ["fall", BOKOBLIN, -250, 250],
    ["del", BOKOBLIN, 250, -250],
    ["tek", TEKTITE, -250, -250],
    ["fz", FREEZARD, 0, -250],
];

const syncOn = (owner) => [
    ...(owner ? [{ op: "setRoomOption", name: "syncNPCs" }] : []),
    { op: "waitRoomOption", name: "syncNPCs", timeoutSec: 30 },
];

module.exports = [
    {
        name: "enemy-health",
        description: "owner sets 200%/300% enemy health: Bokoblins scale on both clients (also one spawned before connecting), a Tektite does not, a get-up scales, damage does not, a disconnect reverts without rounding a live enemy down to 1, a deleted enemy is dropped",
        timeoutSec: 360,
        cvars: CVARS,
        instances: [
            {
                // Connects first, so it owns the room.
                name: "A",
                start: STAGES.southFaron,
                launchDelayMs: 20000,
                steps: [
                    waitStage(STAGES.southFaron),
                    ...connect,
                    { op: "setEnemyHealthPercent", value: 200 },
                    { op: "expectEnemyHealthPercent", value: 200, timeoutSec: 10 },
                    // Barrier signals are only relayed to connected clients.
                    { op: "waitPeers", count: 1, timeoutSec: 120 },
                    ...barrier("health-200", "B"),
                    { op: "spawnEnemy", ...BOKOBLIN, dz: 250, tag: "boko" },
                    { op: "expectEnemyHealth", tag: "boko", health: 80, max: 80, tracked: true },
                    { op: "spawnEnemy", ...TEKTITE, dx: 250, dz: 250, tag: "tek" },
                    { op: "expectEnemyHealth", tag: "tek", health: 80, max: 80, tracked: false },
                    ...barrier("spawned", "B"),
                    { op: "setEnemyHealthPercent", value: 300 },
                    { op: "expectEnemyHealth", tag: "boko", health: 120, max: 120, timeoutSec: 10 },
                    // Damage outside cc_at_check (a wolf bite) is not touched...
                    { op: "setEnemyHealth", tag: "boko", health: 20 },
                    { op: "wait", frames: 5 },
                    { op: "expectEnemyHealth", tag: "boko", health: 20, max: 120, timeoutSec: 0 },
                    // ...but getting back up to the unscaled full health is full scaled health.
                    { op: "setEnemyHealth", tag: "boko", health: 40 },
                    { op: "expectEnemyHealth", tag: "boko", health: 120, max: 120, timeoutSec: 5 },
                    ...barrier("a-done", "B"),
                    // Offline the percent is 100 again.
                    { op: "disconnect" },
                    { op: "expectEnemyHealthPercent", value: 100, timeoutSec: 5 },
                    { op: "expectEnemyHealth", tag: "boko", health: 40, max: 40, timeoutSec: 5 },
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.southFaron,
                steps: [
                    waitStage(STAGES.southFaron),
                    // Spawned offline: vanilla, but tracked so that connecting rescales it.
                    { op: "spawnEnemy", ...BOKOBLIN, dz: 250, tag: "early" },
                    { op: "expectEnemyHealth", tag: "early", health: 40, max: 40, tracked: true },
                    ...connect,
                    { op: "waitPeers", count: 1, timeoutSec: 60 },
                    ...barrier("health-200", "A"),
                    { op: "expectEnemyHealth", tag: "early", health: 80, max: 80, timeoutSec: 10 },
                    { op: "spawnEnemy", ...BOKOBLIN, dx: -250, dz: 250, tag: "boko" },
                    { op: "expectEnemyHealth", tag: "boko", health: 80, max: 80 },
                    ...barrier("spawned", "A"),
                    { op: "expectEnemyHealthPercent", value: 300, timeoutSec: 15 },
                    { op: "expectEnemyHealth", tag: "boko", health: 120, max: 120, timeoutSec: 10 },
                    { op: "expectEnemyHealth", tag: "early", health: 120, max: 120, timeoutSec: 10 },
                    // A deleted enemy leaves the scaling list (the rescale would write to freed memory).
                    { op: "deleteEnemy", tag: "boko" },
                    { op: "expectEnemyGone", tag: "boko", timeoutSec: 5 },
                    { op: "setEnemyHealth", tag: "early", health: 3 },
                    ...barrier("a-done", "A"),
                    // Back at 100%, 3 rounds to 1 (a dying state for some enemies), so it stops at 2.
                    { op: "disconnect" },
                    { op: "expectEnemyHealthPercent", value: 100, timeoutSec: 5 },
                    { op: "expectEnemyHealth", tag: "early", health: 2, max: 40, timeoutSec: 5 },
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "enemy-kill-sync",
        description: "with Sync Enemy Deaths on, Bokoblins A kills (its own death, a puff and delete that sets the zone actor bit, a fall into the void) vanish for B and the zone actor bit is mirrored; a Tektite vanishes too; a plain delete and a Mini Freezard (not allowlisted) do not sync, and nothing echoes back",
        timeoutSec: 300,
        cvars: CVARS,
        instances: [
            {
                // Connects first, so it owns the room.
                name: "A",
                start: STAGES.forestTemple,
                launchDelayMs: 20000,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(true),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", "B"),
                    ...spawnAndSettle(KILL_SYNC_ENEMIES),
                    ...barrier("spawned", "B"),
                    { op: "killEnemy", tag: "real", how: "real" },
                    { op: "killEnemy", tag: "za", how: "disappear", onActor: true },
                    { op: "killEnemy", tag: "fall", how: "fall" },
                    { op: "deleteEnemy", tag: "del" },
                    { op: "killEnemy", tag: "tek", how: "disappear" },
                    { op: "killEnemy", tag: "fz", how: "disappear" },
                    { op: "expectEnemyGone", tag: "real", timeoutSec: 10 },
                    { op: "expectEnemyGone", tag: "za", timeoutSec: 5 },
                    { op: "expectEnemyGone", tag: "fall", timeoutSec: 10 },
                    { op: "expectZoneActor", setId: 0x20 },
                    ...barrier("killed", "B"),
                    { op: "expectEnemySync", sent: 4, received: 0, applied: 0 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                start: STAGES.forestTemple,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(false),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", "A"),
                    ...spawnAndSettle(KILL_SYNC_ENEMIES),
                    ...barrier("spawned", "A"),
                    { op: "expectEnemyGone", tag: "za", timeoutSec: 10 },
                    { op: "expectEnemyGone", tag: "real", timeoutSec: 15 },
                    { op: "expectEnemyGone", tag: "fall", timeoutSec: 15 },
                    { op: "expectEnemyGone", tag: "tek", timeoutSec: 15 },
                    { op: "expectZoneActor", setId: 0x20 },
                    ...barrier("killed", "A"),
                    { op: "wait", frames: 90 },
                    // A plain delete is no defeat, and the Mini Freezard is not on the allowlist.
                    { op: "expectEnemyHealth", tag: "del", timeoutSec: 0 },
                    { op: "expectEnemyHealth", tag: "fz", timeoutSec: 0 },
                    { op: "expectEnemySync", sent: 0, received: 4, applied: 4, echoes: 0, expired: 0 },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "enemy-kill-scope",
        description: "a kill reaches a teammate but not a player on another team, and nobody once the owner turns Sync Enemy Deaths off",
        timeoutSec: 360,
        cvars: CVARS,
        instances: [
            ["A", "red", ["B", "C"]],
            ["B", "red", ["A", "C"]],
            ["C", "blue", ["A", "B"]],
        ].map(([name, team, others]) => {
            const isA = name === "A";
            return {
                name,
                start: STAGES.forestTemple,
                // A connects first, so it owns the room.
                launchDelayMs: isA ? 20000 : 3000,
                steps: [
                    waitStage(STAGES.forestTemple),
                    { op: "connect", team },
                    { op: "waitConnected", timeoutSec: 20 },
                    ...syncOn(isA),
                    { op: "waitPeers", count: 2, sameStage: true, timeoutSec: 150 },
                    ...barrierAll("ready", others),
                    ...spawnAndSettle([["one", BOKOBLIN, 0, 250], ["two", BOKOBLIN, 250, 250]]),
                    ...barrierAll("spawned", others),
                    ...(isA
                        ? [{ op: "killEnemy", tag: "one" }, { op: "expectEnemyGone", tag: "one", timeoutSec: 5 }]
                        : name === "B"
                            ? [{ op: "expectEnemyGone", tag: "one", timeoutSec: 15 }]
                            : []),
                    ...barrierAll("one-killed", others),
                    // C is on another team: its copy stays.
                    ...(name === "C" ? [{ op: "wait", frames: 60 }, { op: "expectEnemyHealth", tag: "one", timeoutSec: 0 }] : []),
                    ...(isA ? [{ op: "setRoomOption", name: "syncNPCs", value: false }] : []),
                    { op: "waitRoomOption", name: "syncNPCs", value: false, timeoutSec: 30 },
                    ...barrierAll("sync-off", others),
                    ...(isA ? [{ op: "killEnemy", tag: "two" }, { op: "expectEnemyGone", tag: "two", timeoutSec: 5 }] : []),
                    ...barrierAll("two-killed", others),
                    ...(isA ? [] : [{ op: "wait", frames: 90 }, { op: "expectEnemyHealth", tag: "two", timeoutSec: 0 }]),
                    { op: "expectEnemySync", ...(isA ? { sent: 1, received: 0 } : name === "B" ? { sent: 0, received: 1, applied: 1 } : { sent: 0, received: 0, applied: 0 }) },
                    ...barrierAll("done", others),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-room-clear",
        description: "with world sync off, A's kill empties the room for B too, so B's own ALLdie sets its room-clear switch",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            return {
                name,
                start: STAGES.linksHouse,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    // Without world sync only B's own ALLdie can set its switch.
                    ...(isA ? [{ op: "setRoomOption", name: "syncWorldState", value: false }] : []),
                    { op: "waitRoomOption", name: "syncWorldState", value: false, timeoutSec: 30 },
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["boko", BOKOBLIN, 0, -150]]),
                    // Event 0xFF, zone switch 0xC5; spawned once the room has an enemy or it fires at once.
                    { op: "spawnEnemy", name: "ALLdie", param: 0xff00c5ff, tag: "alldie" },
                    { op: "expectEnemyHealth", tag: "alldie" },
                    { op: "wait", frames: 90 },
                    { op: "expectSwitch", no: 0xc5, room: 4, set: false, timeoutSec: 0 },
                    ...barrier("spawned", other),
                    ...(isA ? [{ op: "killEnemy", tag: "boko" }] : []),
                    { op: "expectEnemyGone", tag: "boko", timeoutSec: 15 },
                    // ALLdie waits 65 frames after the room is empty.
                    { op: "expectSwitch", no: 0xc5, room: 4, set: true, timeoutSec: 15 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-kill-group",
        description: "a Shadow Beast group condemned on A disappears on B in one go",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            // Group 2, no roof spawn, no path, no switch (d_a_e_s1.cpp daE_S1_Create).
            const S1 = { name: "E_s1", param: 0xffffff2f, dy: 30 };
            const tags = ["s1a", "s1b", "s1c"];
            return {
                name,
                start: STAGES.forestTemple,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["s1a", S1, -250, 250], ["s1b", S1, 0, 250], ["s1c", S1, 250, 250]]),
                    ...barrier("spawned", other),
                    ...(isA
                        ? [{ op: "forceGroupFail", tags }, { op: "expectEnemiesGone", tags, timeoutSec: 15 }]
                        : [{ op: "expectEnemiesGone", tags, maxSpreadTicks: 1, timeoutSec: 15 }]),
                    ...barrier("gone", other),
                    { op: "expectEnemySync", ...(isA ? { sent: 3, received: 0 } : { sent: 0, received: 3, applied: 3 }) },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-hz-sync",
        description: "a Tile Worm A defeats stays on B as a dead tile (defeated in place, not deleted) and its switch syncs; a worm created dead reports nothing",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            return {
                name,
                start: STAGES.forestTemple,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["hz", TILE_WORM, 0, 400]]),
                    ...barrier("spawned", other),
                    ...(isA ? [{ op: "killEnemy", tag: "hz", how: "hz" }] : []),
                    { op: "expectEnemyDeadInPlace", tag: "hz", timeoutSec: 15 },
                    // A's own death wait sets it; B gets it through world sync.
                    { op: "expectSwitch", no: 0x13, room: 0, timeoutSec: 15 },
                    { op: "expectEnemySync", ...(isA ? { sent: 1, received: 0 } : { sent: 0, received: 1, applied: 1 }) },
                    ...barrier("killed", other),
                    // The switch is set now: this one is created dead and never puffs.
                    { op: "spawnEnemy", ...TILE_WORM, ...HOME, dx: 250, dz: 400, tag: "hz2" },
                    { op: "expectEnemyDeadInPlace", tag: "hz2", timeoutSec: 15 },
                    { op: "wait", frames: 90 },
                    { op: "expectEnemySync", ...(isA ? { sent: 1, received: 0 } : { sent: 0, received: 1, applied: 1 }), timeoutSec: 0 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "enemy-kill-rdy12",
        description: "the Twilit Kargorok's Shadow Bulblin rider (variant 12) is not kill-synced: A's kill leaves B's rider and its mount alone",
        timeoutSec: 300,
        cvars: CVARS,
        instances: ["A", "B"].map((name) => {
            const isA = name === "A";
            const other = isA ? "B" : "A";
            return {
                name,
                // A fresh save: the rider refuses to exist once event bit 84 is set.
                start: STAGES.forestTemple,
                launchDelayMs: isA ? 20000 : 1500,
                steps: [
                    waitStage(STAGES.forestTemple),
                    ...connect,
                    ...syncOn(isA),
                    { op: "waitPeers", count: 1, sameStage: true, timeoutSec: 120 },
                    ...barrier("ready", other),
                    ...spawnAndSettle([["rdy", KARGOROK_RIDER, 0, 250]]),
                    ...barrier("spawned", other),
                    ...(isA ? [{ op: "killEnemy", tag: "rdy", how: "disappear" }, { op: "expectEnemyGone", tag: "rdy", timeoutSec: 5 }] : []),
                    ...barrier("killed", other),
                    { op: "wait", frames: 150 },
                    ...(isA ? [] : [{ op: "expectEnemyHealth", tag: "rdy", timeoutSec: 0 }]),
                    { op: "expectEnemySync", sent: 0, received: 0, applied: 0, timeoutSec: 0 },
                    ...barrier("done", other),
                    { op: "quit" },
                ],
            };
        }),
    },
];
