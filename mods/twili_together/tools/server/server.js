/**
 * Twili-Together relay server.
 * Usage:  npm install && node server.js [wsPort] [tcpPort]
 *
 * The server never simulates the game. It keeps connection metadata and relays packets:
 *
 *   Transports WebSocket (ws://, or wss:// with TT_TLS_CERT and TT_TLS_KEY) and length-prefixed
 *              JSON over TCP (tcp://, for LAN). Both kinds of client share rooms and rules.
 *   Handshake  Only app "twili-together" with protocolVersion >= 5 joins. Anything else gets
 *              SERVER_MESSAGE and DISABLE_CLIENT and is disconnected.
 *   Rooms      Keyed by roomId ("" = the public room). The first client owns a room; when the
 *              owner leaves, the longest-connected client takes over. A room left empty for
 *              TT_ROOM_TTL_SEC (default 6 h) is deleted with its caches.
 *   Room state UPDATE_ROOM_STATE is accepted from the owner only and echoed to the room.
 *   Teams      Flag, event, item, dungeon and world-state packets go to the same room, teamId and
 *              save layout, stamped with the sender's clientId, teamId, session key, name and colour.
 *   Catch-up   The last untargeted UPDATE_WORLD_STATE and the addToQueue packets since are kept
 *              per team and layout. REQUEST_WORLD_STATE goes to caught-up teammates on the same
 *              protocol and layout; the cache answers only when none can, and catchUp:true also
 *              replays the queue and the latest STORY_MOVE arrive. Replays skip the requester's
 *              own session.
 *   Presence   PLAYER_UPDATE / PLAYER_SFX go to room members in the same stage, layer and protocol.
 *   Kills      ENEMY_DEFEATED / STORY_EVENT go to teammates in the same stage and layer, uncached.
 *   Story      STORY_MOVE goes to the whole team and is never queued.
 *   Teleport   REQUEST_TELEPORT / TELEPORT_TO go only to the client named, while teleportMode is on.
 *   PvP        DAMAGE_PLAYER goes only to the client named, while pvpMode is on and the rules allow
 *              it, at most once per TT_DAMAGE_INTERVAL_MS per pair. DAMAGE_RESULT goes back to the
 *              attacker. Refusals are answered by the server.
 *   Rejected   ALL_CLIENT_STATE / SERVER_MESSAGE / DISABLE_CLIENT are server-to-client only, and
 *              a repeated HANDSHAKE is ignored.
 *   Heartbeat  WebSocket connections are pinged every TT_PING_MS (default 10 s). TCP clients send
 *              KEEPALIVE and are dropped after TT_TCP_TIMEOUT_MS (default 30 s) of silence.
 */

const { WebSocketServer } = require("ws");
const { EventEmitter } = require("events");
const fs = require("fs");
const net = require("net");
const path = require("path");

const PORT = parseInt(process.argv[2] ?? process.env.TT_PORT ?? "3000", 10);
const TCP_PORT_ARG = String(process.argv[3] ?? process.env.TT_TCP_PORT ?? (PORT === 0 ? "0" : "3001"));
const TCP_PORT = TCP_PORT_ARG.toLowerCase() === "off" ? null : parseInt(TCP_PORT_ARG, 10);
const TLS_CERT = process.env.TT_TLS_CERT || "";
const TLS_KEY = process.env.TT_TLS_KEY || "";
const APP_ID = "twili-together";
const PROTOCOL_VERSION = 5;
const WS_SUBPROTOCOL = `${APP_ID}.${PROTOCOL_VERSION}`;
const PUBLIC_ROOM_ID = "";
const MAX_TEAM_QUEUE = 4000;
const MAX_TCP_FRAME = 1048576;
const PING_MS = envNumber("TT_PING_MS", 10000);
const TCP_TIMEOUT_MS = envNumber("TT_TCP_TIMEOUT_MS", 30000);
const ROOM_TTL_SEC = envNumber("TT_ROOM_TTL_SEC", 21600);
const TELEPORT_MIN_INTERVAL_MS = envNumber("TT_TELEPORT_INTERVAL_MS", 1000);
// PvP: at most one hit per attacker/victim pair per interval.
const DAMAGE_MIN_INTERVAL_MS = envNumber("TT_DAMAGE_INTERVAL_MS", 200);
const MAX_PVP_DAMAGE = 4; // quarter hearts per hit
const PVP_KIND_COUNT = 10;
const PVP_KNOCKBACK_COUNT = 2; // "spl": 0 light, 1 knockdown
const DAMAGE_RESULTS = new Set(["applied", "blocked", "dropped"]);
// Test only: delay what each client receives by up to this many ms, keeping order.
const TEST_JITTER_MS = envNumber("TT_TEST_JITTER_MS", 0);
// Node runs a timer after 1 ms when its delay is above 2^31-1 ms.
const MAX_TIMER_MS = 0x7fffffff;
const TEAM_PACKET_TYPES = new Set([
    "SET_FLAG",
    "UNSET_FLAG",
    "SET_EVENT_BIT",
    "UNSET_EVENT_BIT",
    "GIVE_ITEM",
    "UPDATE_DUNGEON_ITEMS",
    "UPDATE_WORLD_STATE",
    "REQUEST_WORLD_STATE",
]);
const PRESENCE_PACKET_TYPES = new Set(["PLAYER_UPDATE", "PLAYER_SFX"]);
// Teammates in the sender's stage and layer only; never cached or queued.
const TEAM_PRESENCE_PACKET_TYPES = new Set(["ENEMY_DEFEATED", "STORY_EVENT"]);
// The whole team; the latest arrive is replayed on catch-up (see sendTeamStoryMove).
const TEAM_STORY_PACKET_TYPES = new Set(["STORY_MOVE"]);
// Every client obeys these, so a relayed copy could rewrite the roster or disconnect the room.
const SERVER_ONLY_TYPES = new Set(["ALL_CLIENT_STATE", "SERVER_MESSAGE", "DISABLE_CLIENT"]);

