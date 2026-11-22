"""Listening rooms: a shared queue and one playback clock for everyone in the room.

The server owns the state; clients send intents (add, play, pause, seek, ended…) and get the whole
room back after every change. The playback position is `position` seconds at server time `since`
(ms) — while playing, everyone computes the current spot from their clock offset to the server.

Modes:
  "open" — everyone adds to the queue and controls playback;
  "host" — only the host adds and controls, the others listen along.
"""

import random
import secrets
import time

from .db import new_code

MODES = ("open", "host")
MAX_QUEUE = 500
KEEP_PLAYED = 50
# Someone who lost the connection stays in the room this long (reconnects, sleeping laptops).
OFFLINE_GRACE = 45.0
# An empty room waits this long for someone to come back.
EMPTY_ROOM_TTL = 15 * 60.0
# If no client reports the end of a track, the server moves on this long after it should have ended.
END_SLACK = 8.0
# An "ended" from someone other than the host counts only this close to the track's end (s) —
# a client whose stream failed must not skip the track for everybody.
ENDED_WINDOW = 20.0

ROOM_CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"


def server_time():
    return int(time.time() * 1000)


class RoomError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class Room:
    def __init__(self, code, name, mode, host_id):
        self.code = code
        self.name = name
        self.mode = mode
        self.host_id = host_id
        self.members = {}          # user id -> {"id", "name", "avatar", "joined", "online", "left_at"}
        self.queue = []            # [{"itemId", "track", "addedBy"}]
        self.current = -1          # index into queue, -1 = nothing yet
        self.playing = False
        self.position = 0.0        # seconds at `since`
        self.since = server_time()
        self.finished = False      # the queue ran out
        self.empty_since = None
        self.revision = 0

    # ---- members ------------------------------------------------------------

    def join(self, user):
        m = self.members.get(user["id"])
        if m:
            m["online"] += 1
            m["name"] = user["name"]
            m["avatar"] = user.get("avatar", 0)
            m["left_at"] = None
        else:
            self.members[user["id"]] = {"id": user["id"], "name": user["name"], "avatar": user.get("avatar", 0),
                                        "joined": time.monotonic(), "online": 1, "left_at": None}
            if self.host_id not in self.members:
                self.host_id = user["id"]
        self.empty_since = None
        self.revision += 1

    def disconnect(self, user_id):
        """One connection of the user is gone; they stay a member for OFFLINE_GRACE."""
        m = self.members.get(user_id)
        if not m:
            return
        m["online"] = max(0, m["online"] - 1)
        if m["online"] == 0:
            m["left_at"] = time.monotonic()
        self.revision += 1

    def leave(self, user_id):
        if self.members.pop(user_id, None) is None:
            return
        if user_id == self.host_id:
            self._pick_host()
        if not self.members:
            self.empty_since = time.monotonic()
        self.revision += 1

    def _pick_host(self):
        online = [m for m in self.members.values() if m["online"] > 0]
        pool = online or list(self.members.values())
        self.host_id = min(pool, key=lambda m: m["joined"])["id"] if pool else ""

    def expire_members(self, now=None):
        """Removes members offline for longer than the grace period. True when something changed."""
        now = time.monotonic() if now is None else now
        gone = [uid for uid, m in self.members.items()
                if m["online"] == 0 and m["left_at"] is not None and now - m["left_at"] > OFFLINE_GRACE]
        for uid in gone:
            self.leave(uid)
        # The host went offline: someone who is here leads until they come back.
        host = self.members.get(self.host_id)
        if host and host["online"] == 0 and any(m["online"] > 0 for m in self.members.values()):
            if host["left_at"] is not None and now - host["left_at"] > OFFLINE_GRACE / 3:
                self._pick_host()
                self.revision += 1
                return True
        return bool(gone)

    def is_expired(self, now=None):
        now = time.monotonic() if now is None else now
        return not self.members and self.empty_since is not None and now - self.empty_since > EMPTY_ROOM_TTL

    def can_control(self, user_id):
        return self.mode == "open" or user_id == self.host_id

    def _require_control(self, user_id):
        if user_id not in self.members:
            raise RoomError("not_member", "you are not in this room")
        if not self.can_control(user_id):
            raise RoomError("host_only", "only the host controls this room")

    def _require_host(self, user_id):
        if user_id != self.host_id:
            raise RoomError("host_only", "only the host can do that")

    # ---- clock --------------------------------------------------------------

    def elapsed(self, at=None):
        at = server_time() if at is None else at
        if not self.playing:
            return self.position
        return self.position + max(0, at - self.since) / 1000.0

    def _set_clock(self, position, playing):
        self.position = max(0.0, float(position))
        self.playing = playing
        self.since = server_time()

    def current_item(self):
        return self.queue[self.current] if 0 <= self.current < len(self.queue) else None

    # ---- queue --------------------------------------------------------------

    def add(self, user_id, tracks, play_next=False):
        self._require_control(user_id)
        if len(self.queue) + len(tracks) > MAX_QUEUE:
            self._trim_played()
        if len(self.queue) + len(tracks) > MAX_QUEUE:
            raise RoomError("queue_full", "the queue is full")
        items = [{"itemId": new_code(10), "track": t, "addedBy": user_id} for t in tracks]
        if not items:
            return
        if play_next and self.current_item() and not self.finished:
            at = self.current + 1
            self.queue[at:at] = items
        else:
            first_new = len(self.queue)
            self.queue.extend(items)
            # Nothing was playing (new room, or the queue had ended): the first new track starts.
            if self.current < 0 or self.finished:
                self.current = first_new
                self.finished = False
                self._set_clock(0, True)
        self.revision += 1

    def remove(self, user_id, item_id):
        self._require_control(user_id)
        index = self._index(item_id)
        if index == self.current:
            # Removing what's playing skips to the next track.
            del self.queue[index]
            if self.current >= len(self.queue):
                self.current = len(self.queue) - 1
                self.finished = True
                self._set_clock(0, False)
            else:
                self._set_clock(0, self.playing)
        else:
            del self.queue[index]
            if index < self.current:
                self.current -= 1
        self.revision += 1

    def move(self, user_id, item_id, to):
        self._require_control(user_id)
        index = self._index(item_id)
        playing_id = self.current_item()["itemId"] if self.current_item() else None
        item = self.queue.pop(index)
        to = max(0, min(int(to), len(self.queue)))
        self.queue.insert(to, item)
        if playing_id:
            self.current = self._index(playing_id)
        self.revision += 1

    def _upcoming_start(self):
        # The tracks after the one playing; all of them once the queue has run out past its end.
        return len(self.queue) if self.finished else self.current + 1

    def shuffle(self, user_id):
        """Shuffles what is still to play; the track playing stays where it is."""
        self._require_control(user_id)
        start = self._upcoming_start()
        rest = self.queue[start:]
        if len(rest) < 2:
            return
        random.shuffle(rest)
        self.queue[start:] = rest
        self.revision += 1

    def clear(self, user_id):
        """Drops what is still to play; the track playing and what has played stay."""
        self._require_control(user_id)
        start = self._upcoming_start()
        if start >= len(self.queue):
            return
        del self.queue[start:]
        self.revision += 1

    def _index(self, item_id):
        for i, item in enumerate(self.queue):
            if item["itemId"] == item_id:
                return i
        raise RoomError("no_item", "no such track in the queue")

    def _trim_played(self):
        extra = self.current - KEEP_PLAYED
        if extra > 0:
            del self.queue[:extra]
            self.current -= extra

    # ---- playback -----------------------------------------------------------

    def play_item(self, user_id, item_id):
        self._require_control(user_id)
        self.current = self._index(item_id)
        self.finished = False
        self._set_clock(0, True)
        self.revision += 1

    def pause(self, user_id, position=None):
        self._require_control(user_id)
        if not self.playing:
            return
        self._set_clock(self.elapsed() if position is None else position, False)
        self.revision += 1

    def resume(self, user_id):
        self._require_control(user_id)
        if self.playing or not self.current_item():
            return
        if self.finished:
            self.finished = False
            self.position = 0
        self._set_clock(self.position, True)
        self.revision += 1

    def seek(self, user_id, position):
        self._require_control(user_id)
        if not self.current_item():
            return
        duration = self.current_item()["track"].get("duration") or 0
        if duration:
            position = min(position, duration)
        self._set_clock(position, self.playing)
        self.revision += 1

    def next(self, user_id):
        self._require_control(user_id)
        self._advance()

    def previous(self, user_id):
        self._require_control(user_id)
        if self.elapsed() > 3 or self.current <= 0:
            self._set_clock(0, self.playing or self.finished)
            self.finished = False
        else:
            self.current -= 1
            self._set_clock(0, True)
        self.revision += 1

    def ended(self, user_id, item_id):
        """A client reached the end of the track. True when the room moved on."""
        item = self.current_item()
        if not item or item["itemId"] != item_id or not self.playing or user_id not in self.members:
            return False
        if user_id != self.host_id:
            if self.mode == "host":
                return False   # the host's player is the clock
            duration = item["track"].get("duration") or 0
            if duration and self.elapsed() < duration - ENDED_WINDOW:
                return False
        self._advance()
        return True

    def _advance(self):
        if self.current + 1 < len(self.queue):
            self.current += 1
            self.finished = False
            self._set_clock(0, True)
            self._trim_played()
        elif self.current_item():
            self.finished = True
            self._set_clock(0, False)
        self.revision += 1

    def overdue(self):
        """The current track should have ended a while ago and nobody said so (e.g. everyone's offline)."""
        item = self.current_item()
        if not item or not self.playing:
            return False
        duration = item["track"].get("duration") or 0
        return duration > 0 and self.elapsed() > duration + END_SLACK

    def tick(self):
        if self.overdue():
            self._advance()
            return True
        return False

    # ---- settings -----------------------------------------------------------

    def set_mode(self, user_id, mode):
        self._require_host(user_id)
        if mode not in MODES:
            raise RoomError("bad_request", "unknown mode")
        self.mode = mode
        self.revision += 1

    def set_host(self, user_id, new_host):
        self._require_host(user_id)
        if new_host not in self.members:
            raise RoomError("not_member", "that person is not in the room")
        self.host_id = new_host
        self.revision += 1

    def update_member(self, user):
        """A member changed their name or picture. False when they aren't in this room."""
        m = self.members.get(user["id"])
        if not m:
            return False
        m["name"] = user["name"]
        m["avatar"] = user.get("avatar", 0)
        self.revision += 1
        return True

    def rename(self, user_id, name):
        self._require_host(user_id)
        self.name = name
        self.revision += 1

    def kick(self, user_id, target):
        self._require_host(user_id)
        if target == user_id or target not in self.members:
            raise RoomError("not_member", "that person is not in the room")
        self.leave(target)

    # ---- serialization ------------------------------------------------------

    def state(self, names=None):
        names = names or {}
        members = sorted(self.members.values(), key=lambda m: m["joined"])
        return {
            "code": self.code,
            "name": self.name,
            "mode": self.mode,
            "hostId": self.host_id,
            "members": [{"id": m["id"], "name": m["name"], "avatar": m.get("avatar", 0), "online": m["online"] > 0}
                        for m in members],
            "queue": [{"itemId": i["itemId"], "track": i["track"], "addedBy": i["addedBy"],
                       "addedByName": names.get(i["addedBy"]) or self._member_name(i["addedBy"])}
                      for i in self.queue],
            "current": self.current,
            "playing": self.playing,
            "finished": self.finished,
            "position": round(self.position, 3),
            "since": self.since,
            "revision": self.revision,
        }

    def _member_name(self, user_id):
        m = self.members.get(user_id)
        return m["name"] if m else ""

    def preview(self):
        item = self.current_item()
        return {
            "code": self.code, "name": self.name, "mode": self.mode,
            "host": self._member_name(self.host_id),
            "listeners": sum(1 for m in self.members.values() if m["online"] > 0),
            "track": item["track"] if item and not self.finished else None,
            "queued": max(0, len(self.queue) - max(self.current, 0) - 1),
        }


class Rooms:
    def __init__(self, max_rooms=5000):
        self.rooms = {}
        self.max_rooms = max_rooms

    def create(self, user, name, mode):
        if mode not in MODES:
            raise RoomError("bad_request", "unknown mode")
        if len(self.rooms) >= self.max_rooms:
            raise RoomError("busy", "too many rooms right now")
        while True:
            code = "".join(secrets.choice(ROOM_CODE_ALPHABET) for _ in range(6))
            if code not in self.rooms:
                break
        room = Room(code, name, mode, user["id"])
        self.rooms[code] = room
        return room

    def get(self, code):
        return self.rooms.get((code or "").upper())

    def sweep(self):
        """Periodic housekeeping: returns the rooms that changed; drops expired ones."""
        changed = []
        for code, room in list(self.rooms.items()):
            if room.is_expired():
                del self.rooms[code]
                continue
            if room.expire_members() | room.tick():
                changed.append(room)
        return changed

