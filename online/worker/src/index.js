// Room list + hole-punch introducer + relay for the Ridge Racer port's online mode.
// It stores only room names, a version tag, network addresses, the room description the game advertises (host name, course,
// class, handicap), a rough host location for ping estimates and a password hash, and only for as long as a host keeps
// refreshing its room.
// PC players talk to each other directly.  Browser players (no UDP in browsers), and PC players whose routers block a
// direct connection, send their datagrams through /relay instead; those are forwarded, never stored.
//
//   POST /host?mac=&ep=&lep=&name=&ver=&n=&max=[&info=&vis=&pw=&open=]
//                                                 host creates/refreshes its room; reply = pending joiners ("joiner <mac> <ep> <lep>").
//                                                 info = the game's 28-byte room description (hex: host name, course, class,
//                                                 handicap); vis = 0 listed, 1 listed with a password (pw), 2 not listed
//                                                 (joined by room code only).  Only a hash of the password is kept.
//                                                 open = 1 while the host's game accepts entries, 0 during a race.
//   GET  /rooms?ver=[&name=]                      reply = listed rooms ("room <mac> <ep> <lep> <name> <n> <max> <vis> <info> <est> <open>");
//                                                 ep/lep are "-" for password rooms (given out by /join instead); est = rough
//                                                 round trip in ms between the asker and the host from their locations, -1 = unknown
//   GET  /room?ver=&code=                         one room by its code (last 6 hex digits of the host id), listed or not; "none" if absent
//   POST /join?room=&mac=&ep=&lep=&ver=[&pw=][&probe=1] a player tells a host where to send hole-punch packets.  Reply "ok <ep> <lep>"
//                                                 (the host's addresses), or "gone", "full", "badpw", "racing" (try again
//                                                 when the race is over), "version <v>" (the room is on another version),
//                                                 "update <v>" (this game is too old).  probe=1 only checks.
//   POST /leave?mac=                              host closes its room
//   GET  /relay?mac=  (WebSocket)                 binary messages [6-byte destination id][datagram] are forwarded to
//                                                 that id's socket as [6-byte sender id][datagram].  Sender id
//                                                 000000000000 = from the service: "joiner <mac> <ep> <lep>" tells a
//                                                 host at once that someone asked to join (hosts keep this socket open)
//
// Game versions: GAME_VERSIONS in wrangler.toml lists the versions served (comma separated, empty = any).  Another version gets
// "update <GAME_LATEST>" from /rooms, /room, /host and /join (the game then tells the player to update); a room code or
// join for a room on a different (served) version gets "version <that version>".
//
// A peer address of 0.0.0.0:1 means "reachable only through the relay" (a browser player).
//
// Everything is plain text; addresses are IPv4 "a.b.c.d:port"; mac is 12 hex digits; name is 16 hex digits (8 bytes).

const ROOM_TTL_MS = 15000;
const JOINER_TTL_MS = 60000;
const MAX_ROOMS = 500;
const MAX_LIST = 100;
const MAX_RELAY = 400;   // open relay sockets

const RE_MAC = /^[0-9a-f]{12}$/;
const RE_EP = /^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d{1,5}$/;
const RE_NAME = /^[0-9a-f]{16}$/;
const RE_VER = /^[A-Za-z0-9._-]{1,16}$/;
const RE_INFO = /^[0-9a-f]{0,128}$/;
const RE_CODE = /^[0-9a-f]{6}$/;

