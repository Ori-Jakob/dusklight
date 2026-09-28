# Twili-Together autotest

Runs real game instances with the Twili-Together mod (and, for network scenarios, a private relay
server) and checks what each instance reports, its log, and that nobody crashes. Each scenario gets
a PASS/FAIL per instance, the game logs, the server log, and the crash report if an instance
crashed.

```
node mods/twili_together/tools/autotest/run.js                  # every scenario
node mods/twili_together/tools/autotest/run.js smoke            # one scenario
node mods/twili_together/tools/autotest/run.js --list           # what exists
node mods/twili_together/tools/autotest/run.js smoke --repeat 5 # soak
```

Options: `--exe <dusklight.exe>` (default `build/windows-clang-relwithdebinfo/dusklight.exe`),
`--work <dir>` (slot directories, default `work/`), `--out <dir>` (reports, default `out/`),
`--timeout <sec>`, `--repeat <n>`, `--no-kill`.

Test from a copy of the build output (the exe, its DLLs and PDB, `res/` and `mods/`) so the build
directory can relink while tests run, and give concurrent runs their own `--work`:

```
node mods/twili_together/tools/autotest/run.js smoke --exe <copy>/dusklight.exe --work <dir>
```

Needs a GPU (there is no headless mode) and a user config with a valid disc image path: each run
reads `%APPDATA%/TwilitRealm/Dusklight/config.json`. Instances open 800x450 windows in a row,
muted. Don't minimize them: a minimized window skips frames.

Exit code is 0 when every scenario passed. Output goes to `out/<timestamp>/<scenario>/`:
`report.json`, `<instance>.log`, `<instance>.stderr.log`, `<instance>.result.json`,
`<instance>.script.json` and, for network scenarios, `server.log`.

## Isolation

Every instance runs with `--user-dir work/slot<N>` (config, memory card, achievements, mod data
and the mod cache) and `--log-dir work/slot<N>/logs`. The game's cache directory (pipeline caches,
and the logs without `--log-dir`) is the SDL preference path, which `--user-dir` does not move, so
the runner also points `USERPROFILE`, `APPDATA` and `LOCALAPPDATA` at `work/slot<N>/home`. A run
therefore never writes the player's real game data.

Before each scenario the runner rebuilds the slot's `config.json` from the user's, without
`mod.*`, `beacon.*` and `actionBindings.*` keys, plus the test settings: autosave and pause on
focus loss off, background controller input off, audio muted, windowed 800x450. It deletes the
slot's saves, achievements, logs and mod data. The first run seeds each slot's shader caches from
the user's data folder; later runs reuse them.

Mods: every `.dusk` next to the exe loads by default, and the in-tree build puts all in-tree mods
there (`custom_actor_demo` alone places a rock and mines in South Faron). The runner reads each
bundle's `mod.json` and disables every mod except Twili-Together and the scenario's `enableMods`.

The Prelaunch menu is skipped by passing `--stage <start stage>` with `backend.skipPreLaunchUI`
off. With that setting on, the host opens Prelaunch over the running game whenever a mod registers
a game mode (the randomizer does); the boot takeover still happens underneath, but while Prelaunch
is open cutscenes do not update and the memory card thread never starts. The mod's boot takeover
replaces the `--stage` handling anyway.

Test instances ignore the machine's controller: background input is off in the config, and the
mod's pad hook zeroes port 0 whenever no scripted input runs.

## How an instance runs a script

`--cvar mod.dev_n0ted_twili__together.autotest_script=<script.json>` (src/autotest/) makes the mod
take over the boot (pre-hook on `dScnLogo_c::nextSceneChange`): it builds a fresh save and loads
`start`. It then runs one step per simulation tick from `mod_update` and logs each as
`[autotest] step i/n {...}`. The run ends with `[autotest] RESULT PASS|FAIL`, a result file, and
`HostService.request_quit` with exit code 0 (pass), 2 (a step failed) or 3 (global timeout). On a
host without `request_quit` the runner stops the game 30 s after its result file appears.

