// Team games: randomizer seeds found on disk, mismatches, two teams on two games in one room.

const { MODS, STAGES, ITEMS, COMMON_CVARS, RANDOMIZER_MODE_CVARS, tt, waitStage, barrier, meetIn, connect } =
    require("../lib");

// Two generated seeds as the randomizer writes them (yaml-cpp block style), names looked up by
// ItemService check name. 0x28 is a sword: inventory-dependent, so the probe skips it.
const SEEDS = {
    X: {
        hash: "Soldier Beth Dragonfly",
        seed: "EarlyElatedPoe",
        permalink: "djEuMC41LUhFQUQtMjc1ZWQ4NwA3Mzg2MjUzMzQARWFybHlFbGF0ZWRQb2UAAQIDBAUGBwgJ",
        items: {
            ordon_sword: 0x28, ordon_shield: 0x21, coro_lantern: 0x22, sera_reward: 0x23,
            ilia_charm: 0x24, iza_reward_1: 0x25, goats_reward: 0x26, renado_letter: 0x27,
            telma_invoice: 0x2a, wood_statue: 0x2b, zora_armor: 0x2c, "shop:R_SP109:3:62": 0x2d,
        },
    },
    Y: {
        hash: "Goron Mask Bottle",
        seed: "QuietHappyKeese",
        permalink: "djEuMC41LUhFQUQtMjc1ZWQ4NwA3Mzg2MjUzMzQAUXVpZXRIYXBweUtlZXNlAAkIBwYFBAMCAQ",
        items: {
            ordon_sword: 0x2d, ordon_shield: 0x2c, coro_lantern: 0x2b, sera_reward: 0x2a,
            ilia_charm: 0x27, iza_reward_1: 0x26, goats_reward: 0x25, renado_letter: 0x24,
            telma_invoice: 0x23, wood_statue: 0x22, zora_armor: 0x21, "shop:R_SP109:3:62": 0x28,
        },
    },
};

function seedDat(s) {
    const lines = [
        "formatVersion: 3",
        "mSettings:",
        "  Logic Rules: Glitchless",
        "  Small Keys: Keysy",
        "mStartEventFlags:",
        "  - 1025",
        "  - 3074",
        "mStartRegionFlags:",
        "  0:",
        "    - 12",
        "mStartingInventory:",
        "  - 64",
        "mTreasureChestOverrides:",
        `  1027: ${s.items.ordon_shield}`,
        "mPoeOverrides: {}",
        "mItemLocations:",
    ];
    for (const [name, item] of Object.entries(s.items)) {
        lines.push(`  ${name}:`, `    itemId: ${item}`, "    stage: 0", "    flag: 0");
    }
    lines.push(
        "mStartHour: 12",
        "mMapBits: 0",
        "mCustomMessages:",
        "  - group: 1",
        "    name: hint",
        "    text:",
        "      English: !!binary aGludA==",
        "",
        "mTextOverrides:",
        "  English:",
        "    0x1234: !!binary dGV4dA==",
        "");
    return lines.join("\n");
}

function antiSpoilerLog(s) {
    return [
        "Dusklight Randomizer Version: v1.0.5-HEAD-275ed87",
        `# Seed: ${s.seed}`,
        `Permalink: ${s.permalink}`,
        `Hash: ${s.hash}`,
        "",
        "# Settings",
        "Logic Rules: Glitchless",
        "",
    ].join("\n");
}

// Both seeds on every instance's disk: detection has to pick the one that is running.
const SEED_FILES = {};
for (const s of Object.values(SEEDS)) {
    const dir = `mod_data/${MODS.randomizer}/seeds/${s.hash}`;
    SEED_FILES[`${dir}/seed.dat`] = seedDat(s);
    SEED_FILES[`${dir}/${s.hash} Anti-Spoiler Log.txt`] = antiSpoilerLog(s);
}

const runSeed = (s) => ({ op: "fakeRandoResolver", items: s.items });

