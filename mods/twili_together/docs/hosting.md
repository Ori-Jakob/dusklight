# Hosting a Twili-Together relay

Players connect to a small relay server, not to each other. It is a Node.js script in
`tools/server/` with one dependency (`ws`). It uses very little CPU and memory; a Raspberry Pi or
the cheapest VPS is plenty for a few rooms.

## Running it

You need Node.js 18 or newer (tested with 24).

```
cd mods/twili_together/tools/server
npm install
node server.js
```

It listens on two ports:

| Port | Protocol | Used for |
| --- | --- | --- |
| 3000 | WebSocket (`ws://`, or `wss://` with built-in TLS) | same machine, or behind a TLS proxy for internet play |
| 3001 | length-prefixed JSON over TCP (`tcp://`) | LAN play |

`node server.js 4000 4001` picks other ports, `node server.js 3000 off` turns the TCP listener off
and `node server.js 0` uses any free ports. Once both listeners are up it prints:

```
[2026-09-28T14:57:59.317Z] listening on port 3000
[2026-09-28T14:57:59.318Z] tcp relay on port 3001
```

Stop it with Ctrl+C. Rooms live in memory only, so a restart empties them. Players reconnect on
their own and their games send their progress again.

## Settings

All optional, as environment variables:

| Variable | Default | Meaning |
| --- | --- | --- |
| `TT_PORT` | 3000 | WebSocket port when none is given on the command line |
| `TT_TCP_PORT` | 3001 | TCP port when none is given; `off` disables it |
| `TT_TLS_CERT`, `TT_TLS_KEY` | unset | PEM certificate and key; with both set the WebSocket port serves `wss://` itself |
| `TT_PING_MS` | 10000 | WebSocket ping interval; a client that misses a ping is dropped |
| `TT_TCP_TIMEOUT_MS` | 30000 | a TCP client that sends nothing this long is dropped |
| `TT_ROOM_TTL_SEC` | 21600 | how long an empty room keeps its settings and world cache |
| `TT_TELEPORT_INTERVAL_MS` | 1000 | minimum time between teleport requests from one player |
| `TT_DAMAGE_INTERVAL_MS` | 200 | minimum time between PvP hits from one player on another |
| `TT_LOG_PATH` | `twili-together-server.log` next to `server.js` | log file, appended across runs |
| `TT_TEST_JITTER_MS` | 0 | tests only: delays packets to each client by up to this much |

## Which address players use

The game only allows a plain `ws://` connection to its own machine, and it checks `wss://`
certificates like a browser does: a self-signed certificate is refused. So there are three
setups.

### Same computer

Run the relay and connect to `ws://localhost:3000`. Handy for trying things out with two game
windows.

### LAN

Players on your network connect to `tcp://<your LAN IP>:3001`, for example
`tcp://192.168.1.20:3001`. Allow TCP port 3001 through the firewall of the machine running the
relay (on Windows, allow Node.js when it asks). This traffic is not encrypted, so keep it to
networks you trust. Don't forward 3001 to the internet; use `wss://` for that.

### Internet

Put the relay behind a reverse proxy that holds a real certificate (Let's Encrypt is fine) and
have players connect to `wss://your.domain`. You need a domain name pointing at the server and
ports 80 and 443 open for the certificate challenge and the players.

Keep port 3000 closed in the firewall: the relay listens on all interfaces and the proxy reaches
it on localhost.

**Caddy** gets and renews the certificate by itself. The whole `Caddyfile`:

```
twili.example.org {
    reverse_proxy 127.0.0.1:3000
}
```

**nginx** with a certificate from certbot (`certbot --nginx -d twili.example.org` fills in the
certificate lines for you):

```nginx
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}

server {
    listen 443 ssl;
    server_name twili.example.org;

    ssl_certificate     /etc/letsencrypt/live/twili.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/twili.example.org/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:3000;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Host $host;
        proxy_read_timeout 120s;
    }
}
```

The relay pings every 10 s, so the default proxy timeouts are enough; the longer read timeout
above is just headroom. The relay accepts any path, so you can also serve it under a location
like `/twili` and have players use `wss://example.org/twili`.

**Without a proxy**: if you already have a trusted certificate for the server's domain, point the
relay at it and players connect to `wss://your.domain:3000`:

```
TT_TLS_CERT=/etc/letsencrypt/live/twili.example.org/fullchain.pem \
TT_TLS_KEY=/etc/letsencrypt/live/twili.example.org/privkey.pem \
node server.js
```

The files are read at startup, so restart the relay after the certificate renews. The TCP port
never uses TLS.

## Keeping it running (Linux)

A systemd unit, for example `/etc/systemd/system/twili-together.service`:

```ini
[Unit]
Description=Twili-Together relay
After=network.target

[Service]
User=twili
WorkingDirectory=/opt/twili-together/server
ExecStart=/usr/bin/node server.js 3000 off
Environment=TT_LOG_PATH=/var/log/twili-together/server.log
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Then `systemctl enable --now twili-together`. `3000 off` because an internet relay behind a proxy
has no use for the unencrypted TCP port.

## Checking it

`npm test` in `tools/server` runs the protocol tests against a local relay (no game needed). The
log shows every join, leave and refused packet, which is usually enough to see why a player
cannot connect or does not sync. A game with an older protocol is refused with a message in game.
