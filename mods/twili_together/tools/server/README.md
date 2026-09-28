# Twili-Together relay server

Relays packets between Twili-Together clients: rooms, teams, presence, world-state catch-up,
teleport and PvP. It never simulates the game.

```
npm install                          # once; ws is the only dependency
node server.js [wsPort] [tcpPort]    # defaults 3000 and 3001; "off" disables TCP
npm test                             # protocol tests, no game needed
```

`node server.js 0` picks free ports for both listeners. Once they are up the server prints, in this
order:

```
[2026-09-28T14:57:59.317Z] listening on port 3000
[2026-09-28T14:57:59.318Z] tcp relay on port 3001
```

The autotest (`../autotest`) starts `node server.js 0` and reads both ports from these lines.

- Setups (same machine, LAN, internet behind a TLS proxy), settings (`TT_*` environment
  variables) and a systemd unit: [../../docs/hosting.md](../../docs/hosting.md).
- Packets, routing and caches: [../../docs/protocol.md](../../docs/protocol.md).