function envNumber(name, fallback) {
    const v = Number(process.env[name]);
    return Number.isFinite(v) && v > 0 ? v : fallback;
}

// Appended across starts; TT_LOG_PATH lets harnesses keep one log per run.
const LOG_PATH = process.env.TT_LOG_PATH || path.join(__dirname, "twili-together-server.log");
const logStream = fs.createWriteStream(LOG_PATH, { flags: "a" });

function log(line) {
    const ts = new Date().toISOString();
    const full = `[${ts}] ${line}`;
    console.log(full);
    logStream.write(full + "\n");
}

// Packets are logged before validation, so these accept any JSON value.
function hex(v, pad = 0) {
    return (Number.isInteger(v) ? v : 0).toString(16).padStart(pad, "0");
}

function text(v) {
    return ["string", "number", "boolean"].includes(typeof v) ? String(v) : "?";
}

function packetSummary(packet) {
    switch (packet.type) {
        case "UPDATE_CLIENT_STATE":
            // Colour-only update from the picker.
            if (!("stageName" in packet) && packet.color && typeof packet.color === "object") {
                return `${packet.type} color=${text(packet.color.r)},${text(packet.color.g)},${text(packet.color.b)}`;
            }
            // Horse name and parking spot.
            if (!("stageName" in packet) && ("horseName" in packet || "horsePlace" in packet)) {
                const place = packet.horsePlace && typeof packet.horsePlace === "object" ? text(packet.horsePlace.stage) : "none";
                return `${packet.type} horse=${text(packet.horseName)} parked=${place}`;
            }
            return `${packet.type} stage=${text(packet.stageName)} layer=${text(packet.layerNo)} room=${text(packet.roomNo)} saveTbl=${text(packet.saveTblNo)} saveLoaded=${text(packet.isSaveLoaded)}`;
        case "SET_FLAG":
            return `${packet.type} [${text(packet.category)}] no=${text(packet.flagNo)} room=${text(packet.roomNo)} stage=${text(packet.stageName)} saveTbl=${text(packet.saveTblNo)}`;
        case "UNSET_FLAG":
            return `${packet.type} no=${text(packet.flagNo)} room=${text(packet.roomNo)} stage=${text(packet.stageName)} saveTbl=${text(packet.saveTblNo)}`;
        case "SET_EVENT_BIT":
        case "UNSET_EVENT_BIT":
            return `${packet.type} no=0x${hex(packet.no, 4)}`;
        case "GIVE_ITEM":
            return `${packet.type} item=0x${hex(packet.itemNo, 2)}`;
        case "UPDATE_DUNGEON_ITEMS":
            return `${packet.type} saveTbl=${text(packet.saveTblNo)} keyDelta=${text(packet.keyDelta)} bits=0x${hex(packet.dungeonItemBits)}`;
        case "UPDATE_WORLD_STATE":
            return `${packet.type} saveTbl=${text(packet.saveTblNo)} target=${packet.targetClientId ? text(packet.targetClientId) : "team"}`;
        case "REQUEST_WORLD_STATE":
            return `${packet.type}${packet.catchUp === true ? " catchUp" : ""}`;
        case "UPDATE_ROOM_STATE": {
            const state = packet.state && typeof packet.state === "object" ? packet.state : {};
            return `${packet.type} ${Object.entries(state).map(([k, v]) => `${k}=${text(v)}`).join(" ")}`;
        }
        case "PLAYER_SFX":
            return `${packet.type} sound=0x${hex(packet.soundId)} kind=${text(packet.kind)}`;
        case "ENEMY_DEFEATED":
            return `${packet.type} stage=${text(packet.stageName)} layer=${text(packet.layerNo)} kills=${Array.isArray(packet.kills) ? packet.kills.length : "?"}`;
        case "STORY_EVENT":
            return `${packet.type} ph=${text(packet.ph)} id=${text(packet.id)}` +
                (packet.ph === "start" ? ` ${text(packet.stage)}/${text(packet.room)}/${text(packet.layer)} m=${text(packet.m)} req=${text(packet.req)}` : "");
        case "STORY_MOVE": {
            const from = packet.from && typeof packet.from === "object" ? packet.from : {};
            const to = packet.to && typeof packet.to === "object" ? packet.to : {};
            return `${packet.type} ${text(from.stage)} -> ${text(to.stage)}/${text(to.room)}/${text(to.point)} cur=${text(packet.curated) || "-"} qual=${text(packet.qual)}`;
        }
        case "REQUEST_TELEPORT":
            return `${packet.type} target=${text(packet.targetClientId)} req=${text(packet.requestId)}`;
        case "TELEPORT_TO":
            return `${packet.type} to=${text(packet.targetClientId)} req=${text(packet.requestId)} ` +
                (packet.ok === true
                    ? `stage=${text(packet.stageName)} room=${text(packet.roomNo)} layer=${text(packet.layerNo)}`
                    : `refused=${text(packet.reason)}`);
        case "DAMAGE_PLAYER":
            return `${packet.type} target=${text(packet.targetClientId)} hit=${text(packet.hitId)} kind=${text(packet.kind)} dmg=${text(packet.damage)} spl=${text(packet.spl)} blocked=${text(packet.blocked)}`;
        case "DAMAGE_RESULT":
            return `${packet.type} to=${text(packet.targetClientId)} hit=${text(packet.hitId)} ${text(packet.result)} reason=${text(packet.reason)} dmg=${text(packet.damage)}`;
        case "AUTOTEST_SIGNAL":
            return `${packet.type} instance=${text(packet.instance)} name=${text(packet.name)}`;
        default:
            return packet.type;
    }
}

let nextClientId = 1;

