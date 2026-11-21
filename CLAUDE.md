# Everplay — notes for Claude

Music player in C++20 / Qt 6.8 (QML) with TagLib 2 and CMake. See `docs/DEVELOPMENT.md` for the architecture and build steps.

## Commit messages

Commits must read as professional engineering history.

- **Language:** English, matching the existing history.
- **Subject:** `Area: what changed`, at most ~72 characters, no trailing period
  (e.g. `YouTube playback: buffer the stream in memory`, `Lyrics: drop timing without cue ranges`).
  The area is a feature or module (`Lyrics`, `SoundCloud`, `Search`, `Player`, `AudioEngine`, `Installer`…).
- **Body:** a blank line, then plain prose wrapped at ~72 columns. Say what was wrong or missing and how
  the change addresses it. Use `-` bullets for several independent points.
- **Version bumps** are separate commits with exactly `Version X.Y.Z` as the subject.

Never put any of these in a commit message:

- song titles, artist or album names, playlist names, search queries or video IDs used for testing.
  Write "a track with a long intro", not the name of the track.
- account names, e-mail addresses, cookies, tokens or other personal data;
- jokes, emoji, exclamation marks, filler ("small fix", "stuff", "wip", "finally works");
- a log of how the session went ("tried X, then Y"). Describe the result, not the process.

A short mention of new `--tour-*` / `EVERPLAY_*_DEBUG` developer switches is fine. Give the placeholder
syntax (`--tour-listen "<q1>;<q2>"`), never real example values.