Without the cvar the autotest is dormant: no autotest hooks, no ticks (scenario `dormant`).

The mod's own settings are `--cvar` overridable per instance: `tt("name", value)` in `lib.js`
builds `mod.dev_n0ted_twili__together.<name>=<value>` (the loader escapes `.` to `_` and `_` to
`__` in mod ids).

`start` fields: `stage`, `room`, `point`, `layer` (-1 = from story flags), `horseName` (the save's
horse name, default "Epona"), `status` (`armed` = tunic, sword and shield, the default; `new`;
`debug` = full inventory plus some story event bits), `clearTwilight` (default false: it moves many
stages to other layers, Ordon Village to a cutscene layer), `hour`, `eventBits` (event bit numbers
set before the first load, so it already picks the story layer: `[0x4510]`, Day 2 complete, puts
South Faron in twilight), `levels` (`{transform: [n], darkClear: [n]}`) and `firstPickups`
(default false: rupees count as seen before, so a dropped rupee never opens the first-pickup
message that pauses the world).

Pick start and warp stages that load without a forced cutscene on a fresh save; the ones in
`lib.js` `STAGES` are known to.

### Steps

Timings in `frames` are simulation ticks (30 per second), independent of the frame rate.

| op | fields | does |
| --- | --- | --- |
| `waitStage` | `stage`, `timeoutSec` | waits until the player exists and no stage change is pending for 60 ticks |
| `warp` | `stage`, `room`, `point`, `layer` | requests a stage change |
| `walk` | `frames`, `stickX`, `stickY`, `circle`, `buttons`, `async` | synthetic port-0 input; `circle` walks in a slow loop; `async` lets the next steps run meanwhile |
| `wait` | `frames` or `sec` | |
| `markPos` / `expectPos` | —; `minMoved`, `near` (`[x, y, z]`), `maxDist` | remember Link's position / fail unless he moved at least `minMoved` since, or is within `maxDist` of `near` |
| `connect` | `name`, `room`, `team` (default: instance name, run room, no team) | connects to the run's server |
| `disconnect` | | |
| `waitConnected` | `timeoutSec` | until the server assigned us a client id |
| `waitPeers` | `count`, `sameStage`, `timeoutSec` | until that many other clients are online (in our stage and layer) |
| `expectPeers` | `count`, `sameStage` | fails unless exactly that many |
| `waitDummies` / `waitNoDummies` | `count`, `timeoutSec` | until that many remote-player actors exist / none do |
| `checkDummies` | `maxDist` | fails if a peer in our layer has no dummy, or its dummy is further than `maxDist` from the peer's reported position |
| `signal` / `waitSignal` | `name`, `from`, `timeoutSec` | barrier between instances, relayed by the server |
| `giveItem` / `expectItem` | `item`, `timeoutSec` | `execItemGet` (a gameplay give, so it is shared) / wait for the item bit |
| `burnShield` / `expectShield` | —; `item` (255 = none), `owned`, `forSec`, `timeoutSec` | what a wooden shield burning up does (unequipped, first-get bit cleared) / wait for the equipped shield (and its first-get bit) |
| `addKeys` / `expectKeys` | `count`; `forSec`, `timeoutSec` | add to the current stage's small-key count / wait until it equals `count` (and, with `forSec`, stays so that long) |
| `setSwitch` / `unsetSwitch` / `expectSwitch` | `no`, `room` (default: current), `set` | save switch through `dComIfGs_onSwitch` etc. |
| `setEventBit` / `expectEventBit` | `no`, `set` | permanent event bit |
| `log` | `msg` | |
| `quit` / `fail` | `reason` | end the run (`quit` disconnects cleanly first) |
| `exitNow` | | pass and exit immediately without disconnecting, like a crash or a killed process |

