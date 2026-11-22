"""HTTP API, WebSocket hub and link pages."""

import asyncio
import ipaddress
import json
import logging
import secrets
import time
import unicodedata
from collections import defaultdict, deque

from aiohttp import WSMsgType, web

from . import pages
from .db import (Database, LimitError, LoginTaken, check_secret, clean_login, clean_recovery_key, good_password,
                 hash_secret, new_recovery_key)
from .rooms import RoomError, Rooms, server_time
from .tracks import TrackError, clean_track

log = logging.getLogger("nokturn")

API = "/api/v1"
MAX_BODY = 512 * 1024
MAX_WS_MESSAGE = 256 * 1024
MAX_TRACKS_PER_REQUEST = 500


# Typed keys on aiohttp 3.9+; plain strings on older versions (Ubuntu 22.04 ships 3.8).
def _app_key(name):
    return web.AppKey(name, object) if hasattr(web, "AppKey") else name


def _request_key(name):
    return web.RequestKey(name, object) if hasattr(web, "RequestKey") else name


CONFIG, DB, ROOMS, HUB, LIMITS, HOUSEKEEPING = (_app_key(n) for n in
                                                 ("config", "db", "rooms", "hub", "limits", "housekeeping"))
USER = _request_key("user")
DEVICE = _request_key("device")


class Config:
    def __init__(self, db_path, public_url, trust_proxy=True):
        self.db_path = db_path
        self.public_url = public_url.rstrip("/")
        self.trust_proxy = trust_proxy


# ---- helpers ------------------------------------------------------------------


def clean_name(value, limit=40):
    if not isinstance(value, str):
        return ""
    text = "".join(c for c in value if unicodedata.category(c)[0] != "C")
    return " ".join(text.split())[:limit]


def error(status, code, message):
    return web.json_response({"error": code, "message": message}, status=status)


class RateLimiter:
    """At most `count` events per `window` seconds for each key."""

    def __init__(self, count, window):
        self.count = count
        self.window = window
        self.events = defaultdict(deque)

    def allow(self, key):
        now = time.monotonic()
        q = self.events[key]
        while q and now - q[0] > self.window:
            q.popleft()
        if len(q) >= self.count:
            return False
        q.append(now)
        return True

    def prune(self):
        now = time.monotonic()
        for key in [k for k, q in self.events.items() if not q or now - q[-1] > self.window]:
            del self.events[key]


def client_ip(request):
    """The address of whoever is asking (rate limits count per address). Behind a proxy on this machine or its
    private network — nginx on localhost, Caddy in a Docker network — every request comes from the proxy, so its
    word is taken: X-Real-IP (nginx), else the last X-Forwarded-For entry, the one the proxy itself added."""
    remote = request.remote or ""
    if request.app[CONFIG].trust_proxy:
        try:
            near = ipaddress.ip_address(remote)
        except ValueError:
            near = None
        if near and (near.is_loopback or near.is_private):
            forwarded = request.headers.get("X-Forwarded-For", "").split(",")[-1].strip()
            return request.headers.get("X-Real-IP") or forwarded or remote
    return remote


async def read_json(request):
    if request.content_length and request.content_length > MAX_BODY:
        raise web.HTTPRequestEntityTooLarge(max_size=MAX_BODY, actual_size=request.content_length)
    try:
        body = await request.json()
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise web.HTTPBadRequest(text='{"error":"bad_request","message":"invalid JSON"}',
                                 content_type="application/json")
    if not isinstance(body, dict):
        raise web.HTTPBadRequest(text='{"error":"bad_request","message":"expected an object"}',
                                 content_type="application/json")
    return body


def bearer(request):
    header = request.headers.get("Authorization", "")
    return header[7:].strip() if header.startswith("Bearer ") else ""


def current_session(request):
    """(the account, this device's id) for the request's token; the apps name the device in X-Nokturn-Device."""
    token = bearer(request)
    if not token:
        return None, None
    return request.app[DB].session(token, request.headers.get("X-Nokturn-Device"))


def current_user(request):
    return current_session(request)[0]


def require_user(handler):
    async def wrapped(request):
        user, device = current_session(request)
        if not user:
            return error(401, "unauthorized", "sign in first")
        request[USER] = user
        request[DEVICE] = device
        return await handler(request)
    return wrapped


# Browsers can't send an Authorization header with a WebSocket: the web app first asks for a ticket (with its
# token, like any request) and opens the socket with it — single use, half a minute, so the token itself never
# ends up in a URL.
_tickets = {}


@require_user
async def ws_ticket(request):
    now = time.time()
    for k in [k for k, v in _tickets.items() if v[2] < now]:
        del _tickets[k]
    ticket = secrets.token_urlsafe(24)
    _tickets[ticket] = (request[USER], request[DEVICE], now + 30)
    return web.json_response({"ticket": ticket})


def ticket_session(request):
    entry = _tickets.pop(request.query.get("ticket", ""), None)
    return (entry[0], entry[1]) if entry and entry[2] >= time.time() else (None, None)


def clean_tracks(raw, limit=MAX_TRACKS_PER_REQUEST):
    if not isinstance(raw, list):
        return []
    out = []
    for item in raw[:limit]:
        try:
            out.append(clean_track(item))
        except TrackError:
            continue
    return out


# ---- the hub: who is connected --------------------------------------------------


class Connection:
    def __init__(self, ws, user, device=None):
        self.ws = ws
        self.user = {"id": user["id"], "name": user["name"], "avatar": user.get("avatar", 0)}   # what the rooms show of it
        self.device = device
        self.room = None
        self.limiter = RateLimiter(40, 2.0)

    async def send(self, message):
        if self.ws.closed:
            return
        try:
            await self.ws.send_str(json.dumps(message, separators=(",", ":")))
        except (ConnectionResetError, RuntimeError):
            pass


