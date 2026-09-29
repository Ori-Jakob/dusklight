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
`<instance>.script.json` and, for network scenarios, `server.log`. `out/<timestamp>/summary.json`
has every report of the run; with `--repeat` each round goes to `out/<timestamp>/run<N>/`.

## Running the whole suite

Without scenario names the runner runs everything except the manual scenarios (`manualOnly`:
window tours and showcases that only exist for screen captures; run them by name). That is about
130 scenarios and takes around two hours on one runner. To go faster, split the list over two
runners, each with its own `--work` and `--out`:

```
node run.js <first half of the names>  --exe <copy>/dusklight.exe --work w1 --out out1 --repeat 3
node run.js <second half of the names> --exe <copy>/dusklight.exe --work w2 --out out2 --repeat 3
```

More than two runners (or other heavy work on the machine) makes the timing-sensitive scenarios
flaky: they count game ticks, and a starved instance falls behind its peer.

The relay has its own tests, no game needed: `npm test` in `../server`.

Not covered: reloading or disabling the mod from the Mods window while two instances are
connected. Check that by hand after changes to startup, shutdown or the actors.

## Isolation

Every instance runs with `--user-dir work/slot<N>` (config, memory card, achievements, mod data
and the mod cache) and `--log-dir work/slot<N>/logs`. The game's cache directory (pipeline caches,
and the logs without `--log-dir`) is the SDL preference path, which `--user-dir` does not move, so
the runner also points `USERPROFILE`, `APPDATA` and `LOCALAPPDATA` at `work/slot<N>/home`. A run
therefore never writes the player's real game data.

Before each scenario the runner rebuilds the slot's `config.json` from the user's, without
`mod.*` and `actionBindings.*` keys, plus the test settings: autosave and pause on
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
| `walk` | `frames`, `stickX`, `stickY`, `circle`, `buttons`, `async` | synthetic port-0 input (L and R also pull their analog triggers); `circle` walks in a slow loop; `async` lets the next steps run meanwhile |
| `wait` | `frames` or `sec` | |
| `markPos` / `expectPos` | none; `minMoved`, `near` (`[x, y, z]`), `maxDist` | remember Link's position / fail unless he moved at least `minMoved` since, or is within `maxDist` of `near` |
| `connect` | `name`, `room`, `team` (default: instance name, run room, no team) | connects to the run's server |
| `disconnect` | | |
| `waitConnected` | `timeoutSec` | until the server assigned us a client id |
| `waitPeers` | `count`, `sameStage`, `timeoutSec` | until that many other clients are online (in our stage and layer) |
| `expectPeers` | `count`, `sameStage` | fails unless exactly that many |
| `waitDummies` / `waitNoDummies` | `count`, `timeoutSec` | until that many remote-player actors exist / none do |
| `checkDummies` | `maxDist` | fails if a peer in our layer has no dummy, or its dummy is further than `maxDist` from the peer's reported position |
| `signal` / `waitSignal` | `name`, `from`, `timeoutSec` | barrier between instances, relayed by the server |
| `giveItem` / `expectItem` | `item`, `timeoutSec` | `execItemGet` (a gameplay give, so it is shared) / wait for the item bit |
| `burnShield` / `expectShield` | none; `item` (255 = none), `owned`, `forSec`, `timeoutSec` | what a wooden shield burning up does (unequipped, first-get bit cleared) / wait for the equipped shield (and its first-get bit) |
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
| `setRoomOption` / `waitRoomOption` | `name` (`syncWorldState`, `shareWoodenShield`, `teleportMode`, `hidePlayersInCutscene`, `syncNPCs`, `syncEnemyDamage`, `pvpMode`, `pvpFriendlyFire`, `pvpLethal`, `showLocationsMode`, `cutsceneSync`), `value`, `timeoutSec` | sets our room-setting default (the owner's is the room's, so every instance sets it) / waits until the room reports that value |
| `forceLayout` | `layout` (16 hex digits) | before `connect`: announce another save layout, as a different game build would |
| `waitLayoutMismatch` | `peer`, `timeoutSec` | until the incompatible-layout toast was shown for that peer |
| `expectNoItem` | `item`, `forSec` (5) | fails if the item's first-get bit gets set within `forSec` |
| `expectMerges` | `min`, `max`, `timeoutSec` | world-state merges applied so far: waits for `min`, fails above `max` |