/**
 * @typedef {{ conn: any, clientId: number, roomId: string, sessionKey: string, name: string,
 *             teamId: string, color: object, modVersion: string, layout: string,
 *             transport: string, protocolVersion: number, online: boolean,
 *             isSaveLoaded: boolean, caughtUp: boolean, stageName: string, layerNo: number,
 *             roomNo: number, saveTblNo: number, horseName: string, horsePlace: object|null,
 *             lastTeleportRequestAt: number, lastDamageAt: Map<number, number>,
 *             awaitingResults: Map<string, number> }} Client
 * @typedef {{ id: string, clients: Map<number, Client>, ownerClientId: number|null,
 *             state: object, teamStates: Map<string, object>, teamQueues: Map<string, object[]>,
 *             teamStoryMoves: Map<string, object>, expiryTimer: NodeJS.Timeout|null }} Room
 */

/** @type {Map<string, Room>} */
const rooms = new Map();
/** @type {Map<number, Client>} */
const clientsById = new Map();

function defaultRoomState() {
    return {
        pvpMode: false,
        pvpFriendlyFire: false,
        pvpLethal: false,
        showLocationsMode: true,
        teleportMode: false,
        syncWorldState: true,
        shareWoodenShield: true,
        syncNPCs: false,
        cutsceneSync: true,
        hidePlayersInCutscene: false,
        enemyCountMultiplier: 100,
        enemyHealthMultiplier: 100,
    };
}

function getOrCreateRoom(roomId) {
    let room = rooms.get(roomId);
    if (!room) {
        room = {
            id: roomId,
            clients: new Map(),
            ownerClientId: null,
            state: defaultRoomState(),
            teamStates: new Map(),
            teamQueues: new Map(),
            teamStoryMoves: new Map(),
            // Per room lifetime, so a replay tells packets a client already applied from new ones.
            queueEpoch: `${Date.now().toString(36)}${Math.random().toString(36).slice(2, 8)}`,
            teamQueueSeqs: new Map(),
            expiryTimer: null,
        };
        rooms.set(roomId, room);
        log(`[room ${describeRoom(room)}] created`);
    }
    return room;
}

// Empty rooms keep their caches for a while, but not forever.
function scheduleRoomExpiry(room) {
    room.expiryTimer = setTimeout(() => {
        rooms.delete(room.id);
        log(`[room ${describeRoom(room)}] deleted after ${ROOM_TTL_SEC}s empty`);
    }, Math.min(ROOM_TTL_SEC * 1000, MAX_TIMER_MS));
}

function describeRoom(room) {
    return room.id === PUBLIC_ROOM_ID ? "<public>" : room.id;
}

function roomStatePacket(room) {
    return {
        type: "UPDATE_ROOM_STATE",
        state: { ownerClientId: room.ownerClientId ?? 0, ...room.state },
    };
}

function clientStateOf(client, forClientId) {
    return {
        clientId: client.clientId,
        name: client.name,
        teamId: client.teamId,
        color: client.color,
        modVersion: client.modVersion,
        layout: client.layout,
        protocolVersion: client.protocolVersion,
        online: client.online,
        isSaveLoaded: client.isSaveLoaded,
        stageName: client.stageName,
        layerNo: client.layerNo,
        roomNo: client.roomNo,
        saveTblNo: client.saveTblNo,
        horseName: client.horseName,
        horsePlace: client.horsePlace,
        self: client.clientId === forClientId,
    };
}

function send(client, packet) {
    if (client.conn.readyState === 1 /* OPEN */) {
        client.conn.send(JSON.stringify(packet));
    }
}

function broadcastRoom(room, senderClientId, packet) {
    const msg = JSON.stringify(packet);
    for (const [id, c] of room.clients) {
        if (id !== senderClientId && c.conn.readyState === 1) {
            c.conn.send(msg);
        }
    }
}

function broadcastTeam(room, sender, packet) {
    const msg = JSON.stringify(packet);
    for (const [id, c] of room.clients) {
        if (id !== sender.clientId && c.teamId === sender.teamId && c.conn.readyState === 1) {
            c.conn.send(msg);
        }
    }
}

// World packets are raw save data: only teammates on the same save layout get them.
function broadcastWorld(room, sender, packet) {
    const msg = JSON.stringify(packet);
    for (const [id, c] of room.clients) {
        if (id !== sender.clientId && c.teamId === sender.teamId && c.layout === sender.layout &&
            c.conn.readyState === 1) {
            c.conn.send(msg);
        }
    }
}

// Cache, queue and queue numbers are kept per team and save layout.
function worldKey(client) {
    return JSON.stringify([client.teamId, client.layout]);
}

function broadcastPresence(room, sender, packet) {
    if (!sender.online || !sender.isSaveLoaded) return;
    const msg = JSON.stringify(packet);
    for (const [id, c] of room.clients) {
        if (
            id !== sender.clientId &&
            c.conn.readyState === 1 &&
            c.online &&
            c.isSaveLoaded &&
            c.stageName === sender.stageName &&
            c.layerNo === sender.layerNo &&
            // PLAYER_UPDATE's layout is tied to the protocol.
            c.protocolVersion === sender.protocolVersion
        ) {
            c.conn.send(msg);
        }
    }
}

function broadcastTeamPresence(room, sender, packet) {
    if (!sender.online || !sender.isSaveLoaded) return;
    const msg = JSON.stringify(packet);
    for (const [id, c] of room.clients) {
        if (
            id !== sender.clientId &&
            c.conn.readyState === 1 &&
            c.online &&
            c.isSaveLoaded &&
            c.teamId === sender.teamId &&
            c.stageName === sender.stageName &&
            c.layerNo === sender.layerNo &&
            c.protocolVersion === sender.protocolVersion
        ) {
            c.conn.send(msg);
        }
    }
}

// Everything stored or relayed is coerced to the type the game expects.
// Strings are cut by code points and lose lone surrogates and control characters.
function asString(v, fallback, maxLen = 64) {
    if (typeof v !== "string") return fallback;
    return Array.from(v.replace(/[\u0000-\u001f\u007f]/g, ""))
        .filter((ch) => !/^[\ud800-\udfff]$/.test(ch))
        .slice(0, maxLen)
        .join("");
}