class Hub:
    def __init__(self, db, rooms):
        self.db = db
        self.rooms = rooms
        self.by_user = defaultdict(set)

    def add(self, conn):
        self.by_user[conn.user["id"]].add(conn)

    def remove(self, conn):
        conns = self.by_user.get(conn.user["id"])
        if conns:
            conns.discard(conn)
            if not conns:
                del self.by_user[conn.user["id"]]

    async def to_users(self, user_ids, message):
        sends = [c.send(message) for uid in set(user_ids) for c in list(self.by_user.get(uid, ()))]
        if sends:
            await asyncio.gather(*sends)

    async def playlist_changed(self, playlist_id, extra_users=()):
        p = self.db.playlist_row(playlist_id)
        members = self.db.members(playlist_id) if p else []
        message = {"type": "playlist", "id": playlist_id, "version": p["version"] if p else 0}
        await self.to_users(members, message)
        if extra_users:
            await self.to_users(extra_users, {"type": "playlist.removed", "id": playlist_id})

    async def sign_out(self, user_id, device_ids=None):
        """Closes the sockets of signed-out devices (all of the account's with device_ids None)."""
        closing = [c for c in list(self.by_user.get(user_id, ())) if device_ids is None or c.device in device_ids]
        for c in closing:
            await c.ws.close(code=4001, message=b"signed out")

    def room_connections(self, room):
        return [c for uid in room.members for c in self.by_user.get(uid, ()) if c.room is room]

    async def broadcast_room(self, room):
        message = {"type": "room", "room": room.state(), "serverTime": server_time()}
        sends = [c.send(message) for c in self.room_connections(room)]
        if sends:
            await asyncio.gather(*sends)


# ---- REST: accounts -------------------------------------------------------------


async def health(request):
    return web.json_response({"ok": True, "rooms": len(request.app[ROOMS].rooms), "time": server_time()})


async def register(request):
    if not request.app[LIMITS]["register"].allow(client_ip(request)):
        return error(429, "rate_limited", "too many new accounts from this address, try later")
    body = await read_json(request)
    name = clean_name(body.get("name")) or "Listener"
    user, token = request.app[DB].create_user(name)
    return web.json_response({"user": user, "token": token})


@require_user
async def me(request):
    return web.json_response({"user": request[USER]})


@require_user
async def update_me(request):
    body = await read_json(request)
    name = clean_name(body.get("name"))
    if not name:
        return error(400, "bad_request", "name is required")
    user = request[USER]
    request.app[DB].rename_user(user["id"], name)
    user["name"] = name
    await _profile_changed(request, user)
    return web.json_response({"user": user})


MAX_AVATAR = 256 * 1024
AVATAR_TYPES = ((b"\xff\xd8\xff", "image/jpeg"), (b"\x89PNG\r\n\x1a\n", "image/png"))


@require_user
async def set_avatar(request):
    """The profile picture (a JPEG or PNG, as sent), shown to the people in the user's rooms and playlists."""
    if request.content_length and request.content_length > MAX_AVATAR:
        return error(413, "too_large", "the picture is too large")
    image = await request.read()
    if len(image) > MAX_AVATAR:
        return error(413, "too_large", "the picture is too large")
    kind = next((t for magic, t in AVATAR_TYPES if image.startswith(magic)), None)
    if not kind:
        return error(400, "bad_image", "a JPEG or PNG picture is expected")
    user = request[USER]
    user["avatar"] = request.app[DB].set_avatar(user["id"], image, kind)
    await _profile_changed(request, user)
    return web.json_response({"user": user})


@require_user
async def delete_avatar(request):
    user = request[USER]
    user["avatar"] = request.app[DB].set_avatar(user["id"], None)
    await _profile_changed(request, user)
    return web.json_response({"user": user})


async def user_avatar(request):
    row = request.app[DB].avatar(request.match_info["id"])
    if not row:
        return error(404, "not_found", "no picture")
    # the address carries the version (?v=), so a changed picture is a new address
    cache = "public, max-age=31536000, immutable" if request.query.get("v") == str(row["updated"]) else "no-cache"
    return web.Response(body=row["image"], content_type=row["type"], headers={"Cache-Control": cache})


async def _profile_changed(request, user):
    """The user's name or picture changed: their open connections, rooms and shared playlists show the new one."""
    hub = request.app[HUB]
    for conn in list(hub.by_user.get(user["id"], ())):
        conn.user["name"] = user["name"]
        conn.user["avatar"] = user.get("avatar", 0)
    for room in list(request.app[ROOMS].rooms.values()):
        if room.update_member(user):
            await hub.broadcast_room(room)
    for playlist_id in request.app[DB].touch_playlists_of(user["id"]):
        await hub.playlist_changed(playlist_id)


@require_user
async def delete_me(request):
    user = request[USER]
    db, hub = request.app[DB], request.app[HUB]
    member_of = [p["id"] for p in db.playlists_of(user["id"])]
    owned = db.delete_user(user["id"])
    for pid in member_of:
        if pid in owned:
            continue
        await hub.playlist_changed(pid)
    return web.json_response({"deleted": True})


# ---- REST: the library and paired devices -------------------------------------


@require_user
async def get_library(request):
    return web.json_response(request.app[DB].library(request[USER]["id"]))


async def _library_changed(request, version):
    # the account's other devices fetch it again
    await request.app[HUB].to_users([request[USER]["id"]], {"type": "library", "version": version})
    return web.json_response({"version": version})


