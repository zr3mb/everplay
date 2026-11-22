"""SQLite storage: accounts (and the devices signed in to them), the library (liked tracks, history), shared
tracks and playlists. Listening rooms live in memory."""

import hashlib
import hmac
import json
import re
import secrets
import sqlite3
import time

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    id          TEXT PRIMARY KEY,
    token_hash  TEXT NOT NULL UNIQUE,
    name        TEXT NOT NULL,
    created     INTEGER NOT NULL,
    seen        INTEGER NOT NULL,
    login       TEXT,
    password    TEXT NOT NULL DEFAULT '',
    recovery    TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS shares (
    code        TEXT PRIMARY KEY,
    track       TEXT NOT NULL,
    user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created     INTEGER NOT NULL,
    opens       INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS playlists (
    id          TEXT PRIMARY KEY,
    owner_id    TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    created     INTEGER NOT NULL,
    updated     INTEGER NOT NULL,
    version     INTEGER NOT NULL DEFAULT 1,
    origin      TEXT NOT NULL DEFAULT '',
    password    TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS playlist_members (
    playlist_id TEXT NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
    user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    joined      INTEGER NOT NULL,
    PRIMARY KEY (playlist_id, user_id)
);
CREATE INDEX IF NOT EXISTS playlist_members_user ON playlist_members(user_id);
-- The picture on a person's profile, shown to the people in their rooms and shared playlists.
CREATE TABLE IF NOT EXISTS user_avatars (
    user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    image BLOB NOT NULL,
    type TEXT NOT NULL,
    updated INTEGER NOT NULL
);
-- A picture a member chose for a shared playlist: everyone in it sees the same cover.
CREATE TABLE IF NOT EXISTS playlist_covers (
    playlist_id TEXT PRIMARY KEY REFERENCES playlists(id) ON DELETE CASCADE,
    image BLOB NOT NULL,
    type TEXT NOT NULL,
    updated INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS playlist_tracks (
    entry_id    TEXT PRIMARY KEY,
    playlist_id TEXT NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
    position    INTEGER NOT NULL,
    track_id    TEXT NOT NULL,
    track       TEXT NOT NULL,
    added_by    TEXT NOT NULL,
    added_at    INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS playlist_tracks_playlist ON playlist_tracks(playlist_id, position);
CREATE TABLE IF NOT EXISTS tokens (
    token_hash  TEXT PRIMARY KEY,
    user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created     INTEGER NOT NULL,
    id          TEXT,
    device      TEXT NOT NULL DEFAULT '',
    seen        INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS pair_codes (
    code        TEXT PRIMARY KEY,
    user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    expires     INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS library (
    user_id     TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    kind        TEXT NOT NULL,
    track_id    TEXT NOT NULL,
    track       TEXT NOT NULL,
    added_at    INTEGER NOT NULL,
    PRIMARY KEY (user_id, kind, track_id)
);
CREATE INDEX IF NOT EXISTS library_user ON library(user_id, kind, added_at);
CREATE TABLE IF NOT EXISTS library_settings (
    user_id     TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    data        TEXT NOT NULL,
    updated     INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS library_versions (
    user_id     TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
    version     INTEGER NOT NULL
);
"""

MAX_PLAYLIST_TRACKS = 2000
MAX_PLAYLISTS_PER_USER = 200
MAX_LIKED = 5000
MAX_HISTORY = 300
PAIR_CODE_SECONDS = 600
LOGIN = re.compile(r"^[a-z0-9][a-z0-9._-]{2,31}$")
PASSWORD_MIN, PASSWORD_MAX = 8, 200
_RECOVERY_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

# Codes in links: letters and digits only, so they survive any chat app.
_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789"


def new_code(length):
    return "".join(secrets.choice(_ALPHABET) for _ in range(length))


def now_ms():
    return int(time.time() * 1000)


def hash_token(token):
    return hashlib.sha256(token.encode()).hexdigest()


def clean_login(login):
    """A login: 3-32 small letters, digits, dots, dashes and underscores (any case typed is the same login)."""
    login = (login or "").strip().lower() if isinstance(login, str) else ""
    return login if LOGIN.match(login) else None


def good_password(password):
    return isinstance(password, str) and PASSWORD_MIN <= len(password) <= PASSWORD_MAX


def hash_secret(secret):
    """A salted scrypt hash (passwords of accounts and playlists, recovery keys). Slow on purpose: the server
    calls it off the event loop."""
    salt = secrets.token_bytes(16)
    digest = hashlib.scrypt(secret.encode(), salt=salt, n=2 ** 14, r=8, p=1, dklen=32)
    return f"scrypt${salt.hex()}${digest.hex()}"


_DUMMY_HASH = "scrypt$" + "00" * 16 + "$" + "00" * 32


def check_secret(stored, secret):
    """Whether the secret matches the hash; with no hash it still takes as long, so a missing account doesn't
    answer faster than a wrong password."""
    try:
        _kind, salt, digest = (stored or _DUMMY_HASH).split("$")
        given = hashlib.scrypt((secret or "").encode(), salt=bytes.fromhex(salt), n=2 ** 14, r=8, p=1, dklen=32)
    except ValueError:
        return False
    return bool(stored) and hmac.compare_digest(given.hex(), digest)


def new_recovery_key():
    """Shown once when the account gets its password: it sets a new password when the old one is forgotten."""
    raw = "".join(secrets.choice(_RECOVERY_ALPHABET) for _ in range(20))
    return "-".join(raw[i:i + 4] for i in range(0, 20, 4))


def clean_recovery_key(key):
    return "".join(c for c in (key or "").upper() if c in _RECOVERY_ALPHABET) if isinstance(key, str) else ""


def clean_device(name):
    text = "".join(c for c in str(name or "") if c.isprintable() and not 0xD800 <= ord(c) <= 0xDFFF)
    return " ".join(text.split())[:60]


class LoginTaken(Exception):
    pass


class LimitError(Exception):
    pass


class Database:
    def __init__(self, path):
        self.conn = sqlite3.connect(path, isolation_level=None, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA foreign_keys=ON")
        self.conn.executescript(SCHEMA)
        # Databases made before playlists had an origin (a copy of a YouTube Music playlist: "ytmusic:<id>").
        columns = {r["name"] for r in self.conn.execute("PRAGMA table_info(playlists)")}
        if "origin" not in columns:
            self.conn.execute("ALTER TABLE playlists ADD COLUMN origin TEXT NOT NULL DEFAULT ''")
        if "password" not in columns:   # and before they could have a password
            self.conn.execute("ALTER TABLE playlists ADD COLUMN password TEXT NOT NULL DEFAULT ''")
        # ... and before accounts had a login and a list of their devices
        columns = {r["name"] for r in self.conn.execute("PRAGMA table_info(users)")}
        if "login" not in columns:
            self.conn.execute("ALTER TABLE users ADD COLUMN login TEXT")
            self.conn.execute("ALTER TABLE users ADD COLUMN password TEXT NOT NULL DEFAULT ''")
            self.conn.execute("ALTER TABLE users ADD COLUMN recovery TEXT NOT NULL DEFAULT ''")
        columns = {r["name"] for r in self.conn.execute("PRAGMA table_info(tokens)")}
        if "device" not in columns:
            self.conn.execute("ALTER TABLE tokens ADD COLUMN id TEXT")
            self.conn.execute("ALTER TABLE tokens ADD COLUMN device TEXT NOT NULL DEFAULT ''")
            self.conn.execute("ALTER TABLE tokens ADD COLUMN seen INTEGER NOT NULL DEFAULT 0")
        self.conn.execute("CREATE UNIQUE INDEX IF NOT EXISTS users_login ON users(login)")
        # The first device's token used to sit in users itself; every device is in tokens now.
        with self._tx():
            for r in self.conn.execute("SELECT id, token_hash, created, seen FROM users "
                                       "WHERE token_hash NOT LIKE '-%'").fetchall():
                self.conn.execute("INSERT OR IGNORE INTO tokens (token_hash, user_id, created, seen) VALUES (?, ?, ?, ?)",
                                  (r["token_hash"], r["id"], r["created"], r["seen"]))
                self.conn.execute("UPDATE users SET token_hash = ? WHERE id = ?", ("-" + r["id"], r["id"]))
            for r in self.conn.execute("SELECT token_hash FROM tokens WHERE id IS NULL").fetchall():
                self.conn.execute("UPDATE tokens SET id = ? WHERE token_hash = ?", (new_code(10), r["token_hash"]))

    def close(self):
        self.conn.close()

    def _tx(self):
        return _Transaction(self.conn)

    # ---- users --------------------------------------------------------------

    def create_user(self, name, device="", login=None, password_hash="", recovery_hash=""):
        """A new account and the token of the device that made it. With a login (taken: LoginTaken) it's an
        account to sign in to; without one it lives only on that device until it gets a login."""
        user_id = new_code(12)
        t = now_ms()
        try:
            with self._tx():
                self.conn.execute("INSERT INTO users (id, token_hash, name, created, seen, login, password, recovery) "
                                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                                  (user_id, "-" + user_id, name, t, t, login, password_hash, recovery_hash))
                token = self._new_token(user_id, device)
        except sqlite3.IntegrityError:
            raise LoginTaken(login)
        return {"id": user_id, "name": name, "login": login, "avatar": 0}, token

    def _new_token(self, user_id, device=""):
        token = secrets.token_urlsafe(32)
        t = now_ms()
        self.conn.execute("INSERT INTO tokens (token_hash, user_id, created, id, device, seen) VALUES (?, ?, ?, ?, ?, ?)",
                          (hash_token(token), user_id, t, new_code(10), clean_device(device), t))
        return token

    def session(self, token, device=None):
        """(the account, the id of this device) for a device's token, or (None, None). A device that says what
        it is (device) gets that name in the account's list of devices."""
        row = self.conn.execute("SELECT u.id, u.name, u.login, u.seen, t.id AS device_id, t.device, t.seen AS device_seen, "
                                "COALESCE(a.updated, 0) AS avatar FROM tokens t JOIN users u ON u.id = t.user_id "
                                "LEFT JOIN user_avatars a ON a.user_id = u.id WHERE t.token_hash = ?",
                                (hash_token(token),)).fetchone()
        if not row:
            return None, None
        t = now_ms()
        if t - row["seen"] > 3600_000:
            self.conn.execute("UPDATE users SET seen = ? WHERE id = ?", (t, row["id"]))
        device = clean_device(device) if device else row["device"]
        if t - row["device_seen"] > 3600_000 or device != row["device"]:
            self.conn.execute("UPDATE tokens SET seen = ?, device = ? WHERE id = ?", (t, device, row["device_id"]))
        return {"id": row["id"], "name": row["name"], "login": row["login"], "avatar": row["avatar"]}, row["device_id"]

    def user_by_token(self, token):
        return self.session(token)[0]

    # ---- logins ---------------------------------------------------------------

    def account(self, user_id):
        row = self.conn.execute("SELECT id, name, login, password, recovery FROM users WHERE id = ?",
                                (user_id,)).fetchone()
        return dict(row) if row else None

    def account_by_login(self, login):
        row = self.conn.execute("SELECT id, name, login, password, recovery FROM users WHERE login = ?",
                                (login,)).fetchone()
        return dict(row) if row else None

    def set_login(self, user_id, login, password_hash, recovery_hash):
        """Gives an account made on a device a login and a password (LoginTaken if someone has that login)."""
        try:
            self.conn.execute("UPDATE users SET login = ?, password = ?, recovery = ? WHERE id = ?",
                              (login, password_hash, recovery_hash, user_id))
        except sqlite3.IntegrityError:
            raise LoginTaken(login)

    def set_account_password(self, user_id, password_hash, recovery_hash=None, keep_device=None):
        """A new password (and recovery key); every other device of the account is signed out."""
        with self._tx():
            if recovery_hash is None:
                self.conn.execute("UPDATE users SET password = ? WHERE id = ?", (password_hash, user_id))
            else:
                self.conn.execute("UPDATE users SET password = ?, recovery = ? WHERE id = ?",
                                  (password_hash, recovery_hash, user_id))
            self.conn.execute("DELETE FROM tokens WHERE user_id = ? AND id IS NOT ?", (user_id, keep_device))

    def set_recovery(self, user_id, recovery_hash):
        self.conn.execute("UPDATE users SET recovery = ? WHERE id = ?", (recovery_hash, user_id))

    def sign_in(self, user_id, device=""):
        """A token for a device signing in to the account."""
        return self._new_token(user_id, device)

    def devices(self, user_id):
        return [{"id": r["id"], "name": r["device"], "created": r["created"], "seen": r["seen"] or r["created"]}
                for r in self.conn.execute("SELECT id, device, created, seen FROM tokens WHERE user_id = ? "
                                           "ORDER BY seen DESC", (user_id,))]

    def sign_out(self, user_id, device_id):
        return self.conn.execute("DELETE FROM tokens WHERE user_id = ? AND id = ?", (user_id, device_id)).rowcount > 0

    def merge_users(self, source, target):
        """Moves everything of the account `source` (one made on a device that now signs in to `target`) into
        `target` and deletes it: its playlists, the shared ones it's in, its likes, history and shared tracks.
        Returns the ids of the playlists whose people changed."""
        if source == target:
            return []
        with self._tx():
            # A copy of the same YouTube Music / Spotify / SoundCloud playlist: the account's own one stays.
            self.conn.execute("DELETE FROM playlists WHERE owner_id = ? AND origin != '' AND origin IN "
                              "(SELECT origin FROM playlists WHERE owner_id = ? AND origin != '')", (source, target))
            changed = [r["playlist_id"] for r in self.conn.execute(
                "SELECT playlist_id FROM playlist_members WHERE user_id = ?", (source,))]
            self.conn.execute("UPDATE playlists SET owner_id = ? WHERE owner_id = ?", (target, source))
            self.conn.execute("INSERT OR IGNORE INTO playlist_members (playlist_id, user_id, joined) "
                              "SELECT playlist_id, ?, joined FROM playlist_members WHERE user_id = ?", (target, source))
            self.conn.execute("UPDATE playlist_tracks SET added_by = ? WHERE added_by = ?", (target, source))
            self.conn.execute("UPDATE shares SET user_id = ? WHERE user_id = ?", (target, source))
            self.conn.execute("INSERT INTO library (user_id, kind, track_id, track, added_at) "
                              "SELECT ?, kind, track_id, track, added_at FROM library WHERE user_id = ? "
                              "ON CONFLICT(user_id, kind, track_id) DO UPDATE SET added_at = MAX(added_at, excluded.added_at)",
                              (target, source))
            self.conn.execute("DELETE FROM library WHERE user_id = ? AND kind = 'history' AND track_id NOT IN "
                              "(SELECT track_id FROM library WHERE user_id = ? AND kind = 'history' "
                              "ORDER BY added_at DESC LIMIT ?)", (target, target, MAX_HISTORY))
            self.conn.execute("INSERT OR IGNORE INTO library_settings (user_id, data, updated) "
                              "SELECT ?, data, updated FROM library_settings WHERE user_id = ?", (target, source))
            for playlist_id in changed:
                self._touch(playlist_id)
            self.conn.execute("DELETE FROM users WHERE id = ?", (source,))
            self._bump_library(target)
        return changed

    # ---- devices: one account on several devices --------------------------------

    def create_pair_code(self, user_id):
        """A short code another device enters to sign in to this account; single use, ten minutes."""
        t = int(time.time())
        self.conn.execute("DELETE FROM pair_codes WHERE expires < ? OR user_id = ?", (t, user_id))
        code = "".join(secrets.choice("ABCDEFGHJKLMNPQRSTUVWXYZ23456789") for _ in range(8))
        self.conn.execute("INSERT INTO pair_codes (code, user_id, expires) VALUES (?, ?, ?)",
                          (code, user_id, t + PAIR_CODE_SECONDS))
        return code, t + PAIR_CODE_SECONDS

    def redeem_pair_code(self, code, device=""):
        """The account and a new token for the device that entered the code, or None."""
        code = (code or "").strip().upper().replace("-", "").replace(" ", "")
        with self._tx():
            row = self.conn.execute("SELECT user_id, expires FROM pair_codes WHERE code = ?", (code,)).fetchone()
            if not row or row["expires"] < time.time():
                return None
            self.conn.execute("DELETE FROM pair_codes WHERE code = ?", (code,))
            token = self._new_token(row["user_id"], device)
            user = self.conn.execute("SELECT id, name, login FROM users WHERE id = ?", (row["user_id"],)).fetchone()
        return {"id": user["id"], "name": user["name"], "login": user["login"]}, token

    # ---- the library ---------------------------------------------------------------

    def library_version(self, user_id):
        row = self.conn.execute("SELECT version FROM library_versions WHERE user_id = ?", (user_id,)).fetchone()
        return row["version"] if row else 0

    def _bump_library(self, user_id):
        self.conn.execute("INSERT INTO library_versions (user_id, version) VALUES (?, 1) "
                          "ON CONFLICT(user_id) DO UPDATE SET version = version + 1", (user_id,))
        return self.library_version(user_id)

    def library(self, user_id):
        def kind(k, limit):
            # "at": when it was liked / last played (ms) — the apps put other devices' plays in their history
            return [dict(json.loads(r["track"]), at=r["added_at"]) for r in self.conn.execute(
                "SELECT track, added_at FROM library WHERE user_id = ? AND kind = ? ORDER BY added_at DESC LIMIT ?",
                (user_id, k, limit))]
        return {"version": self.library_version(user_id), "liked": kind("liked", MAX_LIKED),
                "history": kind("history", MAX_HISTORY), "organizer": self.organizer(user_id)}

    def organizer(self, user_id):
        """How the apps lay the playlists out: folders, which playlist is in which, the hidden ones."""
        row = self.conn.execute("SELECT data FROM library_settings WHERE user_id = ?", (user_id,)).fetchone()
        return json.loads(row["data"]) if row else None

    def set_organizer(self, user_id, data):
        with self._tx():
            self.conn.execute("INSERT INTO library_settings (user_id, data, updated) VALUES (?, ?, ?) "
                              "ON CONFLICT(user_id) DO UPDATE SET data = excluded.data, updated = excluded.updated",
                              (user_id, json.dumps(data), now_ms()))
            return self._bump_library(user_id)

    def set_liked(self, user_id, track, liked):
        if liked:
            return self.like_many(user_id, [(track, None)])
        with self._tx():
            self.conn.execute("DELETE FROM library WHERE user_id = ? AND kind = 'liked' AND track_id = ?",
                              (user_id, track if isinstance(track, str) else track["id"]))
            return self._bump_library(user_id)

    def like_many(self, user_id, items):
        """[(track, at)]: at (ms) puts the like at its place in the list (an app sending likes made before, in
        their order) and moves one already there; without it the like is new and an existing one stays."""
        with self._tx():
            for track, at in items:
                if at:
                    self.conn.execute(
                        "INSERT INTO library (user_id, kind, track_id, track, added_at) VALUES (?, 'liked', ?, ?, ?) "
                        "ON CONFLICT(user_id, kind, track_id) DO UPDATE SET added_at = excluded.added_at",
                        (user_id, track["id"], json.dumps(track), at))
                else:
                    self.conn.execute("INSERT OR IGNORE INTO library (user_id, kind, track_id, track, added_at) "
                                      "VALUES (?, 'liked', ?, ?, ?)", (user_id, track["id"], json.dumps(track), now_ms()))
            count = self.conn.execute("SELECT COUNT(*) FROM library WHERE user_id = ? AND kind = 'liked'",
                                      (user_id,)).fetchone()[0]
            if count > MAX_LIKED:
                raise LimitError("too many liked tracks")   # nothing of it is kept
            return self._bump_library(user_id)

    def add_history(self, user_id, track):
        with self._tx():
            self.conn.execute("INSERT INTO library (user_id, kind, track_id, track, added_at) VALUES (?, 'history', ?, ?, ?) "
                              "ON CONFLICT(user_id, kind, track_id) DO UPDATE SET added_at = excluded.added_at, "
                              "track = excluded.track", (user_id, track["id"], json.dumps(track), now_ms()))
            self.conn.execute("DELETE FROM library WHERE user_id = ? AND kind = 'history' AND track_id NOT IN "
                              "(SELECT track_id FROM library WHERE user_id = ? AND kind = 'history' "
                              "ORDER BY added_at DESC LIMIT ?)", (user_id, user_id, MAX_HISTORY))
            return self._bump_library(user_id)

    def clear_history(self, user_id):
        with self._tx():
            self.conn.execute("DELETE FROM library WHERE user_id = ? AND kind = 'history'", (user_id,))
            return self._bump_library(user_id)

    def rename_user(self, user_id, name):
        self.conn.execute("UPDATE users SET name = ? WHERE id = ?", (name, user_id))

    def delete_user(self, user_id):
        with self._tx():
            owned = [r["id"] for r in self.conn.execute("SELECT id FROM playlists WHERE owner_id = ?", (user_id,))]
            self.conn.execute("DELETE FROM users WHERE id = ?", (user_id,))
        return owned

    def user_names(self, ids):
        ids = list(set(ids))
        if not ids:
            return {}
        marks = ",".join("?" * len(ids))
        return {r["id"]: r["name"] for r in self.conn.execute(f"SELECT id, name FROM users WHERE id IN ({marks})", ids)}

    # ---- profile pictures ---------------------------------------------------

    def avatar_versions(self, ids):
        """{user id: the version of their picture} for those who have one."""
        ids = list(set(ids))
        if not ids:
            return {}
        marks = ",".join("?" * len(ids))
        return {r["user_id"]: r["updated"] for r in self.conn.execute(
            f"SELECT user_id, updated FROM user_avatars WHERE user_id IN ({marks})", ids)}

    def avatar(self, user_id):
        return self.conn.execute("SELECT image, type, updated FROM user_avatars WHERE user_id = ?", (user_id,)).fetchone()

    def set_avatar(self, user_id, image, kind=""):
        """The user's picture (None removes it); returns its version, 0 without one."""
        with self._tx():
            if image is None:
                self.conn.execute("DELETE FROM user_avatars WHERE user_id = ?", (user_id,))
                return 0
            # strictly newer than the last one: the version is part of the picture's address
            previous = self.avatar_versions([user_id]).get(user_id, 0)
            updated = max(now_ms(), previous + 1)
            self.conn.execute("INSERT INTO user_avatars (user_id, image, type, updated) VALUES (?, ?, ?, ?) "
                              "ON CONFLICT(user_id) DO UPDATE SET image = excluded.image, type = excluded.type, "
                              "updated = excluded.updated", (user_id, image, kind, updated))
            return updated

    def touch_playlists_of(self, user_id):
        """A member's name or picture changed: their playlists get a new version, so the others fetch them."""
        with self._tx():
            ids = [r["playlist_id"] for r in self.conn.execute(
                "SELECT playlist_id FROM playlist_members WHERE user_id = ?", (user_id,))]
            for playlist_id in ids:
                self._touch(playlist_id)
        return ids

    # ---- shared tracks ------------------------------------------------------

    def create_share(self, user_id, track):
        # The same track shared twice by the same person gets the same link.
        for row in self.conn.execute("SELECT code, track FROM shares WHERE user_id = ? ORDER BY created DESC LIMIT 200",
                                     (user_id,)):
            if json.loads(row["track"]).get("id") == track["id"]:
                return row["code"]
        code = new_code(8)
        self.conn.execute("INSERT INTO shares (code, track, user_id, created) VALUES (?, ?, ?, ?)",
                          (code, json.dumps(track), user_id, now_ms()))
        return code

    def get_share(self, code, count_open=False):
        row = self.conn.execute("SELECT s.code, s.track, s.created, s.opens, u.name FROM shares s "
                                "JOIN users u ON u.id = s.user_id WHERE s.code = ?", (code,)).fetchone()
        if not row:
            return None
        if count_open:
            self.conn.execute("UPDATE shares SET opens = opens + 1 WHERE code = ?", (code,))
        return {"code": row["code"], "track": json.loads(row["track"]), "by": row["name"],
                "created": row["created"], "opens": row["opens"]}

    # ---- shared playlists ---------------------------------------------------

    def create_playlist(self, owner_id, name, description, tracks, origin=""):
        count = self.conn.execute("SELECT COUNT(*) FROM playlist_members WHERE user_id = ?", (owner_id,)).fetchone()[0]
        if count >= MAX_PLAYLISTS_PER_USER:
            raise LimitError("too many shared playlists")
        playlist_id = new_code(10)
        t = now_ms()
        with self._tx():
            self.conn.execute("INSERT INTO playlists (id, owner_id, name, description, created, updated, origin) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?)", (playlist_id, owner_id, name, description, t, t, origin))
            self.conn.execute("INSERT INTO playlist_members (playlist_id, user_id, joined) VALUES (?, ?, ?)",
                              (playlist_id, owner_id, t))
            self._insert_tracks(playlist_id, owner_id, tracks)
        return playlist_id

    def _insert_tracks(self, playlist_id, user_id, tracks):
        rows = self.conn.execute("SELECT track_id FROM playlist_tracks WHERE playlist_id = ?", (playlist_id,)).fetchall()
        present = {r["track_id"] for r in rows}
        position = self.conn.execute("SELECT COALESCE(MAX(position), -1) FROM playlist_tracks WHERE playlist_id = ?",
                                     (playlist_id,)).fetchone()[0]
        added = 0
        t = now_ms()
        for track in tracks:
            if track["id"] in present:
                continue
            if len(present) >= MAX_PLAYLIST_TRACKS:
                raise LimitError("the playlist is full")
            position += 1
            self.conn.execute("INSERT INTO playlist_tracks (entry_id, playlist_id, position, track_id, track, added_by, added_at) "
                              "VALUES (?, ?, ?, ?, ?, ?, ?)",
                              (new_code(12), playlist_id, position, track["id"], json.dumps(track), user_id, t))
            present.add(track["id"])
            added += 1
        return added

    def _touch(self, playlist_id):
        self.conn.execute("UPDATE playlists SET version = version + 1, updated = ? WHERE id = ?", (now_ms(), playlist_id))

    def playlist_row(self, playlist_id):
        return self.conn.execute("SELECT * FROM playlists WHERE id = ?", (playlist_id,)).fetchone()

    def is_member(self, playlist_id, user_id):
        return self.conn.execute("SELECT 1 FROM playlist_members WHERE playlist_id = ? AND user_id = ?",
                                 (playlist_id, user_id)).fetchone() is not None

    def members(self, playlist_id):
        return [r["user_id"] for r in self.conn.execute(
            "SELECT user_id FROM playlist_members WHERE playlist_id = ? ORDER BY joined", (playlist_id,))]

    def playlist(self, playlist_id, with_tracks=True):
        p = self.playlist_row(playlist_id)
        if not p:
            return None
        member_ids = self.members(playlist_id)
        tracks = []
        if with_tracks:
            tracks = [{"entryId": r["entry_id"], "track": json.loads(r["track"]), "addedBy": r["added_by"],
                       "addedAt": r["added_at"]}
                      for r in self.conn.execute("SELECT * FROM playlist_tracks WHERE playlist_id = ? ORDER BY position",
                                                 (playlist_id,))]
        names = self.user_names(member_ids + [p["owner_id"]] + [t["addedBy"] for t in tracks])
        avatars = self.avatar_versions(member_ids + [p["owner_id"]])
        for t in tracks:
            t["addedByName"] = names.get(t["addedBy"], "")
        count = len(tracks) if with_tracks else self.conn.execute(
            "SELECT COUNT(*) FROM playlist_tracks WHERE playlist_id = ?", (playlist_id,)).fetchone()[0]
        out = {
            "id": p["id"], "name": p["name"], "description": p["description"],
            "owner": {"id": p["owner_id"], "name": names.get(p["owner_id"], ""), "avatar": avatars.get(p["owner_id"], 0)},
            "members": [{"id": m, "name": names.get(m, ""), "avatar": avatars.get(m, 0)} for m in member_ids],
            "version": p["version"], "created": p["created"], "updated": p["updated"], "count": count,
            "origin": p["origin"], "protected": bool(p["password"]),
            "coverVersion": self.cover_version(playlist_id),
        }
        if with_tracks:
            out["tracks"] = tracks
        return out

    def cover_version(self, playlist_id):
        row = self.conn.execute("SELECT updated FROM playlist_covers WHERE playlist_id = ?", (playlist_id,)).fetchone()
        return row["updated"] if row else 0

    def cover(self, playlist_id):
        return self.conn.execute("SELECT image, type, updated FROM playlist_covers WHERE playlist_id = ?",
                                 (playlist_id,)).fetchone()

    def set_cover(self, playlist_id, image, kind=""):
        """The playlist's own picture (None removes it); the members get a new version of the playlist."""
        with self._tx():
            if image is None:
                self.conn.execute("DELETE FROM playlist_covers WHERE playlist_id = ?", (playlist_id,))
            else:
                # strictly newer than the last one: the version is part of the picture's address
                updated = max(now_ms(), self.cover_version(playlist_id) + 1)
                self.conn.execute("INSERT INTO playlist_covers (playlist_id, image, type, updated) VALUES (?, ?, ?, ?) "
                                  "ON CONFLICT(playlist_id) DO UPDATE SET image = excluded.image, type = excluded.type, "
                                  "updated = excluded.updated", (playlist_id, image, kind, updated))
            self._touch(playlist_id)

    def preview_covers(self, playlist_id, limit=4):
        covers = []
        for r in self.conn.execute("SELECT track FROM playlist_tracks WHERE playlist_id = ? ORDER BY position LIMIT 50",
                                   (playlist_id,)):
            cover = json.loads(r["track"]).get("cover")
            if cover and cover not in covers:
                covers.append(cover)
            if len(covers) >= limit:
                break
        return covers

    def playlists_of(self, user_id):
        ids = [r["playlist_id"] for r in self.conn.execute(
            "SELECT playlist_id FROM playlist_members WHERE user_id = ? ORDER BY joined", (user_id,))]
        return [p for p in (self.playlist(i) for i in ids) if p]

    def join_playlist(self, playlist_id, user_id):
        if self.is_member(playlist_id, user_id):
            return False
        count = self.conn.execute("SELECT COUNT(*) FROM playlist_members WHERE user_id = ?", (user_id,)).fetchone()[0]
        if count >= MAX_PLAYLISTS_PER_USER:
            raise LimitError("too many shared playlists")
        with self._tx():
            self.conn.execute("INSERT INTO playlist_members (playlist_id, user_id, joined) VALUES (?, ?, ?)",
                              (playlist_id, user_id, now_ms()))
            self._touch(playlist_id)
        return True

    def leave_playlist(self, playlist_id, user_id):
        with self._tx():
            self.conn.execute("DELETE FROM playlist_members WHERE playlist_id = ? AND user_id = ?", (playlist_id, user_id))
            self._touch(playlist_id)

    def update_playlist(self, playlist_id, name=None, description=None):
        with self._tx():
            if name is not None:
                self.conn.execute("UPDATE playlists SET name = ? WHERE id = ?", (name, playlist_id))
            if description is not None:
                self.conn.execute("UPDATE playlists SET description = ? WHERE id = ?", (description, playlist_id))
            self._touch(playlist_id)

    def delete_playlist(self, playlist_id):
        self.conn.execute("DELETE FROM playlists WHERE id = ?", (playlist_id,))

    def add_tracks(self, playlist_id, user_id, tracks):
        with self._tx():
            added = self._insert_tracks(playlist_id, user_id, tracks)
            if added:
                self._touch(playlist_id)
        return added

    def set_password(self, playlist_id, password):
        """A password the others enter to join (kept as a salted scrypt hash); empty removes it."""
        stored = hash_secret(password) if password else ""
        with self._tx():
            self.conn.execute("UPDATE playlists SET password = ? WHERE id = ?", (stored, playlist_id))
            self._touch(playlist_id)

    def check_password(self, playlist_id, password):
        row = self.conn.execute("SELECT password FROM playlists WHERE id = ?", (playlist_id,)).fetchone()
        if not row or not row["password"]:
            return True
        return check_secret(row["password"], password)

    def replace_tracks(self, playlist_id, user_id, tracks):
        """The whole list at once (a copy of a YouTube Music playlist following its source)."""
        with self._tx():
            self.conn.execute("DELETE FROM playlist_tracks WHERE playlist_id = ?", (playlist_id,))
            self._insert_tracks(playlist_id, user_id, tracks[:MAX_PLAYLIST_TRACKS])
            self._touch(playlist_id)

    def remove_track(self, playlist_id, entry_id):
        with self._tx():
            removed = self.conn.execute("DELETE FROM playlist_tracks WHERE playlist_id = ? AND entry_id = ?",
                                        (playlist_id, entry_id)).rowcount
            if removed:
                self._touch(playlist_id)
        return removed > 0

    def move_track(self, playlist_id, entry_id, to):
        with self._tx():
            entries = [r["entry_id"] for r in self.conn.execute(
                "SELECT entry_id FROM playlist_tracks WHERE playlist_id = ? ORDER BY position", (playlist_id,))]
            if entry_id not in entries:
                return False
            entries.remove(entry_id)
            entries.insert(max(0, min(to, len(entries))), entry_id)
            for i, e in enumerate(entries):
                self.conn.execute("UPDATE playlist_tracks SET position = ? WHERE entry_id = ?", (i, e))
            self._touch(playlist_id)
        return True


class _Transaction:
    def __init__(self, conn):
        self.conn = conn

    def __enter__(self):
        self.conn.execute("BEGIN")

    def __exit__(self, exc_type, exc, tb):
        self.conn.execute("ROLLBACK" if exc_type else "COMMIT")
        return False