async function sha256hex(s) {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
// Rough round trip between two places: about 1 ms per 100 km of fibre, times 1.5 for indirect routes, plus 8 ms.
function estimateMs(a, b) {
  if (!a || !b) return -1;
  const rad = Math.PI / 180;
  const dLat = (b.lat - a.lat) * rad, dLon = (b.lon - a.lon) * rad;
  const h = Math.sin(dLat / 2) ** 2 + Math.cos(a.lat * rad) * Math.cos(b.lat * rad) * Math.sin(dLon / 2) ** 2;
  const km = 2 * 6371 * Math.asin(Math.min(1, Math.sqrt(h)));
  return Math.round(8 + km * 0.015);
}
function geoOf(request) {
  const lat = Number.parseFloat(request.headers.get("x-rr-lat") || ""), lon = Number.parseFloat(request.headers.get("x-rr-lon") || "");
  return Number.isFinite(lat) && Number.isFinite(lon) ? { lat, lon } : null;
}

function text(body, status = 200) {
  return new Response(body, { status, headers: { "content-type": "text/plain; charset=utf-8", "access-control-allow-origin": "*", "cache-control": "no-store" } });
}
function validEp(ep) {
  if (!RE_EP.test(ep)) return false;
  const [ip, port] = ep.split(":");
  return ip.split(".").every((o) => Number(o) <= 255) && Number(port) >= 1 && Number(port) <= 65535;
}
function intIn(v, lo, hi, dflt) {
  const n = Number.parseInt(v, 10);
  return Number.isFinite(n) && n >= lo && n <= hi ? n : dflt;
}

export class Lobby {
  constructor(state, env) {
    this.state = state;
    this.rooms = new Map();   // mac -> room; state is rebuilt by the hosts' own refreshes if this ever restarts
    this.versions = String((env && env.GAME_VERSIONS) || "").split(",").map((v) => v.trim()).filter(Boolean);
    this.latest = String((env && env.GAME_LATEST) || this.versions[this.versions.length - 1] || "");
  }
  // null = this version is served; otherwise the reply that tells the game to update
  outdated(ver) {
    if (this.versions.length === 0 || this.versions.includes(ver)) return null;
    return text(`update ${this.latest || "newer"}\n`);
  }

  // ---- relay (WebSocket hibernation API: idle sockets cost nothing; each socket is tagged with its player id)
  relayOpen(request, mac) {
    if (request.headers.get("Upgrade") !== "websocket") return text("expected a WebSocket\n", 426);
    for (const old of this.state.getWebSockets(mac)) { try { old.close(4000, "replaced"); } catch (e) {} }
    if (this.state.getWebSockets().length >= MAX_RELAY) return text("relay full\n", 503);
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    this.state.acceptWebSocket(server, [mac]);
    return new Response(null, { status: 101, webSocket: client });
  }

  async webSocketMessage(ws, msg) {
    if (typeof msg === "string") return;
    const b = new Uint8Array(msg);
    if (b.length < 7 || b.length > 1600) return;
    let dst = "";
    for (let i = 0; i < 6; i++) dst += b[i].toString(16).padStart(2, "0");
    const targets = this.state.getWebSockets(dst);
    if (targets.length === 0) return;
    const src = this.state.getTags(ws)[0] || "";
    const out = new Uint8Array(b.length);
    for (let i = 0; i < 6; i++) out[i] = Number.parseInt(src.substr(i * 2, 2), 16) || 0;
    out.set(b.subarray(6), 6);
    for (const t of targets) { try { t.send(out); } catch (e) {} }
  }

  // A message from the service itself: sender id 000000000000, then a text line.
  notify(mac, line) {
    const body = new TextEncoder().encode(line);
    const out = new Uint8Array(6 + body.length);
    out.set(body, 6);
    for (const t of this.state.getWebSockets(mac)) { try { t.send(out); } catch (e) {} }
  }

  async webSocketClose(ws, code, reason) { try { ws.close(code, reason); } catch (e) {} }
  async webSocketError(ws) { try { ws.close(1011, "error"); } catch (e) {} }

  expire(now) {
    for (const [mac, r] of this.rooms) {
      if (now - r.seen > ROOM_TTL_MS) { this.rooms.delete(mac); continue; }
      for (const [jm, j] of r.joiners) if (now - j.t > JOINER_TTL_MS) r.joiners.delete(jm);
    }
  }

  async fetch(request) {
    const url = new URL(request.url);
    const q = url.searchParams;
    const now = Date.now();
    this.expire(now);
    const path = url.pathname;

    const line = (r, me) => {
      const hide = r.vis === 1;
      return `room ${r.mac} ${hide ? "-" : r.ep} ${hide ? "-" : r.lep} ${r.name} ${r.n} ${r.max} ${r.vis} ${r.info || "-"} ${estimateMs(me, r.geo)} ${r.open ? 1 : 0}\n`;
    };

    if (path === "/rooms" && request.method === "GET") {
      const ver = q.get("ver") || "";
      const name = q.get("name");
      if (!RE_VER.test(ver)) return text("bad ver\n", 400);
      if (name !== null && !RE_NAME.test(name)) return text("bad name\n", 400);
      { const old = this.outdated(ver); if (old) return old; }
      const me = geoOf(request);
      const list = [...this.rooms.values()]
        .filter((r) => r.ver === ver && r.vis !== 2 && (name === null || r.name === name))
        .sort((a, b) => b.seen - a.seen)
        .slice(0, MAX_LIST);
      return text(list.map((r) => line(r, me)).join(""));
    }

    if (path === "/room" && request.method === "GET") {
      const ver = q.get("ver") || "", code = (q.get("code") || "").toLowerCase();
      if (!RE_VER.test(ver) || !RE_CODE.test(code)) return text("bad request\n", 400);
      { const old = this.outdated(ver); if (old) return old; }
      for (const r of this.rooms.values()) if (r.ver === ver && r.mac.endsWith(code)) return text(line(r, geoOf(request)));
      for (const r of this.rooms.values()) if (r.mac.endsWith(code)) return text(`version ${r.ver}\n`);   // a race on another version
      return text("none\n");
    }

    if (path === "/host" && request.method === "POST") {
      const mac = q.get("mac") || "", ep = q.get("ep") || "", lep = q.get("lep") || ep, name = q.get("name") || "", ver = q.get("ver") || "";
      if (!RE_MAC.test(mac) || !validEp(ep) || !validEp(lep) || !RE_NAME.test(name) || !RE_VER.test(ver)) return text("bad request\n", 400);
      { const old = this.outdated(ver); if (old) { this.rooms.delete(mac); return old; } }
      let room = this.rooms.get(mac);
      if (!room) {
        if (this.rooms.size >= MAX_ROOMS) return text("full\n", 503);
        room = { mac, joiners: new Map() };
        this.rooms.set(mac, room);
      }
      const info = (q.get("info") || "").toLowerCase(), vis = intIn(q.get("vis"), 0, 2, 0), pw = q.get("pw") || "";
      if (!RE_INFO.test(info) || pw.length > 32) return text("bad request\n", 400);
      room.pwHash = pw ? await sha256hex(mac + ":" + pw) : "";
      Object.assign(room, { ep, lep, name, ver, info, vis: vis === 1 && !pw ? 0 : vis, geo: geoOf(request) || room.geo || null,
                            n: intIn(q.get("n"), 1, 8, 1), max: intIn(q.get("max"), 1, 8, 8), open: intIn(q.get("open"), 0, 1, 1) === 1, seen: now });
      return text([...room.joiners].map(([jm, j]) => `joiner ${jm} ${j.ep} ${j.lep}\n`).join(""));
    }

    if (path === "/join" && request.method === "POST") {
      const roomMac = q.get("room") || "", mac = q.get("mac") || "", ep = q.get("ep") || "", lep = q.get("lep") || ep;
      if (!RE_MAC.test(roomMac) || !RE_MAC.test(mac) || !validEp(ep) || !validEp(lep)) return text("bad request\n", 400);
      const ver = q.get("ver") || "";   // (games before the version check send none)
      if (ver === "" ? this.versions.length > 0 : !RE_VER.test(ver)) return ver === "" ? text(`update ${this.latest || "newer"}\n`) : text("bad request\n", 400);
      { const old = this.outdated(ver); if (old) return old; }
      const room = this.rooms.get(roomMac);
      if (!room) return text("gone\n");
      if (room.ver !== ver) return text(`version ${room.ver}\n`);
      if (room.vis === 1 && (await sha256hex(roomMac + ":" + (q.get("pw") || ""))) !== room.pwHash) return text("badpw\n");
      if (room.n >= room.max && !room.joiners.has(mac)) return text("full\n");
      if (!room.open) return text("racing\n");   // the host's game takes no entries until the race is over (the player can wait)
      if (q.get("probe") === "1") return text(`ok ${room.ep} ${room.lep}\n`);
      if (room.joiners.size >= 32 && !room.joiners.has(mac)) return text("busy\n");
      room.joiners.set(mac, { ep, lep, t: now });
      this.notify(roomMac, `joiner ${mac} ${ep} ${lep}`);   // instant introduction (the host's 3 s poll is the backup)
      return text(`ok ${room.ep} ${room.lep}\n`);
    }

    if (path === "/leave" && request.method === "POST") {
      const mac = q.get("mac") || "";
      if (!RE_MAC.test(mac)) return text("bad request\n", 400);
      this.rooms.delete(mac);
      return text("ok\n");
    }

    if (path === "/relay") {
      const mac = q.get("mac") || "";
      if (!RE_MAC.test(mac)) return text("bad request\n", 400);
      return this.relayOpen(request, mac);
    }

    if (path === "/" ) return text("rr-lobby ok\n");
    return text("not found\n", 404);
  }
}

export default {
  async fetch(request, env) {
    // The asker's rough location (Cloudflare's own lookup) for the ping estimate in the room list; never stored for joiners.
    const h = new Headers(request.headers);
    h.delete("x-rr-lat"); h.delete("x-rr-lon");
    const cf = request.cf || {};
    if (cf.latitude !== undefined && cf.longitude !== undefined) { h.set("x-rr-lat", String(cf.latitude)); h.set("x-rr-lon", String(cf.longitude)); }
    const fwd = new Request(request.url, { method: request.method, headers: h, body: request.body });
    return env.LOBBY.get(env.LOBBY.idFromName("main")).fetch(fwd);
  },
};
