# Sonic 3 AirNet relay server

A small program that forwards the game's online datagrams between the players of one lobby. It is what lets
**browser players** (a browser has no UDP) play online, and it lets browser players and **Windows players** be in the
same lobby: Windows games talk UDP to it, browsers talk WebSocket to it, and the relay passes the very same datagrams
between them. It never reads the payload (the game checks its peers end to end with the lobby code), keeps nothing on
disk and logs only counters, never addresses, codes or game data.

It is a self-contained Node.js program (Node 18 or newer, **no npm packages**) that can be hosted anywhere.
Nothing in the game package starts it, and nobody has deployed one for you: whoever hosts it decides who may use it
and pays for the traffic (roughly 100 to 300 kilobytes per second per playing person).

## Run it (one command)

    node server.mjs                      # WebSocket + health page on TCP 28842, UDP on 28842
    docker build -t s3air-relay . && docker run -d --restart unless-stopped -p 28842:28842/tcp -p 28842:28842/udp s3air-relay

Check it: open `http://localhost:28842/health` (it answers `{"ok":true,...}` with counters only).

| setting (environment variable) | default | meaning |
| --- | --- | --- |
| `PORT` | 28842 | TCP port for WebSocket and `/health`. Hosting platforms set this themselves. |
| `UDP_PORT` | same as `PORT` | UDP port for Windows games; `0` turns UDP off (for hosts that only offer HTTP). |
| `BIND` | 0.0.0.0 | address to listen on |
| `TRUST_PROXY` | 0 | `1` when a reverse proxy or the platform's router is in front: the player's address is then read from `X-Forwarded-For` (rate limits are per player, not per proxy). Never set it when players connect directly: they could fake their address. |
| `ALLOWED_ORIGINS` | (any) | comma separated web page origins that may use the relay, e.g. `https://your-name.github.io`. Windows games send no origin and are not affected. |
| `MAX_LOBBIES` | 512 | lobbies at the same time (4 per address, 6 friends per lobby; built-in limits) |
| `MAX_CONNECTIONS_PER_IP` | 32 | WebSocket connections from one player address |
| `STATS_SECONDS` | 60 | counter line interval (0 = never) |

## Tell the game where it is

* **Browser version:** the page's owner puts the address into `relay_config.js` next to the page
  (`window.S3AIR_RELAY_DEFAULT = 'wss://relay.example.org'`), or a player opens the page as
  `sonic3air_web.html?relay=wss://relay.example.org`, or types it under *Online lobby, RELAY SERVER*.
  A page served over `https://` can only use `wss://` (TLS) relays, except for a relay on the same computer
  (`ws://127.0.0.1:28842`). With no address set, online play in the browser stays off and says why.
* **Windows version:** `settings.json`, `"Online" : { "RelayServer" : "relay.example.org:28842" }` (UDP address). A Windows host with this setting registers its lobby at the relay as usual (CREATE LOBBY), so browser friends can join it with the
  lobby code, and Windows friends fall back to it when the direct connection does not work. Everybody in one lobby must use the same relay,
  the same game version, and the same mods (the browser version has none, so Windows players switch their mods off to play with browsers).

## Where to host it

* **Cheapest way to run the full thing (browsers and Windows players):** a small always-free virtual machine, for example
  an *Always Free* VM of Oracle Cloud (open TCP and UDP 28842 in the cloud firewall and in the VM's own firewall, run
  `node server.mjs` as a systemd service). For `wss://` put a TLS front in front of the TCP port, e.g. Caddy with a free
  DuckDNS name (`reverse_proxy 127.0.0.1:28842`, then run the relay with `TRUST_PROXY=1`), or a free Cloudflare Tunnel.
* **Zero-setup way for browser players only:** any free web host that runs a Docker image or Node program and supports
  WebSockets (Render, Koyeb, Fly.io and similar): connect the repository, use this folder as the root, command
  `node server.mjs`, set `UDP_PORT=0` and `TRUST_PROXY=1`. They give you the `https://`/`wss://` address. Free instances
  usually go to sleep when unused, so the first player may wait up to a minute while the relay wakes up.
  These hosts have no UDP, so Windows players cannot use that relay.
* Do not run it on the PC you play on, and do not leave it open on a home connection.

Plans and limits of these hosts change: check their current terms. None of them was used to test this program.

## What it protects against (and what it does not)

* Taking over a lobby needs a secret that only the registering game has (not just the lobby key that every friend knows).
* Wrong keys get no answer; 8 wrong keys block that address for a minute. Rate limits per address and per friend,
  caps on lobbies and friends, a size cap on every message, ping/pong to drop vanished browsers. A reply is always
  smaller than the request that caused it (no amplification).
* It does not encrypt the game's traffic (the game does not either): whoever runs the relay can see it. Use `wss://`
  so that the path between a browser and the relay is encrypted. It cannot absorb a flood larger than the server's network link.

## Tests

    node --test test/relay.test.mjs        # Node 22 or newer (built-in WebSocket client)

They start the real server on loopback and check the protocol (garbage, versions, sizes), joining and forwarding for all
combinations of UDP and WebSocket hosts and friends, full lobbies, drop of friends and hosts, reconnect (a host takes its lobby back
with its secret, a friend gets a new place), timeouts, wrong keys and blocking, forged tokens, rate limits, the WebSocket
handshake and framing (RFC 6455 example, masking, fragmentation, size and text refusal), connection limits, origins and proxy mode.
