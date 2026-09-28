#!/usr/bin/env node
// Twili-Together test runner; usage and options in README.md.

const { spawn, execFileSync } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");
const zlib = require("node:zlib");
const scenarios = require("./scenarios");
const { MOD_ID, CONFIG_PREFIX, escapeModId } = require("./lib");

const REPO = path.resolve(__dirname, "..", "..", "..", "..");
const SERVER_JS = path.resolve(__dirname, "..", "server", "server.js");
let WORK_DIR = path.join(__dirname, "work");
let OUT_DIR = path.join(__dirname, "out");
const USER_DATA_DIR = path.join(process.env.APPDATA ?? "", "TwilitRealm", "Dusklight");

const TEST_CONFIG = {
    "game.autoSave": false,
    "game.pauseOnFocusLost": false,
    // Otherwise every instance obeys the controller of whoever uses this machine.
    "game.allowBackgroundInput": false,
    "game.enableDiscordPresence": false,
    "game.speedrunMode": false,
    "audio.masterVolume": 0,
    "video.enableFullscreen": false,
    "video.rememberWindowSize": true,
    "video.lastWindowWidth": 800,
    "video.lastWindowHeight": 450,
    // Off plus --stage skips Prelaunch even when a mod registers a game mode (see README).
    "backend.skipPreLaunchUI": false,
    "backend.wasPresetChosen": true,
};
const STRIPPED_KEY_PREFIXES = ["beacon.", "mod.", "actionBindings."];
const SEED_CACHES = ["pipeline_cache.db", "dawn_cache.db"];
const NET_OPS = new Set(["connect", "disconnect", "waitConnected", "waitPeers", "waitDummies",
    "waitNoDummies", "checkDummies", "expectPeers", "signal", "waitSignal"]);
// Fail any scenario: a partly inlined hook target, or our mod failing.
const GLOBAL_REJECT = [
    new RegExp(`for ${MOD_ID.replace(/\./g, "\\.")} was inlined into callers`),
    new RegExp(`${MOD_ID.replace(/\./g, "\\.")}.*failed: `),
];

