# Hosting the relay for free

Two free setups, what each can and cannot do, and the exact settings. Nobody has deployed anything for you: you create the accounts,
and the program (`server.mjs`) has been tested on this PC only (see "What was tested").

Plans and limits below were read from the providers' own pages on **2026-11-25** (each fact says which page). They change; check the
page again before you rely on a number. Whatever could not be confirmed is marked **UNVERIFIED**.

## Which one

| | A. Free web service (Render) | B. Free always-on VM (Oracle Cloud Always Free) |
| --- | --- | --- |
| Browser players | yes | yes |
| Windows / Android games (they speak UDP) | **no** | yes |
| Sleeps | after 15 minutes without inbound traffic; wakes in about a minute | no, but an idle VM can be reclaimed (see B) |
| Free outbound traffic | 5 GB a month for the whole workspace; then services stop until next month (no card on file) or cost $0.15 per GB (card on file) | 10 TB a month |
| Effort (my estimate, not tried) | short, no server administration | about an hour, you run a Linux server |
| Needs | a Render account and the GitHub repository | an Oracle account (card and phone number for verification), a free DuckDNS name |

* **Only browsers play?** Take A. **Windows or Android players must be in the same lobbies as browsers?** Take B: a relay without UDP cannot serve them.
* **UDP cannot be had on a free web service**: Render's web services take public traffic on one HTTP port only (docs: web-services) and
  none of the pages read mentions UDP. A free HTTP-only relay therefore means: Windows and Android games cannot use it, and crossplay
  between exe/Android and browsers is impossible there. Browser versus browser works.
* **How much traffic the relay carries.** README: roughly 100 to 300 KB/s per person while the game runs *through the relay*. When two browsers connect
  directly (WebRTC), the relay carries only the lobby list and a few signalling messages. On A the 5 GB budget is about 4.6 to 14 hours of one
  person playing through the relay (5 GB divided by 300 and by 100 KB/s), so A suits groups whose direct connections work. Direct connections were only tried on one
  PC here (README, "Direct connections"), so how many real pairs succeed is unknown.
* **Looked at and not used** (same date, facts only): Koyeb's pricing page lists no free compute instance (only a free database tier). Fly.io's pricing page says new accounts start with $1 of credit and no card;
  the smallest always-on machine is listed at $2.19 a month and outbound traffic at $0.02 per GB in North America and Europe (docs.fly.io/about/pricing); a page summary also mentioned $5 of usage a month once a card is added: UNVERIFIED.
  The Google Cloud free e2-micro includes 1 GB of outbound traffic a month (docs.cloud.google.com/free/docs/free-cloud-features), against the README's 100 to 300 KB/s per person through the relay.
* Never run the relay on the PC you play on or on a home connection (README).

## Your page's origin (needed by both)

`ALLOWED_ORIGINS` lets only your own page use the relay. It is the page's **scheme and host** and nothing else:

* GitHub Pages site `https://trickernoxics14.github.io/Sonic-3-Airnet/` has the origin **`https://trickernoxics14.github.io`**.
* Another address (custom domain, another account) has its own origin: scheme and host, e.g. `https://play.example.org`.
* The server accepts capital letters, a trailing slash or the page path in the setting and cleans them up. An entry that no browser could ever send (`trickernoxics14.github.io` without `https://`, a `*` wildcard like `https://*.github.io`, a user name, a query, a bad host or port) stops the program at start with a message instead of locking everybody out. Several origins are separated by commas.
* Windows and Android games send no origin and are let in; so is any script, because a script can write any origin it likes. The list keeps *other web pages* from using your relay; it is not a password. An `Origin` header that is present but empty (a browser never sends one) or `null` (a sandboxed frame, a local file) is refused with 403 while a list is set.

## A. Render free web service (browsers only)

Sources: render.com/docs/free, /docs/web-services, /docs/health-checks, /docs/websocket, /docs/outbound-bandwidth, /docs/monorepo-support, /docs/node-version, /docs/your-first-deploy.

**What the free plan does (from those pages)**