@require_user
async def like_track(request):
    if not request.app[LIMITS]["library"].allow(request[USER]["id"]):
        return error(429, "rate_limited", "too many changes, try again later")
    body = await read_json(request)
    # {"track", "at"?} or {"items": [{"track", "at"?}, …]} — "at" (ms) places likes made before
    raw = body.get("items") if isinstance(body.get("items"), list) else [body]
    latest = time.time() * 1000 + 60_000
    items = []
    for item in raw[:MAX_TRACKS_PER_REQUEST]:
        if not isinstance(item, dict):
            continue
        at = item.get("at")
        at = int(at) if isinstance(at, (int, float)) and 0 < at < latest else None
        try:
            items.append((clean_track(item.get("track")), at))
        except TrackError as e:
            if len(raw) == 1:
                return error(400, "bad_track", str(e))
    if not items:
        return error(400, "bad_track", "no track")
    try:
        version = request.app[DB].like_many(request[USER]["id"], items)
    except LimitError as e:
        return error(409, "limit", str(e))
    return await _library_changed(request, version)


@require_user
async def unlike_track(request):
    if not request.app[LIMITS]["library"].allow(request[USER]["id"]):
        return error(429, "rate_limited", "too many changes, try again later")
    version = request.app[DB].set_liked(request[USER]["id"], request.match_info["track"], False)
    return await _library_changed(request, version)


@require_user
async def add_history(request):
    if not request.app[LIMITS]["library"].allow(request[USER]["id"]):
        return error(429, "rate_limited", "too many changes, try again later")
    body = await read_json(request)
    try:
        version = request.app[DB].add_history(request[USER]["id"], clean_track(body.get("track")))
    except TrackError as e:
        return error(400, "bad_track", str(e))
    return await _library_changed(request, version)


@require_user
async def clear_history(request):
    return await _library_changed(request, request.app[DB].clear_history(request[USER]["id"]))


def clean_organizer(body):
    """{"folders": [{"id", "name"}], "assign": {playlist key: folder id}, "hidden": [playlist key],
    "order": [playlist key] (the user's order, dragged into place), "updated": ms}
    — playlist keys as the apps make them ("yt:<id>", "sp:<id>", "shared:<id>", "user:<id>", "liked"…)."""
    def key(value):
        return value.strip()[:160] if isinstance(value, str) and value.strip() else None
    folders, ids = [], set()
    for f in body.get("folders") if isinstance(body.get("folders"), list) else []:
        if not isinstance(f, dict) or len(folders) >= 100:
            continue
        fid, name = key(f.get("id")), clean_name(f.get("name"), 60)
        if fid and name and fid not in ids:
            ids.add(fid)
            folders.append({"id": fid[:40], "name": name})
    assign = {}
    raw = body.get("assign") if isinstance(body.get("assign"), dict) else {}
    for k, v in list(raw.items())[:3000]:
        if key(k) and isinstance(v, str) and v[:40] in ids:
            assign[key(k)] = v[:40]
    hidden = []
    for k in (body.get("hidden") if isinstance(body.get("hidden"), list) else [])[:3000]:
        if key(k) and key(k) not in hidden:
            hidden.append(key(k))
    order = []
    for k in (body.get("order") if isinstance(body.get("order"), list) else [])[:3000]:
        if key(k) and key(k) not in order:
            order.append(key(k))
    updated = body.get("updated")
    updated = int(updated) if isinstance(updated, (int, float)) and updated > 0 else int(time.time() * 1000)
    return {"folders": folders, "assign": assign, "hidden": hidden, "order": order, "updated": updated}


@require_user
async def put_organizer(request):
    if not request.app[LIMITS]["library"].allow(request[USER]["id"]):
        return error(429, "rate_limited", "too many changes, try again later")
    data = clean_organizer(await read_json(request))
    version = request.app[DB].set_organizer(request[USER]["id"], data)
    return await _library_changed(request, version)


@require_user
async def create_pair_code(request):
    if not request.app[LIMITS]["pair"].allow(request[USER]["id"]):
        return error(429, "rate_limited", "too many codes, try again later")
    code, expires = request.app[DB].create_pair_code(request[USER]["id"])
    return web.json_response({"code": code, "expires": expires})


async def redeem_pair_code(request):
    if not request.app[LIMITS]["pair"].allow(client_ip(request)):
        return error(429, "rate_limited", "too many attempts, try again later")
    body = await read_json(request)
    result = request.app[DB].redeem_pair_code(body.get("code"), request.headers.get("X-Nokturn-Device"))
    if not result:
        return error(404, "bad_code", "the code is wrong or has expired")
    user, token = result
    await _leave_device_account(request, user["id"])
    return web.json_response({"user": user, "token": token})


# ---- REST: signing in with a login and a password ----------------------------------
#
# An account starts on one device with no login (the token is all there is); giving it a login and a password
# makes it one to sign in to from any device. A device signing in with an account of its own that has no login
# brings everything of it along, so nothing made there is lost.


def _account_reply(user, **extra):
    return web.json_response({"user": {"id": user["id"], "name": user["name"], "login": user["login"]}, **extra})


async def _leave_device_account(request, target_id):
    """The device that just signed in to target_id: an account of its own with no login joins that one; one
    with a login (another account) only loses this device."""
    user, device = current_session(request)
    if not user or user["id"] == target_id:
        return
    db, hub = request.app[DB], request.app[HUB]
    if user["login"]:
        db.sign_out(user["id"], device)
        await hub.sign_out(user["id"], {device})
        return
    for playlist_id in db.merge_users(user["id"], target_id):
        await hub.playlist_changed(playlist_id)
    await hub.sign_out(user["id"])
    await hub.to_users([target_id], {"type": "library", "version": db.library_version(target_id)})