const localSeed = (s) => ({ op: "expectLocalGame", kind: "randomizer", keyPrefix: "rando/f3/", name: s.hash,
                            verified: true, permalink: s.permalink });

const fakeIdentity = (key, name, permalink) => ({
    op: "setGameIdentity",
    identity: { kind: "randomizer", key, name, mode: "Randomizer 1.0.5", permalink, seed: name, version: "v1.0.5" },
});

const RED = { key: "rando/f3/aaaaaaaaaaaaaaaa", name: "Red Seed", permalink: "cmVkc2VlZHBlcm1hbGluaw==" };
const BLUE = { key: "rando/f3/bbbbbbbbbbbbbbbb", name: "Blue Seed", permalink: "Ymx1ZXNlZWRwZXJtYWxpbms=" };

// Team colours: red's default comes from its id; the players' own colours are set per instance.
const MINIMAP_CVARS = [...COMMON_CVARS, "game.minimalHUD=false", "game.recordingMode=false",
                       "game.debugFlyCam=false", "game.enableMirrorMode=false"];
const OWN = { A: "#13C76D", B: "#FDD835", C: "#7CB342", D: "#E53935" };
const RED_DEFAULT = "#1E88E5";
const PICKED = "#FA059B";
const REPICKED = "#0A64C8";
// Client entry (the Players list), dummy tint, map marker and name tag frame all show `rgb`.
const seesTeamColor = (name, rgb) => [
    { op: "expectPeerColor", name, rgb, timeoutSec: 10 },
    { op: "expectDummyColor", name, rgb, timeoutSec: 10 },
    { op: "expectMapCursor", target: name, color: rgb, timeoutSec: 10 },
    { op: "viewPeer", name, beyond: -180 },
    { op: "wait", sec: 1 },
    { op: "expectNameTag", name, rgb, timeoutSec: 10 },
];

// Every instance waits for every other's signal.
const allBarrier = (name, self, names) => [
    { op: "signal", name },
    ...names.filter((n) => n !== self).map((n) => ({ op: "waitSignal", name, from: n, timeoutSec: 150 })),
];

