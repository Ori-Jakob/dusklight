// Relay protocol tests with fake WebSocket and TCP clients (npm test).

const { test } = require("node:test");
const assert = require("node:assert/strict");
const { spawn } = require("node:child_process");
const crypto = require("node:crypto");
const fs = require("node:fs");
const http = require("node:http");
const net = require("node:net");
const os = require("node:os");
const path = require("node:path");
const WebSocket = require("ws");

const SERVER = path.join(__dirname, "..", "server.js");

// Vanilla and in game, so team traffic flows as it did before team games.
const VANILLA = { inGame: true, kind: "vanilla", key: "vanilla", display: { name: "", mode: "Vanilla" } };
const HANDSHAKE = { type: "HANDSHAKE", app: "twili-together", protocolVersion: 5, modVersion: "test",
                    layout: "0123456789abcdef", name: "P", teamId: "", roomId: "", game: VANILLA };

// Resolves once both the WebSocket and the TCP listener are up.
async function startServer(env = {}, args = ["0"]) {
    const logPath = path.join(fs.mkdtempSync(path.join(os.tmpdir(), "twili-test-")), "server.log");
    const proc = spawn(process.execPath, [SERVER, ...args], {
        env: { ...process.env, TT_LOG_PATH: logPath, ...env },
        stdio: ["ignore", "pipe", "pipe"],
    });
    let out = "";
    let err = "";
    proc.stderr.on("data", (d) => { err += d.toString(); });
    const ports = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`server did not start:\n${out}${err}`)), 5000);
        proc.stdout.on("data", (d) => {
            out += d.toString();
            const ws = out.match(/listening on port (\d+)/);
            const tcp = out.match(/tcp relay on port (\d+)/);
            if (ws && tcp) {
                clearTimeout(timer);
                resolve({ port: parseInt(ws[1], 10), tcpPort: parseInt(tcp[1], 10) });
            }
        });
        proc.on("exit", (code) => reject(new Error(`server exited early (${code}):\n${out}${err}`)));
    });
    return {
        ...ports,
        logPath,
        output: () => out,
        stop: () => new Promise((resolve) => {
            if (proc.exitCode !== null) return resolve();
            proc.once("exit", resolve);
            proc.kill();
        }),
    };
}

// A fake game client that records every packet it receives.
class FakeClient {
    constructor(server, wsOptions, protocols) {
        this.server = server;
        this.wsOptions = wsOptions;
        this.protocols = protocols;
        this.transport = "ws";
        this.opened = false;
        this.inbox = [];
        this.waiters = [];
    }

    open() {
        return new Promise((resolve, reject) => {
            const url = `ws://127.0.0.1:${this.server.port}`;
            this.ws = this.protocols ? new WebSocket(url, this.protocols, this.wsOptions) : new WebSocket(url, this.wsOptions);
            this.whenClosed = new Promise((r) => this.ws.once("close", (code) => r(code)));
            this.ws.on("open", () => {
                this.opened = true;
                resolve();
            });
            this.ws.on("error", reject);
            this.ws.on("message", (data) => this.receive(JSON.parse(data.toString())));
        });
    }

    receive(packet) {
        const w = this.waiters.find((x) => x.pred(packet));
        if (w) {
            // Consumed by a waiter: keep it out of the inbox so expectNone ignores it.
            this.waiters.splice(this.waiters.indexOf(w), 1);
            clearTimeout(w.timer);
            w.resolve(packet);
        } else {
            this.inbox.push(packet);
        }
    }

    send(packet) {
        this.ws.send(typeof packet === "string" ? packet : JSON.stringify(packet));
    }

    // Resolves with the first packet (already received or future) matching pred.
    waitFor(pred, what = "packet", timeoutMs = 2000) {
        const seen = this.inbox.find(pred);
        if (seen) {
            this.inbox.splice(this.inbox.indexOf(seen), 1);
            return Promise.resolve(seen);
        }
        return new Promise((resolve, reject) => {
            const w = { pred, resolve };
            w.timer = setTimeout(() => {
                this.waiters.splice(this.waiters.indexOf(w), 1);
                reject(new Error(`timed out waiting for ${what}; inbox: ${JSON.stringify(this.inbox.map((p) => p.type))}`));
            }, timeoutMs);
            this.waiters.push(w);
        });
    }

    waitType(type, extra = () => true, timeoutMs) {
        return this.waitFor((p) => p.type === type && extra(p), type, timeoutMs);
    }

    // Asserts nothing matching pred arrives within ms.
    async expectNone(pred, what, ms = 300) {
        await new Promise((r) => setTimeout(r, ms));
        const hit = this.inbox.find(pred);
        assert.equal(hit, undefined, `unexpected ${what}: ${JSON.stringify(hit)}`);
    }

    async join(fields = {}) {
        if (!this.opened) await this.open();
        this.send({ ...HANDSHAKE, transport: this.transport, ...fields });
        const all = await this.waitType("ALL_CLIENT_STATE");
        this.self = all.clients.find((c) => c.self);
        this.id = this.self.clientId;
        return all;
    }

    enterStage(stageName, layerNo = 0, extra = {}) {
        this.send({ type: "UPDATE_CLIENT_STATE", online: true, isSaveLoaded: true, stageName, layerNo, roomNo: 0, saveTblNo: 1, ...extra });
    }

    close() {
        return new Promise((resolve) => {
            if (this.ws.readyState === WebSocket.CLOSED) return resolve();
            this.ws.once("close", resolve);
            this.ws.close();
        });
    }
}

// One length-prefixed TCP frame: 4-byte little-endian length, then UTF-8 JSON.
function frame(packet) {
    const body = Buffer.from(typeof packet === "string" ? packet : JSON.stringify(packet));
    const head = Buffer.alloc(4);
    head.writeUInt32LE(body.length);
    return Buffer.concat([head, body]);
}

// The same fake client over the TCP transport.
class TcpFakeClient extends FakeClient {
    constructor(server) {
        super(server);
        this.transport = "tcp";
    }

    open() {
        return new Promise((resolve, reject) => {
            this.sock = net.connect(this.server.tcpPort, "127.0.0.1", () => {
                this.opened = true;
                resolve();
            });
            this.sock.setNoDelay(true);
            this.whenClosed = new Promise((r) => this.sock.once("close", () => r()));
            this.sock.on("error", reject);
            let pending = Buffer.alloc(0);
            this.sock.on("data", (chunk) => {
                pending = Buffer.concat([pending, chunk]);
                while (pending.length >= 4 && pending.length >= 4 + pending.readUInt32LE(0)) {
                    const n = pending.readUInt32LE(0);
                    this.receive(JSON.parse(pending.subarray(4, 4 + n).toString()));
                    pending = pending.subarray(4 + n);
                }
            });
        });
    }

    send(packet) {
        this.write(frame(packet));
    }

    write(bytes) {
        this.sock.write(bytes);
    }

    close() {
        if (!this.sock.destroyed) this.sock.end();
        return this.whenClosed;
    }
}

async function withServer(fn, env, args) {
    const server = await startServer(env, args);
    const clients = [];
    const track = (c) => {
        clients.push(c);
        return c;
    };
    const mk = (wsOptions, protocols) => track(new FakeClient(server, wsOptions, protocols));
    const mkTcp = () => track(new TcpFakeClient(server));
    try {
        await fn(mk, server, mkTcp);
    } finally {
        await Promise.all(clients.map((c) => (c.ws || c.sock ? c.close() : null)));
        await server.stop();
    }
}

const settle = (ms = 100) => new Promise((r) => setTimeout(r, ms));

// The log is written asynchronously, so a line may land after the packet that caused it.
async function waitForLog(server, pattern, timeoutMs = 2000) {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
        const text = fs.existsSync(server.logPath) ? fs.readFileSync(server.logPath, "utf8") : "";
        if (pattern.test(text)) return text;
        if (Date.now() > deadline) assert.fail(`server log never matched ${pattern}:
${text}`);
        await settle(20);
    }
}

// A bare WebSocket upgrade request, to see which subprotocol the server picks.
function rawUpgrade(port, protocols) {
    return new Promise((resolve, reject) => {
        const req = http.request({
            host: "127.0.0.1",
            port,
            headers: {
                Connection: "Upgrade",
                Upgrade: "websocket",
                "Sec-WebSocket-Version": "13",
                "Sec-WebSocket-Key": crypto.randomBytes(16).toString("base64"),
                "Sec-WebSocket-Protocol": protocols,
            },
        });
        req.on("upgrade", (res, socket) => {
            socket.destroy();
            resolve(res);
        });
        req.on("response", resolve);
        req.on("error", reject);
        req.end();
    });
}

test("handshake returns room roster and makes the first client owner", () => withServer(async (mk) => {
    const a = mk();
    const all = await a.join({ name: "Alice" });
    assert.equal(all.clients.length, 1);
    assert.equal(all.clients[0].name, "Alice");
    assert.equal(all.clients[0].self, true);
    assert.equal(all.roomState.ownerClientId, a.id);
    assert.equal(all.roomState.syncWorldState, true);
    assert.equal(all.clients[0].modVersion, "test");
    assert.equal(all.clients[0].layout, "0123456789abcdef");
    assert.equal(all.clients[0].protocolVersion, 5);
    assert.equal("clientVersion" in all.clients[0], false);
}));

test("second client sees the first and the first is told about the second", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A" });
    const all = await b.join({ name: "B" });
    assert.deepEqual(all.clients.map((c) => c.name).sort(), ["A", "B"]);
    assert.equal(all.clients.find((c) => c.name === "B").self, true);
    assert.equal(all.roomState.ownerClientId, a.id);
    const joined = await a.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === b.id);
    assert.equal(joined.name, "B");
    assert.equal(joined.self, false);
}));

test("rooms are isolated", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ roomId: "one" });
    const all = await b.join({ roomId: "two" });
    assert.equal(all.clients.length, 1);
    assert.equal(all.roomState.ownerClientId, b.id);
    a.enterStage("F_SP103");
    b.enterStage("F_SP103");
    await settle();
    a.send({ type: "PLAYER_UPDATE", quiet: true, pos: { x: 1, y: 2, z: 3 } });
    a.send({ type: "AUTOTEST_SIGNAL", name: "x" });
    await b.expectNone((p) => p.type === "PLAYER_UPDATE" || p.type === "AUTOTEST_SIGNAL" ||
        (p.type === "UPDATE_CLIENT_STATE" && p.clientId === a.id), "cross-room packet");
}));

test("presence packets only reach clients in the same stage and layer", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join();
    await b.join();
    await c.join();
    a.enterStage("F_SP103", 0);
    b.enterStage("F_SP103", 0);
    c.enterStage("F_SP103", 1);
    await settle();
    a.send({ type: "PLAYER_UPDATE", quiet: true, pos: { x: 1, y: 2, z: 3 } });
    const got = await b.waitType("PLAYER_UPDATE");
    assert.equal(got.clientId, a.id, "relayed packets are stamped with the sender id");
    assert.deepEqual(got.pos, { x: 1, y: 2, z: 3 });
    await c.expectNone((p) => p.type === "PLAYER_UPDATE", "PLAYER_UPDATE in another layer");
    await a.expectNone((p) => p.type === "PLAYER_UPDATE", "PLAYER_UPDATE echoed to sender");
}));

test("presence packets are not relayed before a save is loaded", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join();
    await b.join();
    b.enterStage("F_SP103", 0);
    a.send({ type: "UPDATE_CLIENT_STATE", online: true, isSaveLoaded: false, stageName: "F_SP103", layerNo: 0 });
    await settle();
    a.send({ type: "PLAYER_UPDATE", quiet: true });
    await b.expectNone((p) => p.type === "PLAYER_UPDATE", "PLAYER_UPDATE from a client with no save");
}));

test("team packets are scoped to the sender's team and stamped", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ teamId: "red" });
    await b.join({ teamId: "red" });
    await c.join({ teamId: "blue" });
    a.send({ type: "SET_FLAG", category: "SWITCH", flagNo: 12, roomNo: 2, stageName: "D_MN05", teamId: "blue" });
    const got = await b.waitType("SET_FLAG");
    assert.equal(got.clientId, a.id);
    assert.equal(got.teamId, "red", "server overrides a spoofed teamId with the sender's real team");
    await c.expectNone((p) => p.type === "SET_FLAG", "SET_FLAG to another team");
}));

test("ENEMY_DEFEATED reaches only teammates in the sender's stage and layer, stamped", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const otherTeam = mk();
    const otherLayer = mk();
    const noSave = mk();
    const otherProtocol = mk();
    await a.join({ teamId: "red", sessionKey: "ka" });
    await b.join({ teamId: "red" });
    await otherTeam.join({ teamId: "blue" });
    await otherLayer.join({ teamId: "red" });
    await noSave.join({ teamId: "red" });
    await otherProtocol.join({ teamId: "red", protocolVersion: 6 });
    a.enterStage("F_SP108", 0);
    b.enterStage("F_SP108", 0);
    otherTeam.enterStage("F_SP108", 0);
    otherLayer.enterStage("F_SP108", 1);
    noSave.enterStage("F_SP108", 0, { isSaveLoaded: false });
    otherProtocol.enterStage("F_SP108", 0);
    await settle();
    const kills = [{ roomNo: 0, procName: 485, params: 0xffffff00, setId: 0xffff, home: { x: 1, y: 2, z: 3 } }];
    a.send({ type: "ENEMY_DEFEATED", v: 1, teamId: "blue", addToQueue: true, stageName: "F_SP108", layerNo: 0, kills });
    const got = await b.waitType("ENEMY_DEFEATED");
    assert.equal(got.clientId, a.id);
    assert.equal(got.teamId, "red", "server overrides a spoofed teamId with the sender's real team");
    assert.equal(got.senderSessionKey, "ka");
    assert.equal(got.addToQueue, false);
    assert.deepEqual(got.kills, kills);
    for (const [c, what] of [[otherTeam, "another team"], [otherLayer, "another layer"], [noSave, "a client with no save"],
        [otherProtocol, "another protocol version"], [a, "the sender"]]) {
        await c.expectNone((p) => p.type === "ENEMY_DEFEATED", `ENEMY_DEFEATED to ${what}`);
    }
}));

test("ENEMY_DAMAGE reaches only teammates in the sender's stage and layer, stamped", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const otherTeam = mk();
    const otherLayer = mk();
    const noSave = mk();
    await a.join({ teamId: "red", sessionKey: "ka" });
    await b.join({ teamId: "red" });
    await otherTeam.join({ teamId: "blue" });
    await otherLayer.join({ teamId: "red" });
    await noSave.join({ teamId: "red" });
    a.enterStage("F_SP108", 0);
    b.enterStage("F_SP108", 0);
    otherTeam.enterStage("F_SP108", 0);
    otherLayer.enterStage("F_SP108", 1);
    noSave.enterStage("F_SP108", 0, { isSaveLoaded: false });
    await settle();
    const hits = [{ roomNo: 0, procName: 485, params: 0xffffff00, setId: 0xffff, dup: 1, home: { x: 1, y: 2, z: 3 },
        dmg: 20, pct: 200, hpAfter: 30 }];
    a.send({ type: "ENEMY_DAMAGE", v: 1, quiet: true, teamId: "blue", addToQueue: true, stageName: "F_SP108", layerNo: 0, hits });
    const got = await b.waitType("ENEMY_DAMAGE");
    assert.equal(got.clientId, a.id);
    assert.equal(got.teamId, "red", "server overrides a spoofed teamId with the sender's real team");
    assert.equal(got.senderSessionKey, "ka");
    assert.equal(got.addToQueue, false);
    assert.deepEqual(got.hits, hits);
    for (const [c, what] of [[otherTeam, "another team"], [otherLayer, "another layer"], [noSave, "a client with no save"],
        [a, "the sender"]]) {
        await c.expectNone((p) => p.type === "ENEMY_DAMAGE", `ENEMY_DAMAGE to ${what}`);
    }
}));