def _login_limited(request, login):
    limits = request.app[LIMITS]
    return not (limits["login_ip"].allow(client_ip(request)) and limits["login_name"].allow(login))


async def create_account(request):
    """{login, password, name?}: with a token of an account that has no login, gives it this login; without
    one, a new account. Answers with a recovery key, shown to the person once."""
    body = await read_json(request)
    login = clean_login(body.get("login"))
    if not login:
        return error(400, "bad_login", "a login has 3-32 letters, digits, dots, dashes or underscores")
    if not good_password(body.get("password")):
        return error(400, "bad_password", "a password has at least 8 characters")
    user, _device = current_session(request)
    if user and user["login"]:
        return error(409, "has_login", "this account already has a login")
    if not user and not request.app[LIMITS]["register"].allow(client_ip(request)):
        return error(429, "rate_limited", "too many new accounts from this address, try later")
    db = request.app[DB]
    if db.account_by_login(login):
        return error(409, "login_taken", "someone already has this login")
    recovery = new_recovery_key()
    password_hash = await asyncio.to_thread(hash_secret, body["password"])
    recovery_hash = await asyncio.to_thread(hash_secret, clean_recovery_key(recovery))
    try:
        if user:
            db.set_login(user["id"], login, password_hash, recovery_hash)
            return _account_reply(dict(user, login=login), recovery=recovery)
        name = clean_name(body.get("name")) or login
        user, token = db.create_user(name, request.headers.get("X-Nokturn-Device"), login, password_hash, recovery_hash)
    except LoginTaken:
        return error(409, "login_taken", "someone already has this login")
    return _account_reply(user, token=token, recovery=recovery)


async def sign_in(request):
    """{login, password}: a token for this device."""
    body = await read_json(request)
    login = clean_login(body.get("login")) or ""
    if _login_limited(request, login):
        return error(429, "rate_limited", "too many attempts, try again in a few minutes")
    db = request.app[DB]
    account = db.account_by_login(login) if login else None
    ok = await asyncio.to_thread(check_secret, account["password"] if account else "", body.get("password"))
    if not ok:
        return error(401, "bad_login", "wrong login or password")
    await _leave_device_account(request, account["id"])
    token = db.sign_in(account["id"], request.headers.get("X-Nokturn-Device"))
    return _account_reply(account, token=token)


async def recover_account(request):
    """{login, recovery, password}: a new password with the recovery key. The other devices are signed out
    and the key is replaced by a new one."""
    body = await read_json(request)
    login = clean_login(body.get("login")) or ""
    if _login_limited(request, login):
        return error(429, "rate_limited", "too many attempts, try again in a few minutes")
    if not good_password(body.get("password")):
        return error(400, "bad_password", "a password has at least 8 characters")
    db = request.app[DB]
    account = db.account_by_login(login) if login else None
    ok = await asyncio.to_thread(check_secret, account["recovery"] if account else "",
                                 clean_recovery_key(body.get("recovery")))
    if not ok:
        return error(401, "bad_recovery", "wrong login or recovery key")
    recovery = new_recovery_key()
    password_hash = await asyncio.to_thread(hash_secret, body["password"])
    recovery_hash = await asyncio.to_thread(hash_secret, clean_recovery_key(recovery))
    await _leave_device_account(request, account["id"])
    db.set_account_password(account["id"], password_hash, recovery_hash)
    await request.app[HUB].sign_out(account["id"])
    token = db.sign_in(account["id"], request.headers.get("X-Nokturn-Device"))
    return _account_reply(account, token=token, recovery=recovery)


async def _check_own_password(request, body):
    """None when the body's "password" is the account's own one, else the error to answer with."""
    user = request[USER]
    if not user["login"]:
        return error(409, "no_login", "this account has no login yet")
    if _login_limited(request, user["login"]):
        return error(429, "rate_limited", "too many attempts, try again in a few minutes")
    account = request.app[DB].account(user["id"])
    if not await asyncio.to_thread(check_secret, account["password"], body.get("password")):
        return error(403, "wrong_password", "wrong password")
    return None


@require_user
async def change_password(request):
    """{password, new}: the other devices are signed out."""
    body = await read_json(request)
    if not good_password(body.get("new")):
        return error(400, "bad_password", "a password has at least 8 characters")
    refused = await _check_own_password(request, body)
    if refused:
        return refused
    user, db = request[USER], request.app[DB]
    others = {d["id"] for d in db.devices(user["id"]) if d["id"] != request[DEVICE]}
    db.set_account_password(user["id"], await asyncio.to_thread(hash_secret, body["new"]), keep_device=request[DEVICE])
    await request.app[HUB].sign_out(user["id"], others)
    return web.json_response({"ok": True, "signedOut": len(others)})


@require_user
async def new_recovery(request):
    """{password}: a new recovery key in place of the old one."""
    body = await read_json(request)
    refused = await _check_own_password(request, body)
    if refused:
        return refused
    recovery = new_recovery_key()
    request.app[DB].set_recovery(request[USER]["id"], await asyncio.to_thread(hash_secret, clean_recovery_key(recovery)))
    return web.json_response({"recovery": recovery})


@require_user
async def list_devices(request):
    devices = request.app[DB].devices(request[USER]["id"])
    for d in devices:
        d["current"] = d["id"] == request[DEVICE]
    return web.json_response({"devices": devices})


@require_user
async def sign_out_device(request):
    user, device_id = request[USER], request.match_info["device"]
    if not user["login"] and device_id == request[DEVICE]:
        return error(409, "no_login", "this account has no login: signing out would lose it")
    if not request.app[DB].sign_out(user["id"], device_id):
        return error(404, "not_found", "no such device")
    await request.app[HUB].sign_out(user["id"], {device_id})
    return web.json_response({"ok": True})