function asInt(v, fallback, min, max) {
    return Number.isInteger(v) && v >= min && v <= max ? v : fallback;
}

function asFinite(v, limit) {
    return typeof v === "number" && Number.isFinite(v) && Math.abs(v) < limit ? v : null;
}

// Where a client's horse waits in another stage, or null.
function asHorsePlace(v) {
    if (!v || typeof v !== "object") return null;
    const stage = asString(v.stage, "", 7);
    const x = asFinite(v.x, 1e6), y = asFinite(v.y, 1e6), z = asFinite(v.z, 1e6);
    if (stage === "" || x === null || y === null || z === null) return null;
    return { stage, room: asInt(v.room, -1, -128, 127), x, y, z, angleY: asInt(v.angleY, 0, -32768, 32767) };
}

function asColor(v, fallback) {
    if (!v || typeof v !== "object") return fallback;
    const ch = (x, def) => asInt(x, def, 0, 255);
    return { r: ch(v.r, fallback.r), g: ch(v.g, fallback.g), b: ch(v.b, fallback.b) };
}

function updateCachedClientState(client, packet) {
    if ("name" in packet) client.name = asString(packet.name, client.name) || client.name;
    if ("color" in packet) client.color = asColor(packet.color, client.color);
    if ("online" in packet) client.online = packet.online === true;
    if ("isSaveLoaded" in packet) client.isSaveLoaded = packet.isSaveLoaded === true;
    // The next save may predate the team's progress, so it has to catch up again.
    if (!client.isSaveLoaded) client.caughtUp = false;
    if ("stageName" in packet) client.stageName = asString(packet.stageName, "", 7);
    if ("layerNo" in packet) client.layerNo = asInt(packet.layerNo, -1, -128, 127);
    if ("roomNo" in packet) client.roomNo = asInt(packet.roomNo, -1, -128, 127);
    if ("saveTblNo" in packet) client.saveTblNo = asInt(packet.saveTblNo, -1, -128, 127);
    if ("horseName" in packet) client.horseName = asString(packet.horseName, client.horseName, 32);
    if ("horsePlace" in packet) client.horsePlace = asHorsePlace(packet.horsePlace);
    // teamId is fixed at handshake.
}

// Unknown keys and wrong types are ignored; numbers are clamped.
const ROOM_STATE_LIMITS = {
    enemyCountMultiplier: [100, 500],
    enemyHealthMultiplier: [100, 500],
};

function applyRoomState(state, incoming) {
    for (const key of Object.keys(state)) {
        if (!(key in incoming)) continue;
        const v = incoming[key];
        if (typeof state[key] === "boolean") {
            if (typeof v === "boolean") state[key] = v;
        } else if (typeof state[key] === "number") {
            const [min, max] = ROOM_STATE_LIMITS[key] ?? [-2147483648, 2147483647];
            if (Number.isInteger(v)) state[key] = Math.min(max, Math.max(min, v));
        }
    }
}

function queueTeamPacket(room, key, packet) {
    let queue = room.teamQueues.get(key);
    if (!queue) {
        queue = [];
        room.teamQueues.set(key, queue);
    }
    queue.push(packet);
    if (queue.length > MAX_TEAM_QUEUE) {
        queue.splice(0, queue.length - MAX_TEAM_QUEUE);
    }
}

// Clients without a session key never match, not even each other.
function sameSession(client, sessionKey) {
    return client.sessionKey !== "" && sessionKey === client.sessionKey;
}

// Cached world state and/or queued packets, skipping the client's own session.
function sendTeamCatchUp(room, client, withState, withQueue) {
    const ownSession = (p) => p.clientId === client.clientId || sameSession(client, p.senderSessionKey);
    const cached = withState ? room.teamStates.get(worldKey(client)) : undefined;
    const sendState = cached !== undefined && !ownSession(cached);
    if (sendState) {
        send(client, { ...cached, fromCache: true });
    }
    let replayed = 0;
    if (withQueue) {
        for (const packet of room.teamQueues.get(worldKey(client)) ?? []) {
            if (ownSession(packet)) continue;
            send(client, { ...packet, fromQueue: true });
            replayed++;
        }
    }
    if (sendState || replayed > 0) {
        log(`[${client.name}] catch-up: ${sendState ? "cached world state" : "no world state"}, ${replayed} queued packet(s)`);
    }
}

// The team's latest STORY_MOVE arrive, never to the session that sent it.
function sendTeamStoryMove(room, client) {
    const cached = room.teamStoryMoves.get(client.teamId);
    if (!cached || cached.clientId === client.clientId || sameSession(client, cached.senderSessionKey)) {
        return;
    }
    send(client, { ...cached, fromCache: true, ageMs: Math.max(0, Date.now() - cached.serverTime) });
    log(`[${client.name}] catch-up: cached story move from ${text(cached.name)}`);
}

function assignOwnerIfNeeded(room) {
    if (room.ownerClientId !== null && room.clients.has(room.ownerClientId)) {
        return false;
    }
    const first = room.clients.keys().next();
    room.ownerClientId = first.done ? null : first.value;
    return true;
}

// Why a HANDSHAKE may not join, or null. Newer protocols join.
function handshakeRefusal(packet) {
    if (packet.app !== APP_ID) {
        return `This server runs Twili-Together protocol ${PROTOCOL_VERSION}; your client is not Twili-Together.`;
    }
    const v = packet.protocolVersion;
    if (!Number.isInteger(v) || v < PROTOCOL_VERSION) {
        return `This server runs Twili-Together protocol ${PROTOCOL_VERSION}; your client speaks protocol ` +
            `${typeof v === "number" ? v : "unknown"}. Update the mod.`;
    }
    return null;
}