test("queued team packets are numbered by the server, live and in the replay", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "t", sessionKey: "ka" });
    await b.join({ teamId: "t", sessionKey: "kb" });
    // A forged number is replaced; a packet that is not queued carries none.
    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true, queueSeq: 999999, queueEpoch: "evil" });
    a.send({ type: "SET_FLAG", flagNo: 2, addToQueue: true });
    a.send({ type: "SET_FLAG", flagNo: 9, addToQueue: false, queueSeq: 5, queueEpoch: "evil" });
    const f1 = await b.waitType("SET_FLAG", (p) => p.flagNo === 1);
    const f2 = await b.waitType("SET_FLAG", (p) => p.flagNo === 2);
    const f9 = await b.waitType("SET_FLAG", (p) => p.flagNo === 9);
    assert.equal(typeof f1.queueEpoch, "string");
    assert.notEqual(f1.queueEpoch, "evil");
    assert.equal(f2.queueEpoch, f1.queueEpoch);
    assert.equal(f2.queueSeq, f1.queueSeq + 1);
    assert.ok(f1.queueSeq < 1000);
    assert.equal(f9.queueSeq, undefined);
    assert.equal(f9.queueEpoch, undefined);
    // b's next session (a reconnect) gets the same numbers in the replay.
    b.close();
    const b2 = mk();
    await b2.join({ teamId: "t", sessionKey: "kb" });
    b2.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const r1 = await b2.waitType("SET_FLAG", (p) => p.flagNo === 1 && p.fromQueue === true);
    const r2 = await b2.waitType("SET_FLAG", (p) => p.flagNo === 2 && p.fromQueue === true);
    assert.deepEqual([r1.queueEpoch, r1.queueSeq, r2.queueSeq], [f1.queueEpoch, f1.queueSeq, f2.queueSeq]);
    // Another team numbers separately (its own epoch).
    const c = mk();
    await c.join({ teamId: "u" });
    const d = mk();
    await d.join({ teamId: "u" });
    c.send({ type: "SET_FLAG", flagNo: 4, addToQueue: true });
    const g = await d.waitType("SET_FLAG", (p) => p.flagNo === 4);
    assert.notEqual(g.queueEpoch, f1.queueEpoch);
    assert.equal(g.queueSeq, 1);
}));

test("ENEMY_DEFEATED is never queued for a teammate who joins later", () => withServer(async (mk) => {
    const a = mk();
    await a.join({ teamId: "t" });
    a.enterStage("F_SP108", 0);
    await settle();
    a.send({ type: "ENEMY_DEFEATED", v: 1, addToQueue: true, stageName: "F_SP108", layerNo: 0, kills: [] });
    a.send({ type: "SET_FLAG", flagNo: 3, addToQueue: true });
    await settle();
    const b = mk();
    await b.join({ teamId: "t" });
    b.enterStage("F_SP108", 0);
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await b.waitType("SET_FLAG", (p) => p.flagNo === 3 && p.fromQueue === true);
    await b.expectNone((p) => p.type === "ENEMY_DEFEATED", "replayed ENEMY_DEFEATED");
}));

test("ENEMY_DAMAGE is never queued for a teammate who joins later", () => withServer(async (mk) => {
    const a = mk();
    await a.join({ teamId: "t" });
    a.enterStage("F_SP108", 0);
    await settle();
    a.send({ type: "ENEMY_DAMAGE", v: 1, addToQueue: true, stageName: "F_SP108", layerNo: 0, hits: [] });
    a.send({ type: "SET_FLAG", flagNo: 3, addToQueue: true });
    await settle();
    const b = mk();
    await b.join({ teamId: "t" });
    b.enterStage("F_SP108", 0);
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await b.waitType("SET_FLAG", (p) => p.flagNo === 3 && p.fromQueue === true);
    await b.expectNone((p) => p.type === "ENEMY_DAMAGE", "replayed ENEMY_DAMAGE");
}));

test("STORY_EVENT reaches only teammates in the sender's stage and layer, stamped and never queued", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const otherTeam = mk();
    const otherLayer = mk();
    await a.join({ teamId: "red", sessionKey: "ka" });
    await b.join({ teamId: "red" });
    await otherTeam.join({ teamId: "blue" });
    await otherLayer.join({ teamId: "red" });
    a.enterStage("F_SP108", 8);
    b.enterStage("F_SP108", 8);
    otherTeam.enterStage("F_SP108", 8);
    otherLayer.enterStage("F_SP108", 14);
    await settle();
    a.send({ type: "STORY_EVENT", sv: 1, ph: "start", id: 7, stage: "F_SP108", room: 0, layer: 8, m: 0,
             teamId: "blue", addToQueue: true });
    const got = await b.waitType("STORY_EVENT");
    assert.equal(got.clientId, a.id);
    assert.equal(got.teamId, "red", "server overrides a spoofed teamId with the sender's real team");
    assert.equal(got.senderSessionKey, "ka");
    assert.equal(got.addToQueue, false);
    for (const [c, what] of [[otherTeam, "another team"], [otherLayer, "another layer"], [a, "the sender"]]) {
        await c.expectNone((p) => p.type === "STORY_EVENT", `STORY_EVENT to ${what}`);
    }
    const late = mk();
    await late.join({ teamId: "red" });
    late.enterStage("F_SP108", 8);
    late.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await late.expectNone((p) => p.type === "STORY_EVENT", "replayed STORY_EVENT");
}));

test("STORY_MOVE reaches teammates in any stage, not other teams, and is never queued", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const otherTeam = mk();
    await a.join({ teamId: "red", sessionKey: "ka" });
    await b.join({ teamId: "red" });
    await otherTeam.join({ teamId: "blue" });
    a.enterStage("R_SP107", 14);
    b.enterStage("R_SP01", 0);
    otherTeam.enterStage("R_SP01", 0);
    await settle();
    const before = Date.now();
    a.send({ type: "STORY_MOVE", sv: 1, ph: "arrive", mid: "m1", name: "Kira", addToQueue: true,
             from: { stage: "F_SP108", room: 0 }, to: { stage: "R_SP107", room: 0, point: 0 } });
    const got = await b.waitType("STORY_MOVE");
    assert.equal(got.clientId, a.id);
    assert.equal(got.teamId, "red");
    assert.equal(got.senderSessionKey, "ka");
    assert.equal(got.addToQueue, false);
    // Process clocks may differ by a millisecond or two.
    assert.ok(Math.abs(got.serverTime - before) < 5000, "stamped with the server's time");
    assert.equal(got.fromCache, undefined);
    await otherTeam.expectNone((p) => p.type === "STORY_MOVE", "STORY_MOVE to another team");
    await a.expectNone((p) => p.type === "STORY_MOVE", "STORY_MOVE to the sender");
}));

test("the latest STORY_MOVE arrive is replayed on catch-up, with its age, except to its own session", () => withServer(async (mk) => {
    const a = mk();
    await a.join({ teamId: "t", sessionKey: "KA" });
    a.enterStage("R_SP107", 14);
    await settle();
    a.send({ type: "STORY_MOVE", sv: 1, ph: "arrive", mid: "old", name: "Kira" });
    a.send({ type: "STORY_MOVE", sv: 1, ph: "arrive", mid: "new", name: "Kira" });
    a.send({ type: "STORY_MOVE", sv: 1, ph: "depart", mid: "later", name: "Kira" });
    await settle(150);

    const b = mk();
    await b.join({ teamId: "t" });
    b.enterStage("R_SP01", 0);
    b.send({ type: "REQUEST_WORLD_STATE" });
    await b.expectNone((p) => p.type === "STORY_MOVE", "a cached story move without catchUp");
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const cached = await b.waitType("STORY_MOVE");
    assert.equal(cached.mid, "new", "only the latest arrive is cached; a depart is not");
    assert.equal(cached.fromCache, true);
    assert.ok(cached.ageMs >= 100, `ageMs ${cached.ageMs}`);
    await b.expectNone((p) => p.type === "STORY_MOVE", "a second cached story move");

    const other = mk();
    await other.join({ teamId: "u" });
    other.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await other.expectNone((p) => p.type === "STORY_MOVE", "another team's story move");

    await a.close();
    const a2 = mk();
    await a2.join({ teamId: "t", sessionKey: "KA" });
    a2.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await a2.expectNone((p) => p.type === "STORY_MOVE", "our own session's story move");
}));

test("queued team packets are replayed to a teammate who joins later", () => withServer(async (mk) => {
    const a = mk();
    await a.join({ teamId: "t" });
    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true });
    a.send({ type: "SET_EVENT_BIT", no: 0x1234, addToQueue: true });
    a.send({ type: "SET_FLAG", flagNo: 2, addToQueue: false });
    await settle();
    const b = mk();
    await b.join({ teamId: "t" });
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const f1 = await b.waitType("SET_FLAG");
    assert.equal(f1.flagNo, 1);
    assert.equal(f1.fromQueue, true);
    const ev = await b.waitType("SET_EVENT_BIT");
    assert.equal(ev.no, 0x1234);
    await b.expectNone((p) => p.type === "SET_FLAG", "non-queued flag replay");

    const c = mk();
    await c.join({ teamId: "other" });
    c.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await c.expectNone((p) => p.type === "SET_FLAG" || p.type === "SET_EVENT_BIT", "replay to another team");
}));

test("LIGHT_DROP is team-scoped, queued and replayed after the cached world state", () => withServer(async (mk) => {
    const a = mk();
    await a.join({ teamId: "t" });
    const mate = mk();
    await mate.join({ teamId: "t" });
    const other = mk();
    await other.join({ teamId: "u" });
    a.send({ type: "UPDATE_WORLD_STATE", save: "AAAA", saveTblNo: 3 });
    a.send({ type: "LIGHT_DROP", area: 1, tbl: 3, tbox: 7, addToQueue: true });
    const live = await mate.waitType("LIGHT_DROP");
    assert.equal(live.tbox, 7);
    assert.equal(live.teamId, "t");
    assert.ok(Number.isInteger(live.queueSeq), "queued packets are numbered");
    await other.expectNone((p) => p.type === "LIGHT_DROP", "LIGHT_DROP to another team");

    await settle();
    const b = mk();
    await b.join({ teamId: "t" });
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const ws = await b.waitType("UPDATE_WORLD_STATE");
    assert.equal(ws.fromCache, true);
    const drop = await b.waitType("LIGHT_DROP");
    assert.equal(drop.fromQueue, true);
    assert.equal(drop.area, 1);
}));

test("team packets carry the sender's name and colour, also when replayed after it left", () => withServer(async (mk) => {
    // Players without a team show in their own colour.
    const a = mk();
    const b = mk();
    await a.join({ name: "Dad", color: { r: 30, g: 60, b: 200 } });
    await b.join({ name: "Mom" });
    a.send({ type: "UPDATE_WORLD_STATE", save: "S", senderName: "Forged" });
    a.send({ type: "GIVE_ITEM", itemNo: 0x44, addToQueue: true, senderName: "Forged", senderColor: { r: 1, g: 2, b: 3 } });
    const live = await b.waitType("GIVE_ITEM");
    assert.equal(live.senderName, "Dad", "the server's name, not the packet's");
    assert.deepEqual(live.senderColor, { r: 30, g: 60, b: 200 });
    await settle();
    await a.close();

    const c = mk();
    await c.join();
    c.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    // Mom has not caught up (no request of her own), so the cache stands in for the team.
    const ws = await c.waitType("UPDATE_WORLD_STATE");
    assert.equal(ws.fromCache, true);
    assert.equal(ws.senderName, "Dad");
    const replay = await c.waitType("GIVE_ITEM");
    assert.equal(replay.fromQueue, true);
    assert.equal(replay.senderName, "Dad");
    assert.deepEqual(replay.senderColor, { r: 30, g: 60, b: 200 });
}));

test("world state is cached for joiners and supersedes the queue", () => withServer(async (mk) => {
    const a = mk();
    await a.join({ teamId: "t" });
    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true });
    a.send({ type: "UPDATE_WORLD_STATE", save: "AAAA", saveTblNo: 3 });
    a.send({ type: "SET_FLAG", flagNo: 2, addToQueue: true });
    await settle();
    const b = mk();
    await b.join({ teamId: "t" });
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const ws = await b.waitType("UPDATE_WORLD_STATE");
    assert.equal(ws.fromCache, true);
    assert.equal(ws.save, "AAAA");
    assert.equal(ws.clientId, a.id);
    const f = await b.waitType("SET_FLAG");
    assert.equal(f.flagNo, 2, "flag queued before the world state was dropped");
    await b.expectNone((p) => p.type === "SET_FLAG", "stale queued flag");
}));

test("REQUEST_WORLD_STATE is forwarded to caught-up teammates, who answer the requester only", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ teamId: "t" });
    await b.join({ teamId: "t" });
    await c.join({ teamId: "t" });
    a.enterStage("F_SP103");
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    a.send({ type: "UPDATE_WORLD_STATE", save: "S1" });
    await b.waitType("UPDATE_WORLD_STATE");
    await c.waitType("UPDATE_WORLD_STATE");
    b.send({ type: "REQUEST_WORLD_STATE" });
    const fwd = await a.waitType("REQUEST_WORLD_STATE");
    assert.equal(fwd.clientId, b.id);
    await b.expectNone((p) => p.type === "UPDATE_WORLD_STATE", "cached state while a loaded teammate can answer");
    await c.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded to a teammate without a save");

    // A targeted answer goes to the requester only.
    a.send({ type: "UPDATE_WORLD_STATE", save: "S2", targetClientId: b.id });
    const answer = await b.waitType("UPDATE_WORLD_STATE", (p) => p.save === "S2");
    assert.equal(answer.targetClientId, b.id);
    await c.expectNone((p) => p.type === "UPDATE_WORLD_STATE" && p.save === "S2", "targeted state to a bystander");
}));

test("catch-up is sent on a catchUp request, not at handshake", () => withServer(async (mk) => {
    const a = mk();
    await a.join({ teamId: "t" });
    a.send({ type: "UPDATE_WORLD_STATE", save: "S1" });
    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true });
    await settle();
    const b = mk();
    await b.join({ teamId: "t" });
    await b.expectNone((p) => p.type === "UPDATE_WORLD_STATE" || p.type === "SET_FLAG", "catch-up at handshake");

    b.send({ type: "REQUEST_WORLD_STATE" });
    const cached = await b.waitType("UPDATE_WORLD_STATE");
    assert.equal(cached.fromCache, true);
    await b.expectNone((p) => p.type === "SET_FLAG", "queue replay without catchUp");

    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await b.waitType("UPDATE_WORLD_STATE", (p) => p.fromCache);
    const f = await b.waitType("SET_FLAG");
    assert.equal(f.flagNo, 1);
    assert.equal(f.fromQueue, true);
}));