@require_user
async def sign_out(request):
    """This device signs out (an account with no login can't: it would be gone)."""
    user = request[USER]
    if not user["login"]:
        return error(409, "no_login", "this account has no login: signing out would lose it")
    request.app[DB].sign_out(user["id"], request[DEVICE])
    await request.app[HUB].sign_out(user["id"], {request[DEVICE]})
    return web.json_response({"ok": True})


@require_user
async def delete_me(request):
    user = request[USER]
    db, hub = request.app[DB], request.app[HUB]
    member_of = [p["id"] for p in db.playlists_of(user["id"])]
    owned = db.delete_user(user["id"])
    for pid in member_of:
        if pid in owned:
            continue
        await hub.playlist_changed(pid)
    return web.json_response({"deleted": True})


# ---- REST: shared tracks --------------------------------------------------------


@require_user
async def create_share(request):
    user = request[USER]
    if not request.app[LIMITS]["share"].allow(user["id"]):
        return error(429, "rate_limited", "too many links, try again later")
    body = await read_json(request)
    try:
        track = clean_track(body.get("track"))
    except TrackError as e:
        return error(400, "bad_track", str(e))
    code = request.app[DB].create_share(user["id"], track)
    return web.json_response({"code": code, "url": f'{request.app[CONFIG].public_url}/t/{code}'})


async def get_share(request):
    share = request.app[DB].get_share(request.match_info["code"], count_open=True)
    if not share:
        return error(404, "not_found", "no such link")
    return web.json_response(share)


# ---- REST: shared playlists -----------------------------------------------------


MAX_COVER = 400 * 1024
COVER_TYPES = ((b"\xff\xd8\xff", "image/jpeg"), (b"\x89PNG\r\n\x1a\n", "image/png"))


def playlist_json(request, p):
    p["url"] = f'{request.app[CONFIG].public_url}/p/{p["id"]}'
    version = p.pop("coverVersion", 0)
    p["cover"] = f'{request.app[CONFIG].public_url}/api/v1/playlists/{p["id"]}/cover?v={version}' if version else ""
    return p


@require_user
async def list_playlists(request):
    playlists = request.app[DB].playlists_of(request[USER]["id"])
    return web.json_response({"playlists": [playlist_json(request, p) for p in playlists]})


@require_user
async def create_playlist(request):
    user = request[USER]
    if not request.app[LIMITS]["playlist"].allow(user["id"]):
        return error(429, "rate_limited", "too many new playlists, try again later")
    body = await read_json(request)
    name = clean_name(body.get("name"), 100)
    if not name:
        return error(400, "bad_request", "name is required")
    description = clean_name(body.get("description"), 500)
    db = request.app[DB]
    try:
        playlist_id = db.create_playlist(user["id"], name, description, clean_tracks(body.get("tracks")),
                                         clean_name(body.get("origin"), 100))
    except LimitError as e:
        return error(403, "limit", str(e))
    return web.json_response(playlist_json(request, db.playlist(playlist_id)))


def _playlist_or_404(request):
    p = request.app[DB].playlist_row(request.match_info["id"])
    if not p:
        raise web.HTTPNotFound(text='{"error":"not_found","message":"no such playlist"}', content_type="application/json")
    return p


@require_user
async def get_playlist(request):
    p = _playlist_or_404(request)
    db = request.app[DB]
    if not db.is_member(p["id"], request[USER]["id"]):
        # Not joined yet: what the invite is about, without the tracks.
        preview = db.playlist(p["id"], with_tracks=False)
        preview["member"] = False
        if preview["protected"]:
            preview.update(name="", description="", count=0, members=[], coverVersion=0)
        return web.json_response(playlist_json(request, preview))
    out = db.playlist(p["id"])
    out["member"] = True
    return web.json_response(playlist_json(request, out))


@require_user
async def join_playlist(request):
    p = _playlist_or_404(request)
    db = request.app[DB]
    user_id = request[USER]["id"]
    if p["password"] and not db.is_member(p["id"], user_id):
        body = await read_json(request) if request.content_length else {}
        password = body.get("password") if isinstance(body.get("password"), str) else ""
        if not password:
            return error(403, "password_required", "this playlist has a password")
        limits = request.app[LIMITS]["password"]
        if not (limits.allow(user_id) and limits.allow(client_ip(request))):
            return error(429, "rate_limited", "too many tries, try again later")
        if not db.check_password(p["id"], password):
            return error(403, "bad_password", "wrong password")
    try:
        joined = db.join_playlist(p["id"], request[USER]["id"])
    except LimitError as e:
        return error(403, "limit", str(e))
    if joined:
        await request.app[HUB].playlist_changed(p["id"])
    out = db.playlist(p["id"])
    out["member"] = True
    return web.json_response(playlist_json(request, out))


@require_user
async def leave_playlist(request):
    p = _playlist_or_404(request)
    user = request[USER]
    db, hub = request.app[DB], request.app[HUB]
    if p["owner_id"] == user["id"]:
        return error(400, "owner", "the owner deletes the playlist instead of leaving it")
    db.leave_playlist(p["id"], user["id"])
    await hub.playlist_changed(p["id"], extra_users=[user["id"]])
    return web.json_response({"left": True})


@require_user
async def replace_playlist_tracks(request):
    p = _playlist_or_404(request)
    if p["owner_id"] != request[USER]["id"]:
        return error(403, "owner_only", "only the owner can replace the tracks")
    body = await read_json(request)
    db = request.app[DB]
    db.replace_tracks(p["id"], request[USER]["id"], clean_tracks(body.get("tracks")))
    await request.app[HUB].playlist_changed(p["id"])
    return web.json_response(playlist_json(request, db.playlist(p["id"])))