function refuseHandshake(conn, packet, message) {
    log(`[${asString(packet.name, "") || "<unnamed>"}] rejected HANDSHAKE (incompatible, app=${text(packet.app)}, ` +
        `p=${text(packet.protocolVersion)}, ${conn.transport}): ${message}`);
    conn.send(JSON.stringify({ type: "SERVER_MESSAGE", message }));
    conn.send(JSON.stringify({ type: "DISABLE_CLIENT", message }));
    conn.close(4000, "incompatible client");
}

function handleHandshake(conn, packet) {
    const clientId = nextClientId++;
    const roomId = asString(packet.roomId, PUBLIC_ROOM_ID).trim();
    const room = getOrCreateRoom(roomId);
    clearTimeout(room.expiryTimer);
    room.expiryTimer = null;

    /** @type {Client} */
    const client = {
        conn,
        clientId,
        roomId,
        sessionKey: asString(packet.sessionKey, ""),
        name: asString(packet.name, "") || `Player ${clientId}`,
        teamId: asString(packet.teamId, ""),
        color: asColor(packet.color, { r: 255, g: 255, b: 255 }),
        modVersion: asString(packet.modVersion, "", 32),
        layout: asString(packet.layout, "", 16),
        transport: conn.transport,
        protocolVersion: Number.isInteger(packet.protocolVersion) ? packet.protocolVersion : 0,
        online: true,
        isSaveLoaded: false,
        caughtUp: false,
        stageName: "",
        layerNo: 0,
        roomNo: 0,
        saveTblNo: -1,
        horseName: "",
        horsePlace: null,
        lastTeleportRequestAt: 0,
        lastDamageAt: new Map(), // victim clientId -> Date.now() of the last forwarded hit
        awaitingResults: new Map(), // "attackerId:hitId" of hits forwarded to this victim -> Date.now()
    };
    room.clients.set(clientId, client);
    clientsById.set(clientId, client);
    // Set early so the close handler removes the client even if the rest throws.
    conn.client = client;
    const ownerChanged = assignOwnerIfNeeded(room);

    send(client, {
        type: "ALL_CLIENT_STATE",
        clients: [...room.clients.values()].map((c) => clientStateOf(c, clientId)),
        roomState: roomStatePacket(room).state,
    });

    broadcastRoom(room, clientId, { type: "UPDATE_CLIENT_STATE", ...clientStateOf(client, -1) });
    if (ownerChanged) {
        // The joiner already has the room state from ALL_CLIENT_STATE.
        broadcastRoom(room, clientId, roomStatePacket(room));
    }

    log(`[+] ${client.name} (id=${clientId}, team="${client.teamId}", session=${client.sessionKey || "none"}, v=${client.modVersion}/p${client.protocolVersion}, layout=${client.layout || "none"}, ${client.transport}) joined room ${describeRoom(room)}. Room size: ${room.clients.size}${room.ownerClientId === clientId ? " (owner)" : ""}`);
}

function handleDisconnect(client) {
    const room = rooms.get(client.roomId);
    clientsById.delete(client.clientId);
    if (!room) return;

    room.clients.delete(client.clientId);
    broadcastRoom(room, -1, { type: "UPDATE_CLIENT_STATE", clientId: client.clientId, online: false });
    if (assignOwnerIfNeeded(room) && room.ownerClientId !== null) {
        broadcastRoom(room, -1, roomStatePacket(room));
        log(`[room ${describeRoom(room)}] owner is now id=${room.ownerClientId}`);
    }
    log(`[-] ${client.name} (id=${client.clientId}) left room ${describeRoom(room)}. Room size: ${room.clients.size}`);
    if (room.clients.size === 0) {
        scheduleRoomExpiry(room);
    }
}

// Only to the named client while teleportMode is on; refusals are answered here.
function handleRequestTeleport(room, client, packet) {
    const requestId = asInt(packet.requestId, 0, 0, 0x7fffffff);
    const targetId = asInt(packet.targetClientId, 0, 0, 0x7fffffff);
    const refuse = (reason) => {
        log(`[${client.name}] REQUEST_TELEPORT -> ${targetId} refused (${reason})`);
        send(client, { type: "TELEPORT_TO", clientId: targetId, targetClientId: client.clientId,
                       requestId, ok: false, reason, fromServer: true });
    };
    const now = Date.now();
    if (now - client.lastTeleportRequestAt < TELEPORT_MIN_INTERVAL_MS) return refuse("rate-limited");
    client.lastTeleportRequestAt = now;
    if (!room.state.teleportMode) return refuse("disabled");
    const target = room.clients.get(targetId);
    if (!target || target.clientId === client.clientId || !target.online) return refuse("offline");
    if (!target.isSaveLoaded || !client.isSaveLoaded) return refuse("not-in-game");
    send(target, { type: "REQUEST_TELEPORT", clientId: client.clientId, targetClientId: targetId, requestId });
}

// Back to the requester only, rebuilt from checked fields.
function handleTeleportTo(room, client, packet) {
    const requester = room.clients.get(asInt(packet.targetClientId, 0, 0, 0x7fffffff));
    if (!requester || requester.clientId === client.clientId) return;
    const out = {
        type: "TELEPORT_TO",
        clientId: client.clientId,
        targetClientId: requester.clientId,
        requestId: asInt(packet.requestId, 0, 0, 0x7fffffff),
        ok: packet.ok === true,
    };
    if (out.ok) {
        const pos = packet.pos && typeof packet.pos === "object" ? packet.pos : {};
        const coord = (v) => (typeof v === "number" && Number.isFinite(v) && Math.abs(v) < 1e6 ? v : null);
        const x = coord(pos.x), y = coord(pos.y), z = coord(pos.z);
        // One character past the limit: a longer name is refused, not cut to another stage.
        const stageName = asString(packet.stageName, "", 8);
        const roomNo = asInt(packet.roomNo, -1, 0, 63);
        const layerNo = asInt(packet.layerNo, -1, 0, 14);
        if (x === null || y === null || z === null || stageName === "" || stageName.length > 7 ||
            roomNo < 0 || layerNo < 0) {
            out.ok = false;
            out.reason = "bad-destination";
        } else {
            Object.assign(out, {
                stageName, roomNo, layerNo, pos: { x, y, z },
                angleY: asInt(packet.angleY, 0, -32768, 32767),
                ageMs: asInt(packet.ageMs, 0, 0, 0x7fffffff),
            });
        }
    } else {
        out.reason = asString(packet.reason, "refused", 32);
    }
    send(requester, out);
}

