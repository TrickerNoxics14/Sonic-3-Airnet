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

Check it: open `http://localhost:28842/health` (it answers `{"ok":true,...}` with counters only). On a deployed relay, `node proxy_check.mjs wss://your-relay.example.org` checks that `TRUST_PROXY` suits the host (a player cannot make up an address).

| setting (environment variable) | default | meaning |
| --- | --- | --- |
| `PORT` | 28842 | TCP port for WebSocket and `/health`. Hosting platforms set this themselves. |
| `UDP_PORT` | same as `PORT` | UDP port for Windows games; `0` turns UDP off (for hosts that only offer HTTP). |
| `BIND` | 0.0.0.0 | address to listen on |
| `TRUST_PROXY` | 0 | how many proxies of your host sit in front of the program (`1` for a platform's router or Caddy, `2` for two in a row; `0` = none). Each proxy adds the address it saw to the end of `X-Forwarded-For`, and the player's address is the entry that many places from the end, so that rate limits and blocks are per player, not per proxy. Whatever stands before it came from the player and is never used. Never set it when players connect directly (they could fake any address). Only whole numbers 0 to 10 are accepted; anything else stops the program. If the platform shows every player with the same address (the guide HOSTING.md says how to notice), raise `MAX_CONNECTIONS_PER_IP` and `MAX_LISTINGS_PER_IP`. |
| `ALLOWED_ORIGINS` | (any) | comma separated web page origins that may use the relay, e.g. `https://your-name.github.io` (scheme and host; capital letters, a trailing slash or the page path are ignored; text that is no origin stops the program, so that a typo cannot open or lock the relay silently). Empty = every page. A browser always sends its page's origin, so this keeps other web sites from using your relay; Windows games and scripts send none (or any text they like), so it is not a login. |
| `MAX_LOBBIES` | 512 | lobbies at the same time (4 per address, 6 friends per lobby; built-in limits) |
| `MAX_CONNECTIONS_PER_IP` | 32 | WebSocket connections from one player address |
| `STATS_SECONDS` | 60 | counter line interval (0 = never) |

## Direct connections (signalling)

Browser players can connect to each other directly (WebRTC data channel, see `checks/package_files/WEB_BUILD.md`). The relay's part is only
**signalling**: four message types (`SIGNAL_C2R` 10, `SIGNAL_R2C` 11, `SIGNAL_H2R` 12, `SIGNAL_R2H` 13, same layout as the data messages) carry the
offer, answer and ICE candidates between the host and a friend of one lobby. The relay forwards them exactly like data (the friend's number is added
by the relay, a friend cannot signal as another friend, strangers and wrong tokens are dropped), but counts them apart (`signals` in `/health`, they are
not game traffic), caps each at 3000 bytes and limits them to 20 per second (burst 60) per friend and direction. Games and relays that do not know these
types drop them, so old and new versions share a lobby (the protocol version stays 2). The relay never reads them, and it needs no STUN or TURN of its own.
When a direct channel works the relay forwards nothing for that player (its byte counters stay near zero) and it is the automatic fallback when the
channel cannot be made or dies.
The web checks (checks/web_net, session P) prove this on one PC only: headless Chrome connects over loopback with STUN off, so there is no real NAT, firewall, packet loss or TURN in the test, and a blocked path is simulated by hiding the ICE candidates. They show that the game really leaves the relay when the channel works and returns to it when the channel fails or recovers, not that two players behind different routers can reach each other.

### What signalling reveals, stores and logs (ICE candidate policy)

* **Who may signal:** only the host and the friends of one lobby, and only to each other. A message is accepted when the sender's connection joined that
  lobby and carries its token; the relay always addresses the other side itself (the host's message names a friend number of *its own* lobby, a friend's
  message always goes to its own lobby's host). Messages from outsiders, with wrong tokens, from a connection that left or moved to another lobby, or
  larger than 3000 bytes are dropped without any answer, so an outsider learns nothing, not even an error.
* **What the relay sends:** only the header, the token or friend number and the payload the other member wrote. The relay never puts an address, port or
  name into a signalling message, and the lobby directory (list, resolve, errors) never contains an address either (tests: signalling with non-loopback
  addresses, `no answer contains an address of a host`).
* **What the relay stores and logs:** nothing of the content. It never parses the payload. The only trace is the counters `signals` and `droppedSignals`
  (in `/health` and the counter line). No candidate, address or offer is kept, written to a file or logged.
* **Candidate policy:** the game adds no candidates of its own and does not filter the browser's. Browsers hide local network addresses behind random `.local` (mDNS) names, so
  the other player does not learn the private address of a home network. A direct link between two players cannot work without each side learning the
  other's **public** address (and port) from the candidates, so a lobby member can read the public IP of the player on the other end (for example
  in the browser's developer tools). This is the nature of a direct link and is why it is only made between members of the same lobby; a player who does
  not want to show their address to the lobby uses the relay only (the direct link is optional and the game falls back to the relay when it fails or is
  off). A public address is shown to nobody outside the lobby, and nobody in the lobby learns anything about players other than the one they are
  directly connected to through the relay's signalling (friends are only connected to the host).
* **STUN/TURN:** the relay runs none and has no TURN. The game (setting `StunServers`, default two public Google STUN servers, empty = none) asks those STUN servers for the player's public address; they see that address by design, which is how a browser learns it. With no STUN server only players in the same network can connect directly.

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

Step by step, with the free plans and limits as read from the providers' own pages (and what could not be verified): **[HOSTING.md](HOSTING.md)**.
In short:

* **Browser players only, no UDP:** a free web service that runs Node and sleeps when idle (HOSTING.md covers Render). Start command `node server.mjs`, settings
  `UDP_PORT=0 TRUST_PROXY=1 ALLOWED_ORIGINS=https://your-name.github.io`. The platform gives the `https://` address; the relay address is the same name with `wss://`.
  The first player after a quiet time waits while it wakes up: the browser build's menus say "Waking the server up, retrying" and retry for 60 seconds.
  These hosts have no UDP, so Windows and Android players cannot use that relay.
* **Browsers and Windows players, always on:** a small always-free virtual machine (HOSTING.md covers Oracle Cloud Always Free) with TCP and UDP open, Caddy in front of the TCP port for `wss://`.
* Do not run it on the PC you play on, and do not leave it open on a home connection.
* Docker: `docker build` in this folder (the image copies every `.mjs` file here; a test checks that it holds everything `server.mjs` imports). Nobody has built the image on a real Docker host for this project.

Plans and limits of these hosts change: HOSTING.md says on which day each was read. Nothing has been deployed by the people who wrote this.

## What it protects against (and what it does not)

* Taking over a lobby needs a secret that only the registering game has (not just the lobby key that every friend knows).
* Wrong keys get no answer; 8 wrong keys block that address for a minute. Rate limits per address and per friend,
  caps on lobbies and friends, a size cap on every message, ping/pong to drop vanished browsers. A reply is always
  smaller than the request that caused it (no amplification).
* It does not encrypt the game's traffic (the game does not either): whoever runs the relay can see it. Use `wss://`
  so that the path between a browser and the relay is encrypted. It cannot absorb a flood larger than the server's network link.

## The public lobby directory (new in 1.3)

The same program is also the **lobby server**: hosts publish their lobby (name, host name, players, playing or waiting, a short
"version and mods" key) and the server hands out a **6 digit lobby code**. Players open *ONLINE > PUBLIC LOBBIES* (a list), press
*QUICK JOIN* (the best open lobby), or type the 6 digits of a lobby (public or private) under *JOIN WITH CODE*. Nothing else is needed:
no second server, no database, nothing on disk. Public lists work for Windows (UDP) and browser (WebSocket) players alike, in the
same list. The directory is on by default (`DIRECTORY=0` turns it off; the relay then works exactly as before).

**Hosting it, and telling the game where it is.** Run it as described above. Then one of:
* put the address into `NetplayServerDefaults.h` (`DEFAULT_LOBBY_SERVER = "lobbies.example.org"`, one line, before building the game), or
* each player types it under *ONLINE > SETTINGS > LOBBY SERVER* (or `settings.json`, `Online` > `LobbyServer`).
The lobby server is also the relay unless a different relay is set. Windows and Android players need the **UDP** port to be reachable
(default 28842); browsers need `wss://` in front. Without UDP (a "web only" host) only browser players can use it.

| setting | default | meaning |
| --- | --- | --- |
| `DIRECTORY` | 1 | `0` = no public lobby list, relay only |
| `MAX_LISTINGS_PER_IP` | 2 | public lobbies one player address may list (raise it if many players share one address) |
| `MAX_LOBBIES_PER_IP` | 4 | relay lobbies one address may register |

Built in (see `directory.mjs`): a listing disappears 20 seconds after its host stopped publishing (the game publishes every 5 seconds and at
every change), at most 256 listings, 16 per page, names are plain letters/digits/spaces and a few marks (20 characters; host name 12), a short
list of the worst words is refused (the lobby is then just called "Lobby"), a released code is not given out again for 5 minutes.

**Security of the codes.** The code is only a *handle* for a lobby. A lobby can be published, changed or closed only by the game that holds
the relay lobby (it must send the relay lobby id, key and host secret, and the relay must have exactly that lobby registered). Lists and
lookups need a *cookie* from a hello message that belongs to the asker's address, so a forged source address cannot make the server send a big
reply (no amplification: every reply to a request without a cookie is smaller than the request). A lookup of a wrong, expired or unknown
code gets the same short answer; 5 wrong codes within a minute block that address for 5 minutes (also for the right code), 25 wrong codes
block the whole /24 (/64 for IPv6). The server never sends any address of a host. With 900,000 possible codes and a guess limit of
about one per minute and address, a private lobby cannot be found by guessing in any reasonable time; but anybody who can *read* a code (it is
shown on the host's screen) can join, so a private code is as private as the way it is shared. The operator of the lobby server can see
what the game sends to it (lobby names, session IDs) and could join any lobby: run it, or choose it, like any server that carries your traffic.

**Versions.** The relay header stays version 2: old games and old relays are not affected. Directory messages carry their own version byte
(now 1); a server answers a hello with the version it speaks, a game that gets no answer says the lobby server is not reachable.
`probe.mjs` is a small tool: `node probe.mjs 127.0.0.1:28842 list` prints the public lobbies, `resolve 123456` a lookup, and
`fake "Name" 60` puts a fake lobby into the list for a minute (for trying the game's list by hand).

## Tests

    node --test test/relay.test.mjs test/directory.test.mjs test/hosting.test.mjs   # Node 22 or newer (built-in WebSocket client); npm test does the same
    node test/browser_origin_check.mjs                                              # by hand: a real headless Chrome as the player (needs Chrome; openssl for the exact GitHub Pages origin)

`test/hosting.test.mjs` (14 tests) starts the real program as a free host would (`UDP_PORT=0 TRUST_PROXY=1 ALLOWED_ORIGINS=https://trickernoxics14.github.io`, bound to 127.0.0.1) and checks:
no UDP socket exists, the allowed page origin can list, publish, resolve, join, signal and send data, other origins and look-alikes get 403, a client without origin is let in, `/health` shows counters only,
the player's address behind one or two proxies (a faked first entry does not help; `websocketAddresses` in `/health` shows how many distinct addresses the connections have), `proxy_check.mjs`, settings that are typed wrongly stop the program, and the Docker image's file list holds everything `server.mjs` imports.
`test/browser_origin_check.mjs` does the same from a real browser page on another origin (https://trickernoxics14.github.io on this PC through a throw-away certificate), with a control relay that has no list.

(34 relay tests: 24 for the relay itself and 10 for the signalling messages: all four UDP/WebSocket host and friend combinations in both directions, payload sizes up to the cap, not counted as game traffic, forged tokens / wrong host token / unknown friend / stranger / oversize dropped, rate limit that does not hold back game data, no crash from signals of a client that never joined, two lobbies never hear each other's signalling, a friend that moved lobby cannot signal into the old one, no datagram contains an address and outsiders get no reply; plus 27 directory tests.)
They start the real server on loopback and check the protocol (garbage, versions, sizes), joining and forwarding for all
combinations of UDP and WebSocket hosts and friends, full lobbies, drop of friends and hosts, reconnect (a host takes its lobby back
with its secret, a friend gets a new place), timeouts, wrong keys and blocking, forged tokens, rate limits, the WebSocket
handshake and framing (RFC 6455 example, masking, fragmentation, size and text refusal), connection limits, origins and proxy mode.