@require_user
async def update_playlist(request):
    p = _playlist_or_404(request)
    if p["owner_id"] != request[USER]["id"]:
        return error(403, "owner_only", "only the owner can rename the playlist")
    body = await read_json(request)
    name = clean_name(body["name"], 100) if "name" in body else None
    if name == "":
        return error(400, "bad_request", "name is required")
    description = clean_name(body["description"], 500) if "description" in body else None
    db = request.app[DB]
    if "password" in body:
        password = body["password"] if isinstance(body["password"], str) else ""
        if len(password) > 100:
            return error(400, "bad_request", "the password is too long")
        db.set_password(p["id"], password)
    db.update_playlist(p["id"], name, description)
    await request.app[HUB].playlist_changed(p["id"])
    return web.json_response(playlist_json(request, db.playlist(p["id"])))


@require_user
async def set_playlist_cover(request):
    """A member's picture for the playlist (a JPEG or PNG, as sent): the cover every member sees."""
    p = _playlist_or_404(request)
    _require_member(request, p)
    if request.content_length and request.content_length > MAX_COVER:
        return error(413, "too_large", "the picture is too large")
    image = await request.read()
    kind = next((t for magic, t in COVER_TYPES if image.startswith(magic)), None)
    if len(image) > MAX_COVER:
        return error(413, "too_large", "the picture is too large")
    if not kind:
        return error(400, "bad_image", "a JPEG or PNG picture is expected")
    db = request.app[DB]
    db.set_cover(p["id"], image, kind)
    await request.app[HUB].playlist_changed(p["id"])
    return web.json_response(playlist_json(request, db.playlist(p["id"])))


@require_user
async def delete_playlist_cover(request):
    p = _playlist_or_404(request)
    _require_member(request, p)
    db = request.app[DB]
    db.set_cover(p["id"], None)
    await request.app[HUB].playlist_changed(p["id"])
    return web.json_response(playlist_json(request, db.playlist(p["id"])))


async def playlist_cover(request):
    row = request.app[DB].cover(request.match_info["id"])
    if not row:
        return error(404, "not_found", "the playlist has no picture of its own")
    # the address carries the version (?v=), so a changed picture is a new address
    cache = "public, max-age=31536000, immutable" if request.query.get("v") == str(row["updated"]) else "no-cache"
    return web.Response(body=row["image"], content_type=row["type"], headers={"Cache-Control": cache})


@require_user
async def delete_playlist(request):
    p = _playlist_or_404(request)
    if p["owner_id"] != request[USER]["id"]:
        return error(403, "owner_only", "only the owner can delete the playlist")
    db = request.app[DB]
    members = db.members(p["id"])
    db.delete_playlist(p["id"])
    await request.app[HUB].to_users(members, {"type": "playlist.removed", "id": p["id"]})
    return web.json_response({"deleted": True})


def _require_member(request, p):
    if not request.app[DB].is_member(p["id"], request[USER]["id"]):
        raise web.HTTPForbidden(text='{"error":"not_member","message":"join the playlist first"}',
                                content_type="application/json")


@require_user
async def add_playlist_tracks(request):
    p = _playlist_or_404(request)
    _require_member(request, p)
    body = await read_json(request)
    tracks = clean_tracks(body.get("tracks"))
    if not tracks:
        return error(400, "bad_track", "no tracks that can be shared (YouTube Music or SoundCloud)")
    db = request.app[DB]
    try:
        added = db.add_tracks(p["id"], request[USER]["id"], tracks)
    except LimitError as e:
        return error(403, "limit", str(e))
    if added:
        await request.app[HUB].playlist_changed(p["id"])
    out = playlist_json(request, db.playlist(p["id"]))
    out["added"] = added
    return web.json_response(out)


@require_user
async def remove_playlist_track(request):
    p = _playlist_or_404(request)
    _require_member(request, p)
    db = request.app[DB]
    if db.remove_track(p["id"], request.match_info["entry"]):
        await request.app[HUB].playlist_changed(p["id"])
    return web.json_response(playlist_json(request, db.playlist(p["id"])))


@require_user
async def move_playlist_track(request):
    p = _playlist_or_404(request)
    _require_member(request, p)
    body = await read_json(request)
    to = body.get("to")
    if not isinstance(to, int):
        return error(400, "bad_request", "'to' must be a number")
    db = request.app[DB]
    if db.move_track(p["id"], request.match_info["entry"], to):
        await request.app[HUB].playlist_changed(p["id"])
    return web.json_response(playlist_json(request, db.playlist(p["id"])))


# ---- REST: rooms (previews; everything else goes over the WebSocket) ------------


async def get_room(request):
    room = request.app[ROOMS].get(request.match_info["code"])
    if not room:
        return error(404, "not_found", "no such room")
    out = room.preview()
    out["url"] = f'{request.app[CONFIG].public_url}/r/{room.code}'
    return web.json_response(out)


# ---- WebSocket ----------------------------------------------------------------