* 750 free instance hours per workspace and month; a spun-down service uses none. One service awake all month needs at most 744, so one fits; a second one that is awake all month would not.
* Spins down after **15 minutes without inbound traffic**; HTTP requests and WebSocket messages on open connections count. Waking takes **"about one minute"** (browsers get a loading page meanwhile).
  The game waits **60 seconds** per request ("Waking the server up, retrying" in the menus), so the first player after a quiet time may need to press the button once more. How Render answers
  a WebSocket request while the service wakes is not documented on those pages: UNVERIFIED. The program itself listens in well under a second here; the minute is Render's.
  Whether a browser's automatic answer to the relay's 20-second ping counts as traffic is not stated: UNVERIFIED (an open lobby page may keep it awake, which is harmless, see the hours above).
* Render "might restart a Free web service at any time". Lobbies live in the relay's memory only, so a restart empties the lobby list, and a sleeping service has to start first (about a minute, above). What is backed by a test: web_net session A (A6a to A6c, A7) drops every WebSocket of a running relay and both browsers connect, register and join again by themselves; the game also repeats its registration every few seconds (comment in `relay_core.mjs`). A restart that wipes the relay's memory was not tried: UNVERIFIED whether a running game carries on across it.
* Outbound traffic (WebSocket messages included) counts against the workspace's included **5 GB** (Hobby workspace). Free memory and CPU are not stated on the Free page: UNVERIFIED. The relay needs about 50 MB when idle (measured here, Node 24).
* No stated maximum number of WebSocket connections and no maximum duration (docs/websocket). The relay's own limits: 32 connections and 4 lobbies per player address, 2 public listings per address, 512 lobbies in all.

**Create it** (Dashboard, New, Web Service; field names as on the Render pages, they may be placed differently):

