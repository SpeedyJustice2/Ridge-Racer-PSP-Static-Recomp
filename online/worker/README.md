# Room list service for online play

A tiny Cloudflare Worker that keeps the list of open rooms and introduces two players' games to each other so they can
connect directly (hole punching).  It stores only a room's name, a version tag, the network addresses of the players, the
room description the game advertises (host name, course, class, handicap), the host's rough location (Cloudflare's own
city-level lookup, used for the "~ms" ping estimate in the server browser) and, for password rooms, a hash of the password,
for about 15 seconds after the host last refreshed.

The server browser in the game uses:

- `GET /rooms?ver=` - listed rooms with player counts, who may join (open / password) and a ping estimate;
- `GET /room?ver=&code=` - one room by its 6-character room code, listed or not (Direct connect);
- `POST /join?...&ver=&pw=&probe=1` - checks a password (or that the room still exists, has space and runs the same version) before joining.

Password rooms do not show their network addresses in the list; the service gives them out only after the right password.

It also relays the game's datagrams (WebSocket `/relay`) for players who cannot connect directly: browser players
(browsers cannot send UDP) and PC players behind routers that block direct connections.  Relayed data is forwarded
immediately and never stored.  PC players who can connect directly never use it.

## One-time deploy (free Cloudflare account)

1. Create a free account at https://dash.cloudflare.com/sign-up (no card needed).
2. Install Node.js (https://nodejs.org), then in this folder:

       npx wrangler login
       npx wrangler deploy

3. It prints an address like `https://rr-lobby.<your-name>.workers.dev`.  Check it in a browser: it says `rr-lobby ok`.

## Game versions (release checklist)

Players only meet players on the same game version, so a fixed release cannot race an old one and desync.  The version is
`RR_GAME_VERSION` in `profiles/ridgeracer/host/rr_version.hpp`; the versions the service accepts are in `wrangler.toml`:

    GAME_VERSIONS = "beta1"        # every version that may still play online, comma separated
    GAME_LATEST = "beta1"          # the one older games are told to get

For a new release: change `RR_GAME_VERSION` (e.g. `beta2`), build the new Setup.exe, then set `GAME_VERSIONS = "beta2"`
(or `"beta1,beta2"` to let both play for a while, each only with its own version), `GAME_LATEST = "beta2"`, and run
`npx wrangler deploy` again.  Games whose version is not listed show "This version of the game is out of date for online
play - download beta2" in the server browser and cannot host or join.  A room code typed by a player on another version
gets "That race is on another version of the game".

## Pointing the game at it

Testing: set `RR_NET=online` and `RR_NET_SERVER=https://rr-lobby.<your-name>.workers.dev` before starting the game.
Releases: build with `-DRR_DEFAULT_ROOM_SERVER="https://..."` so players need no setting.

## Limits (free plan)

Relayed races count too: WebSocket messages are billed at 20 messages per request, and a racing player sends about
20 a second, so one relayed player costs roughly 3,600 requests an hour.


100,000 requests a day in total.  A hosting game uses one request every 3 seconds while it is in the lobby, an open
server browser one every 6 seconds (plus a few relayed ping messages per room, at the 20:1 WebSocket rate), so roughly
80 hosting hours a day fit, before it needs the paid plan ($5 a month).

## Local testing without an account

    npm install wrangler@3
    npx wrangler dev --local --port 8787
    RR_NET=online RR_NET_SERVER=http://127.0.0.1:8787 RR_NET_STUN=off ...
