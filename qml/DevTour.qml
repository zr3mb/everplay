import QtQuick
import QtQuick.Controls
import Everplay

// Developer tool: `Everplay.exe --tour <folder>` walks through the screens
// and saves a screenshot of each one as PNG. Inactive in a normal run.
Item {
    id: root
    property Item target
    property string dir: ""
    property int step: -1

    readonly property bool withLogin: Qt.application.arguments.indexOf("--tour-login") >= 0
    readonly property bool withEngine: Qt.application.arguments.indexOf("--tour-engine") >= 0
    readonly property bool withBig: Qt.application.arguments.indexOf("--tour-big") >= 0
    readonly property bool withEq: Qt.application.arguments.indexOf("--tour-eq") >= 0
    readonly property bool withMatch: Qt.application.arguments.indexOf("--tour-match") >= 0
    // Correcting a Spotify track's YouTube Music match (needs a synced Spotify library).
    property var matchTrack: ({})
    property var matchResults: []
    function firstSpotifyTrack() {
        for (let i = 0; i < Library.tracks.count; ++i) {
            const t = Library.tracks.get(i)
            if (t.source === "spotify")
                return t
        }
        return {}
    }
    // The first visible track row of a Spotify track (TrackRow) — to open its menu.
    function findSpotifyRow(item) {
        if (item.openMenu && item.t && item.t.source === "spotify" && item.visible && item.opacity > 0.5)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = root.findSpotifyRow(item.children[i])
            if (found)
                return found
        }
        return null
    }
    readonly property var matchSteps: [
        { name: "M1-library", run: () => { Nav.finishOnboarding(); Nav.navigate("library"); root.matchTrack = root.firstSpotifyTrack()
                                            console.log("tour: track", root.matchTrack.artist, "-", root.matchTrack.title, root.matchTrack.durationText) } },
        { name: "M2-menu", run: () => { const row = root.findSpotifyRow(root.target); console.log("tour: menu row", row ? row.t.title : "none")
                                        if (row) row.openMenu(row.width - row.menuWidth, row.height) } },
        { name: "M2-auto", run: () => Spotify.rematch(root.matchTrack.id) },
        { name: "M3-dialog", run: () => { console.log("tour: auto match =", Spotify.matchedVideoId(root.matchTrack.id)); Nav.chooseMatch(root.matchTrack) } },
        { name: "M4-results", run: () => {} },
        { name: "M5-choose", run: () => { const r = root.matchResults[root.matchResults.length - 1]
                                          if (r) Spotify.chooseMatch(root.matchTrack.id, r.videoId)
                                          console.log("tour: chosen =", Spotify.matchedVideoId(root.matchTrack.id), "manual =", Spotify.manualMatch(root.matchTrack.id)) } },
        { name: "M6-dialog-again", run: () => Nav.chooseMatch(root.matchTrack) },
        { name: "M7-rematch", run: () => Spotify.rematch(root.matchTrack.id) },
        { name: "M8-result", run: () => console.log("tour: after rematch =", Spotify.matchedVideoId(root.matchTrack.id), "manual =", Spotify.manualMatch(root.matchTrack.id)) }
    ]
    readonly property bool withCovers: Qt.application.arguments.indexOf("--tour-covers") >= 0
    // Playlist covers: the collage of track covers, an uploaded picture (one of the tour's own
    // screenshots), back to the default. Likes a few tracks first, so "Liked" has covers to use.
    readonly property var coverSteps: [
        { name: "C1-liked", run: () => { Nav.finishOnboarding()
                                        let liked = 0
                                        for (let i = 0; i < Library.tracks.count && liked < 6; ++i) {
                                            const t = Library.tracks.get(i)
                                            if (t.cover && !t.liked) { Library.tracks.toggleLiked(i); ++liked }
                                        }
                                        Library.shufflePlaylistCover(0)
                                        console.log("tour: liked mosaic =", JSON.stringify(Library.playlists[0].mosaic))
                                        Nav.navigate("playlist", { kind: "playlist", index: 0 }) } },
        { name: "C2-shuffled", run: () => { Library.shufflePlaylistCover(0); console.log("tour: shuffled =", JSON.stringify(Library.playlists[0].mosaic)) } },
        { name: "C3-custom", run: () => { console.log("tour: upload =", Library.setPlaylistCover(1, "file:///" + root.dir + "/C1-liked.png"),
                                                      Library.playlists[1].cover, Library.playlists[1].customCover)
                                         Nav.navigate("playlist", { kind: "playlist", index: 1 }) } },
        { name: "C4-home", run: () => Nav.navigate("home") },
        { name: "C5-reset", run: () => { Library.resetPlaylistCover(1); console.log("tour: reset =", Library.playlists[1].cover, Library.playlists[1].customCover)
                                        Nav.navigate("playlist", { kind: "playlist", index: 1 }) } }
    ]
    readonly property var eqSteps: [
        { name: "Q1-play", run: () => { Equalizer.applyPreset("Flat"); Player.play(); Nav.navigate("equalizer") } },
        { name: "Q2-flat", run: () => console.log("tour: flat  peak", Equalizer.peak.toFixed(1), "dB, reduction", Equalizer.reduction.toFixed(1)) },
        { name: "Q3-bass", run: () => { Equalizer.applyPreset("Extreme bass"); console.log("tour: curve 30/60/250/1k/10k Hz:", [30, 60, 250, 1000, 10000].map(f => Equalizer.response(1000)[Math.round(Math.log(f / 20) / Math.log(1000) * 999)].toFixed(1)).join(" / ")) } },
        { name: "Q4-bass-meter", run: () => console.log("tour: bass  peak", Equalizer.peak.toFixed(1), "dB, reduction", Equalizer.reduction.toFixed(1)) },
        { name: "Q5-nolimit", run: () => { Equalizer.limiter = false; Equalizer.preamp = 12 } },
        { name: "Q6-clip", run: () => { console.log("tour: no limiter peak", Equalizer.peak.toFixed(1), "dB, clipping", Equalizer.clipping); Equalizer.limiter = true; Equalizer.preamp = 0 } }
    ]
    // Performance (--tour-perf, with EVERPLAY_PERF_DEBUG=1): playback while searching and opening artists.
    readonly property bool withPerf: Qt.application.arguments.indexOf("--tour-perf") >= 0
    function findSearchPage(item) {
        if (item.onlineEnabled !== undefined && item.query !== undefined)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = findSearchPage(item.children[i])
            if (found)
                return found
        }
        return null
    }
    function typeQuery(text) {
        const page = findSearchPage(root.target)
        console.log("tour: step query", JSON.stringify(text), page ? "" : "(no search page)")
        if (page)
            page.query = text
    }
    readonly property var perfSteps: [
        { name: "P0-find", run: () => { Nav.finishOnboarding(); YouTube.search("Daft Punk Around the World") } },
        { name: "P1-play", run: () => { const r = Library.onlineResults(); console.log("tour: step play, tracks", Library.tracks.count, "results", r.length); if (r.length) Player.playFrom(r, r[0]); Nav.navigate("home") } },
        { name: "P2-wait", run: () => console.log("tour: step playing", Player.playing, Player.title) },
        { name: "P3-search", run: () => { console.log("tour: step open search"); Nav.navigate("search") } },
        { name: "P4-q1", run: () => root.typeQuery("d") },
        { name: "P5-q2", run: () => root.typeQuery("da") },
        { name: "P6-q3", run: () => root.typeQuery("daft") },
        { name: "P7-q4", run: () => root.typeQuery("daft punk") },
        { name: "P8-wait", run: () => console.log("tour: step wait") },
        { name: "P9-q5", run: () => root.typeQuery("dawid podsiadlo") },
        { name: "P10-wait", run: () => console.log("tour: step wait") },
        { name: "P11-artist", run: () => { console.log("tour: step artist Daft Punk"); Nav.openArtist("Daft Punk", "") } },
        { name: "P12-wait", run: () => console.log("tour: step wait") },
        { name: "P13-wait", run: () => console.log("tour: step wait") },
        { name: "P14-artist", run: () => { console.log("tour: step artist Taco Hemingway"); Nav.openArtist("Taco Hemingway", "") } },
        { name: "P15-wait", run: () => console.log("tour: step wait") },
        { name: "P16-wait", run: () => console.log("tour: step end, playing", Player.playing) }
    ]
    // Seeking back and forth in a YouTube stream, with lyrics (--tour-seek).
    readonly property bool withSeek: Qt.application.arguments.indexOf("--tour-seek") >= 0
    function seekLog(label) {
        console.log("tour:", label, "playing", Player.playing, "pos", Player.position.toFixed(2), "/", Player.duration,
                    "| lyrics", Lyrics.state, "synced", Lyrics.synced, "line", Lyrics.synced ? Lyrics.lineAt(Player.position) : -1)
    }
    readonly property var seekSteps: [
        { name: "K0-search", run: () => { Nav.finishOnboarding(); YouTube.search("Daft Punk Get Lucky") } },
        { name: "K1-play", run: () => { const r = Library.onlineResults(); console.log("tour: results", r.length); if (r.length) Player.playFrom(r, r[0]); Nav.queueOpen = true } },
        { name: "K2-wait", run: () => root.seekLog("start") },
        { name: "K3-wait", run: () => root.seekLog("playing") },
        { name: "K4-fwd", run: () => { Player.seek(60); root.seekLog("seek 60") } },
        { name: "K5-wait", run: () => root.seekLog("after 60") },
        { name: "K6-back", run: () => { Player.seek(10); root.seekLog("seek 10") } },
        { name: "K7-wait", run: () => root.seekLog("after 10") },
        { name: "K8-back2", run: () => { Player.seek(2); root.seekLog("seek 2") } },
        { name: "K9-wait", run: () => root.seekLog("after 2") }
    ]
    // Moving the sound to another output device while a track plays (--tour-audio-device "<query>@<device name part>"):
    // the position has to keep advancing on the other device and after coming back to the default one.
    readonly property bool withAudioDevice: Qt.application.arguments.indexOf("--tour-audio-device") >= 0
    readonly property var audioDeviceSteps: {
        if (!withAudioDevice)
            return []
        const arg = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-audio-device") + 1].split("@")
        const log = label => console.log("tour: device", label, "| on", Player.outputDeviceName(), "| playing", Player.playing,
                                         "| pos", Player.position.toFixed(2))
        const s = [
            { name: "D1-search", run: () => { Nav.finishOnboarding(); Player.muted = true; YouTube.search(arg[0]) } },
            { name: "D2-play", run: () => { const r = Library.onlineResults(); if (r.length) Player.playFrom(r, r[0]) } }
        ]
        for (let i = 0; i < 3; ++i)
            s.push({ name: "D3-wait" + i, run: () => log("default") })
        s.push({ name: "D4-move", run: () => console.log("tour: device pinned", Player.pinOutputDevice(arg[1] || "")) })
        for (let i = 0; i < 4; ++i)
            s.push({ name: "D5-wait" + i, run: () => log("moved") })
        s.push({ name: "D6-back", run: () => console.log("tour: device unpinned", Player.pinOutputDevice("")) })
        for (let i = 0; i < 3; ++i)
            s.push({ name: "D7-wait" + i, run: () => log("back") })
        return s
    }
    // Albums and singles on an artist profile open (--tour-albums).
    readonly property bool withAlbums: Qt.application.arguments.indexOf("--tour-albums") >= 0
    readonly property var albumSteps: [
        { name: "A1-artist", run: () => { Nav.finishOnboarding(); Nav.openArtist("Faydee", "") } },
        { name: "A2-wait", run: () => {} },
        { name: "A2b-wait", run: () => {} },
        { name: "A2c-wait", run: () => {} },
        { name: "A3-cards", run: () => {
            const secs = YouTube.artist("UC0ITc55ErLkHnGp26P-wmRA").sections || []
            secs.forEach(s => console.log("tour: section", s.title, s.kind, s.items.length, "first:", s.items.length ? s.items[0].kind + " " + s.items[0].title + " pl=" + s.items[0].playlistId + " id=" + s.items[0].browseId : ""))
            root.album = secs.length && secs[0].items.length ? secs[0].items[0] : null
            if (root.album) Nav.navigate("playlist", { kind: "ytcollection", playlistId: root.album.playlistId, title: root.album.title, cover: root.album.cover, label: "ALBUM" })
        } },
        { name: "A4-wait", run: () => {} },
        { name: "A5-rows", run: () => root.logCollection(root.album ? root.album.playlistId : "") },
        { name: "A6-page", run: () => { if (root.album) Nav.navigate("playlist", { kind: "ytcollection", playlistId: root.album.browseId, title: root.album.title, cover: root.album.cover, label: "ALBUM" }) } },
        { name: "A7-wait", run: () => {} },
        { name: "A8-rows", run: () => root.logCollection(root.album ? root.album.browseId : "") }
    ]
    property var album: null
    function logCollection(id) {
        const r = Library.collectionRows(id)
        console.log("tour: collection", id, "rows", r.length, r.length ? Library.tracks.get(r[0]).title + " / " + Library.tracks.get(r[0]).artist + " / cover " + (Library.tracks.get(r[0]).cover !== "") : "")
    }
    // Local playlists: create, add, rename, remove a track, the dialogs, delete (--tour-playlists).
    readonly property bool withPlaylists: Qt.application.arguments.indexOf("--tour-playlists") >= 0
    function userIndex() {
        return Library.playlists.findIndex(p => p.kind === "user")
    }
    function logUser(label) {
        const i = root.userIndex()
        const p = i >= 0 ? Library.playlists[i] : null
        console.log("tour:", label, p ? "index " + i + " '" + p.name + "' desc '" + p.description + "' rows " + Library.playlistRows(i).length
                                        + " page " + Nav.page + " " + JSON.stringify(Nav.args) : "no user playlist")
    }
    readonly property var playlistSteps: [
        { name: "L1-search", run: () => { Nav.finishOnboarding(); YouTube.search("Daft Punk") } },
        { name: "L2-create", run: () => { const r = Library.onlineResults(); console.log("tour: results", r.length, r.slice(0, 4).map(x => Library.tracks.get(x).title + " / " + Library.tracks.get(x).artist + " / " + Library.tracks.get(x).album).join(" | "))
                                          Playlists.createPlaylist("Moja testowa", "", "local", "PRIVATE", r.slice(0, 3)) } },
        { name: "L3-add", run: () => { root.logUser("created"); Playlists.addRows(root.userIndex(), Library.onlineResults().slice(2, 6)) } },
        { name: "L4-page", run: () => root.logUser("added") },
        { name: "L5-rename", run: () => { Playlists.edit(root.userIndex(), "Daft Punk na wieczór", "Na spokojnie"); root.logUser("renamed") } },   // i18n-ignore
        { name: "L6-remove", run: () => { const i = root.userIndex(); Playlists.removeRow(i, Library.playlistRows(i)[0]); root.logUser("removed") } },
        { name: "L7-edit-dialog", run: () => Nav.editPlaylist(root.userIndex()) },
        { name: "L8-create-dialog", run: () => Nav.createPlaylist(Library.onlineResults().slice(0, 2)) },
        { name: "L8b-wait", run: () => {} },
        { name: "L9-delete", run: () => { Playlists.deletePlaylist(root.userIndex()); root.logUser("deleted") } }
    ]
    // Where the lyrics of a few popular tracks come from (--tour-lyrics).
    readonly property bool withLyrics: Qt.application.arguments.indexOf("--tour-lyrics") >= 0
    readonly property var lyricQueries: ["Dua Lipa Levitating", "Ed Sheeran Shape of You", "Dawid Podsiadło Małomiasteczkowy", "Daft Punk Get Lucky"]   // i18n-ignore
    function lyricsLog(q) {
        console.log("tour: lyrics", q, "|", Player.title, "-", Player.artist, "| state", Lyrics.state, "synced", Lyrics.synced,
                    "source", Lyrics.source, "origin", Lyrics.origin, "lines", Lyrics.lines.length)
    }
    readonly property var lyricSteps: {
        const s = [{ name: "Y0", run: () => Nav.finishOnboarding() }]
        lyricQueries.forEach((q, i) => {
            s.push({ name: "Y" + i + "a", run: () => YouTube.search(q) })
            s.push({ name: "Y" + i + "b", run: () => { const r = Library.onlineResults(); if (r.length) Player.playFrom(r, r[0]) } })
            s.push({ name: "Y" + i + "c", run: () => {} })
            s.push({ name: "Y" + i + "d", run: () => root.lyricsLog(q) })
        })
        return s
    }
    // Recommendations on the home page, genres on the search page (--tour-home).
    readonly property bool withHome: Qt.application.arguments.indexOf("--tour-home") >= 0
    function recLog(label) {
        const r = Library.recommendedRows()
        console.log("tour:", label, "| loading", Recommendations.loading, "| based on", JSON.stringify(Recommendations.basedOn),
                    "| picks", r.length, "|", r.slice(0, 10).map(x => Library.tracks.get(x).artist + " - " + Library.tracks.get(x).title).join(" | "))
    }
    // The scrolled content of the current page (PageFlick)
    function findFlick(item) {
        if (item.visible && item.contentY !== undefined && item.contentHeight > item.height + 10 && item.width > (Nav.mobile ? 300 : 500))
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = root.findFlick(item.children[i])
            if (found)
                return found
        }
        return null
    }
    function scrollPage(y) {
        const f = root.findFlick(root.target)
        console.log("tour: scroll", f ? "to " + y + " of " + Math.round(f.contentHeight) : "— nothing to scroll")
        if (f)
            f.contentY = y
    }
    readonly property var homeSteps: [
        { name: "H1-home", run: () => { Nav.finishOnboarding(); Nav.navigate("home"); root.recLog("start") } },
        { name: "H2-wait", run: () => { root.recLog("wait"); root.scrollPage(440) } },
        { name: "H3-wait", run: () => root.recLog("wait") },
        { name: "H4-refresh", run: () => { root.recLog("before refresh"); Recommendations.refresh() } },
        { name: "H5-wait", run: () => root.recLog("refreshing") },
        { name: "H6-refreshed", run: () => { root.recLog("refreshed"); root.scrollPage(440) } },
        { name: "H7-play", run: () => { const r = Library.recommendedRows(); if (r.length) Player.playFrom(r, r[1], "recs")
                                        console.log("tour: all tracks", Library.localTracks().length, "recently added", Library.recentlyAdded(50).length) } },
        { name: "H7b-playing", run: () => console.log("tour: playing", Player.title, "-", Player.artist) },
        { name: "H8-search", run: () => { const g = Library.genres()
                                          console.log("tour: genres", g.length, g.slice(0, 14).map(x => x.name + " (" + x.count + ")").join(", "))
                                          Nav.navigate("search") } },
        { name: "H8b-wait", run: () => console.log("tour: genres", Library.genres().length) },
        { name: "H8c-wait", run: () => console.log("tour: genres", Library.genres().length) },
        { name: "H8d-search", run: () => { const g = Library.genres()
                                           console.log("tour: genres", g.length, g.slice(0, 14).map(x => x.name + " (" + x.count + ")").join(", ")) } },
        { name: "H9-genre", run: () => { const g = Library.genres()
                                         if (g.length) {
                                             Nav.navigate("playlist", { kind: "genre", key: g[0].key, name: g[0].name })
                                             console.log("tour: genre", g[0].name, "rows", Library.tracksOfGenre(g[0].key).length, "mosaic", g[0].mosaic.length)
                                         } } },
        { name: "H9b-genre-loaded", run: () => {} },
        { name: "H10-home", run: () => { console.log("tour: playing", Player.title, "-", Player.artist); Nav.navigate("home") } },
        // Over 30 s of playback by now — the track counts as listened to (recommendations.json, "plays").
        { name: "H11-wait", run: () => {} },
        { name: "H12-wait", run: () => {} },
        { name: "H13-wait", run: () => {} },
        { name: "H13b-wait", run: () => {} },
        { name: "H13c-wait", run: () => {} },
        { name: "H13d-wait", run: () => {} },
        { name: "H14-wait", run: () => console.log("tour: position", Math.round(Player.position), "s of", Player.title) }
    ]
    // Artist profiles: a real artist found by name, then a YouTube channel that uploads remixes (--tour-artist).
    readonly property bool withArtist: Qt.application.arguments.indexOf("--tour-artist") >= 0
    function artistLog(id) {
        const a = YouTube.artist(id)
        console.log("tour: artist", id, a.name, "| photo", (a.photo || "") !== "", "| songs", Library.artistSongRows(id).length,
                    "| all", a.songsPlaylistId, "| sections", (a.sections || []).map(s => s.title + " " + s.kind + " " + s.items.length).join(", "),
                    "| error", a.error || "")
        // What the page shows (its bindings must follow the loaded data)
        const f = root.findFlick(root.target)
        if (f && f.parent.yt !== undefined)
            console.log("tour:   page has", (f.parent.yt.sections || []).length, "sections, height", Math.round(f.contentHeight))
    }
    readonly property var artistSteps: [
        { name: "R1-artist", run: () => { Nav.finishOnboarding(); Nav.openArtist("Kizo", "") } },
        { name: "R2-wait", run: () => {} },
        { name: "R3-loaded", run: () => root.artistLog(YouTube.knownArtistId("Kizo")) },
        { name: "R4-scroll", run: () => root.scrollPage(700) },
        { name: "R5-scroll", run: () => root.scrollPage(1400) },
        { name: "R6-scroll", run: () => root.scrollPage(2100) },
        { name: "R7-channel", run: () => Nav.openArtist("Rogal575", "UC-ijAH6jOi6945c3oytlCuQ") },
        { name: "R8-wait", run: () => {} },
        { name: "R9-loaded", run: () => root.artistLog("UC-ijAH6jOi6945c3oytlCuQ") },
        { name: "R10-scroll", run: () => root.scrollPage(700) }
    ]
    // Liked tracks: click the first row, then "next" must play the second (--tour-liked).
    readonly property bool withLiked: Qt.application.arguments.indexOf("--tour-liked") >= 0
    property var likedPage: null
    function findRows(item) {   // the visible TrackRows of the current page, top to bottom
        let out = []
        if (item.visible && item.trackIndex !== undefined && item.play !== undefined)
            out.push(item)
        for (let i = 0; i < item.children.length; ++i)
            out = out.concat(root.findRows(item.children[i]))
        return out
    }
    function queueLog(label) {
        console.log("tour:", label, "| playing", Player.title, "| shuffle", Player.shuffle, "| context", Player.queueContext,
                    "| up next", Player.upNext.slice(0, 4).map(r => Library.tracks.get(r).title).join(" / "))
    }
    readonly property var likedSteps: [
        { name: "V1-like", run: () => {
            Nav.finishOnboarding()
            const rows = Library.likedTracks().length > 0 ? [] : Library.libraryRows().slice(0, 6)
            rows.forEach(r => { Player.playFrom(rows, r, "test"); if (!Player.liked) Player.toggleLiked() })
            // A "shuffle play" button elsewhere first — it must not leave the shuffle mode on.
            Player.playShuffled(Library.libraryRows().slice(0, 30), "test")
            root.queueLog("shuffle play")
            Player.pause()
            console.log("tour: liked", Library.likedTracks().length)
            const i = Library.playlists.findIndex(p => p.kind === "liked")
            console.log("tour: liked playlist index", i, i >= 0 ? Library.playlists[i].name : "")
            Nav.navigate("playlist", { index: i })
        } },
        { name: "V2-page", run: () => {
            const rows = root.findRows(root.target).sort((a, b) => a.mapToItem(null, 0, 0).y - b.mapToItem(null, 0, 0).y)
            console.log("tour: rows on the page", rows.map(r => Library.tracks.get(r.trackIndex).title).join(" / "))
            console.log("tour: first row queue", rows.length ? (rows[0].queue || []).map(r => Library.tracks.get(r).title).join(" / ") : "-", "| context", rows.length ? rows[0].context : "")
            if (rows.length) rows[0].play()
        } },
        { name: "V3-playing", run: () => root.queueLog("after click") },
        { name: "V4-next", run: () => { Player.next(); root.queueLog("after next") } },
        { name: "V5-next", run: () => root.queueLog("later") }
    ]
    // The in-game overlay, driven like a pad (--tour-overlay).
    readonly property bool withOverlay: Qt.application.arguments.indexOf("--tour-overlay") >= 0
    function overlayLog(label) {
        const m = GameOverlay.view
        if (!m) { console.log("tour:", label, "| overlay closed, open =", GameOverlay.open); return }
        const it = m.items[m.selection] || {}
        console.log("tour:", label, "| category", m.categories[m.cat].key, "| sub", m.sub ? m.sub.title : "-", "| item", m.selection, "/", m.items.length,
                    it.title, "|", it.subtitle || "", "| adjusting", m.adjusting, "| playing", Player.title, "| volume", Math.round(Player.volume * 100))
    }
    function pad(actions) { actions.forEach(a => GameOverlay.simulate(a)) }
    readonly property var overlaySteps: [
        { name: "O1-open", run: () => { Nav.finishOnboarding(); const r = Library.libraryRows(); Player.playFrom(r, r[3], "library"); GameOverlay.show() } },
        { name: "O2-now", run: () => root.overlayLog("now playing") },
        { name: "O3-volume", run: () => { Player.volume = 0.5; root.pad(["down", "down", "down", "accept", "right", "right", "up"]); root.overlayLog("volume (50 + 15)") } },
        { name: "O4-queue", run: () => { root.pad(["back", "right"]); root.overlayLog("queue") } },
        { name: "O5-queue-down", run: () => { root.pad(["down", "down", "down", "down"]); root.overlayLog("queue down") } },
        { name: "O6-playlists", run: () => { root.pad(["right"]); root.overlayLog("playlists") } },
        { name: "O7-open", run: () => { root.pad(["down", "down", "down", "accept"]); root.overlayLog("opened") } },
        { name: "O8-track", run: () => { root.pad(["down", "down", "down"]); root.overlayLog("track") } },
        { name: "O9-play", run: () => { root.pad(["accept"]); root.overlayLog("played") } },
        { name: "O10-back", run: () => { root.pad(["back"]); root.overlayLog("back") } },
        { name: "O11-close", run: () => { root.pad(["back"]); root.overlayLog("closing") } },
        { name: "O12-closed", run: () => root.overlayLog("closed") },
        { name: "O13-reopen", run: () => { GameOverlay.toggle(); root.overlayLog("reopened") } },
        { name: "O14-shown", run: () => { root.overlayLog("shown"); GameOverlay.toggle() } },
        { name: "O15-gone", run: () => root.overlayLog("gone") }
    ]
    // The settings section, then waits for the real hotkey (WM_HOTKEY sent from outside) (--tour-hotkey).
    readonly property bool withHotkey: Qt.application.arguments.indexOf("--tour-hotkey") >= 0
    function hotkeyLog() {
        console.log("tour: hotkey", GameOverlay.hotkey, "| taken", GameOverlay.hotkeyTaken, "| open", GameOverlay.open, "| pad", GameOverlay.padConnected)
        if (GameOverlay.open)
            root.overlayLog("menu")
    }
    readonly property var hotkeySteps: [
        { name: "K1-settings", run: () => { Nav.finishOnboarding(); Nav.navigate("settings") } },
        { name: "K2-scroll", run: () => { root.scrollPage(900); root.hotkeyLog() } },
        { name: "K3-wait", run: () => root.hotkeyLog() },
        { name: "K4-wait", run: () => root.hotkeyLog() },
        { name: "K5-wait", run: () => root.hotkeyLog() },
        { name: "K6-wait", run: () => root.hotkeyLog() },
        { name: "K7-wait", run: () => root.hotkeyLog() },
        { name: "K8-wait", run: () => root.hotkeyLog() }
    ]
    // Open, browse, close — several times, with the menu fully gone in between and not (--tour-overlay-cycle).
    readonly property bool withOverlayCycle: Qt.application.arguments.indexOf("--tour-overlay-cycle") >= 0
    readonly property var overlayCycleSteps: {
        const s = [{ name: "C0-start", run: () => { Nav.finishOnboarding(); const r = Library.libraryRows(); Player.playFrom(r, r[3], "library") } }]
        for (let i = 1; i <= 4; ++i) {
            s.push({ name: "C" + i + "a-open", run: () => { GameOverlay.toggle(); root.overlayLog("cycle " + i + " open") } })
            s.push({ name: "C" + i + "b-browse", run: () => { root.pad(["right", "down", "down", "right", "down", "accept", "down"]); root.overlayLog("cycle " + i + " browse") } })
            s.push({ name: "C" + i + "c-close", run: () => { GameOverlay.toggle(); root.overlayLog("cycle " + i + " close") } })
            if (i % 2 === 0)
                s.push({ name: "C" + i + "d-gone", run: () => root.overlayLog("cycle " + i + " gone") })
        }
        s.push({ name: "C9-end", run: () => root.overlayLog("end") })
        return s
    }
    // Public YouTube Music playlists in the search results, opened and played
    // (--tour-search-playlists ["<query>"], "_" = space).
    readonly property bool withSearchPlaylists: Qt.application.arguments.indexOf("--tour-search-playlists") >= 0
    readonly property string searchPlaylistQuery: {
        const next = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-search-playlists") + 1]
        return next && !next.startsWith("--") ? next.replace(/_/g, " ") : "vixa"
    }
    readonly property var searchPlaylistSteps: [
        { name: "S1-search", run: () => { Nav.finishOnboarding(); Nav.navigate("search", { query: root.searchPlaylistQuery }) } },
        { name: "S2-wait", run: () => {} },
        { name: "S3-results", run: () => {
            const p = YouTube.playlistResults
            console.log("tour: playlists", p.length, "|", p.slice(0, 6).map(x => x.title + " (" + x.subtitle + ")").join(" | "))
            root.scrollPage(2780)
        } },
        { name: "S4-open", run: () => {
            const p = YouTube.playlistResults[0]
            root.album = p ? { playlistId: p.id } : null
            if (p) Nav.navigate("playlist", { kind: "ytcollection", playlistId: p.id, title: p.title, subtitle: p.subtitle, cover: p.cover, label: "PLAYLIST" })
        } },
        { name: "S5-wait", run: () => {} },
        { name: "S6-play", run: () => {
            const r = root.album ? Library.collectionRows(root.album.playlistId) : []
            console.log("tour: playlist rows", r.length, r.length ? Library.tracks.get(r[0]).title : "")
            if (r.length) Player.playFrom(r, r[0], "ytc:" + root.album.playlistId)
        } },
        { name: "S7-playing", run: () => console.log("tour: playing", Player.title, "-", Player.artist, "| up next", Player.upNext.length) }
    ]
    // Lyrics YouTube sends "timed" but without times (--tour-fake-timing): they must come out plain or from LRCLIB.
    readonly property bool withFakeTiming: Qt.application.arguments.indexOf("--tour-fake-timing") >= 0
    readonly property var fakeTimingSteps: {
        const log = () => console.log("tour: lyrics", Player.title, "/", Player.artist, "| state", Lyrics.state, "| synced", Lyrics.synced,
                                      "| source", Lyrics.source, "| lines", Lyrics.lines.length,
                                      "| starts", new Set(Lyrics.lines.filter(l => l.text).map(l => l.start)).size, "| canAlign", Lyrics.canAlign)
        return [
            { name: "F1-search", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: "Pork Pores Porkinson Meloman", filter: 2 }) } },
            { name: "F2-wait", run: () => {} },
            { name: "F3-play", run: () => {
                const r = Library.onlineResults().filter(x => Library.tracks.get(x).title === "Meloman")
                console.log("tour: found", r.length)
                if (r.length) Player.playFrom(r, r[0], "search")
            } },
            { name: "F4-wait", run: log },
            { name: "F5-wait", run: log },
            { name: "F6-lyrics", run: () => { log(); Nav.immersive = true } },
            { name: "F7-shown", run: () => { log(); Player.pause() } }
        ]
    }
    // --tour-stt-download (with --tour-align or --tour-recognize): first the lyric timing components, through
    // the app's own download.
    readonly property var sttDownloadSteps: {
        if (Qt.application.arguments.indexOf("--tour-stt-download") < 0)
            return []
        const steps = [{ name: "G0-download", run: () => {
            console.log("tour: stt", "| engine", Lyrics.sttEngineReady, "| aligner", Lyrics.sttAlignerReady, "| separator", Lyrics.sttSeparatorReady,
                        "| gpu", Lyrics.sttGpuName)
            Lyrics.downloadSttComponents()
        } }]
        for (let i = 0; i < 120; ++i)
            steps.push({ name: "G1-wait" + i, run: () => console.log("tour: stt download", Lyrics.sttDownloading, Lyrics.sttDownloadLabel,
                                                                      Math.round(Lyrics.sttProgress * 100) + "%", "| complete", Lyrics.sttComplete,
                                                                      "| gpu", Lyrics.sttGpuName, "| separates", Lyrics.sttSeparates) })
        return steps
    }
    // Lyric timing (--tour-align "<query>"): the first YouTube result plays muted; plain lyrics get line times,
    // synced ones get every word timed.
    readonly property bool withAlign: Qt.application.arguments.indexOf("--tour-align") >= 0
    readonly property string alignQuery: withAlign ? Qt.application.arguments[Qt.application.arguments.indexOf("--tour-align") + 1].replace(/_/g, " ") : ""
    readonly property var alignSteps: {
        const log = () => console.log("tour: align", Player.title, "| state", Lyrics.state, "| synced", Lyrics.synced, "| canAlign", Lyrics.canAlign,
                                      "| aligning", Lyrics.aligning, Math.round(Lyrics.alignProgress * 100) + "%", Lyrics.alignStage,
                                      "| lines", Lyrics.lines.length, "| words timed", Lyrics.wordTimed, "| separates", Lyrics.sttSeparates,
                                      "| source", Lyrics.source, "| whisper", Lyrics.madeByWhisper, Lyrics.whisperLyricsCount)
        const s = [
            ...root.sttDownloadSteps,
            { name: "A1-search", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: root.alignQuery.split("@")[0], filter: 2 }) } },
            { name: "A2-wait", run: () => {} },
            { name: "A3-play", run: () => {
                // "query@videoId" picks that exact result
                const want = root.alignQuery.split("@")[1] || ""
                const r = Library.onlineResults()
                const pick = r.find(x => Library.tracks.get(x).path === want) || r[0]
                console.log("tour: results", r.length, "| picked", pick ? Library.tracks.get(pick).path : "-")
                if (pick) Player.playFrom(r, pick, "search")
            } },
            { name: "A4-wait", run: log },
            // Lyrics made by whisper are removed first (they're looked up again, then aligned below).
            { name: "A5-align", run: () => {
                log(); Nav.immersive = true
                if (Lyrics.madeByWhisper) Lyrics.removeWhisperLyrics()
                else if (Lyrics.canAlign) Lyrics.alignNow()
                else Lyrics.timeWordsNow()
            } }
        ]
        for (let i = 0; i < 80; ++i)
            s.push({ name: "A6-wait" + i, run: () => { log(); if (Lyrics.canAlign && !Lyrics.aligning) Lyrics.alignNow() } })
        s.push({ name: "A7-lines", run: () => {
            log()
            for (const l of Lyrics.lines)
                console.log("tour: line", l.start.toFixed(1), l.end.toFixed(1), l.words.length, l.text)
        } })
        // The timing settings as they look now
        s.push({ name: "A8-settings", run: () => { Player.pause(); Nav.immersive = false; Nav.navigate("settings"); Lyrics.countWhisperLyrics() } })
        for (const y of [1600, 2000, 2400, 2800])
            s.push({ name: "A9-settings-" + y, run: () => root.scrollPage(y) })
        return s
    }
    // The order of the tracks in a playlist (--tour-track-order "<pair code or ->", with EVERPLAY_SERVER): a local
    // playlist and a shared one made from the first liked tracks, the last track moved to the top of each.
    readonly property bool withTrackOrder: Qt.application.arguments.indexOf("--tour-track-order") >= 0
    readonly property var trackOrderSteps: {
        if (!withTrackOrder)
            return []
        const code = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-track-order") + 1]
        const find = (kind, name) => {   // the newest
            const list = Library.playlists
            for (let i = list.length - 1; i >= 0; --i)
                if (list[i].kind === kind && list[i].name === name)
                    return i
            return -1
        }
        const titles = i => JSON.stringify(Library.playlistRows(i).map(r => Library.tracks.get(r).title.slice(0, 14)))
        const sample = () => Library.likedTracks().slice(0, 4)
        const moveLastFirst = i => { const r = Library.playlistRows(i); return Playlists.moveRow(i, r[r.length - 1], r[0]) }
        return [
            // (no library sync: a profile signed in to YouTube Music would like the test account's tracks there)
            { name: "Q1-link", run: () => { Nav.finishOnboarding(); Player.muted = true; Online.librarySync = false
                                           if (code !== "-") Online.redeemPairCode(code) } },
            { name: "Q2-make", run: () => { Playlists.createPlaylist("Kolejność", "", "local", "", sample())
                                           if (code !== "-") Playlists.createPlaylist("Wspólna kolejność", "", "shared", "", sample()) } },
            { name: "Q2b-wait", run: () => {} },
            { name: "Q2c-wait", run: () => {} },
            { name: "Q3-before", run: () => console.log("tour: local", titles(find("user", "Kolejność")), "| shared", titles(find("shared", "Wspólna kolejność"))) },
            { name: "Q4-move", run: () => console.log("tour: moved", moveLastFirst(find("user", "Kolejność")),
                                                      find("shared", "Wspólna kolejność") >= 0 ? moveLastFirst(find("shared", "Wspólna kolejność")) : "-",
                                                      "| youtube refused", !Playlists.moveRow(Library.playlists.findIndex(p => p.kind === "youtube"), 0, -1)) },
            { name: "Q5-after", run: () => console.log("tour: local", titles(find("user", "Kolejność")), "| shared", titles(find("shared", "Wspólna kolejność"))) },
            { name: "Q6-page", run: () => Nav.navigate("playlist", { kind: "playlist", index: find("user", "Kolejność") }) }
        ]
    }
    // The user's order of the playlists (--tour-order, with a profile signed in to YouTube Music): the last one
    // moved to the top of its group, one into a new folder, one into a wrong group (refused).
    readonly property bool withOrder: Qt.application.arguments.indexOf("--tour-order") >= 0
    readonly property var orderSteps: {
        if (!withOrder)
            return []
        const group = key => (Library.playlistGroups.find(g => g.key === key) || { items: [] }).items
        const names = key => JSON.stringify(group(key).map(i => Library.playlists[i].name.slice(0, 12)))
        return [
            { name: "O1-start", run: () => { Nav.finishOnboarding(); Player.muted = true } },
            { name: "O2-before", run: () => console.log("tour: ytmusic before", names("ytmusic")) },
            { name: "O3-move", run: () => { const g = group("ytmusic")
                                           console.log("tour: moved", Library.reorderPlaylist(g[g.length - 1], g[0], "ytmusic"), names("ytmusic")) } },
            { name: "O4-folder", run: () => { const g = group("ytmusic"); const f = Library.createPlaylistFolder("Ulubione listy", g[2])
                                             console.log("tour: into the folder", Library.reorderPlaylist(g[3], group("folder:" + f)[0], "folder:" + f), names("folder:" + f)) } },
            { name: "O5-wrong", run: () => console.log("tour: wrong group refused", !Library.reorderPlaylist(group("ytmusic")[0], -1, "spotify")) },
            { name: "O6-kept", run: () => console.log("tour: order kept", JSON.stringify(Library.organizer ? 1 : 0), names("ytmusic")) }
        ]
    }
    // Playlist folders and hidden playlists (--tour-folders "<pair code or ->", with EVERPLAY_SERVER): links the
    // account (to see the layout reach it), makes a folder with a playlist in it, hides one, shows the hidden.
    readonly property bool withFolders: Qt.application.arguments.indexOf("--tour-folders") >= 0
    readonly property var folderSteps: {
        if (!withFolders)
            return []
        const code = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-folders") + 1]
        const find = kind => Library.playlists.findIndex(p => p.kind === kind)
        const groups = () => JSON.stringify(Library.playlistGroups.map(g => g.key + ":" + g.items.length))
        return [
            { name: "F1-link", run: () => { Nav.finishOnboarding(); Player.muted = true; if (code !== "-") Online.redeemPairCode(code) } },
            { name: "F2-before", run: () => console.log("tour: groups", groups()) },
            { name: "F3-folder", run: () => { const i = find("shared") >= 0 ? find("shared") : find("history")
                                             console.log("tour: folder", Library.createPlaylistFolder("Na wieczór", i)) } },
            { name: "F4-hide", run: () => { Library.setPlaylistHidden(find("recent"), true); console.log("tour: groups", groups(), "hidden", Library.hiddenCount) } },
            { name: "F6-shown", run: () => { Library.showHidden = true; console.log("tour: groups", groups()) } },
            { name: "F6b-wait", run: () => {} },
            { name: "F7-home", run: () => { const f = root.findFlick(root.target); root.scrollPage(f ? Math.round(f.contentHeight * 0.45) : 0) } },
            { name: "F7b-wait", run: () => {} },
            { name: "F8-account", run: () => console.log("tour: organizer", JSON.stringify(Library.playlistFolders)) },
            { name: "F9-pick", run: () => Nav.moveToFolder(find("history")) }
        ]
    }
    // Dragging the window's edge while music plays (--tour-resize "<query>", with EVERPLAY_AUDIO_DEBUG and
    // EVERPLAY_PERF_DEBUG): the width changes every frame for a while; the log shows buffer underruns.
    readonly property bool withResize: Qt.application.arguments.indexOf("--tour-resize") >= 0
    Timer {
        id: resizer
        property int n: 0
        property int baseWidth: 0
        interval: 16
        repeat: true
        onTriggered: {
            const w = root.Window.window
            if (!baseWidth)
                baseWidth = w.width
            ++n
            w.width = baseWidth - 300 + Math.round(300 * Math.abs(Math.sin(n / 25)))
            // a long layout pass, like a heavy frame while the edge is dragged: the GUI thread is busy
            if (n % 20 === 0) {
                const until = Date.now() + 400
                while (Date.now() < until) {}
            }
        }
    }
    readonly property var resizeSteps: {
        if (!withResize)
            return []
        const q = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-resize") + 1].replace(/_/g, " ")
        const log = what => console.log("tour: " + what, "|", Player.title, "| pos", Player.position.toFixed(1), "| playing", Player.playing)
        const s = [
            { name: "Z0", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: q, filter: 2 }) } },
            { name: "Z1-wait", run: () => {} },
            { name: "Z2-play", run: () => { const r = Library.onlineResults(); if (r.length) Player.playFrom(r, r[0], "search"); Nav.navigate("home") } },
            { name: "Z3-steady", run: () => log("before") },
            { name: "Z4-steady", run: () => log("before") },
            { name: "Z5-resize", run: () => { log("resizing"); resizer.start() } }
        ]
        for (let i = 0; i < 4; ++i)
            s.push({ name: "Z6-" + i, run: () => log("resizing") })
        s.push({ name: "Z7-stop", run: () => { resizer.stop(); log("after") } })
        s.push({ name: "Z8-after", run: () => log("after") })
        // and a transition into the next track, mixed off the GUI thread too
        s.push({ name: "Z9-mix", run: () => { Player.crossfade = 5; Player.seek(Math.max(0, Player.duration - 9)); log("near the end") } })
        for (let i = 0; i < 6; ++i)
            s.push({ name: "Z10-" + i, run: () => log("mixing") })
        return s
    }
    // The full-screen player in small windows (--tour-small "<query>", "_" = space): a 1024x550 second
    // screen, the queue over it, a laptop's height, then the usual size.
    readonly property bool withSmall: Qt.application.arguments.indexOf("--tour-small") >= 0
    readonly property var smallSteps: {
        if (!withSmall)
            return []
        const q = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-small") + 1].replace(/_/g, " ")
        const size = (w, h) => { const win = root.Window.window; win.width = w; win.height = h }
        return [
            { name: "M0", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: q, filter: 2 }) } },
            { name: "M1-wait", run: () => {} },
            { name: "M2-play", run: () => { const r = Library.onlineResults()   // a song (with an album), so it has lyrics
                                           const song = r.find(i => Library.tracks.get(i).album) ?? r[0]
                                           if (r.length) Player.playFrom(r, song, "search")
                                           size(1024, 550); Nav.immersiveLyrics = true; Nav.immersive = true } },
            { name: "M3-1024x550", run: () => { const win = root.Window.window
                                               console.log("tour: window", win.width, "x", win.height, "| min", win.minimumWidth, "| lyrics", Lyrics.state) } },
            { name: "M3b-lyrics", run: () => console.log("tour: lyrics", Lyrics.state) },
            { name: "M3c-cover", run: () => { Nav.immersiveLyrics = false } },
            { name: "M4-queue", run: () => { Nav.immersiveLyrics = true; Nav.immersiveQueue = "half" } },
            { name: "M4b-queue", run: () => {} },
            { name: "M5-1366x700", run: () => { Nav.immersiveQueue = ""; size(1366, 700) } },
            { name: "M6-1600x960", run: () => size(1600, 960) }
        ]
    }
    // Streams fetched ahead (--tour-ahead "<query>", "_" = space): the cursor rests on a found song,
    // then it's played; the whole next track should come in meanwhile, and skipping to it is instant.
    readonly property bool withAhead: Qt.application.arguments.indexOf("--tour-ahead") >= 0
    readonly property var aheadSteps: {
        if (!withAhead)
            return []
        const q = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-ahead") + 1].replace(/_/g, " ")
        const song = () => { const r = Library.onlineResults(); return r.find(i => Library.tracks.get(i).album) ?? r[0] }
        return [
            { name: "Y0", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: q, filter: 2 }) } },
            { name: "Y1-wait", run: () => {} },
            { name: "Y2-hover", run: () => { console.log("tour: hover"); Player.prefetch(song()) } },
            { name: "Y3-play", run: () => { const r = Library.onlineResults(); console.log("tour: play"); Player.playFrom(r, song(), "search") } },
            { name: "Y4-wait", run: () => console.log("tour: next cached", Player.cached(Player.upNext[0] ?? -1)) },
            { name: "Y5-wait", run: () => console.log("tour: next cached", Player.cached(Player.upNext[0] ?? -1)) },
            { name: "Y6-next", run: () => { console.log("tour: next"); Player.next() } },
            { name: "Y7", run: () => console.log("tour: playing", Player.playing) }
        ]
    }
    // The audio cache (--tour-cache "<query>", "_" = space): plays a found song, logs which results are
    // kept on disk; run twice on one profile and the second play comes from the disk, with a bolt in the list.
    readonly property bool withCache: Qt.application.arguments.indexOf("--tour-cache") >= 0
    readonly property var cacheSteps: {
        if (!withCache)
            return []
        const q = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-cache") + 1].replace(/_/g, " ")
        const log = what => console.log("tour: " + what, "| cached", JSON.stringify(Library.onlineResults().map(i => Player.cached(i))),
                                        "| used", Player.cacheUsedMB, "MB of", Player.cacheLimitMB)
        return [
            { name: "K0", run: () => { Nav.finishOnboarding(); Player.muted = true; const win = root.Window.window; win.width = 1500; win.height = 1000
                                      Nav.navigate("search", { query: q, filter: 2 }) } },
            { name: "K1-wait", run: () => log("before") },
            { name: "K2-play", run: () => { const r = Library.onlineResults()
                                           const song = r.find(i => Library.tracks.get(i).album) ?? r[0]
                                           if (r.length) Player.playFrom(r, song, "search") } },
            { name: "K3-wait", run: () => log("playing " + Player.title) },
            { name: "K4-list", run: () => { Player.pause(); log("after") } },
            { name: "K5-settings", run: () => Nav.navigate("settings") },
            { name: "K6-scroll", run: () => root.scrollPage(700) },
            { name: "K7", run: () => {} }
        ]
    }
    // Listening to a few tracks muted, with the lyrics open (--tour-listen "<q1>;<q2>", "_" = space):
    // with EVERPLAY_AUDIO_DEBUG the log shows whether the sound keeps up (buffer underruns).
    readonly property bool withListen: Qt.application.arguments.indexOf("--tour-listen") >= 0
    readonly property var listenSteps: {
        if (!withListen)
            return []
        const queries = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-listen") + 1].replace(/_/g, " ").split(";")
        const log = what => console.log("tour: " + what, "|", Player.title, "| pos", Player.position.toFixed(1), "/", Player.duration,
                                        "| playing", Player.playing, "| buffering", Player.buffering, "| lyrics", Lyrics.state, Lyrics.synced)
        const s = [{ name: "W0", run: () => { Nav.finishOnboarding(); Player.muted = true } }]
        queries.forEach((q, k) => {
            s.push({ name: "W" + k + "a-search", run: () => { Nav.immersive = false; Nav.navigate("search", { query: q, filter: 2 }) } })
            s.push({ name: "W" + k + "b-wait", run: () => {} })
            s.push({ name: "W" + k + "c-play", run: () => { const r = Library.onlineResults(); if (r.length) Player.playFrom(r, r[0], "search"); Nav.immersive = true } })
            for (let i = 0; i < 24; ++i)
                s.push({ name: "W" + k + "d-" + i, run: () => { log("listen"); if (i === 18) Player.seek(0.7 * Player.duration) } })
        })
        return s
    }
    // A playlist into a listening room (--tour-room-add, with EVERPLAY_SERVER): a new room, the dialog,
    // over a hundred tracks at once, then shuffling and clearing what is still to play.
    readonly property bool withRoomAdd: Qt.application.arguments.indexOf("--tour-room-add") >= 0
    readonly property var roomAddSteps: {
        if (!withRoomAdd)
            return []
        const rows = () => Library.libraryRows().filter(r => Library.tracks.get(r).source === "youtube").slice(0, 130)
        const queue = () => (Online.room.queue || []).length
        return [
            { name: "RA1-create", run: () => { Nav.finishOnboarding(); Player.muted = true; Online.createRoom("Tour room", "open") } },
            { name: "RA2-dialog", run: () => { console.log("tour: in room", Online.inRoom, "| rows", rows().length); Nav.addToRoom(rows(), "Tour") } },
            { name: "RA3-add", run: () => { Nav.navigate("room"); Online.addToRoom(rows()) } },
            { name: "RA4-added", run: () => { console.log("tour: queue", queue(), "| upcoming", JSON.stringify((Online.room.queue || []).slice(1, 4).map(i => i.row))) } },
            { name: "RA5-shuffle", run: () => Online.shuffleRoom() },
            { name: "RA6-shuffled", run: () => console.log("tour: after shuffle", queue(), "| upcoming", JSON.stringify((Online.room.queue || []).slice(1, 4).map(i => i.row))) },
            { name: "RA7-clear", run: () => Online.clearRoomQueue() },
            { name: "RA8-cleared", run: () => { console.log("tour: after clear", queue(), "| playing", Online.room.currentRow); Online.leaveRoom() } }
        ]
    }

    // A shared playlist's own cover (--tour-shared-cover "<playlist id>;<picture>", with EVERPLAY_SERVER and a test
    // profile): joining shows the cover the others set; a new one goes to the server for everyone.
    readonly property bool withSharedCover: Qt.application.arguments.indexOf("--tour-shared-cover") >= 0
    readonly property var sharedCoverSteps: {
        if (!withSharedCover)
            return []
        const [id, picture] = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-shared-cover") + 1].split(";")
        const index = () => Library.playlists.findIndex(p => p.kind === "shared" && p.sharedId === id)
        const log = what => { const i = index(); const p = i >= 0 ? Library.playlists[i] : {}
                              console.log("tour:", what, "| index", i, "| cover", p.cover, "| custom", p.customCover) }
        return [
            { name: "SC1-join", run: () => { Nav.finishOnboarding(); Online.openLink(Online.serverUrl + "/p/" + id) } },
            { name: "SC2-joined", run: () => log("joined") },
            { name: "SC3-set", run: () => console.log("tour: set =", Library.setPlaylistCover(index(), "file:///" + picture)) },
            { name: "SC4-after-set", run: () => log("after set") },
            { name: "SC5-reset", run: () => Library.resetPlaylistCover(index()) },
            { name: "SC6-after-reset", run: () => log("after reset") }
        ]
    }

    // The queue over the full-screen player (--tour-queue): the "Up next" bar, half and full height, a jump.
    readonly property bool withQueue: Qt.application.arguments.indexOf("--tour-queue") >= 0
    readonly property var queueSteps: {
        if (!withQueue)
            return []
        const log = () => console.log("tour: queue", Nav.immersiveQueue || "closed", "| left", Player.queueLeft, "| shown",
                                      Player.queueAhead.length, "| seconds", Player.queueSeconds, "| playing", Player.title)
        return [
            { name: "Q1-immersive", run: () => { Nav.finishOnboarding(); Player.muted = true
                                                const rows = Library.libraryRows()
                                                Player.playFrom(rows, rows[0]); Nav.immersive = true } },
            { name: "Q2-half", run: () => { log(); Nav.immersiveQueue = "half" } },
            { name: "Q3-full", run: () => { log(); Nav.immersiveQueue = "full" } },
            { name: "Q4-jump", run: () => { console.log("tour: shuffle", Player.shuffle, "| next", Player.queueAhead.slice(0, 3).map(r => Library.tracks.get(r).title).join(" / "))
                                           Player.jumpTo(Player.queueAhead[2]) } },
            { name: "Q5-closed", run: () => { log(); Nav.immersiveQueue = "" } },
            { name: "Q6-left", run: () => { log(); Nav.immersive = false } }
        ]
    }

    // Online features against a server (--tour-online "<room code>;<playlist id>;<share code>", with
    // EVERPLAY_SERVER and EVERPLAY_ONLINE_DEBUG): a share link, joining a shared playlist and a room,
    // the player's buttons going to the room, the dialogs, leaving.
    // Phone layout (run with --mobile): the pages the base tour skips, the "More" sheet, a track's
    // menu, and a YouTube track played through the server (with EVERPLAY_SERVER_STREAMS=1).
    readonly property bool withMobile: Qt.application.arguments.indexOf("--tour-mobile") >= 0
    function findTrackRow(item) {
        if (item.openMenu && item.t && item.t.title && item.visible && item.opacity > 0.5)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = root.findTrackRow(item.children[i])
            if (found)
                return found
        }
        return null
    }
    readonly property var mobileSteps: {
        if (!withMobile)
            return []
        const query = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-mobile") + 1]
        return [
            { name: "P0-home", run: () => { Player.muted = true; Nav.navigate("home") } },
            { name: "P1-room", run: () => Nav.navigate("room") },
            { name: "P2-equalizer", run: () => Nav.navigate("equalizer") },
            { name: "P3-stats", run: () => Nav.navigate("stats") },
            { name: "P4-more", run: () => { Nav.navigate("home"); Nav.moreRequested(true) } },
            { name: "P5-search", run: () => { Nav.moreRequested(false); Nav.navigate("search", { query: query, filter: 2 }) } },
            { name: "P6-results", run: () => {} },
            { name: "P7-menu", run: () => { const row = root.findTrackRow(root.target); if (row) row.openMenu(row.width - row.menuWidth, row.height) } },
            { name: "P8-play", run: () => { const r = Library.onlineResults(); if (r.length > 0) Player.playFrom(r, r[0]) } },
            { name: "P8b-wait", run: () => {} },
            { name: "P8c-wait", run: () => {} },
            { name: "P8d-wait", run: () => {} },
            { name: "P9-playing", run: () => console.log("tour: playing", Player.playing, "position", Player.position.toFixed(1), "duration", Player.duration) },
            { name: "P10-later", run: () => { console.log("tour: playing", Player.playing, "position", Player.position.toFixed(1)); Player.seek(120) } },
            { name: "P10b-seeked", run: () => console.log("tour: after seeking to 120 s:", Player.playing, Player.position.toFixed(1)) },
            { name: "P11-mini", run: () => Nav.navigate("library") },
            { name: "P12-immersive", run: () => Nav.immersive = true },
            { name: "P13-artist", run: () => { Nav.immersive = false; Nav.openArtist(Player.artist, Player.artistId) } },
            { name: "P14-artist-loaded", run: () => {} },
            { name: "P15-home-later", run: () => { console.log("tour: shelves", Online.musicShelves.length); Nav.navigate("home") } },
            { name: "P16-home-scrolled", run: () => {} }
        ]
    }
    readonly property bool withOnline: Qt.application.arguments.indexOf("--tour-online") >= 0
    readonly property var onlineSteps: {
        if (!withOnline)
            return []
        const [room, playlist, share] = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-online") + 1].split(";")
        const sharedIndex = () => Library.playlists.findIndex(p => p.kind === "shared")
        return [
            { name: "O1-share", run: () => { Nav.finishOnboarding(); Player.muted = true
                                            console.log("tour: link opened =", Online.openLink("everplay://t/" + share)) } },
            { name: "O2-playlist", run: () => { console.log("tour: shared track =", Player.title, "|", Player.artist)
                                               Online.openLink("https://" + Online.serverUrl.replace(/^https?:\/\//, "") + "/p/" + playlist) } },
            { name: "O3-playlist-add", run: () => { const i = sharedIndex()
                                                   console.log("tour: shared playlist", i, i >= 0 ? Library.playlists[i].name : "", "tracks", i >= 0 ? Library.playlistRows(i).length : -1)
                                                   if (i >= 0 && Player.currentIndex >= 0) Playlists.addRows(i, [Player.currentIndex]) } },
            { name: "O4-playlist-after", run: () => { const i = sharedIndex()
                                                     console.log("tour: after adding", i >= 0 ? Library.playlistRows(i).length : -1, JSON.stringify(Online.playlistInfo(Library.playlists[i].sharedId))) } },
            { name: "O5-room", run: () => Online.openLink("everplay://r/" + room) },
            { name: "O6-room-state", run: () => console.log("tour: in room", Online.inRoom, JSON.stringify(Online.room)) },
            { name: "O7-buttons", run: () => { Player.next(); Player.pause(); Player.seek(42) } },
            { name: "O8-after-buttons", run: () => console.log("tour: room playing", Online.room.playing, "| player", Player.title, Player.inRoom) },
            { name: "O9-leave", run: () => { Online.leaveRoom(); console.log("tour: in room after leaving", Online.inRoom, Player.inRoom) } },
            { name: "O10-room-start", run: () => Nav.navigate("room") },
            { name: "O10b-create", run: () => Online.createRoom("Created here", "host") },
            { name: "O10c-created", run: () => console.log("tour: created", Online.inRoom, Online.room.name, Online.room.mode, "host", Online.room.isHost, Online.room.url) },
            { name: "O11-dialog", run: () => Nav.createPlaylist([]) }
        ]
    }
    // The account's library on this device (--tour-library "<pair code>", with EVERPLAY_SERVER): links with a
    // code from another device, shows what came over, then unlikes one track so the other device sees it.
    readonly property bool withLibrarySync: Qt.application.arguments.indexOf("--tour-library") >= 0
    readonly property var librarySyncSteps: {
        if (!withLibrarySync)
            return []
        const code = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-library") + 1]
        const liked = () => Library.likedTracks().map(r => Library.tracks.get(r).title)
        return [
            { name: "S1-link", run: () => { Nav.finishOnboarding(); Player.muted = true
                                           console.log("tour: liked before", JSON.stringify(liked())); Online.redeemPairCode(code) } },
            { name: "S2-settings", run: () => { console.log("tour: registered", Online.registered, "sync", Online.librarySync); Nav.navigate("settings") } },
            { name: "S2b-code", run: () => { Online.createPairCode(); root.scrollPage(2300) } },
            { name: "S3-liked", run: () => { console.log("tour: liked after", JSON.stringify(liked())); Nav.navigate("library") } },
            { name: "S4-home", run: () => Nav.navigate("home") },
            { name: "S5-unlike", run: () => { const r = Library.likedTracks()
                                              if (r.length) { Player.playFrom(r, r[0], "library"); Player.pause() } } },
            { name: "S6-unliked", run: () => { Player.toggleLiked(); console.log("tour: unliked", Player.title) } },
            { name: "S7-after", run: () => console.log("tour: liked at the end", JSON.stringify(liked())) }
        ]
    }
    // Copies of the YouTube Music playlists on the account (--tour-mirrors, with EVERPLAY_SERVER and a profile
    // signed in to YouTube Music): makes the account, then waits while the playlists load and go up.
    readonly property bool withMirrors: Qt.application.arguments.indexOf("--tour-mirrors") >= 0
    readonly property var mirrorSteps: {
        if (!withMirrors)
            return []
        const s = [{ name: "R1-account", run: () => { Nav.finishOnboarding(); Player.muted = true
                                                     console.log("tour: youtube", YouTube.connected, "playlists", YouTube.playlistCount)
                                                     Online.createPairCode() } }]
        for (let i = 2; i <= 16; ++i)
            s.push({ name: "R" + i + "-wait", run: () => console.log("tour: registered", Online.registered, "liked", Library.likedTracks().length) })
        s.push({ name: "R17-library", run: () => Nav.navigate("library") })
        return s
    }
    // Joining a shared playlist that has a password (--tour-password "<playlist id>;<password>", with
    // EVERPLAY_SERVER): the dialog, a wrong password, the right one.
    readonly property bool withPassword: Qt.application.arguments.indexOf("--tour-password") >= 0
    readonly property var passwordSteps: {
        if (!withPassword)
            return []
        const [id, password] = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-password") + 1].split(";")
        const shared = () => Library.playlists.filter(p => p.kind === "shared").map(p => p.name)
        return [
            { name: "P1-open", run: () => { Nav.finishOnboarding(); Player.muted = true
                                           Online.openLink(Online.serverUrl + "/p/" + id) } },
            { name: "P2-dialog", run: () => console.log("tour: shared before", JSON.stringify(shared())) },
            { name: "P3-wrong", run: () => Online.joinWithPassword(id, "wrong-" + password) },
            { name: "P4-wrong-dialog", run: () => console.log("tour: still", JSON.stringify(shared())) },
            { name: "P5-right", run: () => Online.joinWithPassword(id, password) },
            { name: "P6-joined", run: () => console.log("tour: shared after", JSON.stringify(shared()), JSON.stringify(Online.playlistInfo(id))) }
        ]
    }
    // The Everplay account (--tour-account "<login>;<password>", with EVERPLAY_SERVER): an account made on this
    // device gets a login and a password, the recovery key, a new password, signing out and in again, and is
    // deleted at the end (so it can run against the real server).
    readonly property bool withAccount: Qt.application.arguments.indexOf("--tour-account") >= 0
    readonly property var accountSteps: {
        if (!withAccount)
            return []
        const [login, password] = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-account") + 1].split(";")
        const log = what => console.log("tour: " + what, "| registered", Online.registered, "| login", JSON.stringify(Online.login),
                                        "| devices", JSON.stringify(Online.devices.map(d => d.name + (d.current ? " (this)" : ""))))
        return [
            { name: "K1-anonymous", run: () => { Nav.finishOnboarding(); Player.muted = true; Online.createPairCode()
                                                 Nav.navigate("settings", { section: "account" }) } },
            { name: "K2-settings", run: () => log("before") },
            { name: "K3-create", run: () => Nav.account("create") },
            { name: "K4-create", run: () => Online.createLogin(login, password) },
            { name: "K4b-key", run: () => {} },
            { name: "K5-settings", run: () => { log("with a login"); Nav.account("password") } },
            { name: "K6-wrong", run: () => Online.changePassword("not-" + password, password + "2") },
            { name: "K7-changed", run: () => Online.changePassword(password, password + "2") },
            { name: "K8-signed-in", run: () => log("new password") },
            { name: "K9-sign-out", run: () => Online.signOut() },
            { name: "K10-signed-out", run: () => { log("signed out"); Nav.account("signin") } },
            { name: "K11-wrong", run: () => Online.signIn(login, password) },
            { name: "K11b-error", run: () => {} },
            { name: "K12-sign-in", run: () => Online.signIn(login, password + "2") },
            { name: "K13-back", run: () => log("signed in again") },
            { name: "K14-delete", run: () => Online.forgetAccount() },
            { name: "K15-deleted", run: () => log("deleted") }
        ]
    }
    // Seeking in liked SoundCloud tracks, the way the progress bar does it (--tour-sc-seek-liked).
    readonly property bool withScSeekLiked: Qt.application.arguments.indexOf("--tour-sc-seek-liked") >= 0
    readonly property var scSeekLikedSteps: {
        const log = what => console.log("tour: " + what, "|", Player.title, "| pos", Player.position.toFixed(1), "/", Player.duration, "| playing", Player.playing, "| buffering", Player.buffering)
        const s = [
            { name: "Y1-liked", run: () => { Nav.finishOnboarding(); Player.muted = true; console.log("tour: connected", SoundCloud.connected, "liked", SoundCloud.likedCount) } },
            { name: "Y2-play", run: () => {
                const i = Library.playlists.findIndex(p => p.kind === "scliked")
                const r = i >= 0 ? Library.playlistRows(i) : []
                console.log("tour: liked rows", r.length)
                if (r.length) Player.playFrom(r, r[0], "playlist:scliked")
            } },
            { name: "Y3-wait", run: () => log("start") }
        ]
        for (let k = 0; k < 5; ++k) {
            s.push({ name: "Y" + (4 + k * 3) + "-seek", run: () => { log("before seek to 60%"); Player.seek(0.6 * Player.duration) } })
            s.push({ name: "Y" + (5 + k * 3) + "-after", run: () => log("after seek") })
            s.push({ name: "Y" + (6 + k * 3) + "-next", run: () => { log("going next"); Player.next() } })
        }
        return s
    }
    // Seeking around a SoundCloud track several times (--tour-sc-seek): it must stay on the same track.
    readonly property bool withScSeek: Qt.application.arguments.indexOf("--tour-sc-seek") >= 0
    readonly property var scSeekSteps: {
        const log = what => console.log("tour: " + what, "|", Player.title, "| pos", Player.position.toFixed(1), "/", Player.duration, "| playing", Player.playing, "| buffering", Player.buffering)
        const s = [
            { name: "Z1-search", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: "sanah szampan", filter: 3 }) } },
            { name: "Z2-wait", run: () => {} },
            { name: "Z3-play", run: () => { const r = Library.soundCloudResults(); if (r.length) Player.playFrom(r, r[0], "search") } },
            { name: "Z4-wait", run: () => log("start") }
        ]
        // Dragging the progress bar: a seek every 30 ms, from 20 s to 140 s.
        s.push({ name: "Z4b-drag", run: () => {
            log("before drag")
            const timer = Qt.createQmlObject("import QtQuick; Timer { interval: 30; repeat: true }", root)
            let t = 20
            timer.triggered.connect(() => { Player.seek(t); t += 3; if (t > 140) { timer.stop(); timer.destroy() } })
            timer.start()
        } })
        s.push({ name: "Z4c-dragged", run: () => log("after drag") })
        s.push({ name: "Z4d-wait", run: () => log("after drag +2s") })
        s.push({ name: "Z4e-end", run: () => { log("before seek 160"); Player.seek(160) } })
        s.push({ name: "Z4f-end", run: () => log("after seek 160") })
        ;[30, 90, 150, 20, 120, 60].forEach((t, i) => {
            s.push({ name: "Z" + (5 + i * 2) + "-seek" + t, run: () => { log("before seek " + t); Player.seek(t) } })
            s.push({ name: "Z" + (6 + i * 2) + "-after", run: () => log("after seek " + t) })
        })
        return s
    }
    // Searching by title (--tour-search-title): what each source returns for a few known titles.
    readonly property bool withSearchTitle: Qt.application.arguments.indexOf("--tour-search-title") >= 0
    readonly property var searchTitleSteps: {
        const titles = (Qt.application.arguments[Qt.application.arguments.indexOf("--tour-search-title") + 1] || "Get Lucky;Blinding Lights").split(";")
        const s = []
        titles.forEach((q, i) => {
            s.push({ name: "Q" + i + "-search", run: () => { Nav.finishOnboarding(); Nav.navigate("search", { query: q }) } })
            s.push({ name: "Q" + i + "-wait", run: () => {} })
            s.push({ name: "Q" + i + "-results", run: () => {
                const fmt = rows => rows.slice(0, 10).map(r => Library.tracks.get(r).title + " / " + Library.tracks.get(r).artist).join(" | ")
                console.log("tour: [" + q + "] YT:", fmt(Library.onlineResults()))
                console.log("tour: [" + q + "] SC:", fmt(Library.soundCloudResults()))
            } })
        })
        return s
    }
    // The letter-by-letter highlight switch in the settings (--tour-karaoke).
    readonly property bool withKaraoke: Qt.application.arguments.indexOf("--tour-karaoke") >= 0
    readonly property var karaokeSteps: [
        { name: "K1-settings", run: () => { Nav.finishOnboarding(); Nav.navigate("settings") } },
        { name: "K2-lyrics", run: () => root.scrollPage(1500) },
        { name: "K3-off", run: () => { Lyrics.karaoke = false; console.log("tour: karaoke", Lyrics.karaoke, "canTimeWords", Lyrics.canTimeWords) } },
        { name: "K4-on", run: () => { Lyrics.karaoke = true; console.log("tour: karaoke", Lyrics.karaoke) } }
    ]
    // SoundCloud sign-in with a profile that's already signed in (--tour-sc-login): the window, the readout, the sync.
    readonly property bool withScLogin: Qt.application.arguments.indexOf("--tour-sc-login") >= 0
    readonly property var scLoginSteps: {
        const log = () => console.log("tour: sc loggingIn", SoundCloud.loggingIn, "| status", SoundCloud.loginStatus, "| connected", SoundCloud.connected,
                                      "| account", SoundCloud.accountName, "| syncing", SoundCloud.syncing, "| liked", SoundCloud.likedCount, "| playlists", SoundCloud.playlistCount)
        const s = [{ name: "L1-login", run: () => { Nav.finishOnboarding(); Nav.navigate("sources"); SoundCloud.login() } }]
        for (let i = 2; i <= 14; ++i)
            s.push({ name: "L" + i + "-wait", run: log })
        s.push({ name: "L15-sources", run: () => { log(); root.scrollPage(900) } })
        s.push({ name: "L16-sidebar", run: () => { log(); Nav.navigate("playlist", { kind: "playlist", index: Library.playlists.findIndex(p => p.kind === "scliked") }) } })
        s.push({ name: "L17-wait", run: log })
        return s
    }
    // Discovery (--tour-discover "<query>;<soundcloud query>"): a track's radio and how it grows, the radio
    // after a queue's end (autoplay), a SoundCloud track's radio, then the home page's radios and new artists.
    readonly property bool withDiscover: Qt.application.arguments.indexOf("--tour-discover") >= 0
    readonly property var discoverSteps: {
        if (!withDiscover)
            return []
        const arg = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-discover") + 1].split(";")
        const queue = label => {
            const ahead = Player.queueAhead
            console.log("tour: discover", label, "| context", Player.queueContext, "| playing", Player.title, "-", Player.artist,
                        "| ahead", ahead.length, "|", ahead.slice(0, 6).map(r => Library.tracks.get(r).artist + " / " + Library.tracks.get(r).title).join(" | "))
        }
        const s = [
            { name: "V1-search", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: arg[0], filter: 2 }) } },
            { name: "V2-wait", run: () => {} },
            { name: "V3-radio", run: () => { const r = Library.onlineResults(); if (r.length) Player.startRadio(r[0]) } },
            { name: "V4-wait", run: () => {} },
            { name: "V5-station", run: () => queue("station") },
            { name: "V6-near-end", run: () => { const a = Player.queueAhead; if (a.length > 2) Player.jumpTo(a[a.length - 2]) } },
            { name: "V7-wait", run: () => {} },
            { name: "V8-grown", run: () => queue("grown") },
            // Autoplay: one track alone, near its end — a radio of it follows.
            { name: "V9-single", run: () => { const r = Library.onlineResults(); if (r.length > 1) Player.playFrom([r[1]], r[1]) } },
            { name: "V10-wait", run: () => {} },
            { name: "V11-seek", run: () => { queue("single"); Player.seek(Math.max(0, Player.duration - 4)) } },
            { name: "V12-wait", run: () => {} },
            { name: "V13-wait", run: () => {} },
            { name: "V14-after", run: () => queue("after the end") },
            { name: "V15-sc", run: () => Nav.navigate("search", { query: arg[1] || arg[0], filter: 3 }) },
            { name: "V16-wait", run: () => {} },
            { name: "V17-scradio", run: () => { const r = Library.soundCloudResults(); if (r.length) Player.startRadio(r[0]) } },
            { name: "V18-wait", run: () => {} },
            { name: "V19-scstation", run: () => queue("soundcloud station") },
            { name: "V20-home", run: () => { Recommendations.refresh(); Nav.navigate("home") } }
        ]
        for (let i = 0; i < 8; ++i)
            s.push({ name: "V21-wait" + i, run: () => {} })
        s.push({ name: "V22-home", run: () => {
            console.log("tour: discover home | radios", Recommendations.radioSeeds.length, "|",
                        Recommendations.radioSeeds.map(x => x.artist).join(", "), "| recommended", Library.recommendedRows().length,
                        "| new artists", Library.newArtistRows().length, "|",
                        Library.newArtistRows().slice(0, 5).map(r => Library.tracks.get(r).artist).join(", "))
        } })
        s.push({ name: "V23-scrolled", run: () => {
            const find = item => { if (item.objectName === "homeRadios") return item; for (let i = 0; i < item.children.length; ++i) { const f = find(item.children[i]); if (f) return f } return null }
            const r = find(root.target)
            console.log("tour: discover radios section", r ? "y " + Math.round(r.y) + " h " + Math.round(r.height) + " visible " + r.visible : "missing")
            let f = r
            while (f && f.contentY === undefined)
                f = f.parent
            if (f) {
                const at = r.mapToItem(f.contentItem, 0, 0).y
                console.log("tour: discover radios at", Math.round(at), "of", Math.round(f.contentHeight))
                f.contentY = Math.max(0, at - 40)
            }
        } })
        s.push({ name: "V24-check", run: () => {
            const find = item => { if (item.objectName === "homeRadios") return item; for (let i = 0; i < item.children.length; ++i) { const f = find(item.children[i]); if (f) return f } return null }
            const r = find(root.target)
            let f = r
            while (f && f.contentY === undefined)
                f = f.parent
            console.log("tour: discover check | contentY", f ? Math.round(f.contentY) : -1, "| radios on screen at", r ? Math.round(r.mapToItem(null, 0, 0).y) : -1,
                        "| children", r ? r.children[0].children[0].children.length : -1)
        } })
        return s
    }
    // A SoundCloud profile (--tour-sc-profile "<query>"): the profiles found, the first one opened, its
    // popular track played, then the profile opened again from the playing track's artist.
    readonly property bool withScProfile: Qt.application.arguments.indexOf("--tour-sc-profile") >= 0
    readonly property var scProfileSteps: {
        if (!withScProfile)
            return []
        const query = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-sc-profile") + 1]
        return [
            { name: "S1-search", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("search", { query: query, filter: 3 }) } },
            { name: "S2-wait", run: () => {} },
            { name: "S3-found", run: () => {
                console.log("tour: profiles", SoundCloud.userResults.length, "|", SoundCloud.userResults.slice(0, 4).map(u => u.name + " (" + u.subtitle + ")").join(" | "))
                const u = SoundCloud.userResults[0]
                if (u) Nav.openSoundCloudProfile(u.id, u.name, u.photo)
            } },
            { name: "S4-wait", run: () => {} },
            { name: "S5-profile", run: () => {
                const id = Nav.args.userId
                const p = SoundCloud.profile(id)
                const pop = Library.soundCloudProfileRows(id, "popular"), all = Library.soundCloudProfileRows(id, "tracks")
                console.log("tour: profile", p.name, "| loading", p.loading, "| error", p.error || "", "| popular", pop.length,
                            "| tracks", all.length, "| albums", (p.albums || []).length, "| playlists", (p.playlists || []).length)
                if (pop.length) Player.playFrom(pop, pop[0], "scu:" + id + "/popular")
                Nav.navigate("home")
            } },
            { name: "S6-wait", run: () => {} },
            { name: "S7-artist", run: () => {
                console.log("tour: playing", Player.title, "| artistId", Player.artistId, "| playing", Player.playing)
                Nav.openArtist(Player.artist, Player.artistId)
            } },
            { name: "S8-again", run: () => console.log("tour: page", Nav.page, "| user", Nav.args.userId) },
        ]
    }
    // SoundCloud: search, play a track (and seek in it), open a found playlist, the sources (--tour-soundcloud).
    readonly property bool withSoundCloud: Qt.application.arguments.indexOf("--tour-soundcloud") >= 0
    readonly property var soundcloudSteps: [
        { name: "C1-search", run: () => { Nav.finishOnboarding(); Nav.navigate("search", { query: "lofi hip hop", filter: 3 }) } },
        { name: "C2-wait", run: () => {} },
        { name: "C3-results", run: () => {
            const r = Library.soundCloudResults()
            console.log("tour: soundcloud tracks", r.length, "|", r.slice(0, 4).map(x => Library.tracks.get(x).title + " / " + Library.tracks.get(x).artist).join(" | "))
            console.log("tour: soundcloud playlists", SoundCloud.playlistResults.length)
            if (r.length) Player.playFrom(r, r[0], "search")
        } },
        { name: "C4-wait", run: () => {} },
        { name: "C5-playing", run: () => { console.log("tour: playing", Player.title, "| pos", Player.position.toFixed(1), "| dur", Player.duration); Player.seek(60) } },
        { name: "C6-wait", run: () => {} },
        { name: "C7-seeked", run: () => {
            console.log("tour: after seek pos", Player.position.toFixed(1), "playing", Player.playing)
            const p = SoundCloud.playlistResults[0]
            root.album = p ? { playlistId: p.id } : null
            if (p) Nav.navigate("playlist", { kind: "sccollection", playlistId: p.id, title: p.title, subtitle: p.subtitle, cover: p.cover })
        } },
        { name: "C8-wait", run: () => {} },
        { name: "C9-playlist", run: () => {
            const r = root.album ? Library.soundCloudCollectionRows(root.album.playlistId) : []
            console.log("tour: playlist rows", r.length)
            if (r.length > 1) Player.playFrom(r, r[1], "scc:" + root.album.playlistId)
        } },
        { name: "C10-wait", run: () => {} },
        { name: "C11-playing", run: () => { console.log("tour: playing", Player.title, "| pos", Player.position.toFixed(1)); Player.next() } },
        { name: "C12-wait", run: () => {} },
        { name: "C13-next", run: () => { console.log("tour: next", Player.title, "| pos", Player.position.toFixed(1)); Nav.navigate("search", { query: "daft punk" }) } },
        { name: "C14-wait", run: () => {} },
        { name: "C15-all", run: () => root.scrollPage(1400) },
        { name: "C16-sources", run: () => { Player.pause(); Nav.navigate("sources") } },
        { name: "C17-sc", run: () => root.scrollPage(900) },
        { name: "C18-home", run: () => Nav.navigate("home") },
        { name: "C19-bottom", run: () => root.scrollPage(4000) }
    ]
    // Stats, "Recently played" and resuming a long recording (--tour-stats).
    // Lyrics recognized for a track without any (--tour-recognize <folder>: a folder with one recording
    // with words in it; the profile needs CrispASR and the "small" model).
    readonly property bool withRecognize: Qt.application.arguments.indexOf("--tour-recognize") >= 0
    readonly property string recognizeFolder: withRecognize ? Qt.application.arguments[Qt.application.arguments.indexOf("--tour-recognize") + 1] : ""
    function recognizeLog() {
        console.log("tour: lyrics", Lyrics.state, "| can recognize", Lyrics.canTranscribe, "| recognizing", Lyrics.recognizing,
                    Math.round(Lyrics.alignProgress * 100) + "%", Lyrics.alignStage, "| lines", Lyrics.lines.length,
                    "| origin", Lyrics.origin)
    }
    readonly property var recognizeSteps: {
        const steps = []
        steps.push(...root.sttDownloadSteps)
        steps.push(...[
            { name: "R1-add", run: () => { Nav.finishOnboarding(); Nav.queueOpen = true; Lyrics.sttModel = "small"
                                           Library.addFolder("file:///" + root.recognizeFolder.replace(/\\/g, "/")) } },
            { name: "R2-wait", run: () => {} },
            { name: "R3-play", run: () => {
                const r = Library.localTracks()
                console.log("tour: local tracks", r.length, r.length ? Library.tracks.get(r[0]).title : "")
                if (r.length) Player.playFrom(r, r[0], "test")
                Nav.panelTab = "lyrics"
                Nav.immersive = true
            } },
            { name: "R4-wait", run: () => root.recognizeLog() },
            { name: "R5-recognize", run: () => { root.recognizeLog(); Lyrics.transcribeNow() } }
        ])
        for (let i = 0; i < 40; ++i)
            steps.push({ name: "R6-wait" + i, run: () => root.recognizeLog() })
        steps.push({ name: "R7-lines", run: () => {
            root.recognizeLog()
            Lyrics.lines.forEach(l => console.log("tour: line", l.start.toFixed(1), "-", l.end.toFixed(1), "|", l.text))
        } })
        return steps
    }

    readonly property bool withStats: Qt.application.arguments.indexOf("--tour-stats") >= 0
    property int longRow: -1
    property int shortRow: -1
    function statsLog(period) {
        const s = ListeningHistory.stats(period)
        console.log("tour: stats", period, "| minutes", s.minutes, "| plays", s.plays, "| artists", s.artists, "| days", s.days,
                    "| top", (s.topArtists || []).slice(0, 3).map(a => a.name + " " + a.plays).join(", "),
                    "| tracks", (s.topTracks || []).length, "| genres", (s.topGenres || []).map(g => g.name).join(", "),
                    "| peak hour", (s.byHour || []).indexOf(Math.max(...(s.byHour || [0]))))
    }
    readonly property var statsSteps: [
        { name: "T1-stats", run: () => { Nav.finishOnboarding(); Nav.navigate("stats"); ["month", "year", "all"].forEach(p => root.statsLog(p)) } },
        { name: "T2-history", run: () => {
            const i = Library.playlists.findIndex(p => p.kind === "history")
            console.log("tour: history playlist", i, i >= 0 ? Library.playlists[i].name : "", "| rows", i >= 0 ? Library.playlistRows(i).length : -1)
            if (i >= 0) Nav.navigate("playlist", { kind: "playlist", index: i })
        } },
        { name: "T3-long", run: () => {
            const rows = Library.libraryRows()
            root.longRow = rows.find(r => Library.tracks.get(r).duration >= 1200 && Library.tracks.get(r).source === "youtube") ?? -1
            root.shortRow = rows.find(r => Library.tracks.get(r).duration > 0 && Library.tracks.get(r).duration < 400) ?? -1
            console.log("tour: long", root.longRow >= 0 ? Library.tracks.get(root.longRow).title + " " + Library.tracks.get(root.longRow).duration : "none")
            if (root.longRow >= 0) Player.playFrom([root.longRow], root.longRow, "test")
        } },
        { name: "T4-wait", run: () => {} },
        { name: "T5-seek", run: () => { Player.seek(125); console.log("tour: seeked", Player.position.toFixed(0)) } },
        { name: "T6-other", run: () => { if (root.shortRow >= 0) Player.playFrom([root.shortRow], root.shortRow, "test"); console.log("tour: switched to", Player.title) } },
        { name: "T7-back", run: () => { Player.playFrom([root.longRow], root.longRow, "test") } },
        { name: "T8-wait", run: () => console.log("tour: back on", Player.title, "| position", Player.position.toFixed(0)) },
        { name: "T9-wait", run: () => console.log("tour: position", Player.position.toFixed(0)) }
    ]
    // The main screens one after another, for a look at the design (--tour-design): a library track
    // is loaded muted and paused, so the player bar and the full-screen view have something to show.
    // The redesigned interface with something playing (--tour-redesign): plays the listening history
    // muted and walks the main screens.
    readonly property bool withRedesign: Qt.application.arguments.indexOf("--tour-redesign") >= 0
    function historyIndex() { return Library.playlists.findIndex(p => p.kind === "history") }
    function scrollTo(fraction) {
        const f = root.findFlick(root.target)
        if (!f)
            return console.warn("tour: nothing to scroll")
        f.contentY = Math.max(0, (f.contentHeight - f.height) * fraction)
        console.warn("tour: scroll", Math.round(f.contentY), "of", Math.round(f.contentHeight), "view", Math.round(f.height), f)
    }
    // The Nokturn → Everplay intro held on a series of moments (--tour-intro)
    readonly property bool withIntro: Qt.application.arguments.indexOf("--tour-intro") >= 0
    readonly property var introSteps: [
        { name: "I00-0_6", run: () => { Nav.finishOnboarding(); Nav.introTime = 0.6 } },
        { name: "I01-1_2", run: () => { Nav.finishOnboarding(); Nav.introTime = 1.2 } },
        { name: "I02-1_85", run: () => { Nav.finishOnboarding(); Nav.introTime = 1.85 } },
        { name: "I03-2_2", run: () => { Nav.finishOnboarding(); Nav.introTime = 2.2 } },
        { name: "I04-2_55", run: () => { Nav.finishOnboarding(); Nav.introTime = 2.55 } },
        { name: "I05-2_9", run: () => { Nav.finishOnboarding(); Nav.introTime = 2.9 } },
        { name: "I06-3_4", run: () => { Nav.finishOnboarding(); Nav.introTime = 3.4 } },
        { name: "I07-4_0", run: () => { Nav.finishOnboarding(); Nav.introTime = 4.0 } },
        { name: "I08-4_6", run: () => { Nav.finishOnboarding(); Nav.introTime = 4.6 } },
        { name: "I09-5_2", run: () => { Nav.finishOnboarding(); Nav.introTime = 5.2 } },
        { name: "I10-5_8", run: () => { Nav.finishOnboarding(); Nav.introTime = 5.8 } },
        { name: "I11-6_6", run: () => { Nav.finishOnboarding(); Nav.introTime = 6.6 } },
        { name: "I99-done", run: () => Nav.introTime = -1 }
    ]
    readonly property var redesignSteps: [
        { name: "R00-start", run: () => { Nav.finishOnboarding(); Theme.themeId = "everplay"; Theme.shape = "rounded"; Player.muted = true; Nav.queueOpen = true; Nav.sidebarCompact = false
                                          const i = root.historyIndex(); const rows = i >= 0 ? Library.playlistRows(i) : Library.libraryRows()
                                          if (rows.length) Player.playFrom(rows.slice(0, 30), rows[0], "test")
                                          Nav.navigate("home") } },
        { name: "R00-wait", run: () => {} },
        { name: "R01-home", run: () => Nav.navigate("home") },
        { name: "R01b-home-mid", run: () => root.scrollTo(0.45) },
        { name: "R01c-home-end", run: () => root.scrollTo(1.0) },
        { name: "R02-playlist", run: () => Nav.navigate("playlist", { kind: "playlist", index: root.historyIndex() }) },
        { name: "R03-albums", run: () => Nav.navigate("library", { tab: 1 }) },
        { name: "R04-search", run: () => Nav.navigate("search", { query: Player.artist }) },
        { name: "R05-lyrics", run: () => { Nav.panelTab = "lyrics"; Nav.navigate("library") } },
        { name: "R06-eq", run: () => { Nav.panelTab = "queue"; Nav.navigate("equalizer") } },
        { name: "R07-settings", run: () => Nav.navigate("settings") },
        { name: "R08-stats", run: () => Nav.navigate("stats") },
        { name: "R09-compact", run: () => { Nav.sidebarCompact = true; Nav.queueOpen = false; Nav.navigate("home") } },
        { name: "R10-immersive", run: () => { Nav.sidebarCompact = false; Nav.queueOpen = true; Nav.immersive = true } },
        { name: "R11-back", run: () => Nav.immersive = false },
        { name: "R12-semi-home", run: () => { Theme.shape = "semi"; Nav.navigate("home") } },
        { name: "R13-semi-playlist", run: () => Nav.navigate("playlist", { kind: "playlist", index: root.historyIndex() }) },
        { name: "R14-semi-albums", run: () => Nav.navigate("library", { tab: 1 }) },
        { name: "R15-semi-settings", run: () => Nav.navigate("settings") },
        { name: "R16-semi-immersive", run: () => Nav.immersive = true },
        { name: "R17-rounded", run: () => { Nav.immersive = false; Theme.shape = "rounded" } }
    ]
    readonly property bool withDesign: Qt.application.arguments.indexOf("--tour-design") >= 0
    readonly property var designSteps: [
        { name: "D01-home", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.queueOpen = true
                                         const rows = Library.libraryRows().slice(0, 20)
                                         if (rows.length) Player.playFrom(rows, rows[0], "test")
                                         Nav.navigate("home") } },
        { name: "D02-home", run: () => Player.pause() },
        { name: "D03-search", run: () => Nav.navigate("search") },
        { name: "D03b-results", run: () => { const rows = Library.libraryRows()
                                             Nav.navigate("search", { query: rows.length ? Library.tracks.get(rows[0]).artist : "" }) } },
        { name: "D03c-results", run: () => Nav.queueOpen = false },
        { name: "D03d-results-wide", run: () => Nav.queueOpen = true },
        { name: "D04-library", run: () => Nav.navigate("library") },
        { name: "D05-albums", run: () => Nav.navigate("library", { tab: 1 }) },
        { name: "D06-artists", run: () => Nav.navigate("library", { tab: 2 }) },
        { name: "D07-playlist", run: () => Nav.navigate("playlist", { kind: "playlist", index: 1 }) },
        { name: "D08-liked", run: () => Nav.navigate("playlist", { index: Library.playlists.findIndex(p => p.kind === "liked") }) },
        { name: "D09-stats", run: () => Nav.navigate("stats") },
        { name: "D10-eq", run: () => Nav.navigate("equalizer") },
        { name: "D11-sources", run: () => Nav.navigate("sources") },
        { name: "D12-settings", run: () => Nav.navigate("settings") },
        { name: "D13-noqueue", run: () => { Nav.queueOpen = false; Nav.navigate("home") } },
        { name: "D14-compact", run: () => { Nav.sidebarCompact = true; Nav.navigate("library") } },
        { name: "D15-immersive", run: () => { Nav.sidebarCompact = false; Nav.queueOpen = true; Nav.immersive = true } },
        { name: "D16-back", run: () => Nav.immersive = false }
    ]
    // Plugins (--tour-plugins, with EVERPLAY_PLUGIN_CATALOG pointing at a built catalog): installs everything
    // from the catalog, plays muted with the wallpaper, picks a plugin's theme, shows an overlay, removes one.
    readonly property bool withPlugins: Qt.application.arguments.indexOf("--tour-plugins") >= 0
    function pluginLog(label) {
        console.log("tour:", label, "|", Plugins.installed.map(p => p.id + (p.enabled ? "" : " (off)") + (p.error ? " ERROR " + p.error : "")).join(", ") || "none",
                    "| themes", Object.keys(Plugins.themes).join(", "))
    }
    readonly property var pluginSteps: [
        { name: "P01-catalog", run: () => { Nav.finishOnboarding(); Player.muted = true; Nav.navigate("plugins", { tab: 1 }) } },
        { name: "P02-install", run: () => { console.log("tour: catalog", Plugins.catalogState, Plugins.catalog.map(c => c.id + " " + c.version).join(", "))
                                            if (Plugins.catalog.length > 0) Plugins.installFromCatalog(Plugins.catalog[0].id) } },
        { name: "P03-install", run: () => { if (Plugins.catalog.length > 1) Plugins.installFromCatalog(Plugins.catalog[1].id) } },
        { name: "P04-install", run: () => { if (Plugins.catalog.length > 2) Plugins.installFromCatalog(Plugins.catalog[2].id) } },
        { name: "P05-catalog-after", run: () => root.pluginLog("installed") },
        { name: "P06-installed", run: () => Nav.navigate("plugins", { tab: 0 }) },
        { name: "P07-home", run: () => { const rows = Library.libraryRows().slice(0, 10)
                                         if (rows.length) Player.playFrom(rows, rows[0], "test")
                                         Nav.navigate("home") } },
        { name: "P08-wallpaper", run: () => {} },
        { name: "P09-theme", run: () => { Theme.themeId = "night-themes/deep-sea"; Nav.navigate("settings") } },
        { name: "P10-theme-home", run: () => Nav.navigate("library") },
        { name: "P11-badge", run: () => Player.next() },
        { name: "P12-off", run: () => { Plugins.setEnabled("spectrum-wallpaper", false); root.pluginLog("wallpaper off") } },
        { name: "P13-remove", run: () => { Theme.themeId = "everplay"; Plugins.remove("night-themes"); root.pluginLog("removed themes")
                                           Nav.navigate("plugins", { tab: 0 }) } },
        { name: "P14-file", run: () => {
            const dir = Qt.application.arguments[Qt.application.arguments.indexOf("--tour-plugins") + 1]
            Plugins.installFile("file:///" + dir + "/catalog/night-themes-1.0.0.zip")
            Plugins.installFile("file:///" + dir + "/broken-plugin/plugin.json")
            Plugins.installFile("file:///" + dir + "/doc-hello/plugin.json")
            Plugins.installFile("file:///" + dir + "/doc-accent/plugin.json")
            Plugins.setEnabled("spectrum-wallpaper", false)
            const s = Plugins.installed.find(p => p.id === "spectrum-wallpaper")
            const c = Qt.createComponent(s.settings.toString())
            console.log("tour: settings component", c.status === Component.Ready ? "ready" : c.errorString())
            root.pluginLog("from files") } },
        { name: "P15-errors", run: () => { root.pluginLog("errors"); console.log("tour: accent", Theme.accent, "colorA", Player.colorA, "level", Player.level.toFixed(2)); Nav.navigate("home") } },
        { name: "P16-off", run: () => { Plugins.setEnabled("accent-pulse", false); console.log("tour: accent after off", Theme.accent, "colorA", Player.colorA) } },
        { name: "P17-later", run: () => console.log("tour: accent later", Theme.accent, "colorA", Player.colorA) },
        { name: "P18-end", run: () => { Plugins.remove("broken-test"); Player.pause(); root.pluginLog("end") } }
    ]
    readonly property bool withThemes: Qt.application.arguments.indexOf("--tour-themes") >= 0
    // Every theme on the home page, then the Cat theme across the main screens.
    readonly property var themeSteps: [
        { name: "T1-everplay", run: () => { Nav.finishOnboarding(); Nav.queueOpen = true; Theme.themeId = "everplay"; Nav.navigate("home") } },
        { name: "T2-midnight", run: () => { Theme.themeId = "midnight"; Nav.navigate("home") } },
        { name: "T3-sunset", run: () => { Theme.themeId = "sunset"; Nav.navigate("home") } },
        { name: "T4-cat", run: () => { Theme.themeId = "cat"; Nav.navigate("home") } },
        { name: "T5-cat-settings", run: () => Nav.navigate("settings") },
        { name: "T6-cat-library", run: () => Nav.navigate("library") },
        { name: "T7-cat-compact", run: () => { Nav.sidebarCompact = true; Nav.queueOpen = false; Nav.navigate("home") } },
        { name: "T8-back", run: () => { Nav.sidebarCompact = false; Nav.queueOpen = true; Theme.themeId = "everplay" } }
    ]
    readonly property var steps: withIntro ? introSteps : withRedesign ? redesignSteps : withPlugins ? pluginSteps : withDesign ? designSteps : withAhead ? aheadSteps : withCache ? cacheSteps : withSmall ? smallSteps : withSharedCover ? sharedCoverSteps : withQueue ? queueSteps : withRoomAdd ? roomAddSteps : withAccount ? accountSteps : withTrackOrder ? trackOrderSteps : withOrder ? orderSteps : withFolders ? folderSteps : withResize ? resizeSteps : withPassword ? passwordSteps : withMirrors ? mirrorSteps : withLibrarySync ? librarySyncSteps : withMobile ? mobileSteps : withOnline ? onlineSteps : withListen ? listenSteps : withAudioDevice ? audioDeviceSteps : withScProfile ? scProfileSteps : withDiscover ? discoverSteps : withAlign ? alignSteps : withScSeekLiked ? scSeekLikedSteps : withFakeTiming ? fakeTimingSteps : withScSeek ? scSeekSteps : withSearchTitle ? searchTitleSteps : withKaraoke ? karaokeSteps : withScLogin ? scLoginSteps : withSoundCloud ? soundcloudSteps : withRecognize ? recognizeSteps : withThemes ? themeSteps : withStats ? statsSteps : withSearchPlaylists ? searchPlaylistSteps : withOverlayCycle ? overlayCycleSteps : withHotkey ? hotkeySteps : withOverlay ? overlaySteps : withLiked ? likedSteps : withArtist ? artistSteps : withHome ? homeSteps : withLyrics ? lyricSteps : withPlaylists ? playlistSteps : withAlbums ? albumSteps : withSeek ? seekSteps : withPerf ? perfSteps : withCovers ? coverSteps : withMatch ? matchSteps : withEq ? eqSteps : withBig ? bigSteps : withEngine ? engineSteps : withLogin ? loginSteps : baseSteps
    property double navStart: 0
    property bool measuring: false
    readonly property var bigSteps: [
        { name: "B1-home", run: () => console.log("tour: tracks in the model =", Library.tracks.count) },
        { name: "B2-liked", run: () => { root.navStart = Date.now(); root.measuring = true; Nav.navigate("playlist", { kind: "playlist", index: 2 }); console.log("tour: page build", Date.now() - root.navStart, "ms") } },
        { name: "B3-library", run: () => { root.navStart = Date.now(); root.measuring = true; Nav.navigate("library") } }
    ]

    Connections {
        target: root.target ? root.target.Window.window : null
        function onFrameSwapped() {
            if (root.measuring) {
                root.measuring = false
                console.log("tour: first frame after", Date.now() - root.navStart, "ms")
            }
        }
    }
    readonly property var engineSteps: [
        { name: "E1-download", run: () => { console.log("tour: engineReady before =", YouTube.engineReady); YouTube.downloadEngine() } },
        { name: "E2-wait", run: () => console.log("tour: downloading", Math.round(YouTube.engineProgress * 100) + "%") },
        { name: "E3-wait", run: () => {} },
        { name: "E4-wait", run: () => {} },
        { name: "E5-result", run: () => console.log("tour: engineReady after =", YouTube.engineReady, "version", YouTube.engineVersion) }
    ]
    readonly property var loginSteps: [
        { name: "L1-login-start", run: () => { Nav.navigate("sources"); YouTube.login() } },
        { name: "L2-login-wait", run: () => console.log("tour: loggingIn =", YouTube.loggingIn, "|", YouTube.loginStatus) },
        { name: "L3-wait", run: () => console.log("tour: loggingIn =", YouTube.loggingIn, "|", YouTube.loginStatus) },
        { name: "L4-wait", run: () => console.log("tour: loggingIn =", YouTube.loggingIn, "|", YouTube.loginStatus) },
        { name: "L5-login-cancel", run: () => { YouTube.cancelLogin(); console.log("tour: done, loggingIn =", YouTube.loggingIn) } }
    ]
    readonly property var baseSteps: [
        { name: "00a-welcome",      run: () => Nav.startOnboarding() },
        { name: "00b-layout",       run: () => Nav.onboardingStep = 1 },
        { name: "00c-layout-compact", run: () => { Nav.sidebarCompact = true; Nav.queueOpen = false } },
        { name: "00d-colors",       run: () => { Nav.sidebarCompact = false; Nav.queueOpen = true; Nav.onboardingStep = 2 } },
        { name: "00e-music",        run: () => Nav.onboardingStep = 3 },
        { name: "00f-playback",     run: () => { Player.crossfade = 6; Player.crossfadeStyle = "bass"; Nav.onboardingStep = 4 } },
        { name: "00g-integrations", run: () => { Discord.enabled = true; Nav.onboardingStep = 5 } },
        { name: "00h-done",         run: () => Nav.onboardingStep = 6 },
        { name: "01-home",      run: () => { Discord.enabled = false; Player.crossfade = 0; Nav.finishOnboarding(); Player.play(); Nav.navigate("home") } },
        { name: "02-search",    run: () => Nav.navigate("search") },
        { name: "03-library",   run: () => Nav.navigate("library") },
        { name: "04-albums",    run: () => Nav.navigate("library", { tab: 1 }) },
        { name: "05-playlist",  run: () => Nav.navigate("playlist", { kind: "playlist", index: 1 }) },
        { name: "06-sources",   run: () => Nav.navigate("sources") },
        { name: "07-settings",  run: () => Nav.navigate("settings") },
        { name: "08-compact",   run: () => { Nav.sidebarCompact = true; Nav.queueOpen = false; Nav.navigate("home") } },
        { name: "09-immersive", run: () => { Nav.sidebarCompact = false; Nav.queueOpen = true; Nav.immersive = true } },
        { name: "10-yt-search", run: () => { Nav.immersive = false; Nav.navigate("search", { query: "Daft Punk Get Lucky", filter: 2 }) } },
        { name: "11-yt-results", run: () => {} },
        { name: "12-yt-playing", run: () => { const r = Library.onlineResults(); if (r.length > 0) Player.playFrom(r, r[0]) } },
        { name: "13-yt-home", run: () => Nav.navigate("home") },
        { name: "14-yt-immersive", run: () => { Nav.immersive = true } }
    ]

    Connections {
        target: YouTube
        enabled: root.dir !== ""
        function onNotice(text, icon) { console.log("tour: notice:", text) }
    }
    Connections {
        target: SoundCloud
        enabled: root.dir !== ""
        function onNotice(text, icon) { console.log("tour: notice:", text) }
    }
    Connections {
        target: Spotify
        enabled: root.dir !== ""
        function onNotice(text, icon) { console.log("tour: notice:", text) }
        function onMatchesFound(trackId, results, error) {
            root.matchResults = results
            console.log("tour:", results.length, "results", error)
            results.forEach(r => console.log("tour:  ", r.kind, r.current ? "*" : " ", r.durationText, r.artist, "-", r.title))
        }
        function onMatchChanged(trackId) { console.log("tour: matchChanged", trackId) }
    }

    Timer {
        id: timer
        interval: 2200
        running: root.dir !== ""
        repeat: true
        onTriggered: {
            if (root.step >= 0 && root.step < root.steps.length && !root.withPerf && !root.withSeek && !root.withListen && !root.withAudioDevice) {   // screenshots would skew the timings
                const name = root.steps[root.step].name
                // An open dialog lives in the overlay, outside the shell — its content is grabbed instead.
                let source = GameOverlay.open && GameOverlay.view ? GameOverlay.view.parent : root.target
                const overlay = root.target.Overlay.overlay
                if (overlay)
                    for (let i = 0; i < overlay.children.length; ++i) {
                        const popup = overlay.children[i]
                        if (popup.visible && popup.width > 300 && popup.children.length > 0)
                            source = Array.from(popup.children).reduce((a, b) => b.children.length > a.children.length ? b : a)
                    }
                const ok = source.grabToImage(r => console.log("tour: screenshot", name, r.saveToFile(root.dir + "/" + name + ".png")))
                if (!ok) console.log("tour: screenshot rejected", name)
            }
            root.step += 1
            if (root.step < root.steps.length)
                root.steps[root.step].run()
            else if (root.step > root.steps.length)
                Qt.quit()   // one extra tick so the last screenshot has time to save
        }
    }
}