test("the cached state is sent only when no caught-up teammate can answer", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "t" });
    a.enterStage("F_SP103");
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    a.send({ type: "UPDATE_WORLD_STATE", save: "S1" });
    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true });
    await settle();
    await b.join({ teamId: "t" });
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const fwd = await a.waitType("REQUEST_WORLD_STATE");
    assert.equal(fwd.clientId, b.id);
    const f = await b.waitType("SET_FLAG");
    assert.equal(f.fromQueue, true, "the queue is replayed even when a teammate will answer");
    await b.expectNone((p) => p.type === "UPDATE_WORLD_STATE", "cached state while a loaded teammate can answer");

    // Back on the title screen, the teammate can no longer answer.
    a.send({ type: "UPDATE_CLIENT_STATE", isSaveLoaded: false });
    await settle();
    b.send({ type: "REQUEST_WORLD_STATE" });
    const cached = await b.waitType("UPDATE_WORLD_STATE");
    assert.equal(cached.fromCache, true);
    assert.equal(cached.save, "S1");
    await a.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded to a teammate without a save");

    // The save it loads next has not caught up yet either.
    a.enterStage("F_SP103");
    await settle();
    b.send({ type: "REQUEST_WORLD_STATE" });
    await b.waitType("UPDATE_WORLD_STATE", (p) => p.fromCache);
    await a.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded to a freshly loaded save");
}));

test("two teammates loading together do not hold back an absent teammate's cache", () => withServer(async (mk) => {
    const c = mk();
    await c.join({ teamId: "t", sessionKey: "KC" });
    c.enterStage("F_SP103");
    c.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    c.send({ type: "UPDATE_WORLD_STATE", save: "SC" });
    await settle();
    await c.close();

    // Both report a loaded save before either has requested the team state.
    const a = mk();
    const b = mk();
    await a.join({ teamId: "t", sessionKey: "KA" });
    await b.join({ teamId: "t", sessionKey: "KB" });
    a.enterStage("F_SP103");
    b.enterStage("F_SP103");
    await settle();
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const cached = await a.waitType("UPDATE_WORLD_STATE");
    assert.equal(cached.fromCache, true);
    assert.equal(cached.save, "SC");
    await b.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded to a teammate that has not caught up");

    // A merged the cache first, so it answers B in its place.
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const fwd = await a.waitType("REQUEST_WORLD_STATE");
    assert.equal(fwd.clientId, b.id);
    await b.expectNone((p) => p.type === "UPDATE_WORLD_STATE", "cached state while a caught-up teammate can answer");
    a.send({ type: "UPDATE_WORLD_STATE", save: "SA", targetClientId: b.id });
    await b.waitType("UPDATE_WORLD_STATE", (p) => p.save === "SA");

    // Once answered, B is asked by the next requester too.
    const d = mk();
    await d.join({ teamId: "t" });
    d.enterStage("F_SP103");
    d.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await a.waitType("REQUEST_WORLD_STATE", (p) => p.clientId === d.id);
    await b.waitType("REQUEST_WORLD_STATE", (p) => p.clientId === d.id);
    await d.expectNone((p) => p.type === "UPDATE_WORLD_STATE", "cached state while caught-up teammates can answer");
}));

test("targeted world states reach only the target and are not cached", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ teamId: "t" });
    await b.join({ teamId: "t" });
    await c.join({ teamId: "u" });
    a.send({ type: "UPDATE_WORLD_STATE", save: "S1" });
    await b.waitType("UPDATE_WORLD_STATE");
    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true });
    await b.waitType("SET_FLAG");

    a.send({ type: "UPDATE_WORLD_STATE", save: "S2", targetClientId: b.id });
    const answer = await b.waitType("UPDATE_WORLD_STATE", (p) => p.save === "S2");
    assert.equal(answer.targetClientId, b.id);
    a.send({ type: "UPDATE_WORLD_STATE", save: "S3", targetClientId: c.id });
    await c.expectNone((p) => p.type === "UPDATE_WORLD_STATE", "targeted state to another team");

    const d = mk();
    await d.join({ teamId: "t" });
    d.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const cached = await d.waitType("UPDATE_WORLD_STATE");
    assert.equal(cached.save, "S1", "a targeted answer does not replace the cache");
    const f = await d.waitType("SET_FLAG");
    assert.equal(f.flagNo, 1, "a targeted answer does not clear the queue");
}));

test("a reconnecting client is not sent its own session's packets", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ teamId: "t", sessionKey: "KA" });
    await b.join({ teamId: "t", sessionKey: "KB" });
    await c.join({ teamId: "t" });
    a.send({ type: "UPDATE_WORLD_STATE", save: "SA" });
    a.send({ type: "GIVE_ITEM", itemNo: 0x21, addToQueue: true, senderSessionKey: "KB" });
    const live = await b.waitType("GIVE_ITEM");
    assert.equal(live.senderSessionKey, "KA", "the server stamps the sender's real session key");
    b.send({ type: "SET_FLAG", flagNo: 2, addToQueue: true });
    c.send({ type: "SET_FLAG", flagNo: 3, addToQueue: true });
    await settle();
    await a.close();

    const a2 = mk();
    await a2.join({ teamId: "t", sessionKey: "KA" });
    assert.notEqual(a2.id, a.id);
    a2.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await a2.waitType("SET_FLAG", (p) => p.flagNo === 2);
    const f3 = await a2.waitType("SET_FLAG", (p) => p.flagNo === 3);
    assert.equal(f3.senderSessionKey, "");
    await a2.expectNone((p) => p.type === "GIVE_ITEM" || p.type === "UPDATE_WORLD_STATE",
        "packets from the previous connection of the same session");

    // Without a session key, only the requester's own clientId is skipped.
    const d = mk();
    await d.join({ teamId: "t" });
    d.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await d.waitType("UPDATE_WORLD_STATE", (p) => p.save === "SA");
    await d.waitType("GIVE_ITEM");
    await d.waitType("SET_FLAG", (p) => p.flagNo === 2);
    await d.waitType("SET_FLAG", (p) => p.flagNo === 3);
}));

test("the requester's own stale connection is not asked for world state", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await b.join({ teamId: "t" });
    b.send({ type: "UPDATE_WORLD_STATE", save: "SB" });
    await a.join({ teamId: "t", sessionKey: "K" });
    a.enterStage("F_SP103");
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await a.waitType("UPDATE_WORLD_STATE");
    // The same game process reconnects before the server noticed its old connection drop.
    const a2 = mk();
    await a2.join({ teamId: "t", sessionKey: "K" });
    a2.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const cached = await a2.waitType("UPDATE_WORLD_STATE");
    assert.equal(cached.save, "SB");
    await a.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded to the requester's old connection");
}));

test("a teammate on another protocol version is not asked for world state", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "t", protocolVersion: 6 });
    a.enterStage("F_SP103");
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    a.send({ type: "UPDATE_WORLD_STATE", save: "S1" });
    await settle();
    await b.join({ teamId: "t" });
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const cached = await b.waitType("UPDATE_WORLD_STATE");
    assert.equal(cached.fromCache, true, "the cache stands in for a teammate that would drop the request");
    await a.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded across protocol versions");
}));

test("world packets, the world cache and the queue are kept per save layout", () => withServer(async (mk) => {
    const L1 = "1111111111111111";
    const L2 = "2222222222222222";
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ teamId: "t", layout: L1 });
    await b.join({ teamId: "t", layout: L2 });
    await c.join({ teamId: "t", layout: L1 });
    a.enterStage("F_SP103");
    b.enterStage("F_SP103");
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await settle();

    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true });
    a.send({ type: "UPDATE_WORLD_STATE", save: "SA" });
    a.send({ type: "GIVE_ITEM", itemNo: 0x40, addToQueue: true });
    const give = await c.waitType("GIVE_ITEM");
    await c.waitType("UPDATE_WORLD_STATE", (p) => p.save === "SA");
    await b.expectNone((p) => ["SET_FLAG", "GIVE_ITEM", "UPDATE_WORLD_STATE"].includes(p.type),
        "world packet across save layouts");

    // B's state is cached for its own layout and does not replace A's.
    b.send({ type: "UPDATE_WORLD_STATE", save: "SB" });
    b.send({ type: "GIVE_ITEM", itemNo: 0x41, addToQueue: true });
    await a.expectNone((p) => p.type === "UPDATE_WORLD_STATE" || p.type === "GIVE_ITEM", "state from another layout");
    await settle();

    const d = mk();
    await d.join({ teamId: "t", layout: L1 });
    d.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await a.waitType("REQUEST_WORLD_STATE", (p) => p.clientId === d.id);
    await b.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded across save layouts");
    const replayed = await d.waitType("GIVE_ITEM", (p) => p.fromQueue === true);
    assert.equal(replayed.itemNo, 0x40);
    assert.equal(replayed.queueEpoch, give.queueEpoch);
    await d.expectNone((p) => p.type === "GIVE_ITEM" && p.itemNo === 0x41, "queued packet from another layout");

    // A joiner on B's layout with nobody to answer gets B's cache, never A's.
    await b.close();
    const e = mk();
    await e.join({ teamId: "t", layout: L2 });
    e.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const cached = await e.waitType("UPDATE_WORLD_STATE");
    assert.equal(cached.save, "SB");
    assert.equal(cached.fromCache, true);
    const own = await e.waitType("GIVE_ITEM", (p) => p.fromQueue === true);
    assert.equal(own.itemNo, 0x41);
    assert.notEqual(own.queueEpoch, give.queueEpoch);
    await e.expectNone((p) => p.type === "UPDATE_WORLD_STATE" && p.save === "SA", "cache of another layout");

    // A targeted answer never crosses layouts either.
    a.send({ type: "UPDATE_WORLD_STATE", save: "ST", targetClientId: e.id });
    await e.expectNone((p) => p.type === "UPDATE_WORLD_STATE" && p.save === "ST", "targeted state across layouts");
}));

test("UNSET_EVENT_BIT is team-scoped and queued", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ teamId: "red" });
    await b.join({ teamId: "red" });
    await c.join({ teamId: "blue" });
    a.send({ type: "UNSET_EVENT_BIT", no: 0x0a40, teamId: "blue", addToQueue: true });
    const got = await b.waitType("UNSET_EVENT_BIT");
    assert.equal(got.no, 0x0a40);
    assert.equal(got.clientId, a.id);
    assert.equal(got.teamId, "red");
    await c.expectNone((p) => p.type === "UNSET_EVENT_BIT", "UNSET_EVENT_BIT to another team");
    await waitForLog(server, /UNSET_EVENT_BIT no=0x0a40/);

    const d = mk();
    await d.join({ teamId: "red" });
    d.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    const replay = await d.waitType("UNSET_EVENT_BIT");
    assert.equal(replay.no, 0x0a40);
    assert.equal(replay.fromQueue, true);
}));

test("only the owner may change room state", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join();
    await b.join();
    b.send({ type: "UPDATE_ROOM_STATE", state: { pvpMode: true } });
    await a.expectNone((p) => p.type === "UPDATE_ROOM_STATE", "room state from a non-owner");

    a.send({ type: "UPDATE_ROOM_STATE", state: { pvpMode: true, enemyHealthMultiplier: 250, bogus: 1 } });
    const got = await b.waitType("UPDATE_ROOM_STATE");
    assert.equal(got.state.pvpMode, true);
    assert.equal(got.state.enemyHealthMultiplier, 250);
    assert.equal(got.state.ownerClientId, a.id);
    assert.equal("bogus" in got.state, false);
}));

test("room state values are type-checked", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join();
    await b.join();
    a.send({ type: "UPDATE_ROOM_STATE", state: { pvpMode: "yes", syncWorldState: null, enemyHealthMultiplier: 1e9,
        syncEnemyDamage: 0 } });
    const got = await b.waitType("UPDATE_ROOM_STATE");
    assert.equal(typeof got.state.pvpMode, "boolean");
    assert.equal(got.state.syncEnemyDamage, true, "on by default, and a number is no boolean");
    assert.equal(got.state.syncWorldState, true, "invalid value keeps the previous setting");
    assert.equal(got.state.enemyHealthMultiplier, 500, "multiplier is clamped to the UI range");
}));

test("room state carries the enemy count multiplier, clamped to 100-300", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join();
    const state = await b.join();
    assert.equal(state.roomState.enemyCountMultiplier, 100);
    b.send({ type: "UPDATE_ROOM_STATE", state: { enemyCountMultiplier: 300 } });
    await a.expectNone((p) => p.type === "UPDATE_ROOM_STATE", "room state from a non-owner");
    for (const [sent, want] of [[250, 250], [1e9, 300], [50, 100], ["9", 100]]) {
        a.send({ type: "UPDATE_ROOM_STATE", state: { enemyCountMultiplier: sent } });
        const got = await b.waitType("UPDATE_ROOM_STATE");
        assert.equal(got.state.enemyCountMultiplier, want, `enemyCountMultiplier ${sent}`);
    }
}));

test("ownership passes to the next client when the owner leaves", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join();
    await b.join();
    await a.close();
    const left = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.online === false);
    assert.ok(left);
    const rs = await b.waitType("UPDATE_ROOM_STATE");
    assert.equal(rs.state.ownerClientId, b.id);
}));

test("unknown packet types are relayed to the rest of the room", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A", roomId: "r" });
    await b.join({ roomId: "r" });
    a.send({ type: "AUTOTEST_SIGNAL", instance: "a1", name: "ready" });
    const got = await b.waitType("AUTOTEST_SIGNAL");
    assert.equal(got.name, "ready");
    assert.equal(got.instance, "a1");
    assert.equal(got.clientId, a.id);
    await a.expectNone((p) => p.type === "AUTOTEST_SIGNAL", "echo to sender");
    await waitForLog(server, /\[A\] AUTOTEST_SIGNAL instance=a1 name=ready/);
}));

test("server survives malformed input", () => withServer(async (mk) => {
    const a = mk();
    await a.open();
    a.send("not json");
    a.send("null");
    a.send("[1,2,3]");
    a.send({ type: 5 });
    a.send({ type: "SET_FLAG" }); // before handshake: dropped
    a.send({ type: "HANDSHAKE", app: "twili-together", protocolVersion: 5, name: 42, teamId: { x: 1 }, roomId: 7,
             color: "red", modVersion: "v".repeat(40), layout: 123 });
    const all = await a.waitType("ALL_CLIENT_STATE");
    a.id = all.clients.find((c) => c.self).clientId;
    assert.equal(typeof all.clients[0].name, "string");
    assert.equal(typeof all.clients[0].teamId, "string");
    assert.equal(typeof all.clients[0].color.r, "number");
    assert.equal(all.clients[0].modVersion, "v".repeat(32));
    assert.equal(all.clients[0].layout, "");
    a.send({ type: "HANDSHAKE", name: "again" }); // second handshake ignored
    a.send({ type: "UPDATE_CLIENT_STATE", stageName: { evil: true }, layerNo: "x", isSaveLoaded: "yes" });
    a.send({ type: "UPDATE_ROOM_STATE", state: "garbage" });
    await settle();

    const b = mk();
    const all2 = await b.join();
    assert.equal(all2.clients.length, 2, "server still accepting clients");
    const aState = all2.clients.find((c) => c.clientId === a.id);
    assert.equal(typeof aState.stageName, "string");
    assert.equal(typeof aState.layerNo, "number");
    assert.equal(typeof aState.isSaveLoaded, "boolean");

    // Client-state changes are relayed in canonical form, never as the raw packet.
    a.send({ type: "UPDATE_CLIENT_STATE", stageName: ["x"], roomNo: 1e12, color: { r: "a" } });
    const relayed = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id);
    assert.equal(typeof relayed.stageName, "string");
    assert.ok(relayed.roomNo >= -128 && relayed.roomNo <= 127);
    assert.equal(typeof relayed.color.r, "number");
}));