module.exports = [
    {
        name: "rando-seed-match",
        description: "two players running the same randomizer seed: both find it on disk, sync, and the joiner gets the owner's permalink",
        timeoutSec: 360,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        files: SEED_FILES,
        expectLog: [
            /\[game\] seed "Soldier Beth Dragonfly": 12 name lookups, digest [0-9a-f]{16}/,
            /\[game\] seed "Goron Mask Bottle": 12 name lookups/,
            /\[game\] local game: randomizer rando\/f3\/[0-9a-f]{16} "Soldier Beth Dragonfly", permalink known/,
        ],
        rejectLog: [/conflict/i, /unverified match prompt/],
        instances: [
            {
                name: "A",
                launchDelayMs: 10000,
                steps: [
                    runSeed(SEEDS.X),
                    waitStage(STAGES.linksHouse),
                    localSeed(SEEDS.X),
                    ...connect,
                    { op: "expectSyncState", state: "ok" },
                    { op: "expectTeamGame", owner: "self", name: SEEDS.X.hash, permalink: SEEDS.X.permalink },
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "giveItem", item: ITEMS.boomerang },
                    { op: "setSwitch", no: 20 },
                    ...barrier("a-gave", "B"),
                    { op: "expectItem", item: ITEMS.slingshot },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    runSeed(SEEDS.X),
                    waitStage(STAGES.linksHouse),
                    localSeed(SEEDS.X),
                    ...connect,
                    { op: "expectSyncState", state: "ok" },
                    { op: "expectTeamGame", owner: "A", name: SEEDS.X.hash, permalink: SEEDS.X.permalink },
                    { op: "expectCopyText", text: SEEDS.X.permalink },
                    { op: "expectMemberSync", peer: "A", state: "ok" },
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("a-gave", "A"),
                    { op: "expectItem", item: ITEMS.boomerang },
                    { op: "expectSwitch", no: 20 },
                    { op: "giveItem", item: ITEMS.slingshot },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "rando-seed-mismatch",
        description: "a teammate on another randomizer seed is refused: no world data either way, still visible, told the team's seed",
        timeoutSec: 360,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        files: SEED_FILES,
        expectLog: [
            { instance: "B", pattern: /\[game\] local game: randomizer rando\/f3\/[0-9a-f]{16} "Goron Mask Bottle"/ },
            { instance: "B", pattern: /\[game\] sync state pending -> mismatch/ },
        ],
        rejectLog: [/merged world state/],
        instances: [
            {
                name: "A",
                launchDelayMs: 10000,
                steps: [
                    runSeed(SEEDS.X),
                    waitStage(STAGES.linksHouse),
                    localSeed(SEEDS.X),
                    ...connect,
                    { op: "expectSyncState", state: "ok" },
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "expectMemberSync", peer: "B", state: "mismatch" },
                    { op: "giveItem", item: ITEMS.boomerang },
                    { op: "setSwitch", no: 20 },
                    ...barrier("a-gave", "B"),
                    { op: "waitSignal", name: "b-gave", from: "B", timeoutSec: 120 },
                    { op: "expectNoItem", item: ITEMS.slingshot, forSec: 6 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    runSeed(SEEDS.Y),
                    waitStage(STAGES.linksHouse),
                    localSeed(SEEDS.Y),
                    ...connect,
                    { op: "expectSyncState", state: "mismatch" },
                    { op: "expectTeamGame", owner: "A", name: SEEDS.X.hash, permalink: SEEDS.X.permalink },
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("a-gave", "A"),
                    { op: "expectNoItem", item: ITEMS.boomerang, forSec: 8 },
                    { op: "expectSwitch", no: 20, set: false },
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
        name: "vanilla-vs-rando",
        description: "a randomizer player on a vanilla team is refused both ways; the vanilla owner keeps playing",
        timeoutSec: 360,
        cvars: [...COMMON_CVARS, "backend.skipPreLaunchUI=true"],
        enableMods: [MODS.randomizer],
        rejectLog: [/merged world state/],
        instances: [
            {
                name: "A",
                launchDelayMs: 10000,
                steps: [
                    waitStage(STAGES.linksHouse),
                    { op: "expectLocalGame", kind: "vanilla", key: "vanilla" },
                    ...connect,
                    { op: "expectSyncState", state: "ok" },
                    { op: "expectTeamGame", owner: "self", kind: "vanilla", key: "vanilla" },
                    ...meetIn(STAGES.linksHouse, "B"),
                    { op: "expectMemberSync", peer: "B", state: "mismatch" },
                    { op: "giveItem", item: ITEMS.boomerang },
                    ...barrier("a-gave", "B"),
                    { op: "waitSignal", name: "b-gave", from: "B", timeoutSec: 120 },
                    { op: "expectNoItem", item: ITEMS.slingshot, forSec: 6 },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                cvars: RANDOMIZER_MODE_CVARS,
                steps: [
                    waitStage(STAGES.linksHouse),
                    { op: "expectLocalGame", kind: "randomizer", keyPrefix: "rando-probe/", verified: false },
                    ...connect,
                    { op: "expectSyncState", state: "mismatch" },
                    { op: "expectTeamGame", kind: "vanilla", key: "vanilla", noPermalink: true },
                    ...meetIn(STAGES.linksHouse, "A"),
                    ...barrier("a-gave", "A"),
                    { op: "expectNoItem", item: ITEMS.boomerang, forSec: 8 },
                    { op: "giveItem", item: ITEMS.slingshot },
                    { op: "signal", name: "b-gave" },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "two-teams-two-games",
        description: "teams red and blue on different seeds in one room: all four see each other, each team syncs only within itself, teleport across teams is refused",
        timeoutSec: 420,
        cvars: COMMON_CVARS,
        rejectLog: [/unverified match prompt/],
        instances: ["A", "B", "C", "D"].map((name, i, names) => {
            const red = i < 2;
            const mine = red ? RED : BLUE;
            const mate = red ? names[1 - i] : names[5 - i];
            const rival = red ? "C" : "A";
            const myItem = red ? ITEMS.boomerang : ITEMS.slingshot;
            const theirItem = red ? ITEMS.slingshot : ITEMS.boomerang;
            const giver = name === "A" || name === "C";
            return {
                name,
                launchDelayMs: 4000,
                steps: [
                    fakeIdentity(mine.key, mine.name, mine.permalink),
                    waitStage(STAGES.linksHouse),
                    { op: "connect", team: red ? "red" : "blue" },
                    { op: "waitConnected", timeoutSec: 20 },
                    { op: "setRoomOption", name: "teleportMode", value: true },
                    { op: "expectSyncState", state: "ok" },
                    { op: "expectTeamGame", key: mine.key, name: mine.name, permalink: mine.permalink },
                    { op: "waitPeers", count: 3, sameStage: true, timeoutSec: 150 },
                    { op: "waitDummies", count: 3, timeoutSec: 90 },
                    { op: "expectTeamGame", team: red ? "blue" : "red", name: red ? BLUE.name : RED.name,
                      noPermalink: true, noKey: true, sameGame: false },
                    { op: "expectMemberSync", peer: mate, state: "ok" },
                    { op: "waitRoomOption", name: "teleportMode", value: true },
                    { op: "expectTeleportBlocked", peer: rival, code: "other-team" },
                    ...allBarrier("met", name, names),
                    ...(giver ? [{ op: "giveItem", item: myItem }] : []),
                    ...allBarrier("gave", name, names),
                    { op: "expectItem", item: myItem },
                    { op: "expectNoItem", item: theirItem, forSec: 8 },
                    ...allBarrier("done", name, names),
                    { op: "quit" },
                ],
            };
        }),
    },
    {
        name: "promote-and-succession",
        description: "vanilla teams red (A, B) and blue (C): sync stays in red, teleport to blue is refused; A hands red to B and the room to C; when they leave, A (connected first) takes both back",
        timeoutSec: 360,
        cvars: COMMON_CVARS,
        instances: ["A", "B", "C"].map((name, i, names) => {
            const team = name === "C" ? "blue" : "red";
            const common = [
                waitStage(STAGES.linksHouse),
                { op: "connect", team },
                { op: "waitConnected", timeoutSec: 20 },
                { op: "setRoomOption", name: "teleportMode", value: true },
                { op: "expectSyncState", state: "ok" },
                { op: "waitPeers", count: 2, timeoutSec: 120 },
            ];
            const steps = {
                A: [
                    { op: "expectRoomOwner", owner: "self" },
                    { op: "expectTeamGame", owner: "self", key: "vanilla" },
                    { op: "waitRoomOption", name: "teleportMode", value: true },
                    { op: "expectTeleportBlocked", peer: "C", code: "other-team" },
                    ...allBarrier("ready", name, names),
                    { op: "giveItem", item: ITEMS.boomerang },
                    ...allBarrier("gave", name, names),
                    { op: "promote", peer: "B", role: "team" },
                    { op: "promote", peer: "C", role: "room" },
                    { op: "expectTeamGame", owner: "B" },
                    { op: "expectRoomOwner", owner: "C" },
                    ...allBarrier("promoted", name, names),
                    // B quits: red goes back to A, the earliest-connected teammate left.
                    { op: "expectTeamGame", owner: "self", key: "vanilla" },
                    { op: "signal", name: "red-back" },
                    // C quits: the room goes back to A too.
                    { op: "expectRoomOwner", owner: "self" },
                    { op: "quit" },
                ],
                B: [
                    ...allBarrier("ready", name, names),
                    ...allBarrier("gave", name, names),
                    { op: "expectItem", item: ITEMS.boomerang },
                    { op: "expectTeamGame", owner: "self" },
                    { op: "expectRoomOwner", owner: "C" },
                    ...allBarrier("promoted", name, names),
                    { op: "quit" },
                ],
                C: [
                    { op: "expectTeamGame", team: "red", owner: "A", kind: "vanilla", noKey: true, sameGame: true },
                    ...allBarrier("ready", name, names),
                    ...allBarrier("gave", name, names),
                    { op: "expectNoItem", item: ITEMS.boomerang, forSec: 6 },
                    { op: "expectRoomOwner", owner: "self" },
                    { op: "expectTeamGame", team: "red", owner: "B" },
                    ...allBarrier("promoted", name, names),
                    { op: "waitSignal", name: "red-back", from: "A", timeoutSec: 120 },
                    { op: "quit" },
                ],
            };
            return { name, launchDelayMs: 8000, steps: [...common, ...steps[name]] };
        }),
    },
    {
        name: "team-color",
        description: "B and C on team red show to A in red's colour (its default, then the one B picks) on dummy, name tag, map marker and list; D without a team keeps its own; after B leaves, C leads and the colour stays",
        timeoutSec: 480,
        cvars: MINIMAP_CVARS,
        instances: ["A", "B", "C", "D"].map((name, i, names) => {
            const team = name === "B" || name === "C" ? "red" : "";
            const meet = [
                { op: "waitStage", stage: STAGES.forestTemple.stage, timeoutSec: 90 },
                { op: "connect", team },
                { op: "waitConnected", timeoutSec: 20 },
                { op: "waitPeers", count: 3, sameStage: true, timeoutSec: 150 },
                { op: "waitDummies", count: 3, timeoutSec: 90 },
                { op: "walk", frames: 120, circle: true },
                ...allBarrier("met", name, names),
            ];
            const steps = {
                A: [
                    ...seesTeamColor("B", RED_DEFAULT),
                    ...seesTeamColor("C", RED_DEFAULT),
                    ...seesTeamColor("D", OWN.D),
                    ...allBarrier("saw-default", name, names),
                    ...allBarrier("picked", name, names),
                    ...seesTeamColor("B", PICKED),
                    ...seesTeamColor("C", PICKED),
                    ...seesTeamColor("D", OWN.D),
                    ...allBarrier("saw-picked", name, names),
                    // B quits: C leads red and the colour stays.
                    { op: "expectTeamGame", team: "red", owner: "C" },
                    { op: "expectTeamColor", team: "red", rgb: PICKED },
                    ...seesTeamColor("C", PICKED),
                    { op: "signal", name: "c-leads" },
                    { op: "waitSignal", name: "c-picked", from: "C", timeoutSec: 60 },
                    ...seesTeamColor("C", REPICKED),
                    ...barrier("done", "C"),
                    ...barrier("done", "D"),
                    { op: "quit" },
                ],
                B: [
                    { op: "expectTeamColor", rgb: RED_DEFAULT },
                    { op: "expectSelfRow", rgb: RED_DEFAULT },
                    ...allBarrier("saw-default", name, names),
                    { op: "setTeamColor", rgb: PICKED },
                    { op: "expectTeamColor", rgb: PICKED },
                    ...allBarrier("picked", name, names),
                    ...allBarrier("saw-picked", name, names),
                    { op: "quit" },
                ],
                C: [
                    { op: "expectPeerColor", name: "B", rgb: RED_DEFAULT, timeoutSec: 10 },
                    ...allBarrier("saw-default", name, names),
                    ...allBarrier("picked", name, names),
                    { op: "expectSelfRow", rgb: PICKED },
                    { op: "expectPeerColor", name: "B", rgb: PICKED, timeoutSec: 10 },
                    ...allBarrier("saw-picked", name, names),
                    { op: "expectTeamGame", owner: "self" },
                    { op: "waitSignal", name: "c-leads", from: "A", timeoutSec: 60 },
                    { op: "setTeamColor", rgb: REPICKED },
                    { op: "expectTeamColor", rgb: REPICKED },
                    { op: "signal", name: "c-picked" },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
                D: [
                    { op: "expectSelfRow", rgb: OWN.D },
                    ...allBarrier("saw-default", name, names),
                    ...allBarrier("picked", name, names),
                    { op: "expectPeerColor", name: "C", rgb: PICKED, timeoutSec: 10 },
                    { op: "expectPeerColor", name: "A", rgb: OWN.A, timeoutSec: 10 },
                    ...allBarrier("saw-picked", name, names),
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            };
            return {
                name,
                start: STAGES.forestTemple,
                cvars: [tt("color", OWN[name].slice(1).toLowerCase())],
                launchDelayMs: 6000,
                steps: [...meet, ...steps[name]],
            };
        }),
    },
    {
        name: "team-window-tour",
        description: "B on another seed than its team: the Room tab's team card with the permalink and join steps, then the Players tab grouped by team, for captures (manual)",
        manualOnly: true,
        timeoutSec: 300,
        cvars: [...COMMON_CVARS, ...RANDOMIZER_MODE_CVARS],
        enableMods: [MODS.randomizer],
        files: SEED_FILES,
        instances: [
            {
                name: "A",
                launchDelayMs: 10000,
                steps: [
                    runSeed(SEEDS.X),
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "expectSyncState", state: "ok" },
                    ...barrier("done", "B", 240),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    runSeed(SEEDS.Y),
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "expectSyncState", state: "mismatch" },
                    { op: "expectTeamGame", permalink: SEEDS.X.permalink },
                    { op: "wait", sec: 8 },
                    { op: "showWindow", tab: 1 },
                    { op: "wait", sec: 2 },
                    { op: "mark", msg: "TEAM_HOLD room" },
                    { op: "wait", sec: 4 },
                    { op: "showWindow", tab: 2 },
                    { op: "wait", sec: 2 },
                    { op: "mark", msg: "TEAM_HOLD players" },
                    { op: "wait", sec: 4 },
                    { op: "hideWindow" },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
    {
        name: "owner-switch-conflict",
        description: "the team owner loads another game while a teammate plays the team's: asked first, the team switches only on the owner's claim",
        timeoutSec: 300,
        cvars: COMMON_CVARS,
        expectLog: [
            { instance: "A", pattern: /\[game\] 1 teammate\(s\) still play rando\/f3\/aaaaaaaaaaaaaaaa/ },
            { instance: "A", pattern: /\[game\] team game conflict prompt \(not shown under autotest\)/ },
        ],
        instances: [
            {
                name: "A",
                launchDelayMs: 10000,
                steps: [
                    fakeIdentity(RED.key, RED.name, RED.permalink),
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "expectSyncState", state: "ok" },
                    { op: "expectTeamGame", owner: "self", key: RED.key },
                    { op: "expectMemberSync", peer: "B", state: "ok" },
                    fakeIdentity(BLUE.key, BLUE.name, BLUE.permalink),
                    { op: "expectSyncState", state: "mismatch" },
                    { op: "expectTeamGame", key: RED.key },
                    ...barrier("asked", "B"),
                    { op: "claimTeamGame" },
                    { op: "expectTeamGame", key: BLUE.key, permalink: BLUE.permalink },
                    { op: "expectSyncState", state: "ok" },
                    { op: "expectMemberSync", peer: "B", state: "mismatch" },
                    ...barrier("done", "B"),
                    { op: "quit" },
                ],
            },
            {
                name: "B",
                steps: [
                    fakeIdentity(RED.key, RED.name, RED.permalink),
                    waitStage(STAGES.linksHouse),
                    ...connect,
                    { op: "expectSyncState", state: "ok" },
                    { op: "expectMemberSync", peer: "A", state: "mismatch" },
                    { op: "expectTeamGame", owner: "A", key: RED.key },
                    ...barrier("asked", "A"),
                    { op: "expectSyncState", state: "mismatch" },
                    { op: "expectTeamGame", key: BLUE.key, permalink: BLUE.permalink },
                    ...barrier("done", "A"),
                    { op: "quit" },
                ],
            },
        ],
    },
];