1. Connect your GitHub account and pick the repository `Sonic-3-Airnet`, **branch that contains `checks/relay_ws`** (check the branch after merging). Whether the Dashboard can take a public repository URL without connecting an account is not stated on the pages read: UNVERIFIED.
2. Language **Node**. **Root Directory** `checks/relay_ws` (docs/monorepo-support). Build Command `npm install` (the program has no packages; it ends at once). Start Command **`node server.mjs`**.
3. Instance type **Free**.
4. Environment variables (do not set `PORT`: Render sets it, default 10000, and the program uses it):

   | name | value |
   | --- | --- |
   | `UDP_PORT` | `0` |
   | `TRUST_PROXY` | `1` (calibrate it, see below) |
   | `ALLOWED_ORIGINS` | `https://trickernoxics14.github.io` (your page's origin) |

   Node version: Render's default for new services is Node 24 (docs/node-version, read 2026-11-25); the program needs 18 or newer. Leave it.
5. Settings, Health Checks, Edit: path **`/health`** (healthy = a 2xx or 3xx within 5 seconds; the relay answers `{"ok":true,...}`; Render restarts an instance that fails for 60 seconds).
6. Wait for the first deploy. Your address is `https://NAME.onrender.com` (docs/your-first-deploy). The relay address for the game is the same with **`wss://`**: `wss://NAME.onrender.com`.

**Find the right `TRUST_PROXY`.** It is the number of proxies in front of the relay that each add an address to `X-Forwarded-For`. Render's docs say nothing about that header; a user's test on Render's feedback board says Render *appends* to what a player sends; how many entries Render's own chain adds is UNVERIFIED. Two checks tell you:

* `node proxy_check.mjs wss://NAME.onrender.com` (on your PC, Node 22 or newer, in `checks/relay_ws`; the first call may take a minute while the service wakes up). **PROBLEM means the number is too HIGH**: the relay would count an address that the player wrote, so anybody could invent addresses. **Lower** it by one (to 0 if nothing in front adds an address), deploy, run it again. If real players connected during its one second it can say PROBLEM wrongly: run it again before you change anything.
* **OK does not say the number is high enough.** If it is too LOW, every player shows up with the platform's own address: nobody can invent one, but the per-address limits count everybody together (README, "If every player shows up as one address"). To see it, have two people on different networks (for example your PC and a phone on mobile data) open the game's online menu and look at `https://NAME.onrender.com/health`: `websocketClients` 2 and `websocketAddresses` 2 is right; `websocketAddresses` 1 means pooled.

So: start at `1`. PROBLEM: use `0`. OK and `websocketAddresses` shows the players apart: done. OK but pooled: try `2` and run `proxy_check.mjs` again; OK and apart: done; PROBLEM: go back to `1`. A header with fewer entries than the number counts as the address of the connection itself (pooled, never invented), but a player who knows the number can add entries in front, which is why only `proxy_check.mjs` can tell a too high number. Where a number that pools cannot be avoided, raise a limit from the README only if real players are refused.

**When the free plan is not enough**: the traffic budget ends (services stop until the month changes, or the card on file is charged $0.15 per GB, docs/outbound-bandwidth) or you want Windows players: use B, or any small VPS that offers UDP: it is the same program and the same settings as B. Render's paid instance prices are on render.com/pricing (not read: the page did not load as text).

## B. Oracle Cloud Always Free VM (browsers and Windows/Android)

Sources: docs.oracle.com/en-us/iaas/Content/FreeTier/freetier_topic-Always_Free_Resources.htm, .../FreeTier/freetier.htm, .../GSG/Tasks/signingup_topic-Sign_Up_for_Free_Oracle_Cloud_Promotion.htm,
.../Compute/Tasks/connect-to-linux-instance.htm; caddyserver.com/docs/install and /docs/caddyfile/directives/reverse_proxy; packages.ubuntu.com/noble/nodejs; duckdns.org.

**What the free offer is (from those pages)**

* Up to two **VM.Standard.E2.1.Micro** VMs (1/8 OCPU, 1 GB memory, one public IP, internet bandwidth **up to 50 Mbps**) or the Ampere A1 allowance (2 OCPU and 12 GB in total). The Micro is plenty: the relay needs about 50 MB.
  50 Mbps is about 6 MB/s, which is roughly 20 people at once if all of them play through the relay at the README's worst case of 300 KB/s each.
* **10 TB** outbound data per month. Always Free resources "never expire" and stay after the 30-day trial; Always Free compute can only be created in your **home region**, which you pick at sign-up and cannot change.
* Sign-up: Oracle says most users need a **mobile phone number and a credit card**; the card is checked (a small temporary hold) and "will not be charged unless you upgrade". The sign-up guide says debit cards with a major card logo that need no PIN can be used; that virtual and prepaid cards are refused is known to me only from a search summary: UNVERIFIED. Check the rules at sign-up.
* **The catch: an idle VM may be reclaimed.** Oracle: an instance is idle if during 7 days its CPU (95th percentile) is below 20% **and** its network below 20% (memory too, on A1 only). A relay nobody uses looks exactly like that, so expect to lose the VM now and then. The relay keeps nothing on disk: when it happens you create a new VM
  and repeat steps 1 to 6 below (about half an hour), then point the DuckDNS name at the new address. Whether upgrading to Pay As You Go prevents reclaiming is not stated on the pages read: UNVERIFIED.
* Capacity: new VMs can fail with an "out of capacity" error (widely reported; not on the pages read: UNVERIFIED). Try again later.

**1. Create the VM.** Console, Compute, Instances, Create instance: image **Ubuntu** (24.04 offers Node 18.19; see step 3), shape VM.Standard.E2.1.Micro marked *Always Free eligible*, let the console create an SSH key pair and
keep the private key file private (never commit or share it). Note the public IP address. Connect from PowerShell (the SSH client is part of Windows 10 and 11): `ssh -i "C:\path\to\your-key-file" ubuntu@PUBLIC_IP`; the user name of Ubuntu images is `ubuntu`.
Windows may refuse the key until the file's permissions give only your own account access to it (Oracle's connect page lists the steps).

**2. Open the ports, in two places** (everything else stays closed):

| port | for | open to |
| --- | --- | --- |
| UDP 28842 | Windows and Android games | everyone |
| TCP 443 and TCP 80 | browsers (`wss://` through Caddy; port 80 is for the certificate) | everyone |
| TCP 22 | SSH | already open |

