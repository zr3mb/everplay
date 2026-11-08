import QtQuick
import QtQuick.Layouts
import Everplay

Item {
    id: page
    property var args: ({})
    property string query: ""
    property int filter: 0
    readonly property var filterKeys: ["", "local", "youtube", "soundcloud", "radio"]
    readonly property bool searching: query.trim().length > 0 || filter > 0
    // What is found, in the order it's shown: your library (files, radio), then YouTube Music and Spotify
    // (your own tracks there, then the YouTube Music catalog), then SoundCloud, then playlists.
    readonly property var results: {
        if (Library.tracks.revision < 0)   // never: the read makes the binding re-run (a bare read is compiled away)
            return []
        if (filter === 2 || filter === 3)
            return []
        if (filter === 0)
            return Library.search(query, "local").concat(Library.search(query, "radio"))
        return Library.search(query, filterKeys[filter])
    }
    // Online search in the YouTube Music catalog for "All" and "YouTube Music"
    readonly property bool onlineEnabled: query.trim().length > 1 && (filter === 0 || filter === 2)
    readonly property var online: {
        if (Library.tracks.revision < 0 || YouTube.resultsRevision < 0)
            return []
        if (filter !== 0 && filter !== 2)
            return []
        const own = Library.search(query, "youtube").concat(filter === 0 ? Library.search(query, "spotify") : [])
        const seen = new Set(own)
        return own.concat((onlineEnabled ? Library.onlineResults() : []).filter(r => !seen.has(r)))
    }
    property int onlineShown: 40
    readonly property bool withSpotify: filter === 0 && Spotify.connected

    // SoundCloud for "All" and "SoundCloud": your likes there, then its catalog (no account needed).
    readonly property bool soundcloudEnabled: query.trim().length > 1 && (filter === 0 || filter === 3)
    readonly property var soundcloud: {
        if (Library.tracks.revision < 0 || SoundCloud.resultsRevision < 0)
            return []
        if (filter !== 0 && filter !== 3)
            return []
        const own = Library.search(query, "soundcloud")
        const seen = new Set(own)
        return own.concat((soundcloudEnabled ? Library.soundCloudResults() : []).filter(r => !seen.has(r)))
    }
    property int soundcloudShown: 20

    // Your playlists whose name fits ({ p, index } in Library.playlists), then public ones on YouTube Music.
    readonly property var ownPlaylists: {
        const q = query.trim().toLowerCase()
        if (q.length === 0 || filter === 1 || filter === 4)
            return []
        const out = []
        Library.playlists.forEach((p, i) => {
            if ((p.name || "").toLowerCase().indexOf(q) >= 0)
                out.push({ p: p, index: i })
        })
        return out
    }
    function openOwn(index) { Nav.navigate("playlist", { kind: "playlist", index: index }) }
    function playOwn(index) {
        const rows = Library.playlistRows(index)
        if (rows.length === 0) {
            Nav.toast(qsTr("This playlist is still empty"), "music")
            return
        }
        Player.playFrom(rows, rows[0])
    }

    // Public playlists for the query (YouTube Music's own, then users'); they open like an artist's albums.
    readonly property var playlists: onlineEnabled ? YouTube.playlistResults : []
    readonly property var soundcloudPlaylists: soundcloudEnabled ? SoundCloud.playlistResults : []
    // Profiles: shown with "SoundCloud" picked, or for "All" when a profile's name is what was typed.
    readonly property var soundcloudUsers: {
        if (!soundcloudEnabled)
            return []
        if (filter === 3)
            return SoundCloud.userResults
        const q = query.trim().toLowerCase()
        return SoundCloud.userResults.filter(u => String(u.name).toLowerCase() === q).slice(0, 2)
    }
    function openSoundCloud(p) {
        Nav.navigate("playlist", { kind: "sccollection", playlistId: p.id, title: p.title, subtitle: p.subtitle,
                                   cover: p.cover })
    }
    function openPlaylist(p) {
        Nav.navigate("playlist", { kind: "ytcollection", playlistId: p.id, title: p.title, subtitle: p.subtitle,
                                   cover: p.cover, label: qsTr("PLAYLIST") })
    }
    CardShadow { id: playlistShadow; size: 168 }
    CardShadow { id: profileShadow; size: 132; radius: 66 }

    // Rows are built at once (Repeater), so long result lists show in parts.
    property int shown: 40
    onQueryChanged: { shown = 40; onlineShown = 40; soundcloudShown = 20; onlineTimer.restart(); rememberState() }
    onFilterChanged: { shown = 40; onlineShown = 40; soundcloudShown = 20; onlineTimer.restart(); rememberState() }
    function rememberState() {
        if (Nav.page === "search")
            Nav.remember({ query: query, filter: filter })
    }

    Timer {
        id: onlineTimer
        interval: 450
        onTriggered: {
            if (page.onlineEnabled)
                YouTube.search(page.query)
            if (page.soundcloudEnabled)
                SoundCloud.search(page.query)
        }
    }

    // Moods to search for when the library has (almost) no genre tags.
    readonly property var moods: [
        { name: "Ambient",     a: "#9fb5a8", b: "#36504a", seed: 0 },
        { name: "Lo-fi",       a: "#b7aac9", b: "#463c5c", seed: 1 },
        { name: qsTr("Night jazz"),  a: "#cdb68e", b: "#5a4a34", seed: 2 },
        { name: qsTr("Electronic"),  a: "#93a8c3", b: "#36445c", seed: 3 },
        { name: "Indie",       a: "#cda2a6", b: "#5c3a42", seed: 1 },
        { name: qsTr("Classical"),   a: "#c5bd95", b: "#545035", seed: 2 },
        { name: qsTr("Focus"),       a: "#8fb7b3", b: "#2e5150", seed: 0 },
        { name: qsTr("Rainy"),       a: "#a3b3c1", b: "#3b4a57", seed: 3 }
    ]
    // The genres in the user's library (tags, or the artist's genre for streaming tracks), the most tracks first.
    readonly property var genres: {
        if (Library.tracks.revision < 0 || Library.genresRevision < 0)
            return []
        return Library.genres()
    }
    property bool allGenres: false
    readonly property int genreLimit: 12
    readonly property var tiles: {
        const own = genres.slice(0, allGenres ? genres.length : genreLimit).map(g => ({
            name: g.name, key: g.key, count: g.count, a: g.colorA, b: g.colorB, seed: g.seed, cover: g.cover
        }))
        if (own.length >= 4)
            return own
        // A few genres only: moods to search for fill the rest of the grid.
        const names = own.map(t => t.name.toLowerCase())
        return own.concat(moods.filter(m => names.indexOf(m.name.toLowerCase()) < 0).slice(0, 8 - own.length))
    }

    Connections {
        target: Nav
        function onSearchFocusRequested() { field.forceActiveFocus() }
    }
    Component.onCompleted: {
        if (args && args.filter !== undefined)
            filter = args.filter
        if (args && args.query)
            field.text = args.query
        field.forceActiveFocus()
    }

    // A genre from the library (opens its tracks) or a mood (searches for it).
    component MoodTile: Item {
        id: tile
        required property var modelData
        required property int index
        readonly property bool isGenre: modelData.key !== undefined
        signal searchRequested(string text)
        implicitHeight: 124
        opacity: 0
        scale: 0.94

        SequentialAnimation {
            id: tileIn
            PauseAnimation { duration: 60 + tile.index * 45 }
            ParallelAnimation {
                NumberAnimation { target: tile; property: "opacity"; to: 1; duration: Theme.ms(420) }
                NumberAnimation { target: tile; property: "scale"; to: 1; duration: Theme.ms(560); easing.type: Easing.OutBack }
            }
        }
        Component.onCompleted: tileIn.start()

        HoverHandler { id: tileHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            onTapped: {
                if (tile.isGenre)
                    Nav.navigate("playlist", { kind: "genre", key: tile.modelData.key, name: tile.modelData.name })
                else
                    tile.searchRequested(tile.modelData.name)
            }
        }

        Item {
            anchors.fill: parent
            scale: tileHover.hovered ? 1.03 : 1
            Behavior on scale { NumberAnimation { duration: Theme.ms(380); easing.type: Easing.OutCubic } }

            ArtCover {
                anchors.fill: parent
                radius: Theme.r(18)
                colorA: tile.modelData.a
                colorB: tile.modelData.b
                seed: tile.modelData.seed
                decorated: !cornerCover.visible
            }
            // The cover of the genre's biggest album, tilted in the corner
            ArtCover {
                id: cornerCover
                visible: (tile.modelData.cover || "") !== ""
                anchors { right: parent.right; bottom: parent.bottom; rightMargin: 14; bottomMargin: 14 }
                width: 66; height: 66
                radius: Theme.r(8)
                colorA: tile.modelData.a
                colorB: tile.modelData.b
                seed: tile.modelData.seed
                source: tile.modelData.cover || ""
                rotation: tileHover.hovered ? 6 : 14
                Behavior on rotation { NumberAnimation { duration: Theme.ms(420); easing.type: Easing.OutBack } }
            }
            Column {
                anchors { left: parent.left; right: cornerCover.visible ? cornerCover.left : parent.right; bottom: parent.bottom
                          leftMargin: 18; rightMargin: 12; bottomMargin: 16 }
                spacing: 2
                Text {
                    width: parent.width
                    text: tile.modelData.name
                    color: "#f6f2ec"
                    font.family: Theme.fontDisplay
                    font.pixelSize: 21
                    font.weight: Font.Bold
                    fontSizeMode: Text.HorizontalFit   // long names shrink a little before they're cut
                    minimumPixelSize: 15
                    elide: Text.ElideRight
                }
                Text {
                    visible: tile.isGenre
                    text: qsTr("%n tracks", "", tile.modelData.count || 0)
                    color: Qt.rgba(0.965, 0.95, 0.925, 0.72)
                    font.pixelSize: 13
                }
            }
        }
    }

    // The playlists found, in a column beside the tracks (below them in a narrow window): yours whose
    // name fits, then public ones on YouTube Music.
    readonly property var foundPlaylists: {
        const own = ownPlaylists.map(o => ({ own: true, index: o.index, playlist: o.p, subtitle: qsTr("Your playlist") }))
        const pub = playlists.map((p, i) => ({ own: false, data: p, subtitle: p.subtitle || "",
                                               playlist: { name: p.title, cover: p.cover || "", seed: i, icon: "library" } }))
        const sc = soundcloudPlaylists.map((p, i) => ({ own: false, sc: true, data: p, subtitle: "SoundCloud · " + p.subtitle + " · " + qsTr("%n tracks", "", p.count),
                                                       playlist: { name: p.title, cover: p.cover || "", seed: i + 2, icon: "wavecloud" } }))
        return own.concat(pub, sc)
    }
    // Only where the tracks keep a comfortable width next to it (the field and filters need ~500 px).
    readonly property bool sideShown: page.width > 1000 && page.searching && filter !== 1 && filter !== 4
    readonly property bool lookingUp: (onlineEnabled && YouTube.searching) || (soundcloudEnabled && SoundCloud.searching)

    Rectangle {
        id: sidePanel
        visible: page.sideShown
        anchors { top: parent.top; bottom: parent.bottom; right: parent.right; topMargin: 34; bottomMargin: 34; rightMargin: 24 }
        width: 250
        radius: Theme.r(16)
        color: Theme.alpha(Theme.text, 0.035)
        border.width: 1
        border.color: Theme.alpha(Theme.text, 0.05)

        RowLayout {
            id: sideTitle
            anchors { left: parent.left; right: parent.right; top: parent.top; leftMargin: 16; rightMargin: 14; topMargin: 16 }
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: qsTr("Playlists")
                color: Theme.text
                font.family: Theme.fontDisplay
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
            Spinner { visible: page.lookingUp; size: 16; color: Theme.textDim }
        }
        Text {
            visible: page.foundPlaylists.length === 0 && !page.lookingUp
            anchors { left: parent.left; right: parent.right; top: sideTitle.bottom; leftMargin: 16; rightMargin: 16; topMargin: 10 }
            text: page.query.trim().length > 1 ? qsTr("No playlists for this search") : qsTr("Type at least two letters")
            color: Theme.textFaint
            font.pixelSize: 13
            wrapMode: Text.WordWrap
        }
        ListView {
            anchors { left: parent.left; right: parent.right; top: sideTitle.bottom; bottom: parent.bottom
                      leftMargin: 8; rightMargin: 8; topMargin: 10; bottomMargin: 8 }
            clip: true
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            model: page.foundPlaylists.length   // a count: rows stay while typing (see the track lists)
            delegate: PlaylistRow {
                required property int index
                readonly property var entry: page.foundPlaylists[index] ?? ({})
                width: ListView.view.width
                playlist: entry.playlist || ({})
                subtitle: entry.subtitle || ""
                onClicked: entry.own ? page.openOwn(entry.index)
                           : entry.sc ? page.openSoundCloud(entry.data) : page.openPlaylist(entry.data)
                onPlayClicked: entry.own ? page.playOwn(entry.index)
                               : entry.sc ? page.openSoundCloud(entry.data) : page.openPlaylist(entry.data)
            }
        }
    }

    PageFlick {
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom
                  right: sidePanel.visible ? sidePanel.left : parent.right }

        Reveal {
            Text {
                text: qsTr("Search")
                color: Theme.text
                font.family: Theme.fontDisplay
                font.pixelSize: 38
                font.weight: Font.Bold
            }
        }

        Reveal {
            delay: 60
            ColumnLayout {
                width: parent.width
                spacing: 18

                InputField {
                    id: field
                    Layout.fillWidth: true
                    implicitHeight: 56
                    icon: "search"
                    font.pixelSize: 16
                    placeholderText: qsTr("What do you want to listen to today?")
                    onTextChanged: page.query = text
                    Keys.onEscapePressed: text = ""
                }

                SegmentedTabs {
                    model: [qsTr("All"), qsTr("Local"), "YouTube Music", "SoundCloud", qsTr("Radio")]
                    currentIndex: page.filter
                    onActivated: i => page.filter = i
                }
            }
        }

        Loader {
            id: body
            Layout.fillWidth: true
            Layout.preferredHeight: item ? item.implicitHeight : 0
            sourceComponent: page.searching ? resultsView : browseView
            onLoaded: fade.restart()
            NumberAnimation { id: fade; target: body; property: "opacity"; from: 0; to: 1; duration: Theme.ms(320); easing.type: Easing.OutCubic }
        }
    }

    Component {
        id: resultsView

        ColumnLayout {
            spacing: 6

            Text {
                Layout.bottomMargin: 8
                visible: page.results.length > 0
                text: qsTr("In your library: %n results", "", page.results.length)
                      + (page.query.trim() ? " " + qsTr("for “%1”").arg(page.query.trim()) : "")
                color: Theme.textDim
                font.pixelSize: 14
            }

            TrackListHeader { visible: page.results.length > 0 }

            Repeater {
                // A count, not the array: typing changes the list on every key, and an array model would
                // rebuild all its rows each time — with a count the rows stay and only show other tracks.
                model: Math.min(page.results.length, page.shown)
                TrackRow {
                    required property int index
                    Layout.fillWidth: true
                    trackIndex: page.results[index] ?? -1
                    number: index + 1
                    queue: page.results
                    revealDelay: Math.min(index, 12) * 32
                }
            }

            PillButton {
                visible: page.results.length > page.shown
                Layout.topMargin: 6
                kind: "ghost"
                icon: "expandMore"
                text: qsTr("Show all (%1)").arg(page.results.length)
                onClicked: page.shown = page.results.length
            }

            RowLayout {
                visible: page.online.length > 0 || (page.onlineEnabled && YouTube.searching)
                Layout.fillWidth: true
                Layout.topMargin: page.results.length > 0 ? 26 : 0
                Layout.bottomMargin: 8
                spacing: 10
                Icon { name: "playCircle"; size: 20; color: "#cf9d9d" }
                Text {
                    Layout.fillWidth: true
                    text: page.withSpotify ? qsTr("On YouTube Music and Spotify") : qsTr("On YouTube Music")
                    color: Theme.text
                    font.family: Theme.fontDisplay
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Spinner { visible: YouTube.searching; size: 18; color: Theme.textDim }
            }

            Repeater {
                model: Math.min(page.online.length, page.onlineShown)
                TrackRow {
                    required property int index
                    Layout.fillWidth: true
                    trackIndex: page.online[index] ?? -1
                    number: index + 1
                    queue: page.online
                    revealDelay: Math.min(index, 12) * 32
                }
            }

            PillButton {
                visible: page.online.length > page.onlineShown
                Layout.topMargin: 6
                kind: "ghost"
                icon: "expandMore"
                text: qsTr("Show all (%1)").arg(page.online.length)
                onClicked: page.onlineShown = page.online.length
            }

            RowLayout {
                visible: page.soundcloud.length > 0 || page.soundcloudUsers.length > 0
                         || (page.soundcloudEnabled && SoundCloud.searching)
                Layout.fillWidth: true
                Layout.topMargin: page.results.length > 0 || page.online.length > 0 ? 26 : 0
                Layout.bottomMargin: 8
                spacing: 10
                Icon { name: "wavecloud"; size: 20; color: "#d9a27e" }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("On SoundCloud")
                    color: Theme.text
                    font.family: Theme.fontDisplay
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Spinner { visible: SoundCloud.searching; size: 18; color: Theme.textDim }
            }

            // Profiles (artists and other users) whose name fits, as round cards above the tracks.
            Flow {
                visible: page.soundcloudUsers.length > 0
                Layout.fillWidth: true
                Layout.bottomMargin: 14
                spacing: 16
                Repeater {
                    model: page.soundcloudUsers.slice(0, 6)
                    AlbumCard {
                        required property var modelData
                        required property int index
                        width: 132
                        shadow: profileShadow
                        round: true
                        title: modelData.name
                        subtitle: modelData.subtitle || qsTr("Profile")
                        cover: modelData.photo
                        colorA: "#d9a27e"
                        colorB: "#5a3a28"
                        seed: index
                        revealDelay: index * 30
                        onClicked: Nav.openSoundCloudProfile(modelData.id, modelData.name, modelData.photo)
                        onPlayClicked: Nav.openSoundCloudProfile(modelData.id, modelData.name, modelData.photo)
                    }
                }
            }

            Repeater {
                model: Math.min(page.soundcloud.length, page.soundcloudShown)
                TrackRow {
                    required property int index
                    Layout.fillWidth: true
                    trackIndex: page.soundcloud[index] ?? -1
                    number: index + 1
                    queue: page.soundcloud
                    revealDelay: Math.min(index, 12) * 32
                }
            }

            PillButton {
                visible: page.soundcloud.length > page.soundcloudShown
                Layout.topMargin: 6
                kind: "ghost"
                icon: "expandMore"
                text: qsTr("Show all (%1)").arg(page.soundcloud.length)
                onClicked: page.soundcloudShown = page.soundcloud.length
            }

            // Playlists: yours whose name fits, then public ones on YouTube Music and SoundCloud
            RowLayout {
                visible: !page.sideShown && page.foundPlaylists.length > 0
                Layout.fillWidth: true
                Layout.topMargin: page.results.length > 0 || page.online.length > 0 || page.soundcloud.length > 0 ? 26 : 0
                Layout.bottomMargin: 8
                spacing: 10
                Icon { name: "library"; size: 20; color: "#cf9d9d" }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Playlists")
                    color: Theme.text
                    font.family: Theme.fontDisplay
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
            }
            Flow {
                visible: !page.sideShown && page.foundPlaylists.length > 0
                Layout.fillWidth: true
                spacing: 18
                Repeater {
                    model: page.sideShown ? 0 : page.ownPlaylists.length
                    AlbumCard {
                        readonly property var entry: page.ownPlaylists[index] ?? ({ p: {} })
                        required property int index
                        width: 168
                        shadow: playlistShadow
                        title: entry.p.name || ""
                        subtitle: qsTr("Your playlist")
                        colorA: entry.p.colorA || Theme.surface
                        colorB: entry.p.colorB || Theme.surface
                        seed: entry.p.seed || 0
                        cover: entry.p.cover || ""
                        mosaic: entry.p.mosaic || []
                        revealDelay: index * 30
                        onClicked: page.openOwn(entry.index)
                        onPlayClicked: page.playOwn(entry.index)
                    }
                }
                Repeater {
                    model: page.sideShown ? 0 : page.playlists.length
                    AlbumCard {
                        readonly property var entry: page.playlists[index] ?? ({})
                        required property int index
                        width: 168
                        shadow: playlistShadow
                        title: entry.title || ""
                        subtitle: entry.subtitle || ""
                        cover: entry.cover || ""
                        seed: index
                        revealDelay: (page.ownPlaylists.length + index) * 30
                        onClicked: page.openPlaylist(entry)
                        onPlayClicked: page.openPlaylist(entry)
                    }
                }
                Repeater {
                    model: page.sideShown ? 0 : page.soundcloudPlaylists.length
                    AlbumCard {
                        readonly property var entry: page.soundcloudPlaylists[index] ?? ({})
                        required property int index
                        width: 168
                        shadow: playlistShadow
                        title: entry.title || ""
                        subtitle: "SoundCloud · " + (entry.subtitle || "") + " · " + qsTr("%n tracks", "", entry.count || 0)
                        cover: entry.cover || ""
                        seed: index + 2
                        revealDelay: (page.ownPlaylists.length + page.playlists.length + index) * 30
                        onClicked: page.openSoundCloud(entry)
                        onPlayClicked: page.openSoundCloud(entry)
                    }
                }
            }

            ColumnLayout {
                visible: page.results.length === 0 && page.online.length === 0 && page.soundcloud.length === 0
                         && page.foundPlaylists.length === 0 && !page.lookingUp
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 50
                spacing: 10

                Icon {
                    Layout.alignment: Qt.AlignHCenter
                    name: "search"
                    size: 52
                    color: Theme.textFaint
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("No results for “%1”").arg(page.query.trim())
                    color: Theme.text
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Try a different phrase or connect another music source.")
                    color: Theme.textDim
                    font.pixelSize: 14
                }
            }
        }
    }

    Component {
        id: browseView

        ColumnLayout {
            spacing: 18

            SectionHeader {
                Layout.fillWidth: true
                title: page.genres.length > 0 ? qsTr("Your genres") : qsTr("Browse moods")
                subtitle: page.genres.length > 0 ? qsTr("What your library is made of — or just start typing")
                                                 : qsTr("Or just start typing")
            }

            Grid {
                id: moodGrid
                Layout.fillWidth: true
                Layout.preferredWidth: 100
                columns: Math.max(2, Math.floor((width + columnSpacing) / 230))
                columnSpacing: 16
                rowSpacing: 16
                readonly property real cellW: (width - columnSpacing * (columns - 1)) / columns

                Repeater {
                    model: page.tiles
                    MoodTile {
                        width: moodGrid.cellW
                        onSearchRequested: text => field.text = text
                    }
                }
            }

            PillButton {
                visible: page.genres.length > page.genreLimit
                kind: "ghost"
                icon: page.allGenres ? "" : "expandMore"
                text: page.allGenres ? qsTr("Show less") : qsTr("Show all (%1)").arg(page.genres.length)
                fontSize: 13
                onClicked: page.allGenres = !page.allGenres
            }
        }
    }
}