function clampInt(v, fallback, min, max) {
    return Number.isInteger(v) ? Math.min(max, Math.max(min, v)) : fallback;
}

// "" is no team: players without a team fight everybody, teammates only with friendly fire.
function pvpAllowed(state, a, b) {
    return state.pvpFriendlyFire === true || a.teamId === "" || a.teamId !== b.teamId;
}

// Only to the named client; the victim still checks range, age and its own state.
function handleDamagePlayer(room, client, packet) {
    const hitId = asInt(packet.hitId, 0, 0, 0x7fffffff);
    const targetId = asInt(packet.targetClientId, 0, 0, 0x7fffffff);
    const refuse = (reason) => {
        log(`[${client.name}] DAMAGE_PLAYER -> ${targetId} refused (${reason})`);
        send(client, { type: "DAMAGE_RESULT", clientId: targetId, targetClientId: client.clientId,
                       hitId, result: "refused", reason, damage: 0, fromServer: true });
    };
    if (!room.state.pvpMode) return refuse("disabled");
    const target = room.clients.get(targetId);
    if (!target || target.clientId === client.clientId || !target.online) return refuse("offline");
    if (!client.isSaveLoaded || !target.isSaveLoaded) return refuse("not-in-game");
    if (client.stageName === "" || target.stageName !== client.stageName ||
        target.layerNo !== client.layerNo) return refuse("not-same-stage");
    if (target.protocolVersion !== client.protocolVersion) return refuse("protocol");
    if (!pvpAllowed(room.state, client, target)) return refuse("team");
    const now = Date.now();
    if (now - (client.lastDamageAt.get(targetId) ?? 0) < DAMAGE_MIN_INTERVAL_MS) return refuse("rate-limited");
    client.lastDamageAt.set(targetId, now);
    noteAwaitingResult(target, client.clientId, hitId, now);
    send(target, {
        type: "DAMAGE_PLAYER", clientId: client.clientId, targetClientId: targetId, hitId,
        kind: clampInt(packet.kind, 0, 0, PVP_KIND_COUNT - 1),
        damage: clampInt(packet.damage, 0, 0, MAX_PVP_DAMAGE),
        spl: asInt(packet.spl, 0, 0, PVP_KNOCKBACK_COUNT - 1), // unknown: the mildest
        dirY: clampInt(packet.dirY, 0, -32768, 32767),
        blocked: packet.blocked === true,
        viewSeq: clampInt(packet.viewSeq, 0, 0, 0xffffffff),
        stageName: client.stageName, layerNo: client.layerNo,
    });
}

// Hits forwarded to a victim and not answered yet; each gets at most one DAMAGE_RESULT.
const AWAITING_RESULT_TTL_MS = 30000;
const MAX_AWAITING_RESULTS = 64;
function noteAwaitingResult(victim, attackerId, hitId, now) {
    for (const [key, at] of victim.awaitingResults) {
        if (now - at <= AWAITING_RESULT_TTL_MS && victim.awaitingResults.size < MAX_AWAITING_RESULTS) break;
        victim.awaitingResults.delete(key); // oldest first (insertion order)
    }
    victim.awaitingResults.set(`${attackerId}:${hitId}`, now);
}

// Back to the attacker only, rebuilt field by field.
function handleDamageResult(room, client, packet) {
    const attacker = room.clients.get(asInt(packet.targetClientId, 0, 0, 0x7fffffff));
    if (!attacker || attacker.clientId === client.clientId) return;
    const hitId = asInt(packet.hitId, 0, 0, 0x7fffffff);
    const key = `${attacker.clientId}:${hitId}`;
    const at = client.awaitingResults.get(key);
    if (at === undefined || Date.now() - at > AWAITING_RESULT_TTL_MS) {
        log(`[${client.name}] rejected DAMAGE_RESULT ${hitId} (no such hit from ${attacker.clientId})`);
        return;
    }
    client.awaitingResults.delete(key);
    send(attacker, {
        type: "DAMAGE_RESULT", clientId: client.clientId, targetClientId: attacker.clientId,
        hitId,
        result: DAMAGE_RESULTS.has(packet.result) ? packet.result : "dropped",
        reason: asString(packet.reason, "", 32),
        damage: clampInt(packet.damage, 0, 0, MAX_PVP_DAMAGE),
    });
}