function parseArgs(argv) {
    const opts = {
        exe: path.join(REPO, "build", "windows-clang-relwithdebinfo", "dusklight.exe"),
        timeout: null,
        repeat: 1,
        kill: true,
        list: false,
        names: [],
    };
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        if (a === "--exe") opts.exe = path.resolve(argv[++i]);
        else if (a === "--timeout") opts.timeout = parseInt(argv[++i], 10);
        else if (a === "--repeat") opts.repeat = parseInt(argv[++i], 10);
        else if (a === "--no-kill") opts.kill = false;
        else if (a === "--list") opts.list = true;
        else if (a === "--work") WORK_DIR = path.resolve(argv[++i]);
        else if (a === "--out") OUT_DIR = path.resolve(argv[++i]);
        else if (a.startsWith("--")) throw new Error(`unknown option ${a}`);
        else opts.names.push(a);
    }
    return opts;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function timestamp() {
    const d = new Date();
    const p = (n) => String(n).padStart(2, "0");
    return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

function readText(file) {
    try {
        return fs.readFileSync(file, "utf8");
    } catch {
        return "";
    }
}

// Minimal zip reader for a bundle's mod.json.
function readZipEntry(file, entryName) {
    const buf = fs.readFileSync(file);
    let eocd = -1;
    for (let i = buf.length - 22; i >= Math.max(0, buf.length - 22 - 65535); i--) {
        if (buf.readUInt32LE(i) === 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return null;
    const count = buf.readUInt16LE(eocd + 10);
    let p = buf.readUInt32LE(eocd + 16);
    for (let n = 0; n < count && buf.readUInt32LE(p) === 0x02014b50; n++) {
        const method = buf.readUInt16LE(p + 10);
        const compressed = buf.readUInt32LE(p + 20);
        const nameLen = buf.readUInt16LE(p + 28);
        const extraLen = buf.readUInt16LE(p + 30);
        const commentLen = buf.readUInt16LE(p + 32);
        const local = buf.readUInt32LE(p + 42);
        const name = buf.toString("utf8", p + 46, p + 46 + nameLen);
        if (name === entryName) {
            const start = local + 30 + buf.readUInt16LE(local + 26) + buf.readUInt16LE(local + 28);
            const data = buf.subarray(start, start + compressed);
            return method === 8 ? zlib.inflateRawSync(data) : data;
        }
        p += 46 + nameLen + extraLen + commentLen;
    }
    return null;
}

function listExeMods(exe) {
    const dir = path.join(path.dirname(exe), "mods");
    const ids = [];
    let entries = [];
    try {
        entries = fs.readdirSync(dir, { withFileTypes: true });
    } catch {
        return ids;
    }
    for (const e of entries) {
        try {
            let manifest = null;
            if (e.isFile() && e.name.endsWith(".dusk")) {
                manifest = readZipEntry(path.join(dir, e.name), "mod.json");
            } else if (e.isDirectory() && fs.existsSync(path.join(dir, e.name, "mod.json"))) {
                manifest = fs.readFileSync(path.join(dir, e.name, "mod.json"));
            }
            if (manifest) ids.push(JSON.parse(manifest.toString("utf8")).id);
        } catch (err) {
            console.warn(`    cannot read mod manifest ${e.name}: ${err.message}`);
        }
    }
    return ids;
}

// The SDL preference path under the slot's redirected home.
const slotHome = (dir) => path.join(dir, "home");
const slotCacheDir = (dir) => path.join(slotHome(dir), "AppData", "Roaming", "TwilitRealm", "Dusklight");

function prepareSlot(index, modIds, enableMods) {
    const dir = path.join(WORK_DIR, `slot${index}`);
    fs.mkdirSync(dir, { recursive: true });
    for (const name of ["logs", "USA", "PAL", "JPN", "achievements.json", "states.json", "mod_saves.json",
        "mod_data", "mods"]) {
        fs.rmSync(path.join(dir, name), { recursive: true, force: true });
    }
    let config = {};
    try {
        config = JSON.parse(readText(path.join(USER_DATA_DIR, "config.json")) || "{}");
    } catch {
    }
    for (const key of Object.keys(config)) {
        if (STRIPPED_KEY_PREFIXES.some((p) => key.startsWith(p))) delete config[key];
    }
    const mods = {};
    for (const id of modIds) {
        mods[`mod.${escapeModId(id)}.enabled`] = id === MOD_ID || enableMods.includes(id);
    }
    fs.writeFileSync(path.join(dir, "config.json"), JSON.stringify({ ...config, ...TEST_CONFIG, ...mods }, null, 4));

    const cacheDir = slotCacheDir(dir);
    fs.mkdirSync(cacheDir, { recursive: true });
    fs.mkdirSync(path.join(slotHome(dir), "AppData", "Local"), { recursive: true });
    for (const name of SEED_CACHES) {
        const dst = path.join(cacheDir, name);
        const src = path.join(USER_DATA_DIR, name);
        if (!fs.existsSync(dst) && fs.existsSync(src)) fs.copyFileSync(src, dst);
    }
    return dir;
}

function findSlotLog(dir) {
    const logs = path.join(dir, "logs");
    try {
        const files = fs.readdirSync(logs).filter((f) => /^dusklight-.*\.log$/.test(f)).sort();
        return files.length ? path.join(logs, files[files.length - 1]) : null;
    } catch {
        return null;
    }
}

function usesNetwork(scenario) {
    return scenario.server ?? scenario.instances.some((i) => i.steps.some((s) => NET_OPS.has(s.op)));
}

// `ports` restarts on the ports of an earlier run; the default picks free ones.
async function startServer(outDir, extraEnv = {}, ports = null) {
    const logPath = path.join(outDir, "server.log");
    const args = ports ? [SERVER_JS, String(ports.port), String(ports.tcpPort)] : [SERVER_JS, "0"];
    const proc = spawn(process.execPath, args, {
        env: { ...process.env, ...extraEnv, TT_LOG_PATH: logPath },
        stdio: ["ignore", "pipe", "pipe"],
    });
    const [port, tcpPort] = await new Promise((resolve, reject) => {
        let out = "";
        const timer = setTimeout(() => reject(new Error(`server did not start:\n${out}`)), 10000);
        proc.stdout.on("data", (d) => {
            out += d.toString();
            const ws = out.match(/listening on port (\d+)/);
            const tcp = out.match(/tcp relay on port (\d+)/);
            if (ws && tcp) {
                clearTimeout(timer);
                resolve([parseInt(ws[1], 10), parseInt(tcp[1], 10)]);
            }
        });
        proc.stderr.on("data", (d) => (out += d.toString()));
        proc.on("exit", (code) => reject(new Error(`server exited (${code}):\n${out}`)));
    });
    return { proc, port, tcpPort, logPath, extraEnv, outDir };
}

// scenario.restartServer: once the relay logs AUTOTEST_SIGNAL `afterSignal`, it is killed and
// started again on the same ports `downMs` later; the clients have to find it on their own.
async function watchServerRestart(scenario, server) {
    const spec = scenario.restartServer;
    if (!spec || server.restarted || server.restarting) return;
    const log = readText(server.logPath);
    if (!new RegExp(`AUTOTEST_SIGNAL instance=\\S+ name=${spec.afterSignal}\\b`).test(log)) return;
    server.restarting = true;
    console.log(`    relay: killing it after signal '${spec.afterSignal}', restart in ${spec.downMs ?? 3000} ms`);
    await new Promise((resolve) => {
        server.proc.once("exit", resolve);
        server.proc.kill();
    });
    await sleep(spec.downMs ?? 3000);
    const next = await startServer(server.outDir, server.extraEnv, server);
    server.proc = next.proc;
    server.restarted = true;
    server.restarting = false;
    console.log(`    relay: restarted on ${server.port}/${server.tcpPort}`);
}

function killTree(pid) {
    try {
        execFileSync("taskkill", ["/PID", String(pid), "/T", "/F"], { stdio: "ignore" });
    } catch {
    }
}

// Cosmetic; never activates the window.
function placeWindow(pid, index) {
    const x = 20 + index * 820;
    const ps = [
        "$sig = '[DllImport(\"user32.dll\")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);'",
        "Add-Type -MemberDefinition $sig -Name W -Namespace TT",
        "for ($i = 0; $i -lt 120; $i++) {",
        `  $p = Get-Process -Id ${pid} -ErrorAction SilentlyContinue; if (-not $p) { exit }`,
        // SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
        `  if ($p.MainWindowHandle -ne 0) { [TT.W]::SetWindowPos($p.MainWindowHandle, [IntPtr]::Zero, ${x}, 60, 0, 0, 0x15) | Out-Null; exit }`,
        "  Start-Sleep -Milliseconds 500",
        "}",
    ].join("\n");
    const child = spawn("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", ps], { stdio: "ignore" });
    child.on("error", () => {});
}

function analyzeLog(text, stderr) {
    const lines = text.split(/\r?\n/);
    const crashIdx = lines.findIndex((l) => /APPLICATION CRASHED/.test(l));
    let crash = crashIdx >= 0 ? lines.slice(crashIdx, crashIdx + 30).join("\n") : null;
    if (!crash) {
        const errLines = stderr.split(/\r?\n/);
        const i = errLines.findIndex((l) => /APPLICATION CRASHED/.test(l));
        if (i >= 0) crash = errLines.slice(i, i + 30).join("\n");
    }
    const steps = lines.filter((l) => l.includes("[autotest]"));
    const ours = (l) => l.includes(MOD_ID) || l.includes("[autotest]");
    const warnings = lines.filter((l) => /\b(WARN|WARNING|ERROR)\b/.test(l) && ours(l));
    const fatal = lines.filter((l) => /\bFATAL\b/.test(l)).slice(0, 5);
    return { lines, crash, steps, warnings, fatal };
}

function logFindings(scenario, instName, lines) {
    const problems = [];
    const applies = (check) => !(check instanceof RegExp) && check.instance && check.instance !== instName;
    const patternOf = (check) => (check instanceof RegExp ? check : check.pattern);
    for (const check of scenario.expectLog ?? []) {
        if (applies(check)) continue;
        const re = patternOf(check);
        if (!lines.some((l) => re.test(l))) problems.push(`log never matched ${re}`);
    }
    for (const check of [...GLOBAL_REJECT, ...(scenario.rejectLog ?? [])]) {
        if (applies(check)) continue;
        const re = patternOf(check);
        const hit = lines.find((l) => re.test(l));
        if (hit) problems.push(`log matched ${re}: ${hit.trim()}`);
    }
    return problems;
}

async function runScenario(scenario, opts, outRoot) {
    const outDir = path.join(outRoot, scenario.name);
    fs.mkdirSync(outDir, { recursive: true });
    const room = `autotest-${scenario.name}`;
    const timeoutSec = opts.timeout ?? scenario.timeoutSec ?? 240;
    let server = null;
    if (usesNetwork(scenario)) {
        if (!fs.existsSync(SERVER_JS)) throw new Error(`scenario ${scenario.name} needs the relay server (${SERVER_JS})`);
        server = await startServer(outDir, scenario.serverEnv);
    }
    const url = !server ? "" :
        scenario.transport === "tcp" ? `tcp://127.0.0.1:${server.tcpPort}` : `ws://127.0.0.1:${server.port}`;
    console.log(`\n=== ${scenario.name}: ${scenario.description}`);
    console.log(`    ${server ? `server ${url}` : "no server"}, timeout ${timeoutSec}s, out ${outDir}`);

    const modIds = listExeMods(opts.exe);
    if (!modIds.includes(MOD_ID)) throw new Error(`${MOD_ID} is not next to ${opts.exe} (mods/*.dusk)`);
    for (const id of scenario.enableMods ?? []) {
        if (!modIds.includes(id)) throw new Error(`scenario ${scenario.name} enables ${id}, which is not next to the exe`);
    }

    const procs = [];
    for (const [index, inst] of scenario.instances.entries()) {
        const dataDir = prepareSlot(index, modIds, scenario.enableMods ?? []);
        // Fixture files, relative to the user dir (e.g. a randomizer seed under mod_data).
        for (const [rel, content] of Object.entries({ ...scenario.files, ...inst.files })) {
            const dst = path.join(dataDir, rel);
            fs.mkdirSync(path.dirname(dst), { recursive: true });
            fs.writeFileSync(dst, content);
        }
        const script = {
            instance: inst.name,
            url,
            room,
            timeoutSec,
            resultPath: path.join(outDir, `${inst.name}.result.json`),
            start: inst.start,
            steps: inst.steps,
        };
        const scriptPath = path.join(outDir, `${inst.name}.script.json`);
        // script: false runs without a script for runSec; only the log is judged.
        const scripted = inst.script !== false;
        if (scripted) fs.writeFileSync(scriptPath, JSON.stringify(script, null, 2));
        const s = inst.start;
        const args = [
            "--user-dir", dataDir,
            "--log-dir", path.join(dataDir, "logs"),
            // Skips Prelaunch; the boot takeover replaces it anyway.
            "--stage", `${s.stage},${s.room ?? 0},${s.point ?? 0},${s.layer ?? -1}`,
            ...(scripted ? ["--cvar", `${CONFIG_PREFIX}autotest_script=${scriptPath}`] : []),
            ...[...(scenario.cvars ?? []), ...(inst.cvars ?? [])].flatMap((c) => ["--cvar", c]),
        ];
        const home = slotHome(dataDir);
        const env = {
            ...process.env,
            USERPROFILE: home,
            APPDATA: path.join(home, "AppData", "Roaming"),
            LOCALAPPDATA: path.join(home, "AppData", "Local"),
        };
        // A crash after the log closed only shows in stderr.
        const stderrPath = path.join(outDir, `${inst.name}.stderr.log`);
        const stderrFd = fs.openSync(stderrPath, "w");
        const proc = spawn(opts.exe, args, { cwd: path.dirname(opts.exe), env, stdio: ["ignore", "ignore", stderrFd] });
        fs.closeSync(stderrFd);
        const entry = {
            name: inst.name, dataDir, proc, stderrPath, exitCode: null, scripted,
            stopAt: scripted ? null : Date.now() + (inst.runSec ?? 30) * 1000,
            killedForCrash: false, killedForTimeout: false, killedAfterResult: false, stoppedAsPlanned: false,
        };
        proc.on("exit", (code) => (entry.exitCode = code));
        procs.push(entry);
        placeWindow(proc.pid, index);
        console.log(`    launched ${inst.name} (pid ${proc.pid}, data ${dataDir})`);
        await sleep(inst.launchDelayMs ?? 1500);
    }

    const deadline = Date.now() + (timeoutSec + 30) * 1000;
    const resultPath = (p) => path.join(outDir, `${p.name}.result.json`);
    const resultSeen = new Map();
    while (procs.some((p) => p.exitCode === null)) {
        await sleep(500);
        if (server) await watchServerRestart(scenario, server);
        for (const p of procs) {
            if (p.exitCode !== null || p.killedForCrash) continue;
            const log = findSlotLog(p.dataDir);
            if (/APPLICATION CRASHED/.test(readText(log ?? "")) || /APPLICATION CRASHED/.test(readText(p.stderrPath))) {
                console.log(`    ${p.name} crashed; stopping it`);
                p.killedForCrash = true;
                await sleep(1500);
                killTree(p.proc.pid);
                continue;
            }
            if (p.stopAt !== null && Date.now() > p.stopAt) {
                p.stoppedAsPlanned = true;
                killTree(p.proc.pid);
                continue;
            }
            // Hosts without request_quit cannot end the game themselves.
            if (fs.existsSync(resultPath(p))) {
                if (!resultSeen.has(p)) resultSeen.set(p, Date.now());
                else if (Date.now() - resultSeen.get(p) > 30000) {
                    console.log(`    ${p.name} wrote its result but did not exit; stopping it`);
                    p.killedAfterResult = true;
                    killTree(p.proc.pid);
                }
            }
        }
        if (Date.now() > deadline) {
            for (const p of procs.filter((x) => x.exitCode === null)) {
                console.log(`    ${p.name} still running at timeout; killing`);
                p.killedForTimeout = true;
                if (opts.kill) killTree(p.proc.pid);
            }
            break;
        }
    }
    await sleep(500);
    if (server) server.proc.kill();

    const report = { scenario: scenario.name, url, room, instances: [] };
    let pass = true;
    for (const p of procs) {
        const logPath = findSlotLog(p.dataDir);
        const text = logPath ? readText(logPath) : "";
        if (logPath) fs.copyFileSync(logPath, path.join(outDir, `${p.name}.log`));
        const stderr = readText(p.stderrPath);
        const a = analyzeLog(text, stderr);
        let result = null;
        try {
            result = JSON.parse(readText(resultPath(p)));
        } catch {
        }
        const findings = logFindings(scenario, p.name, a.lines);
        const exitOk = p.exitCode === 0 || (p.killedAfterResult && result?.exitCode === 0);
        const ok = !a.crash && !p.killedForTimeout && findings.length === 0 &&
            (p.scripted ? exitOk && result?.pass === true : p.stoppedAsPlanned);
        pass &&= ok;
        report.instances.push({
            name: p.name, ok, exitCode: p.exitCode, killedForCrash: p.killedForCrash,
            killedForTimeout: p.killedForTimeout, killedAfterResult: p.killedAfterResult,
            log: path.join(outDir, `${p.name}.log`), result, crash: a.crash, warnings: a.warnings,
            findings,
        });
        const outcome = !p.scripted ? "unscripted run" :
            result ? `result=${result.pass ? "pass" : "fail"}${result.reason ? ` "${result.reason}"` : ""}` : "no result";
        console.log(`    ${ok ? "PASS" : "FAIL"} ${p.name}: exit=${p.exitCode}${p.killedForTimeout ? " (timeout)" : ""}${a.crash ? " (CRASH)" : ""} ${outcome}`);
        if (a.crash) console.log(a.crash.split("\n").map((l) => `        ${l}`).join("\n"));
        for (const l of a.fatal) if (!a.crash) console.log(`        ${l}`);
        for (const f of findings) console.log(`        ${f}`);
        if (!ok && !a.crash) {
            for (const l of a.steps.slice(-8)) console.log(`        ${l}`);
        }
        if (!ok && result?.pass === true) {
            // Passed, then exited badly: show what it printed after its result.
            const tail = stderr.split(/\r?\n/);
            const closed = tail.findLastIndex((l) => /\[autotest\] RESULT/.test(l));
            for (const l of tail.slice(closed + 1).filter(Boolean).slice(-40)) console.log(`        stderr: ${l}`);
        }
        if (a.warnings.length) {
            console.log(`        ${a.warnings.length} Twili-Together warning(s):`);
            for (const l of a.warnings.slice(0, 5)) console.log(`          ${l}`);
        }
    }
    if (scenario.restartServer && server && !server.restarted) {
        pass = false;
        console.log(`    FAIL: the relay was never restarted (signal '${scenario.restartServer.afterSignal}' not seen)`);
    }
    if (server) {
        report.serverErrors = readText(server.logPath).split(/\r?\n/).filter((l) => /ws error|rejected|error/i.test(l));
        for (const l of report.serverErrors.slice(0, 5)) console.log(`    server: ${l}`);
    }
    report.pass = pass;
    fs.writeFileSync(path.join(outDir, "report.json"), JSON.stringify(report, null, 2));
    return report;
}

async function main() {
    const opts = parseArgs(process.argv.slice(2));
    if (opts.list) {
        for (const s of scenarios.all) console.log(`${s.name.padEnd(24)} ${s.description}`);
        return 0;
    }
    if (!fs.existsSync(opts.exe)) {
        console.error(`game executable not found: ${opts.exe}`);
        return 2;
    }
    const selected = opts.names.length
        ? opts.names.map((n) => {
            const s = scenarios.all.find((x) => x.name === n);
            if (!s) throw new Error(`unknown scenario ${n} (try --list)`);
            return s;
        })
        : scenarios.all.filter((s) => !s.manualOnly);

    let outRoot = path.join(OUT_DIR, timestamp());
    if (fs.existsSync(outRoot)) outRoot += `-${process.pid}`;
    const reports = [];
    for (let rep = 0; rep < opts.repeat; rep++) {
        for (const s of selected) {
            reports.push(await runScenario(s, opts, opts.repeat > 1 ? path.join(outRoot, `run${rep + 1}`) : outRoot));
        }
    }
    const failed = reports.filter((r) => !r.pass);
    console.log(`\n${reports.length - failed.length}/${reports.length} scenario run(s) passed. Output: ${outRoot}`);
    fs.writeFileSync(path.join(outRoot, "summary.json"), JSON.stringify(reports, null, 2));
    return failed.length ? 1 : 0;
}

main().then((code) => process.exit(code), (err) => {
    console.error(err);
    process.exit(2);
});