test("client state updates are relayed to the room", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A" });
    await b.join({ name: "B" });
    a.enterStage("D_MN05", 2, { roomNo: 4, saveTblNo: 16 });
    const got = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.stageName === "D_MN05");
    assert.equal(got.layerNo, 2);
    assert.equal(got.roomNo, 4);
    assert.equal(got.saveTblNo, 16);
    assert.equal(got.isSaveLoaded, true);
    assert.equal(got.name, "A");
    assert.equal(got.self, false);
}));

test("player colour is live for the room and for late joiners", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A", color: { r: 255, g: 255, b: 255 } });
    await b.join({ name: "B" });
    a.enterStage("R_SP01", 0);
    await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.stageName === "R_SP01");

    // A colour-only update (what the picker sends) keeps the scene, so peers keep their dummy.
    a.send({ type: "UPDATE_CLIENT_STATE", color: { r: 10, g: 20, b: 30 } });
    const u = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.color.r === 10);
    assert.deepEqual(u.color, { r: 10, g: 20, b: 30 });
    assert.equal(u.stageName, "R_SP01");
    assert.equal(u.isSaveLoaded, true);
    await a.expectNone((p) => p.type === "UPDATE_CLIENT_STATE" && p.clientId === a.id, "echo to the sender");
    await waitForLog(server, /\[A\] UPDATE_CLIENT_STATE color=10,20,30/);

    const c = mk();
    const all = await c.join({ name: "C" });
    assert.deepEqual(all.clients.find((x) => x.clientId === a.id).color, { r: 10, g: 20, b: 30 });

    // Out-of-range or non-integer channels keep their previous value, one by one.
    a.send({ type: "UPDATE_CLIENT_STATE", color: { r: 300, g: -1, b: 40 } });
    const v = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.color.b === 40);
    assert.deepEqual(v.color, { r: 10, g: 20, b: 40 });
    a.send({ type: "UPDATE_CLIENT_STATE", color: { r: 1.5, g: "7", b: 50 } });
    const w = await c.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.color.b === 50);
    assert.deepEqual(w.color, { r: 10, g: 20, b: 50 });
    a.send({ type: "UPDATE_CLIENT_STATE", color: "red" });
    const x = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id);
    assert.deepEqual(x.color, { r: 10, g: 20, b: 50 });
}));

test("horse name and parking spot are sanitized, relayed and cached for late joiners", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A" });
    await b.join({ name: "B" });
    a.enterStage("F_SP108", 0);
    await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.stageName === "F_SP108");

    const place = { stage: "F_SP121", room: 6, x: -43741.5, y: -7425, z: 106889, angleY: -23665 };
    a.send({ type: "UPDATE_CLIENT_STATE", horseName: "Lilly", horsePlace: place });
    const u = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.horseName === "Lilly");
    assert.deepEqual(u.horsePlace, place);
    assert.equal(u.stageName, "F_SP108");  // a horse update keeps the scene
    await a.expectNone((p) => p.type === "UPDATE_CLIENT_STATE" && p.clientId === a.id, "echo to the sender");
    await waitForLog(server, /\[A\] UPDATE_CLIENT_STATE horse=Lilly parked=F_SP121/);

    const c = mk();
    const all = await c.join({ name: "C" });
    const row = all.clients.find((x) => x.clientId === a.id);
    assert.equal(row.horseName, "Lilly");
    assert.deepEqual(row.horsePlace, place);

    // A non-string name keeps the old one; 40 code points become 32; a malformed place is null.
    a.send({ type: "UPDATE_CLIENT_STATE", horseName: 7, horsePlace: { stage: "", x: 1, y: 2, z: 3 } });
    const v = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.horsePlace === null);
    assert.equal(v.horseName, "Lilly");
    a.send({ type: "UPDATE_CLIENT_STATE", horseName: "x".repeat(40), horsePlace: { stage: "F_SP121", x: Infinity, y: 0, z: 0 } });
    const w = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.horseName.startsWith("xx"));
    assert.equal(w.horseName.length, 32);
    assert.equal(w.horsePlace, null);
    a.send({ type: "UPDATE_CLIENT_STATE", horsePlace: { stage: "F_SP121_TOOLONG", room: 999, x: 1, y: 2, z: 3, angleY: 1.5 } });
    const x = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.horsePlace !== null);
    assert.deepEqual(x.horsePlace, { stage: "F_SP121", room: -1, x: 1, y: 2, z: 3, angleY: 0 });
}));

test("names are cut by code points and never keep a lone surrogate or a control character", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A" });
    await b.join({ name: "B" });
    // 31 letters and an emoji (two UTF-16 units): cut at 32 code points it survives whole.
    const horse = "h".repeat(31) + "\u{1F434}" + "tail";
    a.send({ type: "UPDATE_CLIENT_STATE", horseName: horse });
    const u = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.horseName.startsWith("hh"));
    assert.equal(u.horseName, "h".repeat(31) + "\u{1F434}");
    // A lone high surrogate and control characters are dropped: the game's parser rejects them.
    a.send(`{"type":"UPDATE_CLIENT_STATE","name":"Da\\ud83dd\\u0007!","horseName":"Li\\nlly"}`);
    const v = await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.horseName === "Lilly");
    assert.equal(v.name, "Dad!");
}));

test("hostile field types and unserializable packets do not kill the server", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "t" });
    await b.join({ teamId: "t" });
    // Interpolating an object whose toString is not a function throws.
    const evil = { toString: 1 };
    for (const type of ["UPDATE_CLIENT_STATE", "SET_FLAG", "UNSET_FLAG", "SET_EVENT_BIT", "UNSET_EVENT_BIT", "GIVE_ITEM",
        "UPDATE_DUNGEON_ITEMS", "UPDATE_WORLD_STATE", "REQUEST_WORLD_STATE", "UPDATE_ROOM_STATE", "PLAYER_SFX",
        "ENEMY_DEFEATED", "ENEMY_DAMAGE", "STORY_EVENT", "STORY_MOVE"]) {
        a.send({
            type, no: evil, itemNo: evil, flagNo: evil, roomNo: evil, stageName: evil, saveTblNo: evil, layerNo: evil,
            isSaveLoaded: evil, category: evil, keyDelta: evil, dungeonItemBits: evil, targetClientId: evil,
            soundId: evil, kind: evil, kills: evil, hits: evil, state: { pvpMode: evil, nested: [evil] },
            ph: evil, id: evil, stage: evil, room: evil, layer: evil, m: evil, req: evil, from: evil, to: evil,
            curated: evil, qual: evil,
        });
    }
    a.send({ type: "AUTOTEST_SIGNAL", instance: evil, name: evil });
    for (const type of ["DAMAGE_PLAYER", "DAMAGE_RESULT", "PVP_KNOCKOUT"]) {
        a.send({ type, targetClientId: evil, hitId: evil, kind: evil, damage: evil, spl: evil, dirY: evil,
                 blocked: evil, viewSeq: evil, result: evil, reason: evil, knockout: evil });
    }
    // Parses, but is nested too deeply for JSON.stringify: relaying it throws.
    const deep = "[".repeat(100000) + "]".repeat(100000);
    a.send(`{"type":"GIVE_ITEM","itemNo":1,"addToQueue":true,"junk":${deep}}`);
    a.send(`{"type":"UPDATE_WORLD_STATE","save":"X","junk":${deep}}`);
    a.send({ type: "SET_FLAG", flagNo: 7, addToQueue: true });
    await b.waitType("SET_FLAG", (p) => p.flagNo === 7);

    const log = await waitForLog(server, /SET_FLAG \[\?\] no=7/);
    const failures = log.split("\n").filter((l) => l.includes(" failed: ")).map((l) => l.match(/\] (\w+) failed: (\w+)/).slice(1));
    assert.deepEqual(failures, [["GIVE_ITEM", "RangeError"], ["UPDATE_WORLD_STATE", "RangeError"]],
        "only the unserializable packets fail; log formatting never throws");

    // Neither unserializable packet was stored, so catch-up still works.
    const c = mk();
    await c.join({ teamId: "t" });
    c.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await c.waitType("SET_FLAG", (p) => p.flagNo === 7);
    await c.expectNone((p) => p.type === "GIVE_ITEM" || p.type === "UPDATE_WORLD_STATE", "stored unserializable packet");
}));

test("clients cannot relay server-only packets", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    await a.join({ roomId: "r" });
    await b.join({ roomId: "r" });
    for (const type of ["ALL_CLIENT_STATE", "SERVER_MESSAGE", "DISABLE_CLIENT"]) {
        a.send({ type, clients: [], message: "bye" });
    }
    a.send({ type: "HANDSHAKE", name: "again", roomId: "r" });
    a.send({ type: "AUTOTEST_SIGNAL", name: "after" });
    await b.waitType("AUTOTEST_SIGNAL");
    await b.expectNone((p) => ["ALL_CLIENT_STATE", "SERVER_MESSAGE", "DISABLE_CLIENT", "HANDSHAKE"].includes(p.type) ||
        (p.type === "UPDATE_CLIENT_STATE" && p.name === "again"), "client-originated server packet");
    const log = await waitForLog(server, /rejected HANDSHAKE/);
    for (const type of ["ALL_CLIENT_STATE", "SERVER_MESSAGE", "DISABLE_CLIENT"]) {
        assert.match(log, new RegExp(`rejected ${type}`));
    }
}));

test("the heartbeat drops a connection that stops answering pings", () => withServer(async (mk) => {
    const a = mk({ autoPong: false });
    const b = mk();
    await a.join({ roomId: "hb" });
    const closed = new Promise((resolve) => a.ws.once("close", resolve));
    const all = await b.join({ roomId: "hb" });
    assert.equal(all.clients.length, 2);
    await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.online === false, 3000);
    const rs = await b.waitType("UPDATE_ROOM_STATE", undefined, 3000);
    assert.equal(rs.state.ownerClientId, b.id, "a dead connection does not stay room owner");
    await closed;
    await settle(800);
    assert.equal(b.ws.readyState, WebSocket.OPEN, "a client that answers pings stays connected");
}, { TT_PING_MS: "200" }));

test("a heartbeat interval too long for a Node timer is capped, not run every millisecond", () => withServer(async (mk) => {
    const a = mk({ autoPong: false });
    await a.join();
    let pings = 0;
    a.ws.on("ping", () => pings++);
    await settle(300);
    assert.equal(pings, 0);
    assert.equal(a.ws.readyState, WebSocket.OPEN);
}, { TT_PING_MS: String(2 ** 32) }));

test("an empty room is deleted after its TTL", () => withServer(async (mk, server) => {
    const a = mk();
    await a.join({ roomId: "gone", teamId: "t" });
    a.send({ type: "UPDATE_ROOM_STATE", state: { pvpMode: true } });
    await a.waitType("UPDATE_ROOM_STATE");
    a.send({ type: "UPDATE_WORLD_STATE", save: "S1" });
    await settle();
    await a.close();
    await waitForLog(server, /\[room gone\] deleted after 0\.3s empty/);

    const b = mk();
    const all = await b.join({ roomId: "gone", teamId: "t" });
    assert.equal(all.roomState.pvpMode, false, "the room starts over with default settings");
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await b.expectNone((p) => p.type === "UPDATE_WORLD_STATE", "cached state of a deleted room");
}, { TT_ROOM_TTL_SEC: "0.3" }));

test("joining an empty room cancels its expiry", () => withServer(async (mk, server) => {
    const a = mk();
    await a.join({ roomId: "kept" });
    a.send({ type: "UPDATE_ROOM_STATE", state: { pvpMode: true } });
    await a.waitType("UPDATE_ROOM_STATE");
    await a.close();
    await waitForLog(server, /left room kept\. Room size: 0/);

    const b = mk();
    const all = await b.join({ roomId: "kept" });
    assert.equal(all.roomState.pvpMode, true);
    await settle(800);
    const c = mk();
    const all2 = await c.join({ roomId: "kept" });
    assert.equal(all2.roomState.pvpMode, true, "the room outlived its TTL because someone joined");
    assert.equal(all2.clients.length, 2);
    assert.doesNotMatch(fs.readFileSync(server.logPath, "utf8"), /\[room kept\] deleted/);
}, { TT_ROOM_TTL_SEC: "0.3" }));

// Teleport: every client joins the public room in F_SP108; the first joiner owns the room.
async function teleportRoom(mk, n = 2, enable = true) {
    const cs = [];
    for (let i = 0; i < n; i++) {
        const c = mk();
        await c.join({ name: `P${i}` });
        c.enterStage("F_SP108");
        cs.push(c);
    }
    await settle();
    if (enable) {
        cs[0].send({ type: "UPDATE_ROOM_STATE", state: { teleportMode: true } });
        await cs[0].waitType("UPDATE_ROOM_STATE", (p) => p.state.teleportMode === true);
    }
    return cs;
}

test("REQUEST_TELEPORT is refused by the server while teleporting is off", () => withServer(async (mk) => {
    const [a, b] = await teleportRoom(mk, 2, false);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 7 });
    const r = await a.waitType("TELEPORT_TO");
    assert.deepEqual([r.ok, r.reason, r.requestId, r.clientId, r.targetClientId, r.fromServer],
        [false, "disabled", 7, b.id, a.id, true]);
    await b.expectNone((p) => p.type === "REQUEST_TELEPORT", "forwarded request");
}));

test("teleport request and answer reach only the two clients involved, sanitized", () => withServer(async (mk) => {
    const [a, b, c] = await teleportRoom(mk, 3);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 1, junk: { x: 1 } });
    const req = await b.waitType("REQUEST_TELEPORT");
    assert.deepEqual(req, { type: "REQUEST_TELEPORT", clientId: a.id, targetClientId: b.id, requestId: 1 });
    b.send({ type: "TELEPORT_TO", targetClientId: a.id, requestId: 1, ok: true, stageName: "F_SP108", roomNo: 0,
             layerNo: 0, pos: { x: 1.5, y: 2, z: -3 }, angleY: 16384, ageMs: 40, clientId: 999, extra: true });
    const ans = await a.waitType("TELEPORT_TO");
    assert.deepEqual(ans, { type: "TELEPORT_TO", clientId: b.id, targetClientId: a.id, requestId: 1, ok: true,
                            stageName: "F_SP108", roomNo: 0, layerNo: 0, pos: { x: 1.5, y: 2, z: -3 },
                            angleY: 16384, ageMs: 40 });
    b.send({ type: "TELEPORT_TO", targetClientId: a.id, requestId: 2, ok: false, reason: { toString: 1 } });
    const refusal = await a.waitType("TELEPORT_TO");
    assert.deepEqual([refusal.ok, refusal.reason, refusal.fromServer], [false, "refused", undefined]);
    await c.expectNone((p) => p.type === "REQUEST_TELEPORT" || p.type === "TELEPORT_TO", "teleport packet for a bystander");
}));

test("TELEPORT_TO with an unusable destination becomes a refusal", () => withServer(async (mk) => {
    const [a, b] = await teleportRoom(mk);
    b.send({ type: "TELEPORT_TO", targetClientId: a.id, requestId: 3, ok: true, stageName: "F_SP108", roomNo: 0,
             layerNo: 0, pos: { x: "1", y: 2, z: 1e9 } });
    const ans = await a.waitType("TELEPORT_TO");
    assert.deepEqual([ans.ok, ans.reason, ans.pos, ans.stageName], [false, "bad-destination", undefined, undefined]);
    // Cutting an 8-character name to 7 could name another stage.
    b.send({ type: "TELEPORT_TO", targetClientId: a.id, requestId: 4, ok: true, stageName: "F_SP1080", roomNo: 0,
             layerNo: 0, pos: { x: 1, y: 2, z: 3 } });
    assert.equal((await a.waitType("TELEPORT_TO", (p) => p.requestId === 4)).reason, "bad-destination");
}));

