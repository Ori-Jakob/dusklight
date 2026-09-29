# Twili-Together protocol

Protocol version 5. Clients never talk to each other directly: every packet goes to the relay
(`tools/server/server.js`), which decides who gets it. The relay keeps connection metadata and a
few caches but never simulates the game.

## Transport

Every message is one JSON object with a `type` string.

| URL | Transport | Framing |
| --- | --- | --- |
| `wss://host[:port][/path]` | WebSocket over TLS | one JSON text message per packet |
| `ws://localhost:port` | plain WebSocket | same; the game allows plain `ws://` only to localhost |
| `tcp://host:port` | plain TCP | 4-byte little-endian length N (1 to 1048576), then N bytes of UTF-8 JSON |

WebSocket clients may offer the subprotocol `twili-together.5`. The relay pings WebSocket
connections every 10 s. TCP clients send `{"type":"KEEPALIVE"}` every 5 s, the relay answers it,
and either side drops a TCP peer that stays silent for 30 s. KEEPALIVE is never relayed.

The client polls the connection from the game thread once per tick. After an unrequested drop it
reconnects on its own (1, 2, 4, 8, 15, then every 30 s) with the same session key, so the relay
treats it as the same player coming back. A first connect that fails is not retried.

## Joining

The client's first packet is the handshake:

```json
{
  "type": "HANDSHAKE",
  "app": "twili-together",
  "protocolVersion": 5,
  "modVersion": "0.5.0",
  "layout": "1f0c9a2b44e7d310",
  "transport": "ws",
  "name": "Ori",
  "teamId": "red",
  "roomId": "friday",
  "sessionKey": "k3j2...",
  "color": {"r": 255, "g": 64, "b": 64},
  "game": {"inGame": true, "kind": "vanilla", "key": "vanilla", "display": {"name": "", "mode": ""}}
}
```

- `layout` is a hash of the save structures (16 hex digits). World data only flows between
  clients with the same layout, since the world state is sent as raw save bytes.
- `sessionKey` is random per game process. It lets a reconnect replace the old connection and
  keeps the relay from replaying a player's own packets back to them.