A script that runs out of steps passes. The network steps need the session (it installs the
autotest's `NetDriver`); without it they fail with "needs the network session".

### Feature steps

Features register more steps from their own files through `src/autotest/AutoTestSteps.hpp`.

Motion (`StepsMotion.cpp`):

| op | fields | does |
| --- | --- | --- |
| `poseSelfTest` | | playout buffer rules and the cutscene classifier; needs no peer |
| `waitRoomState` | `key`, `value`, `timeoutSec` | until the room state's `key` equals `value` |
| `forceCutscene` | `on` (true, false, null = real events) | forces our cutscene state |
| `beginEvent` / `endEvent` | `kind` (`compulsory`), `timeoutSec` | orders a compulsory event / resets the running one |
| `expectDummyHidden` | `hidden`, `timeoutSec` | every peer in our layer has a dummy, hidden (or shown) |
| `expectPeerFlag` | `flag` (`inCutscene`, `wolf`), `set`, `timeoutSec` | the PLAYER_UPDATE presence flag of every peer in our layer |
| `measureDummyMotion` | `frames`, `settleFrames`, `minSpeed`, `maxBadRatio`, `self` | per-tick motion of the first peer's dummy (or our player) and its playout clock; fails on stalls and bursts |
| `dumpDummies` | | logs every remote client's pose and dummy state |
| `teleportSelf` | `dx`, `dy`, `dz` | moves our player at once |
| `expectDummyTeleport` | `minDist`, `maxPreStep`, `timeoutSec` | the first peer's dummy jumps `minDist` within one frame, without a slide before |

World sync (`StepsWorld.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setRoomOption` / `waitRoomOption` | `name` (`syncWorldState`, `shareWoodenShield`, `teleportMode`, `hidePlayersInCutscene`, `syncNPCs`, `pvpMode`, `pvpFriendlyFire`, `pvpLethal`, `showLocationsMode`, `cutsceneSync`), `value`, `timeoutSec` | sets our room-setting default (the owner's is the room's, so every instance sets it) / waits until the room reports that value |
| `forceLayout` | `layout` (16 hex digits) | before `connect`: announce another save layout, as a different game build would |
| `waitLayoutMismatch` | `peer`, `timeoutSec` | until the incompatible-layout toast was shown for that peer |
| `expectNoItem` | `item`, `forSec` (5) | fails if the item's first-get bit gets set within `forSec` |
| `expectMerges` | `min`, `max`, `timeoutSec` | world-state merges applied so far: waits for `min`, fails above `max` |

## Adding a scenario

Core scenarios are in `scenarios.js`; feature scenarios in `scenarios/<feature>.js`, each
exporting an array. A scenario has:

- `name`, `description`, `timeoutSec`;
- `cvars`: extra `--cvar` overrides for every instance;
- `serverEnv`: environment for the relay server (network scenarios only);
- `transport`: `"tcp"` connects over `tcp://` to the relay's TCP port instead of WebSocket;
- `restartServer`: `{afterSignal, downMs}` kills the relay once it logs that AUTOTEST_SIGNAL and
  starts it again on the same ports `downMs` later (default 3000); the scenario fails if it never
  happened;
- `server`: force (`true`) or suppress (`false`) the relay server; by default it runs when a step
  uses a network op;
- `enableMods`: ids of other mods next to the exe to keep enabled (`MODS` in `lib.js`);
- `expectLog` / `rejectLog`: regexes (or `{pattern, instance}`) every instance log must / must not
  match. Every scenario also rejects a HookService "was inlined into callers" warning for our mod
  (the hook would miss the inlined calls) and a failure of our mod;
- `instances`: one object per instance with `name`, optional `start`, optional `cvars`, optional
  `launchDelayMs` (delay before the *next* instance is launched), and `steps`. `script: false` with
  `runSec` runs the game without a script for that long and judges only its log.

The helpers in `lib.js`: `meetIn(stage, other)` waits for both players in a stage, checks each
other's dummy and barriers; `barrier(name, other)` synchronises two instances; `tt(name, value)`
builds a mod-setting `--cvar`.
