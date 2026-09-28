# Twili-Together relay server

Relays packets between Twili-Together clients: rooms, teams, presence, world-state catch-up,
teleport and PvP. It never simulates the game. The relay rules are summarized at the top of
`server.js`.

```
npm install                          # once; ws is the only dependency
node server.js [wsPort] [tcpPort]
npm test
```

With no arguments the WebSocket listener uses port 3000 and the TCP listener port 3001.
`node server.js 0` picks free ports for both, and `off` as the TCP port runs WebSocket only. Once
the listeners are up the server prints, in this order:

```
[2026-09-28T14:57:59.317Z] listening on port 3000
[2026-09-28T14:57:59.318Z] tcp relay on port 3001
```

The autotest (`../autotest`) starts `node server.js 0` and reads the port from the first line.

## Environment

- `TT_PORT`: WebSocket port when no argument is given (3000).
- `TT_TCP_PORT`: TCP port when no second argument is given (3001, or 0 when the WebSocket port is
  0). `off` disables the TCP listener.
- `TT_TLS_CERT`, `TT_TLS_KEY`: PEM certificate and key. With both set, the WebSocket port serves
  `wss://` itself (see below).
- `TT_PING_MS`: WebSocket ping interval. A connection that has not answered the previous ping is
  dropped (10000).
- `TT_TCP_TIMEOUT_MS`: a TCP connection that sends nothing for this long is dropped (30000).
- `TT_ROOM_TTL_SEC`: how long an empty room keeps its settings and caches (21600).
- `TT_TELEPORT_INTERVAL_MS`: minimum time between teleport requests from one client (1000).
- `TT_DAMAGE_INTERVAL_MS`: minimum time between forwarded PvP hits per attacker and victim (200).
- `TT_LOG_PATH`: log file, appended across runs (default `twili-together-server.log` next to
  `server.js`).
- `TT_TEST_JITTER_MS`: test only. Delays each packet sent to a client by up to this many ms,
  without reordering (0).

A numeric variable that is not a positive number falls back to its default.

## Connecting from the game

The game accepts `ws://` only for localhost, and `wss://` only with a publicly trusted
certificate (a self-signed one is rejected). That leaves three setups:

- Same machine: `ws://localhost:3000`.
- LAN: `tcp://<host>:3001`. Plain TCP, no encryption.
- Internet: `wss://` through a TLS-terminating reverse proxy that holds a real certificate.
  With Caddy, which obtains the certificate itself, this Caddyfile is enough:

  ```
  twili.example.org {
      reverse_proxy 127.0.0.1:3000
  }
  ```

  Players connect to `wss://twili.example.org`. The relay still listens on all interfaces, so
  keep port 3000 closed in the firewall.

Without a proxy, point `TT_TLS_CERT` and `TT_TLS_KEY` at a trusted certificate for the server's
domain (for example from certbot) and players connect to `wss://<domain>:3000`. The files are
read once at startup, so restart the server after renewing. The TCP port is never encrypted.

## Protocol notes

A client joins with a `HANDSHAKE` carrying `app: "twili-together"` and an integer
`protocolVersion` of 5 or higher, plus `modVersion`, `layout`, `transport`, `name`, `teamId`,
`roomId`, `sessionKey` and `color`. Any other handshake is answered with `SERVER_MESSAGE` and
`DISABLE_CLIENT`, both carrying the reason, and the connection is closed (WebSocket close code
4000). Newer protocol versions join, but presence, kills, story events, world-state requests
and PvP only pass between clients on the same version.

The handshake may also carry `game`, the client's game identity; `GAME_IDENTITY` updates it:
`{inGame, kind, key, display: {name, mode}, share: {permalink, seed, version}, allowUnverified}`.
`key` is `vanilla`, `rando/<format>/<digest>` (a randomizer seed found on disk and checked
against the running randomizer), `rando-probe/<fingerprint>` (checked only by probing item
checks: syncs once the player confirmed it, `allowUnverified`) or `mode/<game mode id>`. Each
team has a leader and a team game, and team traffic only flows between members in game on it.
`TEAM_STATE {teamId, ownerClientId, game, members: [{clientId, sync, ...}]}` goes to the room
on every change (and to a joiner for every team); only the team's own members get the keys and
`game.share`. `TEAM_GAME_CONFLICT` asks a team leader who loaded another game while teammates
still play the team's; `CLAIM_TEAM_GAME` switches the team to the leader's game anyway.

The room owner and each team leader are the longest-connected member of the room or team; when
one leaves, the next by connection order takes over (the team keeps its game). The owner may hand
the room to any member with `SET_ROOM_OWNER {targetClientId}`, a leader the team to a teammate
with `SET_TEAM_OWNER {targetClientId}`. A named team has a colour: a default picked from its id, until
its leader sets one with `SET_TEAM_COLOR {color}` (kept through leader changes). Client state
carries each player's own `color` and the `displayColor` everyone shows them in: the team colour
for team members, their own otherwise; world packets are stamped with it (`senderColor`).
Teleport requests across teams are refused (`other-team`)
unless the room's `teleportAcrossTeams` is on, and always between different team games
(`other-game`).

WebSocket clients may offer the subprotocol `twili-together.5`. The server selects it when it is
offered and accepts connections that offer none or only unknown ones.

On TCP, every message in either direction is a 4-byte little-endian length N (1 to 1048576)
followed by N bytes of UTF-8 JSON. A length outside that range closes the connection, and a
packet too large for a frame is not sent to a TCP client (the server logs it). Clients send
`{"type":"KEEPALIVE"}` every 5 s. The server answers it on TCP and ignores it on WebSocket,
where ping/pong does the same job; it is never logged or relayed.