test("REQUEST_TELEPORT to someone gone or in another room is refused", () => withServer(async (mk) => {
    const [a] = await teleportRoom(mk);
    const other = mk();
    await other.join({ roomId: "elsewhere" });
    other.enterStage("F_SP108");
    a.send({ type: "REQUEST_TELEPORT", targetClientId: other.id, requestId: 4 });
    assert.equal((await a.waitType("TELEPORT_TO")).reason, "offline");
    await other.expectNone((p) => p.type === "REQUEST_TELEPORT", "cross-room request");
}));

test("REQUEST_TELEPORT needs a save loaded on both ends", () => withServer(async (mk) => {
    const [a, b] = await teleportRoom(mk);
    b.send({ type: "UPDATE_CLIENT_STATE", isSaveLoaded: false });
    await a.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === b.id && p.stageName === "F_SP108" && p.isSaveLoaded === false);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 8 });
    assert.equal((await a.waitType("TELEPORT_TO")).reason, "not-in-game");
    await b.expectNone((p) => p.type === "REQUEST_TELEPORT", "request to a client without a save");
}));

test("REQUEST_TELEPORT is rate limited per requester", () => withServer(async (mk) => {
    const [a, b] = await teleportRoom(mk);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 5 });
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 6 });
    await b.waitType("REQUEST_TELEPORT", (p) => p.requestId === 5);
    assert.equal((await a.waitType("TELEPORT_TO", (p) => p.requestId === 6)).reason, "rate-limited");
    await b.expectNone((p) => p.type === "REQUEST_TELEPORT" && p.requestId === 6, "rate-limited request");
}));

test("presence packets only reach clients on the same protocol version", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ protocolVersion: 6 });
    await b.join({ protocolVersion: 6 });
    await c.join({ protocolVersion: 5 });
    for (const x of [a, b, c]) x.enterStage("F_SP103", 0);
    await settle();
    a.send({ type: "PLAYER_UPDATE", quiet: true, seq: 7, k: 1, p: [1, 2, 3] });
    const got = await b.waitType("PLAYER_UPDATE");
    assert.deepEqual({ ...got, clientId: undefined }, { type: "PLAYER_UPDATE", quiet: true, seq: 7, k: 1, p: [1, 2, 3], clientId: undefined },
        "compact packets are relayed unchanged apart from clientId");
    assert.equal(got.clientId, a.id);
    await c.expectNone((p) => p.type === "PLAYER_UPDATE", "PLAYER_UPDATE across protocol versions");
}));

test("test jitter delays packets without reordering them", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join();
    await b.join();
    a.enterStage("F_SP103", 0);
    b.enterStage("F_SP103", 0);
    await settle(200);
    for (let seq = 1; seq <= 40; seq++) {
        a.send({ type: "PLAYER_UPDATE", quiet: true, seq });
    }
    a.send({ type: "AUTOTEST_SIGNAL", name: "after" });
    await b.waitType("AUTOTEST_SIGNAL", () => true, 5000);
    const seqs = b.inbox.filter((p) => p.type === "PLAYER_UPDATE").map((p) => p.seq);
    assert.deepEqual(seqs, Array.from({ length: 40 }, (_, i) => i + 1), "all updates arrive, in order, before the signal");
}, { TT_TEST_JITTER_MS: "50" }));

// PvP: every client joins the public room in F_SP108; the first joiner turns PvP on.
async function pvpRoom(mk, teams = ["", ""], extra = {}, enable = true) {
    const cs = [];
    for (let i = 0; i < teams.length; i++) {
        const c = mk();
        await c.join({ name: `P${i}`, teamId: teams[i] });
        c.enterStage("F_SP108");
        cs.push(c);
    }
    await settle();
    if (enable) {
        cs[0].send({ type: "UPDATE_ROOM_STATE", state: { pvpMode: true, ...extra } });
        await cs[0].waitType("UPDATE_ROOM_STATE", (p) => p.state.pvpMode === true);
    }
    return cs;
}

const hit = (target, hitId, extra = {}) => ({ type: "DAMAGE_PLAYER", targetClientId: target.id, hitId, kind: 0,
                                             damage: 2, spl: 0, dirY: 100, blocked: false, viewSeq: 50, ...extra });

test("DAMAGE_PLAYER is refused by the server while PvP is off", () => withServer(async (mk) => {
    const [a, b] = await pvpRoom(mk, ["", ""], {}, false);
    a.send(hit(b, 1));
    const r = await a.waitType("DAMAGE_RESULT");
    assert.deepEqual([r.result, r.reason, r.hitId, r.clientId, r.targetClientId, r.fromServer, r.damage],
        ["refused", "disabled", 1, b.id, a.id, true, 0]);
    await b.expectNone((p) => p.type === "DAMAGE_PLAYER", "forwarded hit");
}));

test("DAMAGE_PLAYER reaches only its target, rebuilt, clamped and stamped with the attacker's stage", () => withServer(async (mk) => {
    const [a, b, c] = await pvpRoom(mk, ["", "", ""]);
    a.send(hit(b, 7, { kind: 99, damage: 50, spl: 2, dirY: 99999, blocked: "yes", viewSeq: 12.5,
                       stageName: "X", layerNo: 9, clientId: 999, junk: { x: 1 } }));
    const got = await b.waitType("DAMAGE_PLAYER");
    assert.deepEqual(got, { type: "DAMAGE_PLAYER", clientId: a.id, targetClientId: b.id, hitId: 7, kind: 11,
                            damage: 4, spl: 0, dirY: 32767, blocked: false, viewSeq: 0, stageName: "F_SP108",
                            layerNo: 0 });
    await c.expectNone((p) => p.type === "DAMAGE_PLAYER" || p.type === "DAMAGE_RESULT", "PvP packet for a bystander");
    await a.expectNone((p) => p.type === "DAMAGE_RESULT", "refusal of an allowed hit");
    await settle(250);
    a.send(hit(b, 8, { spl: 1, kind: 10 }));
    const spinner = await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 8);
    assert.deepEqual([spinner.spl, spinner.kind], [1, 10], "knockdown and the spinner kind pass");
}));

test("DAMAGE_PLAYER is refused across stage, layer and protocol", () => withServer(async (mk) => {
    const [a, b] = await pvpRoom(mk);
    b.enterStage("F_SP108", 1);
    await a.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === b.id && p.layerNo === 1);
    a.send(hit(b, 1));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 1)).reason, "not-same-stage");
    b.enterStage("F_SP121", 0);
    await a.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === b.id && p.stageName === "F_SP121");
    a.send(hit(b, 2));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 2)).reason, "not-same-stage");

    const newer = mk();
    await newer.join({ name: "newer", protocolVersion: 6 });
    newer.enterStage("F_SP108");
    await a.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === newer.id && p.stageName === "F_SP108");
    a.send(hit(newer, 3));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 3)).reason, "protocol");
    await b.expectNone((p) => p.type === "DAMAGE_PLAYER", "hit across stages");
    await newer.expectNone((p) => p.type === "DAMAGE_PLAYER", "hit across protocols");
}));

test("DAMAGE_PLAYER needs a save on both ends and a target in the room", () => withServer(async (mk) => {
    const [a, b] = await pvpRoom(mk);
    b.send({ type: "UPDATE_CLIENT_STATE", isSaveLoaded: false });
    await a.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === b.id && p.stageName === "F_SP108" && p.isSaveLoaded === false);
    a.send(hit(b, 1));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 1)).reason, "not-in-game");
    b.enterStage("F_SP108");
    await a.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === b.id && p.isSaveLoaded === true);
    a.send({ type: "UPDATE_CLIENT_STATE", isSaveLoaded: false });
    await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.isSaveLoaded === false);
    a.send(hit(b, 2));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 2)).reason, "not-in-game");
    await b.expectNone((p) => p.type === "DAMAGE_PLAYER", "hit without a save");

    const other = mk();
    await other.join({ roomId: "elsewhere" });
    other.enterStage("F_SP108");
    a.enterStage("F_SP108");
    await b.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === a.id && p.isSaveLoaded === true);
    a.send(hit(other, 3));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 3)).reason, "offline");
    a.send(hit(a, 4));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 4)).reason, "offline", "hitting yourself");
    await other.expectNone((p) => p.type === "DAMAGE_PLAYER", "cross-room hit");
}));

test("same named team needs pvpFriendlyFire; players without a team always fight", () => withServer(async (mk) => {
    const [a, b, c, d] = await pvpRoom(mk, ["red", "red", "blue", ""]);
    a.send(hit(b, 1));
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 1)).reason, "team");
    await b.expectNone((p) => p.type === "DAMAGE_PLAYER", "friendly fire while off");
    a.send(hit(c, 2));
    await c.waitType("DAMAGE_PLAYER", (p) => p.hitId === 2);
    d.send(hit(a, 3));
    await a.waitType("DAMAGE_PLAYER", (p) => p.hitId === 3);
    a.send(hit(d, 4));
    await d.waitType("DAMAGE_PLAYER", (p) => p.hitId === 4);

    a.send({ type: "UPDATE_ROOM_STATE", state: { pvpFriendlyFire: true } });
    await b.waitType("UPDATE_ROOM_STATE", (p) => p.state.pvpFriendlyFire === true);
    await settle(250);  // clear of the rate limit for a -> b
    a.send(hit(b, 5));
    await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 5);
}));

test("two players without a team are not teammates for PvP", () => withServer(async (mk) => {
    const [a, b] = await pvpRoom(mk, ["", ""]);
    a.send(hit(b, 1));
    await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 1);
}));

test("DAMAGE_PLAYER is rate limited per attacker and target", () => withServer(async (mk) => {
    const [a, b, c] = await pvpRoom(mk, ["", "", ""]);
    a.send(hit(b, 1));
    a.send(hit(b, 2));
    a.send(hit(c, 3));
    await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 1);
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 2)).reason, "rate-limited");
    await c.waitType("DAMAGE_PLAYER", (p) => p.hitId === 3);
    c.send(hit(b, 4));
    await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 4);
    await b.expectNone((p) => p.type === "DAMAGE_PLAYER" && p.hitId === 2, "rate-limited hit");
    await settle(250);
    a.send(hit(b, 5));
    await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 5);
}));

test("DAMAGE_RESULT reaches the attacker only, sanitized", () => withServer(async (mk) => {
    const [a, b, c] = await pvpRoom(mk, ["", "", ""]);
    a.send(hit(b, 9));
    await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 9);
    b.send({ type: "DAMAGE_RESULT", targetClientId: a.id, hitId: 9, result: "applied", reason: "", damage: 4,
             clientId: 999, fromServer: true, extra: 1 });
    const got = await a.waitType("DAMAGE_RESULT");
    assert.deepEqual(got, { type: "DAMAGE_RESULT", clientId: b.id, targetClientId: a.id, hitId: 9, result: "applied",
                            reason: "", damage: 4 });
    await settle(250); // past the per-pair hit interval
    a.send(hit(b, 10));
    await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === 10);
    b.send({ type: "DAMAGE_RESULT", targetClientId: a.id, hitId: 10, result: "refused", reason: "x".repeat(100),
             damage: 1e9 });
    const bad = await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 10);
    assert.deepEqual([bad.result, bad.reason.length, bad.damage, bad.fromServer], ["dropped", 32, 4, undefined]);
    await c.expectNone((p) => p.type === "DAMAGE_RESULT", "result for a bystander");
}));

test("an applied knockout is announced to the whole room; others are not", () => withServer(async (mk) => {
    const [a, b, c] = await pvpRoom(mk, ["", "", ""]);
    const room = [a, b, c];
    const answer = async (hitId, result, knockout) => {
        a.send(hit(b, hitId));
        await b.waitType("DAMAGE_PLAYER", (p) => p.hitId === hitId);
        b.send({ type: "DAMAGE_RESULT", targetClientId: a.id, hitId, result, reason: "", damage: 4, knockout });
        await a.waitType("DAMAGE_RESULT", (p) => p.hitId === hitId);
        await settle(250); // past the per-pair hit interval
    };
    await answer(1, "applied", "floor");
    for (const cl of room) {
        const ko = await cl.waitType("PVP_KNOCKOUT");
        assert.deepEqual(ko, { type: "PVP_KNOCKOUT", attackerClientId: a.id, victimClientId: b.id, knockout: "floor" });
    }
    await answer(2, "applied", "ko");
    for (const cl of room) {
        assert.equal((await cl.waitType("PVP_KNOCKOUT")).knockout, "ko");
    }
    await answer(3, "blocked", "ko");
    await answer(4, "applied", "dead");
    await answer(5, "applied", undefined);
    for (const cl of room) {
        await cl.expectNone((p) => p.type === "PVP_KNOCKOUT", "knockout for a block, a bad value or none");
    }
    // Only the relay announces one.
    a.send({ type: "PVP_KNOCKOUT", attackerClientId: a.id, victimClientId: c.id, knockout: "ko" });
    await c.expectNone((p) => p.type === "PVP_KNOCKOUT" && p.victimClientId === c.id, "client-made knockout");
}));

test("DAMAGE_RESULT only answers a hit the server forwarded, once", () => withServer(async (mk) => {
    const [a, b, c] = await pvpRoom(mk, ["", "", ""]);
    // Never forwarded: made up by b, or a hit c got.
    b.send({ type: "DAMAGE_RESULT", targetClientId: a.id, hitId: 5, result: "applied", damage: 4 });
    a.send(hit(c, 6));
    await c.waitType("DAMAGE_PLAYER", (p) => p.hitId === 6);
    b.send({ type: "DAMAGE_RESULT", targetClientId: a.id, hitId: 6, result: "applied", damage: 4 });
    await a.expectNone((p) => p.type === "DAMAGE_RESULT", "result for a hit b never received");
    // Answered once; a second answer is dropped.
    c.send({ type: "DAMAGE_RESULT", targetClientId: a.id, hitId: 6, result: "applied", damage: 2 });
    assert.equal((await a.waitType("DAMAGE_RESULT", (p) => p.hitId === 6)).damage, 2);
    c.send({ type: "DAMAGE_RESULT", targetClientId: a.id, hitId: 6, result: "applied", damage: 2 });
    await a.expectNone((p) => p.type === "DAMAGE_RESULT", "second result for one hit");
}));

test("room state carries shareWoodenShield, on by default and owner-only", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const all = await a.join();
    assert.equal(all.roomState.shareWoodenShield, true);
    await b.join();
    b.send({ type: "UPDATE_ROOM_STATE", state: { shareWoodenShield: false } });
    await a.expectNone((p) => p.type === "UPDATE_ROOM_STATE", "room state from a non-owner");
    a.send({ type: "UPDATE_ROOM_STATE", state: { shareWoodenShield: false } });
    assert.equal((await b.waitType("UPDATE_ROOM_STATE")).state.shareWoodenShield, false);
    a.send({ type: "UPDATE_ROOM_STATE", state: { shareWoodenShield: "no" } });
    assert.equal((await b.waitType("UPDATE_ROOM_STATE")).state.shareWoodenShield, false, "non-boolean ignored");
}));

test("room state carries pvpFriendlyFire and pvpLethal, off by default and owner-only", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const all = await a.join();
    assert.equal(all.roomState.pvpFriendlyFire, false);
    assert.equal(all.roomState.pvpLethal, false);
    await b.join();
    b.send({ type: "UPDATE_ROOM_STATE", state: { pvpLethal: true } });
    await a.expectNone((p) => p.type === "UPDATE_ROOM_STATE", "room state from a non-owner");
    a.send({ type: "UPDATE_ROOM_STATE", state: { pvpFriendlyFire: true, pvpLethal: 1 } });
    const got = await b.waitType("UPDATE_ROOM_STATE");
    assert.deepEqual([got.state.pvpFriendlyFire, got.state.pvpLethal], [true, false]);
}));