function handlePacket(client, packet) {
    const room = rooms.get(client.roomId);
    if (!room) return;

    if (SERVER_ONLY_TYPES.has(packet.type)) {
        log(`[${client.name}] rejected ${packet.type} (server-only packet)`);
        return;
    }

    if (!packet.quiet) {
        log(`[${client.name}] ${packetSummary(packet)}`);
    }

    if (packet.type === "UPDATE_CLIENT_STATE") {
        updateCachedClientState(client, packet);
        // Relay the sanitized server-side view, never the raw packet.
        broadcastRoom(room, client.clientId, { type: "UPDATE_CLIENT_STATE", ...clientStateOf(client, -1) });
        return;
    }

    // Tag all relayed packets with the sender's identity.
    const relayed = { ...packet, clientId: client.clientId };
    // Only the server numbers queued packets.
    delete relayed.queueEpoch;
    delete relayed.queueSeq;

    if (packet.type === "UPDATE_ROOM_STATE") {
        if (room.ownerClientId !== client.clientId) {
            log(`[${client.name}] rejected UPDATE_ROOM_STATE (not the owner)`);
            return;
        }
        const incoming = packet.state && typeof packet.state === "object" ? packet.state : packet;
        applyRoomState(room.state, incoming);
        broadcastRoom(room, -1, roomStatePacket(room));
        return;
    }

    if (packet.type === "REQUEST_TELEPORT") {
        handleRequestTeleport(room, client, packet);
        return;
    }
    if (packet.type === "TELEPORT_TO") {
        handleTeleportTo(room, client, packet);
        return;
    }
    if (packet.type === "DAMAGE_PLAYER") {
        handleDamagePlayer(room, client, packet);
        return;
    }
    if (packet.type === "DAMAGE_RESULT") {
        handleDamageResult(room, client, packet);
        return;
    }

    if (PRESENCE_PACKET_TYPES.has(packet.type)) {
        broadcastPresence(room, client, relayed);
        return;
    }

    if (TEAM_PRESENCE_PACKET_TYPES.has(packet.type)) {
        relayed.teamId = client.teamId;
        relayed.senderSessionKey = client.sessionKey;
        relayed.addToQueue = false;
        broadcastTeamPresence(room, client, relayed);
        return;
    }

    if (TEAM_STORY_PACKET_TYPES.has(packet.type)) {
        relayed.teamId = client.teamId;
        relayed.senderSessionKey = client.sessionKey;
        relayed.serverTime = Date.now();
        relayed.addToQueue = false;
        // Relayed first, so a packet that cannot be serialized throws before it is cached.
        broadcastTeam(room, client, relayed);
        if (packet.ph === "arrive") {
            room.teamStoryMoves.set(client.teamId, relayed);
        }
        return;
    }

    if (TEAM_PACKET_TYPES.has(packet.type)) {
        relayed.teamId = client.teamId;
        relayed.senderSessionKey = client.sessionKey;
        // The server's view of the sender, so queued and cached packets still name them.
        relayed.senderName = client.name;
        relayed.senderColor = client.color;

        // Stored packets are relayed first, so an unserializable one throws before it is stored.
        if (packet.type === "UPDATE_WORLD_STATE") {
            if (packet.targetClientId) {
                // An answer to one REQUEST_WORLD_STATE; never cached, as cache and queue change together.
                const target = room.clients.get(packet.targetClientId);
                if (target && target.teamId === client.teamId && target.layout === client.layout) {
                    send(target, relayed);
                    target.caughtUp = true;
                }
                return;
            }
            broadcastWorld(room, client, relayed);
            // Fresh full state supersedes everything queued for this team and layout.
            room.teamStates.set(worldKey(client), relayed);
            room.teamQueues.set(worldKey(client), []);
            return;
        }

        if (packet.type === "REQUEST_WORLD_STATE") {
            // Caught-up teammates (same protocol and layout) answer, not the requester's old connection.
            const live = [...room.clients.values()].filter((c) =>
                c.clientId !== client.clientId && c.teamId === client.teamId && c.isSaveLoaded &&
                c.caughtUp && c.protocolVersion === client.protocolVersion &&
                c.layout === client.layout && !sameSession(client, c.sessionKey));
            // The cache stands in only for absent teammates: it may predate flags cleared since.
            sendTeamCatchUp(room, client, live.length === 0, packet.catchUp === true);
            if (packet.catchUp === true) {
                sendTeamStoryMove(room, client);
            }
            if (live.length === 0) {
                // Ordered connection: this catch-up is merged before any request forwarded later.
                client.caughtUp = true;
            }
            for (const c of live) {
                send(c, relayed);
            }
            return;
        }

        if (packet.addToQueue) {
            // Numbered so a reconnecting client can skip replayed packets it already applied.
            const seq = (room.teamQueueSeqs.get(worldKey(client)) ?? 0) + 1;
            room.teamQueueSeqs.set(worldKey(client), seq);
            relayed.queueEpoch = `${room.queueEpoch}:${client.layout}:${client.teamId}`;
            relayed.queueSeq = seq;
        }
        broadcastWorld(room, client, relayed);
        if (packet.addToQueue) {
            queueTeamPacket(room, worldKey(client), relayed);
        }
        return;
    }

    // Unknown or generic packets: relay to the whole room.
    broadcastRoom(room, client.clientId, relayed);
}

// A TCP socket with the surface the relay uses on a WebSocket. Each message is a 4-byte
// little-endian length followed by that many bytes of UTF-8 JSON.
class TcpConnection extends EventEmitter {
    constructor(socket) {
        super();
        this.socket = socket;
        this.transport = "tcp";
        this.readyState = 1;
        this.isAlive = true;
        this.client = null;
        this.pending = Buffer.alloc(0);
        this.closeTimer = null;
        socket.setNoDelay(true);
        this.idleTimer = setTimeout(() => {
            log(`[${this.describe()}] missed keepalive, dropping connection`);
            this.terminate();
        }, Math.min(TCP_TIMEOUT_MS, MAX_TIMER_MS));
        socket.on("data", (chunk) => this.onData(chunk));
        socket.on("error", (err) => this.emit("error", err));
        socket.on("close", () => {
            this.readyState = 3;
            clearTimeout(this.idleTimer);
            clearTimeout(this.closeTimer);
            this.emit("close");
        });
    }

    describe() {
        return this.client ? this.client.name : "<no handshake>";
    }

    onData(chunk) {
        if (this.readyState !== 1) return;
        this.idleTimer.refresh();
        this.pending = this.pending.length ? Buffer.concat([this.pending, chunk]) : chunk;
        while (this.readyState === 1 && this.pending.length >= 4) {
            const n = this.pending.readUInt32LE(0);
            if (n < 1 || n > MAX_TCP_FRAME) {
                log(`[${this.describe()}] bad tcp frame length ${n}, dropping connection`);
                this.terminate();
                return;
            }
            if (this.pending.length < 4 + n) break;
            const body = this.pending.subarray(4, 4 + n);
            this.pending = this.pending.subarray(4 + n);
            this.emit("message", body);
        }
    }