async def websocket(request):
    user, device = current_session(request)
    if not user:
        user, device = ticket_session(request)
    if not user:
        return error(401, "unauthorized", "sign in first")
    ws = web.WebSocketResponse(heartbeat=25, max_msg_size=MAX_WS_MESSAGE)
    await ws.prepare(request)
    hub = request.app[HUB]
    conn = Connection(ws, user, device)
    hub.add(conn)
    await conn.send({"type": "hello", "user": user, "serverTime": server_time()})
    try:
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                if msg.type == WSMsgType.ERROR:
                    break
                continue
            if not conn.limiter.allow(0):
                await conn.send({"type": "error", "error": "rate_limited", "message": "slow down"})
                continue
            try:
                data = json.loads(msg.data)
            except json.JSONDecodeError:
                continue
            if not isinstance(data, dict):
                continue
            try:
                await handle_message(request.app, conn, data)
            except RoomError as e:
                await conn.send({"type": "error", "error": e.code, "message": str(e), "request": data.get("type")})
            except (TypeError, ValueError, KeyError) as e:
                await conn.send({"type": "error", "error": "bad_request", "message": str(e), "request": data.get("type")})
    finally:
        hub.remove(conn)
        if conn.room:
            room = conn.room
            conn.room = None
            room.disconnect(user["id"])
            await hub.broadcast_room(room)
    return ws


async def handle_message(app, conn, data):
    kind = data.get("type")
    hub, rooms = app[HUB], app[ROOMS]
    user_id = conn.user["id"]

    if kind == "ping":
        await conn.send({"type": "pong", "t": data.get("t"), "serverTime": server_time()})
        return

    if kind == "room.create":
        if not app[LIMITS]["room"].allow(user_id):
            raise RoomError("rate_limited", "too many new rooms, try again later")
        name = clean_name(data.get("name"), 60) or f'{conn.user["name"]}\'s room'
        room = rooms.create(conn.user, name, data.get("mode", "open"))
        await _enter_room(hub, conn, room)
        return

    if kind == "room.join":
        room = rooms.get(data.get("code"))
        if not room:
            raise RoomError("no_room", "this room doesn't exist anymore")
        await _enter_room(hub, conn, room)
        return

    room = conn.room
    if room is None:
        raise RoomError("no_room", "you are not in a room")

    if kind == "room.leave":
        conn.room = None
        room.leave(user_id)
        await conn.send({"type": "room.left", "code": room.code})
        await hub.broadcast_room(room)
        return

    position = data.get("position")
    position = float(position) if isinstance(position, (int, float)) else None

    if kind == "room.add":
        tracks = clean_tracks(data.get("tracks"), 100)
        if not tracks:
            raise RoomError("bad_track", "only YouTube Music and SoundCloud tracks can go into a room")
        room.add(user_id, tracks, play_next=bool(data.get("next")))
    elif kind == "room.remove":
        room.remove(user_id, str(data.get("itemId")))
    elif kind == "room.move":
        room.move(user_id, str(data.get("itemId")), int(data.get("to")))
    elif kind == "room.shuffle":
        room.shuffle(user_id)
    elif kind == "room.clear":
        room.clear(user_id)
    elif kind == "room.play":
        room.play_item(user_id, str(data.get("itemId")))
    elif kind == "room.pause":
        room.pause(user_id, position)
    elif kind == "room.resume":
        room.resume(user_id)
    elif kind == "room.seek":
        if position is None:
            raise RoomError("bad_request", "position is required")
        room.seek(user_id, position)
    elif kind == "room.next":
        room.next(user_id)
    elif kind == "room.previous":
        room.previous(user_id)
    elif kind == "room.ended":
        if not room.ended(user_id, str(data.get("itemId"))):
            return
    elif kind == "room.mode":
        room.set_mode(user_id, data.get("mode"))
    elif kind == "room.host":
        room.set_host(user_id, str(data.get("userId")))
    elif kind == "room.rename":
        name = clean_name(data.get("name"), 60)
        if not name:
            raise RoomError("bad_request", "name is required")
        room.rename(user_id, name)
    elif kind == "room.kick":
        target = str(data.get("userId"))
        room.kick(user_id, target)
        for c in list(hub.by_user.get(target, ())):
            if c.room is room:
                c.room = None
                await c.send({"type": "room.left", "code": room.code, "reason": "kicked"})
    else:
        raise RoomError("bad_request", f"unknown message type {kind!r}")
    await hub.broadcast_room(room)


async def _enter_room(hub, conn, room):
    if conn.room is room:
        await conn.send({"type": "room", "room": room.state(), "serverTime": server_time()})
        return
    if conn.room is not None:
        previous = conn.room
        conn.room = None
        # Another connection of the same person may still be in the old room.
        if any(c.room is previous for c in hub.by_user.get(conn.user["id"], ())):
            previous.disconnect(conn.user["id"])
        else:
            previous.leave(conn.user["id"])
        await hub.broadcast_room(previous)
    conn.room = room
    room.join(conn.user)
    await hub.broadcast_room(room)


# ---- link pages -----------------------------------------------------------------


def _html(text, status=200):
    return web.Response(text=text, status=status, content_type="text/html", charset="utf-8",
                        headers={"Cache-Control": "no-cache", "X-Content-Type-Options": "nosniff",
                                 "Referrer-Policy": "no-referrer",
                                 "Content-Security-Policy": "default-src 'none'; img-src https: http: data:; "
                                                            "style-src 'unsafe-inline'"})


async def track_page(request):
    lang = pages.language(request)
    code = request.match_info["code"]
    url = f'{request.app[CONFIG].public_url}/t/{code}'
    share = request.app[DB].get_share(code)
    if not share:
        return _html(pages.missing_page(lang, "track", url), 404)
    return _html(pages.track_page(lang, share, url))


async def room_page(request):
    lang = pages.language(request)
    code = request.match_info["code"].upper()
    url = f'{request.app[CONFIG].public_url}/r/{code}'
    room = request.app[ROOMS].get(code)
    if not room:
        return _html(pages.missing_page(lang, "room", url), 404)
    return _html(pages.room_page(lang, room.preview(), url))