test("a HANDSHAKE from an older protocol or another app is refused and never joins", () => withServer(async (mk, server, mkTcp) => {
    const a = mk();
    await a.join({ name: "A", roomId: "r" });
    const cases = [
        [mk(), { protocolVersion: 4 }, /your client speaks protocol 4\. Update the mod\./],
        [mk(), { app: undefined }, /not Twili-Together/],
        [mk(), { app: "other-mod", protocolVersion: 2 }, /not Twili-Together/],
        [mk(), { protocolVersion: "5" }, /speaks protocol unknown/],
        [mk(), { protocolVersion: 5.5 }, /speaks protocol 5\.5/],
        [mkTcp(), { protocolVersion: 4 }, /speaks protocol 4\./],
    ];
    for (const [c, fields, pattern] of cases) {
        await c.open();
        c.send({ ...HANDSHAKE, name: "Old", roomId: "r", transport: c.transport, ...fields });
        // A valid retry on the same connection is ignored too.
        c.send({ ...HANDSHAKE, name: "Retry", roomId: "r", transport: c.transport });
        const msg = await c.waitType("SERVER_MESSAGE");
        const off = await c.waitType("DISABLE_CLIENT");
        assert.match(msg.message, /^This server runs Twili-Together protocol 5; /);
        assert.match(msg.message, pattern);
        assert.equal(off.message, msg.message);
        const code = await c.whenClosed;
        if (c.transport === "ws") assert.equal(code, 4000);
        assert.equal(c.inbox.find((p) => p.type === "ALL_CLIENT_STATE"), undefined, "a refused client joined");
    }
    await a.expectNone((p) => p.type === "UPDATE_CLIENT_STATE" && p.clientId !== a.id, "a refused client in the roster");
    const b = mk();
    const all = await b.join({ name: "B", roomId: "r" });
    assert.deepEqual(all.clients.map((c) => c.name).sort(), ["A", "B"]);
    const log = await waitForLog(server, /\[Old\] rejected HANDSHAKE \(incompatible, app=\?, p=5, ws\)/);
    assert.match(log, /\[Old\] rejected HANDSHAKE \(incompatible, app=twili-together, p=4, tcp\)/);
    assert.doesNotMatch(log, /Retry/);
}));

test("a newer protocol joins", () => withServer(async (mk) => {
    const a = mk();
    const all = await a.join({ protocolVersion: 6 });
    assert.equal(all.clients[0].protocolVersion, 6);
}));

test("the twili-together.5 subprotocol is selected when offered, and is optional", () => withServer(async (mk, server) => {
    const a = mk(undefined, ["twili-together.5"]);
    await a.join({ name: "A" });
    assert.equal(a.ws.protocol, "twili-together.5");
    const b = mk(undefined, ["something.1", "twili-together.5"]);
    await b.join({ name: "B" });
    assert.equal(b.ws.protocol, "twili-together.5");
    const c = mk();
    const all = await c.join({ name: "C" });
    assert.equal(c.ws.protocol, "");
    assert.deepEqual(all.clients.map((x) => x.name).sort(), ["A", "B", "C"]);
    // Only unknown ones: accepted without a subprotocol (the ws client library itself would refuse that).
    const res = await rawUpgrade(server.port, "something.1, other.2");
    assert.equal(res.statusCode, 101);
    assert.equal(res.headers["sec-websocket-protocol"], undefined);
}));

test("TCP and WebSocket clients share a room and presence flows both ways", () => withServer(async (mk, server, mkTcp) => {
    const w = mk();
    const t = mkTcp();
    await w.join({ name: "W", roomId: "mix", teamId: "t" });
    const all = await t.join({ name: "T", roomId: "mix", teamId: "t" });
    assert.deepEqual(all.clients.map((c) => c.name).sort(), ["T", "W"]);
    assert.equal(all.roomState.ownerClientId, w.id);
    const joined = await w.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === t.id);
    assert.deepEqual([joined.name, joined.modVersion, joined.layout], ["T", "test", "0123456789abcdef"]);
    w.enterStage("F_SP103");
    t.enterStage("F_SP103");
    await w.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === t.id && p.stageName === "F_SP103");
    await t.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === w.id && p.stageName === "F_SP103");
    w.send({ type: "PLAYER_UPDATE", quiet: true, seq: 1 });
    t.send({ type: "PLAYER_UPDATE", quiet: true, seq: 2 });
    const fromW = await t.waitType("PLAYER_UPDATE");
    assert.deepEqual([fromW.clientId, fromW.seq], [w.id, 1]);
    const fromT = await w.waitType("PLAYER_UPDATE");
    assert.deepEqual([fromT.clientId, fromT.seq], [t.id, 2]);
    t.send({ type: "SET_FLAG", flagNo: 5, addToQueue: true });
    assert.equal((await w.waitType("SET_FLAG")).clientId, t.id);
    await waitForLog(server, /\[\+\] T \(id=\d+, team="t", session=none, v=test\/p5, layout=0123456789abcdef, tcp\) joined room mix/);
    await t.close();
    await w.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === t.id && p.online === false);
}));

test("TCP frames split byte by byte or sent back to back both parse", () => withServer(async (mk, server, mkTcp) => {
    const w = mk();
    await w.join({ name: "W", roomId: "f" });
    const t = mkTcp();
    await t.open();
    const bytes = frame({ type: "HANDSHAKE", app: "twili-together", protocolVersion: 5, name: "Split", roomId: "f" });
    for (let i = 0; i < bytes.length; i++) {
        t.write(bytes.subarray(i, i + 1));
        await settle(1);
    }
    const all = await t.waitType("ALL_CLIENT_STATE");
    t.id = all.clients.find((c) => c.self).clientId;
    assert.deepEqual(all.clients.map((c) => c.name).sort(), ["Split", "W"]);
    // Two frames and the start of a third in one write, the rest of the third later.
    const signal = (name) => frame({ type: "AUTOTEST_SIGNAL", instance: "t", name });
    const third = signal("three");
    t.write(Buffer.concat([signal("one"), signal("two"), third.subarray(0, 7)]));
    await settle(50);
    t.write(third.subarray(7));
    for (const name of ["one", "two", "three"]) {
        assert.equal((await w.waitType("AUTOTEST_SIGNAL", (p) => p.name === name)).clientId, t.id);
    }
}));

test("an out-of-range TCP frame length closes that connection and the server keeps serving", () => withServer(async (mk, server, mkTcp) => {
    const w = mk();
    await w.join({ name: "W", roomId: "o" });
    const t = mkTcp();
    await t.join({ name: "T", roomId: "o" });
    const head = Buffer.alloc(4);
    head.writeUInt32LE(1048577);
    t.write(head);
    await t.whenClosed;
    await w.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === t.id && p.online === false);
    await waitForLog(server, /\[T\] bad tcp frame length 1048577, dropping connection/);

    const z = mkTcp();
    await z.open();
    z.write(Buffer.alloc(4));
    await z.whenClosed;
    await waitForLog(server, /bad tcp frame length 0, dropping connection/);

    // A frame of exactly 1 MiB is accepted.
    const t2 = mkTcp();
    const all = await t2.join({ name: "T2", roomId: "o" });
    assert.equal(all.clients.length, 2);
    const skeleton = JSON.stringify({ type: "AUTOTEST_SIGNAL", name: "big", pad: "" });
    const big = frame(JSON.stringify({ type: "AUTOTEST_SIGNAL", name: "big", pad: "x".repeat(1048576 - skeleton.length) }));
    assert.equal(big.readUInt32LE(0), 1048576);
    t2.write(big);
    assert.equal((await w.waitType("AUTOTEST_SIGNAL", (p) => p.name === "big")).clientId, t2.id);
    w.send({ type: "AUTOTEST_SIGNAL", name: "still" });
    await t2.waitType("AUTOTEST_SIGNAL", (p) => p.name === "still");
}));

test("KEEPALIVE is answered on TCP only and never relayed or logged", () => withServer(async (mk, server, mkTcp) => {
    const w = mk();
    await w.join({ name: "W", roomId: "k" });
    const t = mkTcp();
    await t.open();
    t.send({ type: "KEEPALIVE" });
    assert.deepEqual(await t.waitType("KEEPALIVE"), { type: "KEEPALIVE" }, "answered before the handshake");
    await t.join({ name: "T", roomId: "k" });
    const u = mkTcp();
    await u.join({ name: "U", roomId: "k" });
    t.send({ type: "KEEPALIVE" });
    await t.waitType("KEEPALIVE");
    w.send({ type: "KEEPALIVE" });
    t.send({ type: "AUTOTEST_SIGNAL", instance: "t", name: "after" });
    await w.waitType("AUTOTEST_SIGNAL");
    await u.waitType("AUTOTEST_SIGNAL");
    await w.expectNone((p) => p.type === "KEEPALIVE", "KEEPALIVE to a WebSocket client");
    await u.expectNone((p) => p.type === "KEEPALIVE", "relayed KEEPALIVE");
    const log = await waitForLog(server, /\[T\] AUTOTEST_SIGNAL instance=t name=after/);
    assert.doesNotMatch(log, /KEEPALIVE/);
}));

test("a silent TCP client is dropped after TT_TCP_TIMEOUT_MS", () => withServer(async (mk, server, mkTcp) => {
    const w = mk();
    await w.join({ name: "W", roomId: "s" });
    const k = mkTcp();
    await k.join({ name: "K", roomId: "s" });
    const keepalive = setInterval(() => k.send({ type: "KEEPALIVE" }), 100);
    try {
        const t = mkTcp();
        await t.join({ name: "T", roomId: "s" });
        await w.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === t.id && p.online === false, 3000);
        await t.whenClosed;
        await waitForLog(server, /\[T\] missed keepalive, dropping connection/);
        await settle(500);
        await w.expectNone((p) => p.type === "UPDATE_CLIENT_STATE" && p.clientId === k.id && p.online === false,
            "a TCP client that sends KEEPALIVE dropped");
        assert.equal(k.sock.destroyed, false);
    } finally {
        clearInterval(keepalive);
    }
}, { TT_TCP_TIMEOUT_MS: "300" }));

test("startup prints the WebSocket port, then the TCP port", () => withServer(async (mk, server) => {
    const out = server.output();
    const ws = out.match(/^\[[^\]]+\] listening on port (\d+)\r?$/m);
    const tcp = out.match(/^\[[^\]]+\] tcp relay on port (\d+)\r?$/m);
    assert.ok(ws && tcp, out);
    assert.ok(ws.index < tcp.index, out);
    assert.equal(parseInt(ws[1], 10), server.port);
    assert.equal(parseInt(tcp[1], 10), server.tcpPort);
    assert.notEqual(server.tcpPort, server.port);
    assert.equal(out.match(/listening on port/g).length, 1);
}));

test("the server restarts on the same explicit ports", async () => {
    const first = await startServer();
    const { port, tcpPort } = first;
    await first.stop();
    await withServer(async (mk, server, mkTcp) => {
        assert.deepEqual([server.port, server.tcpPort], [port, tcpPort]);
        await mk().join({ name: "W" });
        const all = await mkTcp().join({ name: "T" });
        assert.deepEqual(all.clients.map((c) => c.name).sort(), ["T", "W"]);
    }, {}, [String(port), String(tcpPort)]);
});

// --- Team games: owner, team game, member states and routing by game.

const rando = (key, extra = {}) => ({ inGame: true, kind: "randomizer", key,
                                     display: { name: `Seed ${key.slice(-2)}`, mode: "Randomizer 1.0.5" }, ...extra });
const AT_TITLE = { inGame: false, kind: "vanilla" };
const SHARE = { permalink: "djEuMC41LUhFQUQtMjc1ZWQ4NwA3Mzg2MjUzMzQ=", seed: "EarlyElatedPoe", version: "v1.0.5-HEAD-275ed87" };

function publish(c, game) {
    c.send({ type: "GAME_IDENTITY", ...game });
}

const syncOf = (p, id) => p.members.find((m) => m.clientId === id)?.sync;

// The first TEAM_STATE for teamId that satisfies pred (older ones stay in the inbox).
function teamState(c, teamId, pred = () => true, timeoutMs) {
    return c.waitType("TEAM_STATE", (p) => p.teamId === teamId && pred(p), timeoutMs);
}

// Waits until every listed member of teamId has the given sync state, as c sees it.
function waitSync(c, teamId, states) {
    return teamState(c, teamId, (p) => Object.entries(states).every(([id, s]) => syncOf(p, Number(id)) === s));
}

test("the room owner owns the no-team group; a named team is owned by its first member", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ name: "A" });
    await b.join({ name: "B", teamId: "red" });
    await c.join({ name: "C" });
    const none = await teamState(c, "", (p) => p.members.length === 2);
    assert.equal(none.ownerClientId, a.id);
    const red = await teamState(c, "red");
    assert.equal(red.ownerClientId, b.id);
    assert.equal(red.game.kind, "vanilla");
    assert.equal(red.game.key, undefined, "other teams do not see the game key");
}));

test("a joiner gets every team's TEAM_STATE right after ALL_CLIENT_STATE", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "red", game: rando("rando/f3/aaaaaaaaaaaaaaaa") });
    await b.join({ teamId: "blue" });
    const types = [];
    const c = mk();
    await c.open();
    c.ws.on("message", (d) => types.push(JSON.parse(d.toString()).type));
    await c.join({ teamId: "red" });
    await teamState(c, "blue");
    await teamState(c, "red");
    assert.equal(types[0], "ALL_CLIENT_STATE");
    assert.equal(types[1], "TEAM_STATE");
}));

test("the first member in game sets the team game, even while the owner is at the title screen", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A", teamId: "red", game: AT_TITLE });
    await b.join({ name: "B", teamId: "red", game: AT_TITLE });
    let s = await teamState(a, "red", (p) => p.members.length === 2);
    assert.equal(s.ownerClientId, a.id);
    assert.equal(s.game, null);
    assert.deepEqual([syncOf(s, a.id), syncOf(s, b.id)], ["pending", "pending"]);
    publish(b, rando("rando/f3/1111111111111111"));
    s = await teamState(a, "red", (p) => p.game !== null);
    assert.equal(s.game.key, "rando/f3/1111111111111111");
    assert.equal(syncOf(s, b.id), "ok");
    assert.equal(syncOf(s, a.id), "pending");
    assert.equal(s.ownerClientId, a.id, "ownership stays with the owner");
}));

test("a non-owner on another game is mismatched and never changes the team game", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "red", game: rando("rando/f3/1111111111111111") });
    await b.join({ teamId: "red", game: rando("rando/f3/2222222222222222") });
    const s = await waitSync(a, "red", { [a.id]: "ok", [b.id]: "mismatch" });
    assert.equal(s.game.key, "rando/f3/1111111111111111");
    publish(b, AT_TITLE);
    await waitSync(a, "red", { [b.id]: "pending" });
    publish(b, rando("rando/f3/1111111111111111"));
    await waitSync(a, "red", { [b.id]: "ok" });
}));