**Do not open TCP 28842.** Caddy reaches it on the VM itself; opened to the world, a player could connect straight to it and invent its own `X-Forwarded-For` address, and would skip the TLS front.
(The program cannot listen on the loopback address for TCP alone: `BIND` is for UDP too.)

* Oracle console: the subnet's Security List (or the instance's Network Security Group), Ingress rules, source `0.0.0.0/0`, one rule for UDP 28842, one for TCP 443, one for TCP 80.
* On the VM: Oracle's developer blog says Ubuntu images block everything but SSH with iptables rules (file `/etc/iptables/rules.v4`), that UFW should not be used, and that a new rule placed after the final REJECT rule has no effect.
  I could not open that page (HTTP 403 for my tool) and know it only from a search summary: **UNVERIFIED**, check it. With that layout:
  `sudo iptables -L INPUT -n --line-numbers` shows the rules; insert at the line of the REJECT rule (N), then save:
  `sudo iptables -I INPUT N -p udp --dport 28842 -j ACCEPT`, the same for `-p tcp --dport 443` and `--dport 80`, then `sudo netfilter-persistent save` (is that tool on your image? UNVERIFIED, otherwise write the rules into `/etc/iptables/rules.v4` as the blog describes). Do not touch the SSH rule.

**3. Node.js.** The relay needs Node 18 or newer. `sudo apt-get install -y nodejs` gives 18.19 on Ubuntu 24.04 (packages.ubuntu.com, universe). Node 18 is no longer in nodejs.org's list of current releases (LTS lines 26, 24 and 22 on 2026-11-25) and the relay was not run on 18 here: UNVERIFIED.
Prefer a current LTS: nodejs.org/en/download shows the install commands for your system. `node --version` must print 18 or more.

**4. Copy the relay.** On the VM: `mkdir ~/s3air-relay`. On your PC, in the repository's `checks` folder (copies the whole folder, so no file is forgotten): `scp -i "C:\path\to\your-key-file" -r relay_ws ubuntu@PUBLIC_IP:~/s3air-relay/`. Try it by hand:
`cd ~/s3air-relay/relay_ws && UDP_PORT=28842 node server.mjs` shows `s3air_relay: WebSocket/HTTP on TCP 28842, UDP on 28842 ...`; in a second SSH window `curl http://127.0.0.1:28842/health` answers `{"ok":true,"protocol":2,...}`. Stop with Ctrl+C.

**5. Run it as a service.** `sudo nano /etc/systemd/system/s3air-relay.service`:

    [Unit]
    Description=Sonic 3 AirNet relay
    After=network-online.target
    Wants=network-online.target

    [Service]
    User=ubuntu
    WorkingDirectory=/home/ubuntu/s3air-relay/relay_ws
    ExecStart=/usr/bin/node server.mjs
    Environment=PORT=28842
    Environment=UDP_PORT=28842
    Environment=TRUST_PROXY=1
    Environment=ALLOWED_ORIGINS=https://trickernoxics14.github.io
    Restart=always
    RestartSec=5
    NoNewPrivileges=true

    [Install]
    WantedBy=multi-user.target

`ExecStart` needs the full path from `which node`. `TRUST_PROXY=1` is right because Caddy (step 7) is the one proxy in front. Then `sudo systemctl daemon-reload && sudo systemctl enable --now s3air-relay` and `systemctl status s3air-relay` (it must say active).
Logs: `journalctl -u s3air-relay -f` (counters only: never addresses, codes or game data).

**6. A free name and HTTPS.** Browsers need a name for the `wss://` certificate. duckdns.org ("free dynamic DNS", sign-in with Google, reddit, GitHub or Twitter on its page): create a name `your-name`, set its IP to the VM's address. Its token is a secret: do not paste it anywhere.
Check with `nslookup your-name.duckdns.org`. Install Caddy from its own package source with the commands on caddyserver.com/docs/install (Debian/Ubuntu; check the page before pasting, repository addresses change), then `sudo nano /etc/caddy/Caddyfile`:

    your-name.duckdns.org {
        reverse_proxy 127.0.0.1:28842
    }