    send(data) {
        if (this.readyState !== 1) return;
        const body = Buffer.from(data, "utf8");
        if (body.length > MAX_TCP_FRAME) {
            log(`[${this.describe()}] tcp packet of ${body.length} bytes not sent (over the frame limit)`);
            return;
        }
        const head = Buffer.alloc(4);
        head.writeUInt32LE(body.length, 0);
        this.socket.write(Buffer.concat([head, body]));
    }

    ping() {}

    // Flushes what was sent, then closes; a peer that keeps its side open is cut off.
    close() {
        if (this.readyState !== 1) return;
        this.readyState = 2;
        clearTimeout(this.idleTimer);
        this.socket.end();
        this.closeTimer = setTimeout(() => this.socket.destroy(), 2000);
    }

    terminate() {
        if (this.readyState === 3) return;
        this.readyState = 3;
        this.socket.destroy();
    }
}

function createWebSocketServer() {
    const handleProtocols = (protocols) => (protocols.has(WS_SUBPROTOCOL) ? WS_SUBPROTOCOL : false);
    if (TLS_CERT && TLS_KEY) {
        const https = require("https");
        const server = https.createServer({ cert: fs.readFileSync(TLS_CERT), key: fs.readFileSync(TLS_KEY) }, (req, res) => {
            res.writeHead(426, { "Content-Type": "text/plain" });
            res.end("Upgrade Required");
        });
        const wss = new WebSocketServer({ server, handleProtocols });
        server.listen(PORT);
        return wss;
    }
    return new WebSocketServer({ port: PORT, handleProtocols });
}

const useTls = Boolean(TLS_CERT && TLS_KEY);
log(`Twili-Together relay server starting on ${useTls ? "wss" : "ws"}://localhost:${PORT}`);
log(`Logging to ${LOG_PATH}`);
if (useTls) {
    log(`TLS on: cert ${TLS_CERT}, key ${TLS_KEY}`);
} else {
    log(`TLS off${TLS_CERT || TLS_KEY ? " (TT_TLS_CERT and TT_TLS_KEY must both be set)" : ""}: plain ws, use a TLS-terminating proxy for wss://`);
}

const wss = createWebSocketServer();

function startTcpRelay() {
    if (TCP_PORT === null) {
        log("tcp relay off");
        return;
    }
    const tcpServer = net.createServer((socket) => onConnection(new TcpConnection(socket)));
    tcpServer.listen(TCP_PORT, () => log(`tcp relay on port ${tcpServer.address().port}`));
}

// The TCP line always follows the WebSocket one.
wss.on("listening", () => {
    log(`listening on port ${wss.address().port}`);
    startTcpRelay();
});

// Drops WebSocket connections that stop answering pings.
setInterval(() => {
    for (const ws of wss.clients) {
        if (!ws.isAlive) {
            log(`[${ws.client ? ws.client.name : "<no handshake>"}] missed a heartbeat, dropping connection`);
            ws.terminate();
            continue;
        }
        ws.isAlive = false;
        ws.ping();
    }
}, Math.min(PING_MS, MAX_TIMER_MS));

function addTestJitter(conn) {
    const rawSend = conn.send.bind(conn);
    const queue = [];
    let timer = null;
    const pump = () => {
        timer = null;
        const now = Date.now();
        while (queue.length && queue[0].at <= now) {
            const { data } = queue.shift();
            if (conn.readyState === 1) rawSend(data);
        }
        if (queue.length) timer = setTimeout(pump, queue[0].at - now);
    };
    conn.send = (data) => {
        const now = Date.now();
        // Never earlier than the previous packet: jitter, not reordering.
        const at = Math.max(queue.length ? queue[queue.length - 1].at : 0, now + Math.random() * TEST_JITTER_MS);
        queue.push({ at, data });
        if (!timer) timer = setTimeout(pump, at - now);
    };
}

const KEEPALIVE_MSG = JSON.stringify({ type: "KEEPALIVE" });

function onMessage(conn, data) {
    if (conn.readyState !== 1) return; // closing, e.g. after a refused handshake
    let packet;
    try {
        packet = JSON.parse(data.toString());
    } catch {
        return;
    }
    if (!packet || typeof packet.type !== "string") return;

    if (packet.type === "KEEPALIVE") {
        if (conn.transport === "tcp") conn.send(KEEPALIVE_MSG);
        return;
    }

    // The try blocks keep one bad packet from taking the relay down for every room.
    if (packet.type === "HANDSHAKE") {
        if (conn.client !== null) {
            log(`[${conn.client.name}] rejected HANDSHAKE (already joined)`);
            return;
        }
        const refusal = handshakeRefusal(packet);
        if (refusal !== null) {
            refuseHandshake(conn, packet, refusal);
            return;
        }
        try {
            handleHandshake(conn, packet);
        } catch (e) {
            log(`HANDSHAKE failed: ${e.stack}`);
            conn.terminate();
        }
        return;
    }

    if (conn.client === null) return; // drop packets before handshake
    try {
        handlePacket(conn.client, packet);
    } catch (e) {
        log(`[${conn.client.name}] ${packet.type} failed: ${e.stack}`);
    }
}

function onConnection(conn) {
    conn.client = null; // the Client, once handleHandshake registers it
    conn.isAlive = true;
    if (TEST_JITTER_MS > 0) addTestJitter(conn);
    conn.on("message", (data) => onMessage(conn, data));
    conn.on("close", () => {
        if (conn.client !== null) {
            handleDisconnect(conn.client);
            conn.client = null;
        }
    });
    conn.on("error", (err) => {
        log(`${conn.transport} error: ${err.message}`);
    });
}

wss.on("connection", (ws) => {
    ws.transport = "ws";
    ws.on("pong", () => {
        ws.isAlive = true;
    });
    onConnection(ws);
});