test("the owner switches the team game while no teammate plays it; the others turn mismatched", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A", teamId: "red", game: rando("rando/f3/1111111111111111") });
    await b.join({ name: "B", teamId: "red", game: AT_TITLE });
    publish(a, rando("rando/f3/3333333333333333"));
    const s = await teamState(b, "red", (p) => p.game?.key === "rando/f3/3333333333333333");
    assert.equal(syncOf(s, a.id), "ok");
    assert.equal(syncOf(s, b.id), "pending");
    publish(b, rando("rando/f3/1111111111111111"));
    await waitSync(b, "red", { [b.id]: "mismatch" });
    await a.expectNone((p) => p.type === "TEAM_GAME_CONFLICT", "a conflict with nobody playing");
}));

test("an owner switching games while a teammate plays is asked; only CLAIM_TEAM_GAME switches", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A", teamId: "red", game: rando("rando/f3/1111111111111111", { share: SHARE }) });
    await b.join({ name: "B", teamId: "red", game: rando("rando/f3/1111111111111111") });
    await waitSync(a, "red", { [a.id]: "ok", [b.id]: "ok" });
    publish(a, rando("rando/f3/4444444444444444"));
    const conflict = await a.waitType("TEAM_GAME_CONFLICT");
    assert.equal(conflict.teamId, "red");
    assert.equal(conflict.teammatesInGame, 1);
    assert.equal(conflict.yourKey, "rando/f3/4444444444444444");
    assert.equal(conflict.game.key, "rando/f3/1111111111111111");
    assert.equal(conflict.game.share.permalink, SHARE.permalink);
    const s = await waitSync(b, "red", { [a.id]: "mismatch", [b.id]: "ok" });
    assert.equal(s.game.key, "rando/f3/1111111111111111");
    await b.expectNone((p) => p.type === "TEAM_GAME_CONFLICT", "a conflict sent to a non-owner");

    b.send({ type: "CLAIM_TEAM_GAME" });
    await waitForLog(server, /\[B\] rejected CLAIM_TEAM_GAME \(not the team owner\)/);
    a.send({ type: "CLAIM_TEAM_GAME" });
    const claimed = await waitSync(b, "red", { [a.id]: "ok", [b.id]: "mismatch" });
    assert.equal(claimed.game.key, "rando/f3/4444444444444444");
    await b.expectNone((p) => p.type === "CLAIM_TEAM_GAME" || p.type === "GAME_IDENTITY", "relayed team-game packet");
}));

test("the leader leaving keeps the team game; the earliest-connected teammate leads next", () => withServer(async (mk) => {
    const owner = mk();
    const early = mk();
    const matched = mk();
    await owner.join({ name: "O", teamId: "red", game: rando("rando/f3/1111111111111111") });
    await early.join({ name: "E", teamId: "red", game: rando("rando/f3/9999999999999999") });
    await matched.join({ name: "M", teamId: "red", game: rando("rando/f3/1111111111111111") });
    await owner.close();
    const s = await teamState(early, "red", (p) => !p.members.some((m) => m.clientId === owner.id));
    assert.equal(s.ownerClientId, early.id, "connection order, not who plays the team game");
    assert.equal(s.game.key, "rando/f3/1111111111111111", "the new leader inherits the team game");
    assert.equal(syncOf(s, early.id), "mismatch");
    const late = mk();
    await late.join({ name: "L", teamId: "red", game: AT_TITLE });
    const seen = await teamState(late, "red");
    assert.equal(seen.game.key, "rando/f3/1111111111111111");
    assert.equal(seen.ownerClientId, early.id);
}));

test("room ownership and team leadership pass on by connection order", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    const d = mk();
    await a.join({ name: "A", teamId: "red" });
    await b.join({ name: "B", teamId: "blue" });
    await c.join({ name: "C", teamId: "red" });
    await d.join({ name: "D", teamId: "red" });
    await a.close();
    const room1 = await d.waitType("UPDATE_ROOM_STATE", (p) => p.state.ownerClientId !== a.id);
    assert.equal(room1.state.ownerClientId, b.id);
    assert.equal((await teamState(d, "red", (p) => p.members.length === 2)).ownerClientId, c.id);
    await c.close();
    assert.equal((await teamState(d, "red", (p) => p.members.length === 1)).ownerClientId, d.id);
    await b.close();
    const room2 = await d.waitType("UPDATE_ROOM_STATE", (p) => p.state.ownerClientId !== b.id);
    assert.equal(room2.state.ownerClientId, d.id);
    const e = mk();
    const all = await e.join({ name: "E", teamId: "red" });
    assert.equal(all.roomState.ownerClientId, d.id, "a newcomer does not take over");
    assert.equal((await teamState(e, "red")).ownerClientId, d.id);
}));

test("the room owner can hand the room to another member; nobody else can", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    const c = mk();
    const other = mk();
    await a.join({ name: "A" });
    await b.join({ name: "B", teamId: "red" });
    await c.join({ name: "C" });
    await other.join({ name: "O", roomId: "elsewhere" });
    b.send({ type: "SET_ROOM_OWNER", targetClientId: b.id });
    await waitForLog(server, /\[B\] rejected SET_ROOM_OWNER \(not the owner\)/);
    a.send({ type: "SET_ROOM_OWNER", targetClientId: other.id });
    a.send({ type: "SET_ROOM_OWNER", targetClientId: 9999 });
    a.send({ type: "SET_ROOM_OWNER", targetClientId: a.id });
    await waitForLog(server, /\[A\] rejected SET_ROOM_OWNER \(no such member\)[\s\S]*\[A\] rejected SET_ROOM_OWNER \(no such member\)[\s\S]*\[A\] rejected SET_ROOM_OWNER \(no such member\)/);
    await c.expectNone((p) => p.type === "UPDATE_ROOM_STATE", "a refused promotion");
    a.send({ type: "SET_ROOM_OWNER", targetClientId: b.id });
    for (const x of [a, b, c]) {
        assert.equal((await x.waitType("UPDATE_ROOM_STATE")).state.ownerClientId, b.id);
    }
    // Only the new owner changes room settings now.
    a.send({ type: "UPDATE_ROOM_STATE", state: { teleportMode: true } });
    await waitForLog(server, /\[A\] rejected UPDATE_ROOM_STATE \(not the owner\)/);
    b.send({ type: "UPDATE_ROOM_STATE", state: { teleportMode: true } });
    assert.equal((await c.waitType("UPDATE_ROOM_STATE")).state.teleportMode, true);
    // Team leadership is separate: A still leads the no-team group.
    assert.equal((await teamState(c, "", (p) => p.members.length === 2)).ownerClientId, a.id);
    // The promoted owner leaving hands the room back by connection order.
    await b.close();
    assert.equal((await c.waitType("UPDATE_ROOM_STATE", (p) => p.state.ownerClientId !== b.id)).state.ownerClientId, a.id);
}));

test("a team leader can hand the team to a teammate, who then decides the team game", () => withServer(async (mk, server) => {
    const X = "rando/f3/1111111111111111";
    const Y = "rando/f3/2222222222222222";
    const a = mk();
    const b = mk();
    const c = mk();
    const blue = mk();
    await a.join({ name: "A", teamId: "red", game: rando(X) });
    await b.join({ name: "B", teamId: "red", game: AT_TITLE });
    await c.join({ name: "C", teamId: "red", game: AT_TITLE });
    await blue.join({ name: "U", teamId: "blue" });
    b.send({ type: "SET_TEAM_OWNER", targetClientId: b.id });
    await waitForLog(server, /\[B\] rejected SET_TEAM_OWNER \(not the team leader\)/);
    a.send({ type: "SET_TEAM_OWNER", targetClientId: blue.id });
    await waitForLog(server, /\[A\] rejected SET_TEAM_OWNER \(no such teammate\)/);
    // The room owner leads red only; blue's leader is blue's own.
    a.send({ type: "SET_TEAM_OWNER", targetClientId: b.id });
    const s = await teamState(c, "red", (p) => p.ownerClientId === b.id);
    assert.equal(s.game.key, X);
    const theirs = await teamState(blue, "red", (p) => p.ownerClientId === b.id);
    assert.equal(theirs.ownerClientId, b.id, "other teams see the new leader");
    // A no longer switches the team; B does.
    publish(a, rando(Y));
    await waitSync(c, "red", { [a.id]: "mismatch" });
    publish(b, rando(Y));
    assert.equal((await teamState(c, "red", (p) => p.game.key === Y)).ownerClientId, b.id);
    await a.expectNone((p) => p.type === "TEAM_GAME_CONFLICT", "a conflict for a former leader");
}));

test("a team left empty keeps its game; the first one back owns it and switches it by playing another", () => withServer(async (mk) => {
    const keep = mk();
    await keep.join({ teamId: "blue" });
    const a = mk();
    await a.join({ teamId: "red", game: rando("rando/f3/1111111111111111") });
    await a.close();
    const b = mk();
    await b.join({ teamId: "red", game: AT_TITLE });
    let s = await teamState(b, "red");
    assert.equal(s.ownerClientId, b.id);
    assert.equal(s.game.key, "rando/f3/1111111111111111");
    await b.close();
    const e = mk();
    await e.join({ teamId: "red", game: rando("rando/f3/5555555555555555") });
    s = await teamState(e, "red");
    assert.equal(s.ownerClientId, e.id);
    assert.equal(s.game.key, "rando/f3/5555555555555555");
}));

test("the no-team group's leader passes to its earliest-connected member", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ name: "A" });
    await b.join({ name: "B", teamId: "red" });
    await c.join({ name: "C" });
    await a.close();
    const s = await teamState(c, "", (p) => p.members.length === 1);
    assert.equal(s.ownerClientId, c.id, "B (the new room owner) plays in red, so C leads the no-team group");
    const red = await teamState(c, "red");
    assert.equal(red.ownerClientId, b.id);
}));

test("a probe-matched member syncs only after it confirmed the unverified match", () => withServer(async (mk) => {
    const probe = "rando-probe/0123456789abcdef";
    const a = mk();
    const b = mk();
    await a.join({ teamId: "red", game: rando(probe) });
    await b.join({ teamId: "red", game: rando(probe) });
    a.enterStage("F_SP103");
    b.enterStage("F_SP103");
    await waitSync(a, "red", { [a.id]: "unverified", [b.id]: "unverified" });
    a.send({ type: "SET_FLAG", flagNo: 1 });
    await b.expectNone((p) => p.type === "SET_FLAG", "SET_FLAG from an unconfirmed member");
    publish(a, rando(probe, { allowUnverified: true }));
    await waitSync(b, "red", { [a.id]: "ok", [b.id]: "unverified" });
    a.send({ type: "SET_FLAG", flagNo: 2 });
    await b.expectNone((p) => p.type === "SET_FLAG", "SET_FLAG to an unconfirmed member");
    publish(b, rando(probe, { allowUnverified: true }));
    await waitSync(a, "red", { [b.id]: "ok" });
    a.send({ type: "SET_FLAG", flagNo: 3 });
    assert.equal((await b.waitType("SET_FLAG")).flagNo, 3);
}));

test("team traffic flows only between members on the team game; presence reaches everyone", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    const off = mk();
    await a.join({ name: "A", teamId: "red", game: rando("rando/f3/1111111111111111") });
    await b.join({ name: "B", teamId: "red", game: rando("rando/f3/1111111111111111") });
    await off.join({ name: "X", teamId: "red", game: rando("rando/f3/2222222222222222") });
    for (const c of [a, b, off]) c.enterStage("F_SP103");
    await waitSync(a, "red", { [off.id]: "mismatch" });
    await settle();
    a.send({ type: "SET_FLAG", flagNo: 1, addToQueue: true });
    a.send({ type: "GIVE_ITEM", itemNo: 0x40, addToQueue: true });
    a.send({ type: "ENEMY_DEFEATED", stageName: "F_SP103", layerNo: 0, kills: [] });
    a.send({ type: "ENEMY_DAMAGE", stageName: "F_SP103", layerNo: 0, hits: [] });
    a.send({ type: "STORY_MOVE", ph: "arrive", from: {}, to: {} });
    await b.waitType("SET_FLAG");
    await b.waitType("GIVE_ITEM");
    await b.waitType("ENEMY_DEFEATED");
    await b.waitType("ENEMY_DAMAGE");
    await b.waitType("STORY_MOVE");
    await off.expectNone((p) => ["SET_FLAG", "GIVE_ITEM", "ENEMY_DEFEATED", "ENEMY_DAMAGE", "STORY_MOVE"].includes(p.type),
        "team traffic to a mismatched member");

    off.send({ type: "SET_FLAG", flagNo: 9, addToQueue: true });
    off.send({ type: "UPDATE_WORLD_STATE", save: "WRONG" });
    off.send({ type: "STORY_EVENT", ph: "start" });
    off.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await a.expectNone((p) => ["SET_FLAG", "UPDATE_WORLD_STATE", "STORY_EVENT", "REQUEST_WORLD_STATE"].includes(p.type),
        "team traffic from a mismatched member");
    await off.expectNone((p) => p.fromQueue === true || p.fromCache === true, "catch-up for a mismatched member");
    await waitForLog(server, /\[X\] team traffic dropped \(SET_FLAG\): sync mismatch, game rando\/f3\/2222222222222222/);
    const text = fs.readFileSync(server.logPath, "utf8");
    assert.equal(text.match(/\[X\] team traffic dropped/g).length, 1, "logged once per member and game");

    a.send({ type: "PLAYER_UPDATE", quiet: true, pos: { x: 1, y: 2, z: 3 } });
    assert.equal((await off.waitType("PLAYER_UPDATE")).clientId, a.id);
    off.send({ type: "PLAYER_UPDATE", quiet: true, pos: { x: 4, y: 5, z: 6 } });
    assert.equal((await a.waitType("PLAYER_UPDATE")).clientId, off.id);
}));

test("a client without a game identity is pending and gets no team traffic", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "red" });
    await b.join({ teamId: "red", game: undefined });
    const s = await waitSync(a, "red", { [a.id]: "ok", [b.id]: "pending" });
    assert.equal(s.game.key, "vanilla");
    a.send({ type: "SET_FLAG", flagNo: 1 });
    await b.expectNone((p) => p.type === "SET_FLAG", "SET_FLAG to a member without a game");
}));

test("two teams on different seeds share a room: presence crosses, world sync does not", () => withServer(async (mk) => {
    const r1 = mk();
    const r2 = mk();
    const b1 = mk();
    const b2 = mk();
    await r1.join({ teamId: "red", game: rando("rando/f3/1111111111111111", { share: SHARE }) });
    await r2.join({ teamId: "red", game: rando("rando/f3/1111111111111111") });
    await b1.join({ teamId: "blue", game: rando("rando/f3/2222222222222222") });
    await b2.join({ teamId: "blue", game: rando("rando/f3/2222222222222222") });
    for (const c of [r1, r2, b1, b2]) c.enterStage("F_SP103");
    await waitSync(b2, "blue", { [b1.id]: "ok", [b2.id]: "ok" });
    await settle();
    r1.send({ type: "SET_FLAG", flagNo: 1 });
    b1.send({ type: "SET_FLAG", flagNo: 2 });
    assert.equal((await r2.waitType("SET_FLAG")).flagNo, 1);
    assert.equal((await b2.waitType("SET_FLAG")).flagNo, 2);
    await b1.expectNone((p) => p.type === "SET_FLAG", "red's flag on blue");
    await r1.expectNone((p) => p.type === "SET_FLAG", "blue's flag on red");
    r1.send({ type: "PLAYER_UPDATE", quiet: true });
    await b1.waitType("PLAYER_UPDATE");
    await b2.waitType("PLAYER_UPDATE");
}));