and `sudo systemctl reload caddy`. Caddy gets the certificate itself once the name points at the VM and ports 80 and 443 are open. Its `reverse_proxy` supports WebSocket without extra settings and, by default, ignores an `X-Forwarded-For`
sent by the player (caddyserver.com docs), so the relay sees exactly one address entry: the player's.

**7. Test and calibrate.** In a browser: `https://your-name.duckdns.org/health` shows the counters with a padlock. On your PC: `node proxy_check.mjs wss://your-name.duckdns.org` must print OK (PROBLEM would mean `TRUST_PROXY` is too high: lower it; with Caddy as the one proxy `1` is right and `/health` then shows each player apart). UDP: start a Windows game with the relay set (below) and create a lobby; if it fails the usual cause is a missing UDP rule in one of the two firewalls.

**When the free VM goes away**: create a new one (steps 1 to 6, nothing else to restore), or move to any small VPS with UDP; the settings are the same.

## Telling the game where it is

* **Browser version:** the page's owner puts the address into **`relay_config.js`**, the file next to `sonic3air_web.html`. In the repository it is
  `sonic3air_online_stable_source/Oxygen/sonic3air/build/_emscripten/dist/relay_config.js` (the web build copies it next to the page). It is **empty on purpose** until you have an address:

      window.S3AIR_RELAY_DEFAULT = 'wss://NAME.onrender.com';   // or 'wss://your-name.duckdns.org'

  It is a plain script: you can edit it in the published folder without rebuilding the game. Players can also open the page as `sonic3air_web.html?relay=wss://NAME.onrender.com` (the link wins) or type the address under *Online lobby, RELAY SERVER* (saved in their browser).
  A page on `https://` only accepts `wss://` addresses. With no address anywhere the lobby says "No relay server is set. Online play needs one: enter its address in the lobby." and nothing else happens.
* **Windows version (B only):** `settings.json`, `"Online" : { "RelayServer" : "your-name.duckdns.org:28842" }` (the UDP address, not `wss://`). Everybody in one lobby must use the same relay, game version and mods (the browser version has none: Windows players switch mods off to play with browsers).

## Settings at a glance

`node server.mjs` (Node 18 or newer, no packages). All settings are environment variables; the full table is in README.md.

| | A. Render | B. Oracle VM |
| --- | --- | --- |
| start command | `node server.mjs` (root directory `checks/relay_ws`) | `node server.mjs` (systemd) |
| `PORT` | set by Render (10000) | `28842` |
| `UDP_PORT` | `0` | `28842` |
| `TRUST_PROXY` | `1`, then calibrate | `1` (Caddy) |
| `ALLOWED_ORIGINS` | your page's origin | your page's origin |
| health check | `GET /health` | `GET /health` |
| relay address for the page | `wss://NAME.onrender.com` | `wss://your-name.duckdns.org` |

## What was tested, and what was not

Tested on one PC (Windows, Node 24) with the program started exactly as in the table (`UDP_PORT=0 TRUST_PROXY=1 ALLOWED_ORIGINS=https://trickernoxics14.github.io`, bound to 127.0.0.1):
`test/hosting.test.mjs` (no UDP socket, an allowed page lists, publishes, resolves, joins, signals and sends data, other origins and an empty Origin get 403, `/health` shows counters only, the player's address behind proxies including a header shorter than the number, `proxy_check.mjs` through small proxies that append, add nothing, overwrite, or chain two and three at `TRUST_PROXY` 0 to 3, bad settings stop the program, the Docker file list and its guard)
and `test/browser_origin_check.mjs` (a real headless Chrome on the page origin `https://trickernoxics14.github.io` against the relay on another origin, with a control relay that has no list). Also `Get-NetUDPEndpoint` showed 0 UDP endpoints for the relay process with `UDP_PORT=0`.

**Not tested:** any real host (Render, Oracle, Caddy, DuckDNS: nothing was created or deployed), a real WebSocket wake-up from sleep, Render's actual `X-Forwarded-For` chain, Docker (not installed here; the image's file list is checked, the image was never built), Node 18, UDP from a Windows game to a VM, Oracle's firewall steps, the real traffic of a game through a relay.
