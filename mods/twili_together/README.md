# Twili-Together

Online co-op for Dusklight. Play Twilight Princess together: you see the other players in your
world as full Link models, with their clothes, items, wolf form, Midna and Epona, and hear their
sounds. Players on the same team share their progress: flags, items, keys, heart pieces and story
events. Optional extras are teleporting to a teammate, synced enemy deaths, tougher enemies and
PvP.

It is a native mod for Dusklight's mod framework (mod id `dev.n0ted.twili_together`) and works
alongside the randomizer mod.

## Install

You need a Dusklight build with mod support and your own game disc image, like for playing alone.

1. Get `twili_together.dusk` (a release, or build it: see [Building](#building)).
2. Copy it into your mods folder:
   - Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
   - Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
   - macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`
3. Start the game. The mod is enabled by default; the Mods window lists it and has a small panel
   with the connection status and an "Open Twili-Together" button.

Everyone in a session should run the same Dusklight version and the same Twili-Together version.

## Connecting

Someone has to run a relay server first; see [docs/hosting.md](docs/hosting.md). It takes a
minute on any computer with Node.js.

1. Open **Twili-Together** in the menu bar. The window has three tabs: Connection, Room and Players.
2. On **Connection**, set your name, pick a colour, and enter the **Server URL**:
   - `ws://localhost:3000` when the relay runs on your own computer,
   - `tcp://192.168.1.20:3001` (the host's address) on a LAN,
   - `wss://twili.example.org` for a relay on the internet.
3. Optionally fill in **Room** and **Team** (see below), then press **Connect**.
4. Load a save or start a new file. Other players show up once you are in the same area.

After a lost connection the mod reconnects by itself with growing pauses, unless you turn
**Reconnect Automatically** off. Name, team and room changes take effect on the next connect.

## Rooms, teams and leaders

- **Room**: only players with the same room code on the same server see each other. An empty room
  code is the server's public room.
- **Team**: players with the same team name share progress while they play the same game (see
  the randomizer section). Players on other teams are still visible but keep their own progress.
  Players who leave Team empty count as one group and share progress with each other.
- **Room owner**: the first player in a room. The owner's Room tab settings apply to everybody.
  The owner can hand the room to someone else from the Players tab. When the owner leaves, the
  player who has been connected longest takes over.
- **Team leader**: works the same way within a team. The leader decides the team's game and
  colour.
- **Team colour**: every named team has a colour, picked from its name until the leader chooses
  one under Team Colour on the Room tab. Teammates show in it everywhere: tunic, name tag, map
  marker and player list. Players without a team show in their own colour.

## Playing a randomizer seed together

World sync assumes everyone on a team plays the exact same seed, so a team only syncs between
members who run the team's game. The first member in game sets it; after that only the leader
can switch it ("Make My Game the Team's Game" on the Room tab).

To get everyone on the leader's seed:

1. The leader generates a seed in the randomizer and starts it, then connects.
2. Teammates connect with the same room and team. The Room tab shows the team's seed, its
   permalink and these steps:
3. Press **Copy Permalink**.
4. Not in the randomizer yet? Reset from the menu bar and pick Randomizer in the launcher.
5. In the Randomizer tab, under Seed Management, press **Paste Permalink** (it replaces your
   randomizer settings), then **Generate Seed**.
6. On file select, choose an empty file, pick the seed with the name the Room tab shows under
   Play, and press **Start Randomizer**.

The mod recognizes the seed by comparing it with the seeds generated on your computer, so each
player needs to have generated it. If it can only compare a few item checks (for example the seed
was generated elsewhere), the match counts as unverified: press **Sync Unverified Match** to sync
anyway. A vanilla save never syncs with a randomizer team and the other way round; the player can
still play and is still visible.

## Settings

Personal settings, on the Connection tab:

| Setting | Default | |
| --- | --- | --- |
| Name, Team, Room | empty | as above |
| Colour | white | your colour, when your team has none |
| Item Pickups | on | a toast with the item's icon when a teammate picks something up; items synced while you were away are summarized |
| Your Own Pickups | off | toasts for your own pickups too |
| Story Prompts | on | ask whether to follow when a teammate's story event moves them |
| Server URL | empty | see Connecting |
| Reconnect Automatically | on | |
| Always Sync Unverified Randomizer Games | off | (Room tab) skip the Sync Unverified Match question |

Room settings, on the Room tab. Everyone sets their own values, but only the room owner's apply;
the others see the owner's values, greyed out.

| Setting | Default | |
| --- | --- | --- |
| Sync World State | on | teams share flags, items, keys and save progress |
| Share Wooden Shields | on | off: a burnt wooden shield stays burnt for everyone who lost it |
| Sync Enemy Deaths | off | a regular enemy a teammate defeats near you disappears for you too (not bosses; the drop goes to whoever defeated it) |
| Share Enemy Damage | on | with Sync Enemy Deaths: teammates wear down the same enemies, so your hits weaken their copy too |
| Cutscene Sync | on | teammates standing safely in the same room watch a story cutscene together |
| Hide Players in Cutscenes | off | hide other players, tags and markers during your cutscenes |
| PvP Mode | off | players can hurt each other |
| Friendly Fire | off | PvP within a team too |
| Lethal PvP | off | off: PvP hits never take you below one heart |
| Show Locations | on | other players on the minimap and the dungeon map |
| Teleport to Player | off | teleport buttons on the Players tab |
| Teleport Across Teams | off | teleport to other teams' players (never between different games) |
| Enemy Health | 100% | health of regular enemies, 100% to 500% |

All settings are stored in the game's `config.json` as `mod.dev_n0ted_twili__together.<name>`
(for example `server_url`, `team_id`, `sync_world_state`) and can be set from the command line
with `--cvar mod.dev_n0ted_twili__together.server_url=wss://twili.example.org`.

## The Players tab

Lists everyone in the room, grouped by team, with where they are and whether they sync with you.
From there you can teleport to a player (when the room allows it), hand over the room or your
team, and use **Catch up to story**: when a teammate's story event moved them (for example
Link's capture into Hyrule Castle) and you were not there, this takes you to the matching place
with the matching form.

## Known limitations

- Internet play needs a relay behind a real TLS certificate; the game refuses self-signed ones.
  LAN play over `tcp://` is unencrypted.
- World sync needs the same Dusklight build on the whole team: the save is exchanged as raw data,
  and a teammate with a different save layout gets a warning and no world data.
- A randomizer team has to run the same seed, generated on every player's computer (or confirmed
  as an unverified match).
- Story sync knows a handful of story moves (the capture, the escape from the castle, the
  twilight gate, the Faron Spring). Other cutscenes only play for the player who triggers them.
- While the game is paused by "Pause on Focus Lost", the mod does not run: a `tcp://`
  connection drops after 30 s and reconnects when you come back.
- Name tags are drawn under the HUD.
- The ImGui state-share debug tool is not supported while connected.
- Reloading or disabling the mod from the Mods window while connected is not covered by the
  automated tests.
- Only Windows has been tested.
- There is no public relay; someone in your group hosts one.

## Known issues

Two rare crashes come from races in Dusklight itself, not in the mod:

- **Crash when quitting.** JSystem's static loader lists (open DVD files, mounted archives) are
  changed from the main thread and the audio/DVD loader thread without a lock. The damage stays
  silent until the lists are destroyed at exit, which then crashes in `JSUPtrList::~JSUPtrList`.
  It showed up in about 1 of 100 to 150 test runs.
- **Crash at boot in Dawn.** aurora creates and releases Dawn bind groups on two threads without
  a lock, which can crash inside `CreateBindGroup` in `webgpu_dawn.dll` at boot or during a stage
  load. It is rare (3 in about 1000 game launches) and reproduces with all mods disabled.

## Building

The mod builds with the rest of Dusklight (it is listed in the root `CMakeLists.txt`), using the
`windows-clang-relwithdebinfo` preset or any preset with mod support. The build puts
`twili_together.dusk` into `build/<preset>/mods/`, where the game finds it next to the exe.
Its `CMakeLists.txt` also has the standalone configure the other mods in `mods/` use (not tested
yet for this mod).

`TWILI_ENABLE_AUTOTEST` (on by default) compiles in the scripted test driver. It stays dormant
unless the `autotest_script` setting is given, so the tested binary is the shipped one.

## More

- [docs/hosting.md](docs/hosting.md): running the relay, LAN and internet setups.
- [docs/protocol.md](docs/protocol.md): packets and how the relay routes them.
- [tools/autotest/README.md](tools/autotest/README.md): the automated two-instance tests.
- [tools/server/README.md](tools/server/README.md): relay server reference.