test("the permalink reaches the team only; other teams see the mode and seed name", () => withServer(async (mk) => {
    const r1 = mk();
    const r2 = mk();
    const blue = mk();
    const green = mk();
    await r1.join({ teamId: "red", game: rando("rando/f3/1111111111111111", { share: SHARE }) });
    await r2.join({ teamId: "red", game: AT_TITLE });
    await blue.join({ teamId: "blue", game: rando("rando/f3/2222222222222222") });
    const mine = await teamState(r2, "red", (p) => p.game !== null);
    assert.deepEqual(mine.game.share, SHARE);
    assert.equal(mine.game.key, "rando/f3/1111111111111111");
    const theirs = await teamState(blue, "red");
    assert.deepEqual(theirs.game, { kind: "randomizer", display: { name: "Seed 11", mode: "Randomizer 1.0.5" } });
    assert.equal(theirs.sameGameAsYours, undefined);
    assert.ok(theirs.members.every((m) => !("key" in m)), "no member keys for other teams");
    assert.equal(theirs.members.find((m) => m.clientId === r1.id).name, "Seed 11");
    await green.join({ teamId: "green", game: rando("rando/f3/1111111111111111") });
    const race = await teamState(green, "red");
    assert.equal(race.sameGameAsYours, true);
    assert.equal(race.game.share, undefined);
    await settle();
    const leaked = [...blue.inbox, ...green.inbox].some((p) => JSON.stringify(p).includes(SHARE.permalink));
    assert.equal(leaked, false, "red's permalink reached another team");
}));

test("a member with the permalink fills in a team game announced without one", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "red", game: rando("rando/f3/1111111111111111") });
    await b.join({ teamId: "red", game: AT_TITLE });
    let s = await teamState(b, "red", (p) => p.game !== null);
    assert.equal(s.game.share, undefined);
    publish(b, rando("rando/f3/1111111111111111", { share: SHARE }));
    s = await teamState(a, "red", (p) => p.game?.share !== undefined);
    assert.equal(s.game.share.permalink, SHARE.permalink);
}));

test("the world cache and queue are kept per team game; switching back resumes the old one", () => withServer(async (mk) => {
    const X = "rando/f3/1111111111111111";
    const Y = "rando/f3/2222222222222222";
    const a = mk();
    await a.join({ name: "A", teamId: "red", game: rando(X) });
    a.enterStage("F_SP103");
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    a.send({ type: "UPDATE_WORLD_STATE", save: "SX" });
    a.send({ type: "GIVE_ITEM", itemNo: 0x40, addToQueue: true });
    await settle();
    publish(a, rando(Y));
    await teamState(a, "red", (p) => p.game.key === Y);
    a.send({ type: "UPDATE_WORLD_STATE", save: "SY" });
    a.send({ type: "GIVE_ITEM", itemNo: 0x41, addToQueue: true });
    await settle();
    await a.close();

    const b = mk();
    await b.join({ name: "B", teamId: "red", game: rando(Y) });
    b.enterStage("F_SP103");
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    assert.equal((await b.waitType("UPDATE_WORLD_STATE")).save, "SY");
    const gy = await b.waitType("GIVE_ITEM", (p) => p.fromQueue === true);
    assert.equal(gy.itemNo, 0x41);
    assert.ok(gy.queueEpoch.endsWith(`:red:${Y}`), gy.queueEpoch);
    await b.expectNone((p) => p.save === "SX" || p.itemNo === 0x40, "the other game's cache");

    // B, alone and so the owner, switches the team back to X: X's cache is still there.
    publish(b, rando(X));
    await teamState(b, "red", (p) => p.game.key === X);
    b.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    assert.equal((await b.waitType("UPDATE_WORLD_STATE", (p) => p.save === "SX")).fromCache, true);
    assert.equal((await b.waitType("GIVE_ITEM", (p) => p.itemNo === 0x40)).fromQueue, true);
}));

test("a member that turns ok catches up before it answers anyone's world-state request", () => withServer(async (mk) => {
    const X = "rando/f3/1111111111111111";
    const a = mk();
    const b = mk();
    await a.join({ teamId: "red", game: rando(X) });
    await b.join({ teamId: "red", game: rando("rando/f3/2222222222222222") });
    a.enterStage("F_SP103");
    b.enterStage("F_SP103");
    a.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await settle();
    publish(b, rando(X));
    await waitSync(a, "red", { [b.id]: "ok" });
    const c = mk();
    await c.join({ teamId: "red", game: rando(X) });
    c.enterStage("F_SP103");
    await settle();
    c.send({ type: "REQUEST_WORLD_STATE", catchUp: true });
    await a.waitType("REQUEST_WORLD_STATE", (p) => p.clientId === c.id);
    await b.expectNone((p) => p.type === "REQUEST_WORLD_STATE", "request forwarded to a member that never caught up");
}));

test("hostile GAME_IDENTITY payloads are sanitized and cannot break the team", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    await a.join({ name: "A", teamId: "red", game: { inGame: true, kind: "randomizer", key: "rando/../\u0000bad key!" } });
    let s = await teamState(a, "red");
    assert.equal(s.game, null, "an invalid key is no key");
    assert.equal(syncOf(s, a.id), "pending");
    for (const bad of [
        { inGame: "yes", key: "vanilla" },
        { inGame: true, key: "x".repeat(97) },
        { inGame: true, key: 42, kind: { a: 1 } },
        { inGame: true, key: "vanilla", display: "nope", share: [1, 2] },
        { inGame: true, key: "vanilla", kind: "evil", display: { name: "N".repeat(500), mode: ["m"] },
          share: { permalink: "not a permalink!", seed: 7, version: "v".repeat(300) } },
    ]) {
        publish(a, bad);
    }
    s = await teamState(a, "red", (p) => p.game?.share !== undefined);
    assert.deepEqual(s.game, { kind: "", key: "vanilla", display: { name: "N".repeat(64), mode: "" },
                               share: { permalink: "", seed: "", version: "v".repeat(64) } });
    await b.join({ teamId: "red" });
    await waitSync(b, "red", { [b.id]: "ok" });
    a.send({ type: "TEAM_STATE", teamId: "red", ownerClientId: 99, game: null, members: [] });
    a.send({ type: "TEAM_GAME_CONFLICT", teamId: "red" });
    await waitForLog(server, /\[A\] rejected TEAM_GAME_CONFLICT \(server-only packet\)/);
    await b.expectNone((p) => (p.type === "TEAM_STATE" && p.ownerClientId === 99) || p.type === "TEAM_GAME_CONFLICT",
        "a client-made team packet");
}));

test("teleport across teams: randomizer teams only with teleportAcrossTeams and never between games", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    const d = mk();
    await a.join({ name: "A", teamId: "red", game: rando("rando/f3/1111111111111111") });
    await b.join({ name: "B", teamId: "blue", game: rando("rando/f3/2222222222222222") });
    await c.join({ name: "C", teamId: "green", game: rando("rando/f3/1111111111111111") });
    await d.join({ name: "D", teamId: "red", game: rando("rando/f3/1111111111111111") });
    for (const x of [a, b, c, d]) x.enterStage("F_SP108");
    a.send({ type: "UPDATE_ROOM_STATE", state: { teleportMode: true } });
    await a.waitType("UPDATE_ROOM_STATE", (p) => p.state.teleportMode === true);
    await settle();
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 1 });
    assert.equal((await a.waitType("TELEPORT_TO", (p) => p.requestId === 1)).reason, "other-team");
    d.send({ type: "REQUEST_TELEPORT", targetClientId: a.id, requestId: 2 });
    assert.equal((await a.waitType("REQUEST_TELEPORT", (p) => p.requestId === 2)).clientId, d.id, "teammates still teleport");
    a.send({ type: "UPDATE_ROOM_STATE", state: { teleportAcrossTeams: true } });
    await a.waitType("UPDATE_ROOM_STATE", (p) => p.state.teleportAcrossTeams === true);
    await settle(1100);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 3 });
    assert.equal((await a.waitType("TELEPORT_TO", (p) => p.requestId === 3)).reason, "other-game");
    await settle(1100);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: c.id, requestId: 4 });
    await c.waitType("REQUEST_TELEPORT", (p) => p.requestId === 4);
}));

test("vanilla teams racing: teleport across teams is refused unless the room allows it", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    await a.join({ teamId: "red" });
    await b.join({ teamId: "blue" });
    a.enterStage("F_SP108");
    b.enterStage("F_SP108");
    a.send({ type: "UPDATE_ROOM_STATE", state: { teleportMode: true } });
    await a.waitType("UPDATE_ROOM_STATE", (p) => p.state.teleportMode === true);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 5 });
    assert.equal((await a.waitType("TELEPORT_TO", (p) => p.requestId === 5)).reason, "other-team");
    await b.expectNone((p) => p.type === "REQUEST_TELEPORT", "a request across teams");
    a.send({ type: "UPDATE_ROOM_STATE", state: { teleportAcrossTeams: true } });
    await a.waitType("UPDATE_ROOM_STATE", (p) => p.state.teleportAcrossTeams === true);
    await settle(1100);
    a.send({ type: "REQUEST_TELEPORT", targetClientId: b.id, requestId: 6 });
    await b.waitType("REQUEST_TELEPORT", (p) => p.requestId === 6);
}));

test("vanilla teams racing: world sync stays within each team, presence is room-wide", () => withServer(async (mk) => {
    const r1 = mk();
    const r2 = mk();
    const b1 = mk();
    await r1.join({ teamId: "red" });
    await r2.join({ teamId: "red" });
    await b1.join({ teamId: "blue" });
    for (const c of [r1, r2, b1]) c.enterStage("F_SP103");
    await waitSync(b1, "blue", { [b1.id]: "ok" });
    await settle();
    r1.send({ type: "SET_EVENT_BIT", no: 0x2908, addToQueue: true });
    assert.equal((await r2.waitType("SET_EVENT_BIT")).no, 0x2908);
    await b1.expectNone((p) => p.type === "SET_EVENT_BIT", "red's event bit on blue");
    r1.send({ type: "PLAYER_UPDATE", quiet: true });
    await b1.waitType("PLAYER_UPDATE");
    const blueView = await teamState(r1, "blue");
    assert.equal(blueView.sameGameAsYours, true, "both play vanilla");
}));

test("room state carries teleportAcrossTeams, off by default and owner-only", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const all = await a.join();
    assert.equal(all.roomState.teleportAcrossTeams, false);
    await b.join();
    b.send({ type: "UPDATE_ROOM_STATE", state: { teleportAcrossTeams: true } });
    await settle();
    a.send({ type: "UPDATE_ROOM_STATE", state: { teleportAcrossTeams: "yes" } });
    const s = await b.waitType("UPDATE_ROOM_STATE");
    assert.equal(s.state.teleportAcrossTeams, false);
}));

// --- Team colours

const RED_DEFAULT = { r: 0x1e, g: 0x88, b: 0xe5 };

test("team members show in their team's colour, players without a team in their own", () => withServer(async (mk) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ name: "A", color: { r: 10, g: 20, b: 30 } });
    await b.join({ name: "B", teamId: "red", color: { r: 40, g: 50, b: 60 } });
    await c.join({ name: "C", teamId: "red", color: { r: 70, g: 80, b: 90 } });
    const d = mk();
    const all = await d.join({ name: "D" });
    const row = (id) => all.clients.find((x) => x.clientId === id);
    assert.deepEqual(row(a.id).displayColor, { r: 10, g: 20, b: 30 });
    const teamDefault = row(b.id).displayColor;
    assert.deepEqual(row(c.id).displayColor, teamDefault, "one colour for the whole team");
    assert.deepEqual(row(b.id).color, { r: 40, g: 50, b: 60 }, "the player's own colour is kept");
    assert.deepEqual(teamDefault, RED_DEFAULT, "a stable default from the team id");
    assert.deepEqual((await teamState(d, "red")).color, teamDefault);
    assert.equal((await teamState(d, "")).color, null, "players without a team have no team colour");
    // A member's own colour change does not change how the team shows.
    b.send({ type: "UPDATE_CLIENT_STATE", color: { r: 1, g: 2, b: 3 } });
    const upd = await d.waitType("UPDATE_CLIENT_STATE", (p) => p.clientId === b.id);
    assert.deepEqual(upd.color, { r: 1, g: 2, b: 3 });
    assert.deepEqual(upd.displayColor, teamDefault);
    // World packets carry the display colour.
    b.send({ type: "SET_FLAG", flagNo: 1 });
    assert.deepEqual((await c.waitType("SET_FLAG")).senderColor, teamDefault);
}));

test("only the team leader sets the team colour, and everyone sees it at once", () => withServer(async (mk, server) => {
    const a = mk();
    const b = mk();
    const c = mk();
    await a.join({ name: "A" });
    await b.join({ name: "B", teamId: "red" });
    await c.join({ name: "C", teamId: "red" });
    c.send({ type: "SET_TEAM_COLOR", color: { r: 1, g: 1, b: 1 } });
    await waitForLog(server, /\[C\] rejected SET_TEAM_COLOR \(not the team leader\)/);
    a.send({ type: "SET_TEAM_COLOR", color: { r: 1, g: 1, b: 1 } });
    await waitForLog(server, /\[A\] rejected SET_TEAM_COLOR \(no team\)/);
    await a.expectNone((p) => p.type === "UPDATE_CLIENT_STATE" && p.displayColor?.r === 1, "a refused team colour");
    b.send({ type: "SET_TEAM_COLOR", color: { r: 250, g: 5, b: 155 } });
    const want = { r: 250, g: 5, b: 155 };
    for (const viewer of [a, b, c]) {
        for (const member of [b, c]) {
            const p = await viewer.waitType("UPDATE_CLIENT_STATE", (x) => x.clientId === member.id && x.displayColor?.r === 250);
            assert.deepEqual(p.displayColor, want);
            assert.equal("self" in p, false, "a colour-only update keeps the receiver's own row");
        }
    }
    assert.deepEqual((await teamState(a, "red", (p) => p.color.r === 250)).color, want);
    const late = mk();
    const all = await late.join({ name: "L", teamId: "red" });
    for (const row of all.clients.filter((x) => x.teamId === "red")) assert.deepEqual(row.displayColor, want);
    // Junk keeps the current colour.
    b.send({ type: "SET_TEAM_COLOR", color: { r: "x", g: -5, b: 999 } });
    assert.deepEqual((await teamState(a, "red", (p) => p.members.length === 3)).color, want);
}));

test("the team colour survives promotion and succession", () => withServer(async (mk) => {
    const want = { r: 12, g: 34, b: 56 };
    const a = mk();
    const b = mk();
    const c = mk();
    const watcher = mk();
    await a.join({ name: "A", teamId: "red" });
    await b.join({ name: "B", teamId: "red" });
    await c.join({ name: "C", teamId: "red" });
    await watcher.join({ name: "W" });
    a.send({ type: "SET_TEAM_COLOR", color: want });
    await teamState(watcher, "red", (p) => p.color.r === 12);
    a.send({ type: "SET_TEAM_OWNER", targetClientId: b.id });
    assert.deepEqual((await teamState(watcher, "red", (p) => p.ownerClientId === b.id)).color, want);
    await b.close();
    const s = await teamState(watcher, "red", (p) => !p.members.some((m) => m.clientId === b.id));
    assert.equal(s.ownerClientId, a.id);
    assert.deepEqual(s.color, want);
    await a.close();
    assert.deepEqual((await teamState(watcher, "red", (p) => p.members.length === 1)).color, want);
    c.send({ type: "SET_TEAM_COLOR", color: { r: 99, g: 99, b: 99 } });
    assert.equal((await teamState(watcher, "red", (p) => p.color.r === 99)).ownerClientId, c.id);
}));