async def playlist_page(request):
    lang = pages.language(request)
    playlist_id = request.match_info["id"]
    url = f'{request.app[CONFIG].public_url}/p/{playlist_id}'
    db = request.app[DB]
    p = db.playlist(playlist_id, with_tracks=False)
    if not p:
        return _html(pages.missing_page(lang, "playlist", url), 404)
    covers = [] if p["protected"] else db.preview_covers(playlist_id)
    return _html(pages.playlist_page(lang, p, covers, url))


async def home(request):
    return _html(pages.home_page(pages.language(request), request.app[CONFIG].public_url))


# ---- the app ------------------------------------------------------------------


async def housekeeping(app):
    hub, rooms = app[HUB], app[ROOMS]
    ticks = 0
    while True:
        await asyncio.sleep(1.0)
        try:
            for room in rooms.sweep():
                await hub.broadcast_room(room)
            ticks += 1
            if ticks % 600 == 0:
                for limiter in app[LIMITS].values():
                    limiter.prune()
        except Exception:   # keep the loop alive whatever happens to one room
            log.exception("housekeeping failed")


async def _start_background(app):
    app[HOUSEKEEPING] = asyncio.create_task(housekeeping(app))


async def _stop_background(app):
    app[HOUSEKEEPING].cancel()
    try:
        await app[HOUSEKEEPING]
    except asyncio.CancelledError:
        pass
    for conns in list(app[HUB].by_user.values()):
        for c in list(conns):
            await c.ws.close(code=1001, message=b"server shutdown")
    app[DB].close()


@web.middleware
async def json_errors(request, handler):
    try:
        return await handler(request)
    except web.HTTPException as e:
        if request.path.startswith(API) and e.content_type != "application/json":
            return error(e.status, "http_error", e.reason)
        raise


def create_app(config):
    app = web.Application(client_max_size=MAX_BODY, middlewares=[json_errors])
    app[CONFIG] = config
    app[DB] = Database(config.db_path)
    app[ROOMS] = Rooms()
    app[HUB] = Hub(app[DB], app[ROOMS])
    app[LIMITS] = {
        "register": RateLimiter(20, 3600),
        "share": RateLimiter(120, 3600),
        "playlist": RateLimiter(30, 3600),
        "room": RateLimiter(30, 3600),
        "library": RateLimiter(3000, 3600),
        "pair": RateLimiter(20, 3600),
        "login_ip": RateLimiter(30, 900),
        "login_name": RateLimiter(10, 900),
        "password": RateLimiter(10, 900),
    }
    r = app.router
    r.add_get(f"{API}/health", health)
    r.add_post(f"{API}/register", register)
    r.add_get(f"{API}/me", me)
    r.add_patch(f"{API}/me", update_me)
    r.add_delete(f"{API}/me", delete_me)
    r.add_put(f"{API}/me/avatar", set_avatar)
    r.add_delete(f"{API}/me/avatar", delete_avatar)
    r.add_get(f"{API}/users/{{id}}/avatar", user_avatar)
    r.add_post(f"{API}/shares", create_share)
    r.add_get(f"{API}/shares/{{code}}", get_share)
    r.add_get(f"{API}/playlists", list_playlists)
    r.add_post(f"{API}/playlists", create_playlist)
    r.add_get(f"{API}/playlists/{{id}}", get_playlist)
    r.add_patch(f"{API}/playlists/{{id}}", update_playlist)
    r.add_delete(f"{API}/playlists/{{id}}", delete_playlist)
    r.add_get(f"{API}/playlists/{{id}}/cover", playlist_cover)
    r.add_put(f"{API}/playlists/{{id}}/cover", set_playlist_cover)
    r.add_delete(f"{API}/playlists/{{id}}/cover", delete_playlist_cover)
    r.add_post(f"{API}/playlists/{{id}}/join", join_playlist)
    r.add_post(f"{API}/playlists/{{id}}/leave", leave_playlist)
    r.add_post(f"{API}/playlists/{{id}}/tracks", add_playlist_tracks)
    r.add_put(f"{API}/playlists/{{id}}/tracks", replace_playlist_tracks)
    r.add_delete(f"{API}/playlists/{{id}}/tracks/{{entry}}", remove_playlist_track)
    r.add_post(f"{API}/playlists/{{id}}/tracks/{{entry}}/move", move_playlist_track)
    r.add_get(f"{API}/rooms/{{code}}", get_room)
    r.add_get(f"{API}/ws", websocket)
    r.add_post(f"{API}/ws/ticket", ws_ticket)
    r.add_get(f"{API}/library", get_library)
    r.add_post(f"{API}/library/liked", like_track)
    r.add_delete(f"{API}/library/liked/{{track}}", unlike_track)
    r.add_post(f"{API}/library/history", add_history)
    r.add_delete(f"{API}/library/history", clear_history)
    r.add_post(f"{API}/me/pair", create_pair_code)
    r.add_put(f"{API}/library/organizer", put_organizer)
    r.add_post(f"{API}/pair", redeem_pair_code)
    r.add_post(f"{API}/account", create_account)
    r.add_post(f"{API}/account/login", sign_in)
    r.add_post(f"{API}/account/recover", recover_account)
    r.add_post(f"{API}/account/password", change_password)
    r.add_post(f"{API}/account/recovery", new_recovery)
    r.add_post(f"{API}/account/logout", sign_out)
    r.add_get(f"{API}/account/devices", list_devices)
    r.add_delete(f"{API}/account/devices/{{device}}", sign_out_device)
    r.add_get("/", home)
    r.add_get("/t/{code}", track_page)
    r.add_get("/r/{code}", room_page)
    r.add_get("/p/{id}", playlist_page)
    app.on_startup.append(_start_background)
    app.on_cleanup.append(_stop_background)
    return app