- `game` says what the player runs; see [Team games](#team-games).

A handshake with another `app` or a `protocolVersion` below 5 gets `SERVER_MESSAGE` and
`DISABLE_CLIENT` (both with a `message`) and the connection is closed. Packets sent before the
handshake are ignored, and so is a second handshake.

On success the relay answers with `ALL_CLIENT_STATE`:

```json
{"type": "ALL_CLIENT_STATE", "clients": [ ...client states... ], "roomState": { ... }}
```

then one `TEAM_STATE` per team in the room. Everyone else in the room gets an
`UPDATE_CLIENT_STATE` with the new player.

A client state holds `clientId`, `name`, `teamId`, `color` (the player's own), `displayColor`
(what everyone shows them in: the team colour on a named team, otherwise their own), `modVersion`,
`layout`, `protocolVersion`, `online`, `isSaveLoaded`, `stageName`, `layerNo`, `roomNo`,
`saveTblNo`, `horseName`, `horsePlace` and `self` (true in your own entry).

## Rooms and owners

Players with the same `roomId` share a room (empty is the public room). The first player in a room
owns it. When the owner leaves, the player who has been connected longest takes over. The owner
can hand the room over with `SET_ROOM_OWNER {targetClientId}`.

`UPDATE_ROOM_STATE {state}` is accepted only from the owner and goes to the whole room. Unknown
keys and wrong types are ignored, numbers are clamped. The state:

| Key | Default | Meaning |
| --- | --- | --- |
| `syncWorldState` | true | teams share flags, items and save progress |
| `shareWoodenShield` | true | wooden shields sync like other items |
| `syncNPCs` | false | enemy deaths sync within a stage |
| `syncEnemyDamage` | true | with `syncNPCs`: teammates' hits on an enemy weaken our copy too |
| `cutsceneSync` | true | story cutscenes pull in teammates in the same room |
| `hidePlayersInCutscene` | false | hide other players during your cutscenes |
| `pvpMode`, `pvpFriendlyFire`, `pvpLethal` | false | PvP rules |
| `showLocationsMode` | true | map markers |
| `teleportMode` | false | teleport to a player |
| `teleportAcrossTeams` | false | teleport to other teams' players |
| `enemyHealthMultiplier` | 100 | percent, 100 to 500 |
| `enemyCountMultiplier` | 100 | percent, 100 to 300: extra copies of stage-placed regular enemies, made alike by every client when an area loads |

The relay adds `ownerClientId`. An empty room keeps its settings and caches for 6 hours.

## Team games

Players on the same `teamId` are a team. Each team has a leader (the longest-connected member, or
whoever the leader promoted with `SET_TEAM_OWNER {targetClientId}`) and a team game: what the
team plays.

A client's game identity is `{inGame, kind, key, display: {name, mode}, share, allowUnverified}`,
sent in the handshake and again with `GAME_IDENTITY` whenever it changes. `key` is one of:

- `vanilla`
- `rando/<format>/<digest>`: a randomizer seed the client found among its generated seeds and
  matched against the running randomizer. `share` then carries `{permalink, seed, version}` from
  the seed's anti-spoiler log.
- `rando-probe/<fingerprint>`: a randomizer game the client could only identify by probing a few
  item checks. It syncs only after the player confirms it (`allowUnverified`).
- `mode/<game mode id>` for other game modes.

The first member in game sets the team game. After that only the leader changes it: if teammates
still play the old game, the leader gets `TEAM_GAME_CONFLICT` and must answer with
`CLAIM_TEAM_GAME` to switch.

`TEAM_STATE {teamId, ownerClientId, game, color, members: [{clientId, sync, ...}]}` goes to the
room on every change. A member's `sync` is `ok` (in game on the team game), `unverified`,
`pending` (not in game yet) or `mismatch`. Only members of that team get the game keys and the
permalink. Team traffic (everything under World sync, story and kills) only flows between members
whose state is `ok`.

A named team also has a colour, a default derived from its id until the leader sends
`SET_TEAM_COLOR {color}`. Members show in it (`displayColor`).

## Packets by routing

The relay stamps every relayed packet with the sender's `clientId`.

### Room-wide

| Packet | Sent by | Notes |
| --- | --- | --- |
| `UPDATE_CLIENT_STATE` | client | stage, layer, room, save table and whether a save is loaded; or only `color`; or only `horseName` / `horsePlace`. The relay stores it and forwards its own view |
| `UPDATE_ROOM_STATE` | owner | see above |
| `AUTOTEST_SIGNAL` | client | test barrier between instances |

### Presence (same room, stage, layer and protocol)

| Packet | Notes |
| --- | --- |
| `PLAYER_UPDATE` | the player's pose, once per game tick while someone is there to see it |
| `PLAYER_SFX` | `{soundId, kind, mapInfo, sq}`: a Link (kinds 0-6) or Midna (7 voice, 8 sound) sound to play on the puppet when it shows pose tick `sq` |

A `PLAYER_UPDATE` carries `seq` (the sender's tick), `ep` (an epoch that changes on a scene
change), `fl` (presence flags: wolf, in cutscene, shield up, ...) and short keys with integer
arrays. A keyframe (`k: 1`, at least every 30 ticks and whenever the audience changes) has every
field; the packets in between only carry what changed. Receivers buffer about 2 s of samples and
play them back a little behind the newest one, so network jitter does not show. The main groups:

| Keys | What |
| --- | --- |
| `p`, `a`, `sa`, `ba`, `tw` | position, angles, body angle, twist |
| `la`, `ua`, `lf`, `uf`, `lr`, `ur` | lower and upper body animations: clips, frames, blend ratios |
| `eq`, `hi`, `ij`, `ib`, `pr` | clothes, sword, shield and item in hand, hand shapes, item clip, loaded ammo |
| `md`, `mf`, `ma`, `me`, `mw` | Midna on the wolf's back, her eyes, and her place when she is off it |
| `tf` | a running transformation |
| `sx`, `sf` | status effects: frozen, burning, electrocuted, shield burning |
| `wx` | wolf attacks: spin, Midna's dome, lock-on jumps |
| `x0`..`x7`, `xe`, `hk`, `bc`, `ls`, `hx`, `fr` | items in the world (arrows, bombs, boomerang, ...), one-shot events, clawshot, ball and chain, level sounds, bottle, fishing rod |
| `ox`, `op`, `os`, `oa`, `of`, `ow`, `ok`, `or`, `oz` | the player's Epona, and Zelda behind its rider |

Presence is best effort: when the send queue is full, deltas are dropped and the next keyframe
repairs the stream.

### World sync (same room, team, team game and save layout)

| Packet | Fields | Notes |
| --- | --- | --- |
| `SET_FLAG` / `UNSET_FLAG` | `category`, `flagNo`, `roomNo`, `stageName`, `saveTblNo` | switches, chests, item flags |
| `SET_EVENT_BIT` / `UNSET_EVENT_BIT` | `no` | story event bits; a few local-only bits are never sent |
| `GIVE_ITEM` | `itemNo` | the receiver grants it through the game's item service, so a randomizer applies its own logic |
| `UPDATE_DUNGEON_ITEMS` | `saveTblNo`, `keyDelta`, `dungeonItemBits` | small keys, map, compass, boss key |
| `LIGHT_DROP` | `area`, `tbl`, `tbox` | a tear of light picked up. Unless that tear's TBOX bit is already set, the receiver sets it, adds one to the area's count (at most 16) and removes the tear from its screen; the bit keeps replays from counting twice |
| `UPDATE_WORLD_STATE` | `fmt` (`dsv1`), `save`, `dan`, `danStageNo`, `saveTblNo`, optional `targetClientId` | the whole save as base64; merged, never simply copied |
| `REQUEST_WORLD_STATE` | `catchUp` | asks the team for its state |

Each of these carries `teamId`, `layout`, `protocolVersion` and `addToQueue`. The relay adds
`senderSessionKey`, `senderName` and `senderColor`.

Catch-up: the relay keeps, per team, team game and layout, the last untargeted
`UPDATE_WORLD_STATE` and every `addToQueue` packet since (numbered with `queueEpoch` and
`queueSeq`, so a reconnecting client skips what it already applied). A joining client sends
`REQUEST_WORLD_STATE`. Teammates who are online and caught up answer with a targeted
`UPDATE_WORLD_STATE`; the cache answers only when none can. With `catchUp: true` the relay also
replays the queue and the team's last 16 story moves, oldest first. A fresh untargeted world state
replaces the cache and empties the queue.

### Kills and story (teammates only)

| Packet | Goes to | Notes |
| --- | --- | --- |
| `ENEMY_DEFEATED` | teammates in the same stage and layer | `{v: 2, stageName, layerNo, kills: [...]}`; a kill is the enemy's spawn key `{roomNo, procName, params, setId, home, dup}` (`dup` 0, or 1 to 4 for an Enemy Count extra) plus `fxSize`, `fxType`, `zoneActor`. Never cached, a replay would delete an enemy that respawned |
| `ENEMY_DAMAGE` | teammates in the same stage and layer | `{v: 1, quiet: true, stageName, layerNo, hits: [...]}`; a hit is a spawn key as in `ENEMY_DEFEATED` plus `dmg` (1 to 30000), `pct` (the sender's health percent for that enemy, 100 to 500) and `hpAfter` (the sender's health left at 100%, or -1). The receiver lowers its copy's health by `dmg` converted to its own percent, and further to `hpAfter` if that is lower (a hit it missed), never below 2 (11 for Wooden Puppets and Skulltulas): deaths stay with `ENEMY_DEFEATED`. Never cached |
| `STORY_EVENT` | teammates in the same stage and layer | `ph` = `start`, `joined` or `end`; a story cutscene teammates in the same room may watch too. With `req: "npc"` it carries `npc: {prof, room, params, set, home, k, sd}`: the NPC's spawn key, the entry of its event table it ordered and a digest of the story flags; a teammate joins through its own copy of that NPC, only for allowlisted events and never in a randomizer game |
| `STORY_MOVE` | the whole team | `ph: "arrive"`: a story event moved the sender to another stage. Teammates get a prompt to follow. The last 16 per team are replayed on catch-up, oldest first. Besides `from`, `to`, `event`, `qual` and `hops` it carries `hl` (each arrival: `at`, the arrival demo's `m`, `sw`, `name`), `bits` (event bits the move set), `key`, `th` (it ended on a cutscene or battle layer) and `boss` |

### Teleport (one target, while `teleportMode` is on)

1. The requester sends `REQUEST_TELEPORT {targetClientId, requestId}`.
2. The relay forwards it to the target, or answers itself with `TELEPORT_TO {ok: false, reason,
   fromServer: true}`. Reasons: `rate-limited` (more than one request a second), `disabled`,
   `offline`, `not-in-game`, `other-team` (unless `teleportAcrossTeams`), `other-game` (always
   between different team games).
3. The target answers `TELEPORT_TO {targetClientId, requestId, ok, stageName, roomNo, layerNo,
   pos, angleY, ageMs}` with the last spot it stood on solid ground, or `ok: false` and a reason.
   The relay checks the fields and passes it back.

### PvP (one target, while `pvpMode` is on)

The attacker's game decides that a hit landed and sends `DAMAGE_PLAYER {targetClientId, hitId,
kind, damage, spl, dirY, blocked, viewSeq}` (`spl` 0 light, 1 knockdown; `damage` in quarter
hearts, at most 4). The relay forwards it only when both players are in game in the same stage
and layer and the team rules allow it (friendly fire), at most every 200 ms per attacker and
victim; otherwise it answers with `DAMAGE_RESULT {result: "refused", reason}`. The victim
checks again, plays the hit through its own damage code and answers `DAMAGE_RESULT {targetClientId,
hitId, result, reason, damage, knockout}` with `result` = `applied`, `blocked` or `dropped`. The
relay passes the result back to the attacker.

`kind`: 0 sword, 1 wolf, 2 arrow, 3 bomb, 4 slingshot, 5 boomerang, 6 clawshot, 7 shield bash,
8 Ball and Chain, 9 stomp, 10 spinner, 11 horse. The victim clamps `damage` and `spl` to what its
own table allows for the kind (README, PvP); an unknown kind from a newer client is clamped to
the last one it knows.

`knockout` is only set on an applied hit: `ko` when it took the victim's last heart (`pvpLethal`),
`floor` when it took them down to the one-heart floor. The relay then sends `PVP_KNOCKOUT
{attackerClientId, victimClientId, knockout}` to the whole room, which shows it as a toast.

### Server to client only

`ALL_CLIENT_STATE`, `TEAM_STATE`, `TEAM_GAME_CONFLICT`, `SERVER_MESSAGE`, `DISABLE_CLIENT` and
`PVP_KNOCKOUT`. The relay drops them when a client sends them. `DISABLE_CLIENT` disconnects the
client without an automatic reconnect.