Fidelity (`StepsFidelity.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setClothes` | `item` (0x2E Ordon, 0x2F Kokiri, 0x30 Magic Armor, 0x31 Zora), `timeoutSec` | changes our clothes like the collection screen; done once the new body is bound |
| `setBoots` | `on` | iron boots on (assigned to X) or off; done once that held for 10 ticks |
| `patchPlayerUpdate` | `patch`, `packets` (60) | merges `patch` into our next PLAYER_UPDATEs, each a keyframe |
| `assignItemX` | `item` | an owned item on X (press it with a walk's `buttons`: X = 0x400) |
| `expectDummyLook` | `clothes`, `casualHead`, `heavyBoots`, `zoraMask`, `lantern`, `heldItem`, `basePack`, `basePackAnm`, `standIn`, `ground`, `hidden`, `armorDrained`, `armorSettled`, `maxMissing`, `frames`, `timeoutSec` | the first peer's dummy shows every field given, then for `frames` more ticks |

Wolf, Midna and transformations (`StepsWolf.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setForm` | `form` (`wolf`, `human`) | the form Link spawns in on the next stage load (follow with a warp) |
| `transform` | `form`, `trace`, `timeoutSec` | transforms our player; with `trace` every stage and every expected emitter must show |
| `expectLocalForm` | `form` | |
| `expectRemoteForm` | `form`, `body` (form or `hidden`), `count`, `timeoutSec` | peers send `form` and their dummies show `body` |
| `expectCleanAnims` | `frames` (120), `maxRefused` (0) | no dummy refuses more clips than that |
| `setMidna` | `ride`, `visible`, `timeoutSec` | Midna on our wolf's back (visible in the light world: transform level 3) |
| `expectRemoteMidna` | `mode` (`drawn`, `shadow`, `none`), `upper`, `hairHand`, `leftHand`, `rightHand`, `tired`, `maxRefused`, `count`, `frames`, `timeoutSec` | peers' dummies pose their Midna so |
| `transformFxSelfTest` | | replay planner and codec; needs no peer |
| `expectRemoteTransformFx` | `form`, `maxLag`, `minSilhouette`, `fur`, `maxAnchorErr`, `skip`, `hidden`, `anchorSource`, `timeoutSec` | the first peer's next transformation as sent and as its dummy replayed it |
| `expectNoTransformFx` | `frames` | the dummy replays no transformation |
| `expectDummyTransformFx` | `emitters`, `tev`, `silhouette`, `anchorSource`, `frames`, `timeoutSec` | exactly these emitters this tick, and the other fields |
| `waitRemoteTransformPhase` | `phase` (`A`, `swap`, `C`), `timeoutSec` | until the first peer sends that stage |

Wolf attacks (`StepsWolfFx.cpp`):

| op | fields | does |
| --- | --- | --- |
| `wolfSpin` | `dir` (`right`, `left`), `trail`, `timeoutSec` | spin attack on our wolf |
| `wolfDome` | `force`, `radius`, `minLocks`, `timeoutSec` | Midna's lock-on dome (B held by an async walk before it) |
| `expectLocalWolfFx` | `spin`, `charge`, `dome`, `minRadius`, `lockBlur`, `timeoutSec` | what our capture sends |
| `expectPeerWolfFx` | `spin`, `dome`, `minRadius`, `lockBlur`, `lockDashSeq`, `hairAim`, `timeoutSec` | what the first peer last sent |
| `expectRemoteWolfFx` | `spin`, `lastSpin`, `minSpinTicks`, `maxSpinTicks`, `spinEmitters`, `dome`, `domeShown`, `minRadius`, `maxRadius`, `lockBlur`, `minLockDashes`, `maxLockDashes`, `hairAim`, `count`, `frames`, `timeoutSec` | what the peers' dummies show |
| `viewDummy` | `back`, `side`, `up` | our camera behind our Link, looking at the first peer's dummy |

Status effects (`StepsStatusFx.cpp`):

| op | fields | does |
| --- | --- | --- |
| `forceStatus` | `kind` (`freeze`, `burn`, `shieldBurn`, `douse`, `elec`, `hurt`, `chill`, `extinguish`), `value`, `frames`, `timeoutSec` | puts the status on our player the way the game does |
| `expectLocalStatus` | `frozen`, `iceBlock`, `elec`, `armorDrained`, `firePoints`, `shieldBurnMin`/`Max`, `shield`, `shieldInHand`, `damageTimerMin`, `sinkMin`/`Max`, `timeoutSec` | our capture and player |
| `expectDummyStatus` | `frozen`, `iceBlock`, `thaws`, `fireMin`/`Max`, `fireEmittersMin`/`Max`, `fireReceived`, `shieldBurnMin`/`Max`, `shieldBurnFx`, `shieldBurnOuts`, `shieldItem`, `elec`, `elecFx`, `damageTimerMin`/`Max`, `flashesMin`, `iceWait`, `sinkMin`/`Max`, `statusFlags`, `frames`, `timeoutSec` | the first peer's dummy |
| `statusFxPack` | `iceBlock`, `elec` | logs whether this stage's particle pack has these effects |

Items and projectiles (`StepsItemFx.cpp`). Kinds in `{kind: n}` fields are `arrow`, `boomerang`,
`bomb`, `spinner` and `crodBall`; event types `explode`, `hitMark`, `sound`, `water` and
`particle`:

| op | fields | does |
| --- | --- | --- |
| `spawnLocalItem` | `kind` (`arrow`, `bombArrow`, `seed`, `bomb`, `waterBomb`, `bombling`), `count`, `forward`, `up`, `pitch`, `aimAtDummy`, `atDummy`, `timeoutSec` | our real item actors, without the aim |
| `explodeAtDummy` | | a bomb arrow's explosion beside the first peer's dummy |
| `injectItemFx` | `slots`, `events`, `hook`, `ball`, `packets` | our next updates show these |
| `setOil` | `value` (21600) | lantern oil |
| `expectLocalItemFx` | `objects`, `events`, `hookOutTicks`, `ironBallTicks`, `levelSfxTicks`, `minHookDist`, `timeoutSec` | our capture's totals |
| `markRemoteItemFx` | | remembers the first peer dummy's counters |
| `expectRemoteItemFx` | `minDrawn` / `maxDrawn` (`{kind: n}` drawn now), `minSeen` (`{kind: n}` since the mark), `explosions`, `hitMarks`, `sounds`, `splashes`, `particles`, `maxExplosions` (since the mark), `tornado`, `boomCharge`, `minLights`, `maxLights`, `minEmitters`, `maxHeapUsed`, `hookChain`, `minTipDist`, `minHookShots` (chains since the mark), `minHookPeak` (the latest chain's longest tip distance), `minIronBallMode`, `maxIronBallMode`, `minIronBallDist`, `lanternFlame`, `minLanternGlow`, `maxLanternGlow`, `levelSfx` (level sounds started since the mark); `frames` (0), `timeoutSec` (20) | until the first peer's dummy shows every field given, then for `frames` more ticks |

Life, PvP and enemies (`StepsPvp.cpp`, `StepsEnemy.cpp`):

| op | fields | does |
| --- | --- | --- |
| `setLife` / `markLife` | `value` (12, quarter hearts) | sets / remembers our life |
| `expectLife` / `expectLifeDelta` | `value`, `min`, `max` / `delta`, `frames`, `timeoutSec` | our life (saved plus the meter's pending change) |
| `approachDummy` | `dist` (110) | our Link in front of the first peer's dummy, facing it |
| `sendPvpHit` | `target`, `kind` (`sword`), `damage` (2), `knockback` (`light`, `knockdown`), `dirY`, `blocked` | a real DAMAGE_PLAYER without our collision pass or cooldown |
| `expectPvpResult` | `target`, `result` (string or list), `reason`, `damage`, `timeoutSec` | the DAMAGE_RESULT of our last hit on `target` |
| `expectPvpStats` | `target`, `sent`, `applied`, `blocked`, `dropped`, `refused`, `damage`, `hitStops`, `timeoutSec` (0) | our counters for hits on `target` (only the fields given) |
| `expectPvpTaken` | `count` (applied + blocked), `blocked`, `dropped`, `damage`, `reason`, `timeoutSec` (0) | our counters for hits on us |
| `expectDummyHurtbox` | `registered` (true), `guard`, `timeoutSec` (0) | the first peer's dummy registered its PvP hurtbox this tick |
| `expectReaction` | `knockback` (`light`, `knockdown`, `none`), `timeoutSec` | our Link plays that reaction (`none`: neither, and no i-frames) |
| `expectHitStop` | `target`, `count` (1), `maxFrames` (3), `none`, `frames` (30), `timeoutSec` (5) | the scene's pause timer runs after `count` hit-stops of ours on `target`, at most `maxFrames` long (start the swing with an async `walk`); `none`: no pause for `frames` |
| `expectHitMarker` | `target`, `result` (`applied`, `blocked`), `text`, `damage`, `drawn`, `timeoutSec` (10) | the latest marker a DAMAGE_RESULT from `target` made (`drawn`: a frame drew markers) |
| `expectLockOn` | `locked` (true), `frames` (30), `timeoutSec` (5) | Z-targeting (hold L with an async `walk`, buttons 0x40) is locked on the first peer's dummy; `locked: false`: not for `frames` |
| `expectPvpToast` | `text`, `count` (1), `max`, `none`, `frames`, `timeoutSec` (10) | knockout toasts with that text ("A beat B", "A knocked out B") |
| `chargeDummy` | `dist` (600), `charge` (true), `timeoutSec` (10) | our ridden Epona, `dist` in front of the first peer's dummy, gallops at it (spurred) until we send it a hit |
| `spawnEnemy` | `name`, `param`, `dx`, `dy`, `dz`, `tag`, `anchor` (`player`, `playerHome`), `setId`, `placed` | an actor in the current room's layer (`placed`: seen by Enemy Count as a stage placement) |
| `expectEnemyHealth` | `tag`, `health`, `max`, `tracked`, `timeoutSec` | until the tagged actor runs with that health, max and scaling |
| `setEnemyHealthPercent` | `value`, `timeoutSec` | once we own the room: its enemy health percent |
| `expectEnemyHealthPercent` | `value`, `timeoutSec` | the percent we apply (the room's while joined, else 100) |
| `setEnemyCountPercent` / `expectEnemyCountPercent` | `value`, `timeoutSec` | the same for the enemy count percent |
| `expectEnemyCount` | `name`, `count`, `maxDist` (from our Link's home), `timeoutSec` | exactly that many running enemies of that actor name in the current room |
| `tagExtra` | `tag`, `of`, `dup` (1), `timeoutSec` | tags extra `dup` of the enemy tagged `of`, once it runs |
| `expectEnemyExtras` | `of`, `count`, `timeoutSec` | exactly `count` extras of `of`, each with setID 0xFFFF, its defeated switch stripped and in the original's layer |
| `enemyDigest` / `expectPeerEnemyDigest` | `maxDist` / `from`, `maxDist`, `timeoutSec` | signals a hash of the room's enemies (name, params, home, extra index) / waits for `from`'s to match ours |
| `setEnemyHealth` / `deleteEnemy` | `tag`, `health` / `tag` | writes health directly (a bite, a get-up) / deletes the actor |
| `expectEnemyGone` / `expectEnemiesGone` | `tag` / `tags`, `maxSpreadTicks`, `timeoutSec` | no longer running (and not scaled) / all gone within that many ticks of each other |
| `killEnemy` | `tag`, `how` (`disappear`, `real`, `fall`, `hz`, `hzReal`), `size`, `type`, `onActor` | a puff and delete (with the zone actor bit after it), a Bokoblin's own death or void fall, or a Tile Worm's death wait after its puff / its own death roll |
| `expectEnemyDeadInPlace` | `tag`, `timeoutSec` | the tagged Tile Worm still runs, dead (its tile only) and out of the ENEMY group |
| `expectZoneActor` | `setId`, `room`, `set`, `timeoutSec` | the zone actor bit |
| `expectEnemySync` | `sent`, `received`, `applied`, `echoes`, `expired`, `timeoutSec` | the enemy-death sync counters given |
| `damageEnemy` | `tag`, `amount` | lowers health as an attributed hit of ours (unlike `setEnemyHealth`), so it is shared |
| `sendEnemyDamageForTest` | `tag`, `dmg`, `pct` | sends an ENEMY_DAMAGE for the tagged enemy with any sender percent (not counted as sent) |
| `expectEnemyDamage` | `sent`, `received`, `applied`, `floored`, `dropped`, `timeoutSec` | the shared-damage counters given (hits, not packets) |
| `forceGroupFail` | `tags` | tagged Shadow Beasts into their downed wait, so the group is condemned |
| `dumpEnemies` | | logs the current room's enemies, their scaling and extra index |

Epona (`StepsHorse.cpp`):

| op | fields | does |
| --- | --- | --- |
| `spawnHorse` | `dist` (250), `side`, `timeoutSec` | our Epona shown in front of our Link (created when the stage has none) |
| `rideHorse` | `ride` (true), `mode` (`force`, `mount`), `timeoutSec` | on (forced or the real mount) or off our horse |
| `expectLocalHorse` | `present` (true), `ridden`, `timeoutSec` | our horse as shown and ridden |
| `setHorseHidden` | `hidden` (true) | our horse's call wait on or off |
| `expectRemoteHorse` | `count`, `name`, `ridden`, `parked`, `maxDist`, `maxSeatDist`, `card`, `tagLine2`, `onScreen`, `tint`, `vanilla`, `hueGain`, `frames`, `timeoutSec` | peers' puppets as the fields say, held for `frames` ticks |
| `expectNoRemoteHorse` | `timeoutSec` | no puppet shown, no horse card drawn |
| `expectRidePrompt` | `target` (`own`, `remote`), `frames` (60), `dist` (110) | GET_ON on our horse / never on the puppet |
| `expectNotRiding` | `frames` (30) | our Link rides nothing |
| `clearEnemies` | `radius` (8000) | deletes the enemies near our Link |
| `horseCamera` | `target` (`remote`, `own`, `both`), `back`, `up`, `side`, `height`, `hold` / `release` | the camera behind our Link looking at the horse; `hold` keeps it (CameraService operator) |

Teleport (`StepsTeleport.cpp`):

| op | fields | does |
| --- | --- | --- |
| `teleportTo` | `target`, `expect` (`arrived`, `local`, `stage`, `refused`, `timeout`, `failed`, or a list), `reason`, `timeoutSec` | asks to teleport to that peer and waits for the result |
| `expectNear` | `target`, `maxDist` (250), `timeoutSec` | our Link within `maxDist` of the peer's reported position |

Team games (`StepsTeamGame.cpp`). A team syncs only between members in game on its team game;
the randomizer seed a player runs is found among the generated seeds on disk:

| op | fields | does |
| --- | --- | --- |
| `setGameIdentity` | `identity` (`kind`, `key`, `name`, `mode`, `permalink`, `seed`, `version`, `inGame`; `{}` restores detection) | announce this game instead of the detected one |
| `fakeRandoResolver` | `items` (`{checkName: itemNo}`), `clear` | an ItemService check resolver answering those checks, as a loaded randomizer seed does |
| `expectLocalGame` | `kind`, `key`, `keyPrefix`, `name`, `verified`, `permalink`, `timeoutSec` | until the detected game matches every field given |
| `expectSyncState` | `state` (`ok`, `pending`, `unverified`, `mismatch`), `timeoutSec` | until the server reports that state for us |
| `expectMemberSync` | `peer`, `state`, `timeoutSec` | the same for a teammate |
| `expectTeamGame` | `team` (default ours), `key`, `keyPrefix`, `kind`, `name`, `permalink`, `noPermalink`, `noKey`, `sameGame`, `owner` (peer name or `self`), `timeoutSec` | until the team's state matches every field given |
| `expectCopyText` | `text`, `timeoutSec` | what Copy Permalink would put on the clipboard (the clipboard is left alone) |
| `claimTeamGame` / `confirmUnverified` | | the Room tab's "Make My Game the Team's Game" / "Sync Unverified Match" |
| `promote` | `peer`, `role` (`room`, `team`) | the Players tab's "Make ... Room Owner" / "Make ... Team Leader", confirmed |
| `expectRoomOwner` | `owner` (peer name or `self`), `timeoutSec` | until the room state names that owner |
| `setTeamColor` / `expectTeamColor` | `rgb`; `team` (default ours), `rgb`, `timeoutSec` | the Room tab's Team Colour (leader only) / until the server reports that team colour |
| `expectTeleportBlocked` | `peer`, `code` (`other-team`, `other-game`, ..., null for none), `timeoutSec` | until teleporting to that peer is refused for that reason |

Story (`StepsStory.cpp`):

| op | fields | does |
| --- | --- | --- |
| `storySelfTest` | | classification, curated moves, segments, spawn form, entrances, STORY_MOVE round trip |
| `dumpStory` | | logs the story state and the room's map events, exits, PLYR points and event tags |
| `expectLayer` | `layer` or `natural` | our layer, or the one our flags pick here |
| `triggerStory` | `via` (`requester`, `walk`, `arrival`), `mapToolId`, `point`, `layer` | starts a story event from its natural trigger |
| `expectStoryMove` | `role` (`sent`, `received`, `none`, `notReceived`), `curated`, `qualHas`, `cached`, `sec`, `timeoutSec` | our sent / received moves |
| `expectPrompt` / `expectNoPrompt` | `kind` (`move`, `inconsistent`) / `sec` | that pop-up shows / none is offered for `sec` |
| `answerPrompt` | `answer` (`follow`, `decline`, `catchup`) | presses the pop-up's button |
| `catchUp` | `expectKind` (`entrance`, `teleport`, `none`), `start` (true) | the Players tab's Catch up to story, confirmed |
| `expectStoryLoad` | `state` (`idle`, `waiting`, `loading`, `arrived`, `failed`), `reason`, `timeoutSec` | the follow / catch-up load |
| `expectConsistent` | `value`, `segment`, `timeoutSec` | whether our position fits our story |
| `expectJoin` | `state` (or a list), `reason`, `holdSec`, `timeoutSec` | the pull-in state; with `holdSec` it must hold |
| `waitStorySettled` | `timeoutSec` | our relocation chain played out and nothing ran for 60 ticks |
| `forcePullInBlocker` | `code` (null: the real checks) | forces the pull-in safety check |
| `setTransformLevel` / `setDarkClear` / `expectTransformLevel` | `level`, `set` | level bits of our save |
| `dismissSaveRequest` | `optional`, `timeoutSec` | answers the capture's save prompt with no |
| `storyPrompts` | `value` | the per-player pop-up setting |

Colouring (`StepsRecolor.cpp`):

| op | fields | does |
| --- | --- | --- |
| `recolorSelfTest` | | the recolour maths |
| `expectDummyRecolor` | `set`, `rgb` or `peerColor` or `vanilla`, `name`, `hueTol`, `minSat`, `c1Max`, `c1Min`, `maxBytes`, `frames`, `timeoutSec` | the dummy wears `set` in that colour, only in the tunic-coloured blocks |
| `markRecolor` / `expectRecolorDelta` | `name`; `minApplies`, `maxApplies`, `minDraws`, `maxGpuGrowthMB`, `maxPrivateGrowthMB` | recolours, draws and process memory since the mark |
| `colorCycle` | `count` (200), `everyTicks` (8), `s`, `v` | changes our colour like the picker |
| `setRupees` | `value`, `timeoutSec` | in Magic Armor, done once its BRK follows |
| `recolorCamera` | `target` (`dummy`, `self`), `dist`, `height`, `up`, `side` | our camera close on the dummy, for captures |

Hook cost (`StepsPerf.cpp`):

| op | fields | does |
| --- | --- | --- |
| `measureHookCost` | `frames` (300), `maxMs` (0.1) | calls of our every-tick hook targets x one host dispatch, per tick |

UI: colour and window (`StepsColor.cpp`). The picker is the host's colour control, driven by key
presses posted to the game's own window (so it needs no focus) and checked through the setting:

| op | fields | does |
| --- | --- | --- |
| `colorMathSelfTest` | `chunk` | the colour maths over all 2^24 colours |
| `setColor` / `expectColor` | `rgb` (`"#RRGGBB"`) or `r`, `g`, `b`; `timeoutSec` | writes / waits for the colour setting |
| `expectColorHsv` | `h`, `hTol`, `hMin`, `hMax`, `s`, `v`, `tol`, `timeoutSec` | the setting's colour as HSV |
| `expectPeerColor` / `expectSelfRow` | `name`, `rgb`; `saveLoaded`, `stage` | a peer's / our own client entry |
| `expectDummyColor` | `name`, `rgb`, `timeoutSec` | the peer's dummy tints (and, with recolouring, its textures) in that colour |
| `resetColorPushes` / `expectColorPushes` | `min`, `max` | colour updates we sent since the reset |
| `showWindow` / `hideWindow` | `tab` (0 Connection, 1 Room, 2 Players) | the Twili-Together window |
| `openColorPicker` / `closeColorPicker` | | the window's colour control pressed (the square has focus) / a cancel |
| `pickerKey` | `key` (`left`, `right`, `up`, `down`, `confirm`, `cancel`, `next`, `prev`), `count`, `holdMs` | taps, one per tick; `holdMs` holds one, repeating every tick after 0.32 s |
| `resizeWindow` | `width`, `height` | the game window's client size |
| `mark` | `msg` | logged as a warning, which flushes the log: a trigger for window captures |

Map cursors (`StepsMapCursor.cpp`). Targets are peer names or `#<client id>`:

| op | fields | does |
| --- | --- | --- |
| `mapCursorSelfTest` | | projection, facing, outline, pins, colours |
| `showMinimap` | `minAlpha`, `timeoutSec` | until the minimap draws on screen |
| `injectMapCursorClients` / `clearMapCursorClients` | `clients` (`id`, `color`, `dx`, `dz`, `dy`, `angle`), `unit` (`cm`, `texel`) | extra markers around our map position |
| `expectMapCursor` | `surface` (`minimap`, `dmap`), `target(s)`, `color(s)`, `pinned`, `floorDelta`, `facing`, `count`, `holdTicks`, `maxAgeTicks`, `timeoutSec` | a fresh frame with those markers in exactly those colours |
| `expectNoMapCursor` | `surface`, `target`, `reason`, `holdTicks` (30), `timeoutSec` | no marker, for that reason: a frame reason (`locationsOff`, `cutscene`, `notConnected`, `alphaZero`), a client reason (`noSave`, `noUpdate`, `otherStage`, `otherLayer`, `otherFloor`, `badPose`) or `absent` |
| `checkMapCursorTransform` | `target`, `tolPx` | anchors and facings against the offscreen pass's matrices |
| `expectMapPaletteClean` | `groups`, `holdTicks` | the dungeon map palette is untouched |
| `dumpMapCursors` | `surface` | logs the frame and `MAPCURSOR_PROBE` screen fractions |
| `openPauseMap` | `timeoutSec` | the dungeon map, as the map button opens it (a B press closes it) |

Item toasts (`StepsItemToast.cpp`), checked through the toast history:

| op | fields | does |
| --- | --- | --- |
| `itemToastSelfTest` | `items` | icons and names from the game files |
| `itemToastOption` | `name` (`item_toasts`, `item_toasts_own`), `value` | |
| `clearItemToasts` | | forgets the history and what waits |
| `testItemToast` | `item`, `name`, `r`, `g`, `b`, `count`, `saveTbl` | a live toast as if a teammate picked the item up |
| `expectItemToast` / `expectNoItemToast` | `kind` (`item`, `own`, `summary`, `merged`), `from`, `item`, `textContains`, `minCount`, `fromPacket`, `hold`, `pushed`, `rendered`, `timeoutSec` / `forSec` | a matching toast (`rendered`: pushed with that item's `item://` icon) |
| `expectItemToastCount` / `expectItemToastItems` | the same fields and `count` or `min`/`max`; `from`, `count` | how many toasts / items counted |
| `dumpItemToasts` | | logs the history |

Name tags (`StepsNameTags.cpp`):

| op | fields | does |
| --- | --- | --- |
| `expectNameTag` | `name`, `shown`, `gate`, `line2`, `rgb`, `holdTicks`, `timeoutSec` | the peer's tag in the last presented frame (or none, for `gate`); `rgb` is its frame colour |
| `viewPeer` | `name`, `yaw`, `beyond` | moves our player past the peer's dummy, seen from our camera, so the peer shows `yaw` degrees off centre |

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
- `files`: `{path: text}` written into every instance's user dir before launch (an instance's own
  `files` too), e.g. randomizer seeds under `mod_data/`;
- `expectLog` / `rejectLog`: regexes (or `{pattern, instance}`) every instance log must / must not
  match. Every scenario also rejects a HookService "was inlined into callers" warning for our mod
  (the hook would miss the inlined calls) and a failure of our mod;
- `instances`: one object per instance with `name`, optional `start`, optional `cvars`, optional
  `launchDelayMs` (delay before the *next* instance is launched), and `steps`. `script: false` with
  `runSec` runs the game without a script for that long and judges only its log.

The helpers in `lib.js`: `meetIn(stage, other)` waits for both players in a stage, checks each
other's dummy and barriers; `barrier(name, other)` synchronises two instances; `tt(name, value)`
builds a mod-setting `--cvar`.
