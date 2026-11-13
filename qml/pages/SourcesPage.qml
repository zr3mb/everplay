import QtQuick
import QtQuick.Layouts
import QtQuick.Dialogs
import Everplay

// Managing music sources. Local files and YouTube Music really work;
// radio and custom streams are only an interface for now.
Item {
    id: page
    property var args: ({})

    function src(id) { return Library.sources.find(s => s.id === id) || {} }



    component ListLine: Item {
        id: line
        property string icon: "folder"
        property color iconColor: Theme.textDim
        property string title: ""
        property string detail: ""
        property bool live: false
        signal activated
        Layout.fillWidth: true
        implicitHeight: 46

        HoverHandler { id: lineHover }
        Rectangle {
            anchors.fill: parent
            radius: Theme.r(12)
            color: Theme.alpha(Theme.text, lineHover.hovered ? 0.06 : 0.03)
            Behavior on color { ColorAnimation { duration: Theme.fast } }
        }
        Icon {
            id: lineIcon
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            name: line.icon
            size: 18
            color: line.iconColor
        }
        Rectangle {
            visible: line.live
            anchors { left: lineIcon.right; leftMargin: -4; top: lineIcon.top; topMargin: -2 }
            width: 7; height: 7; radius: 3.5
            color: Theme.danger
            SequentialAnimation on opacity {
                running: line.live
                loops: Animation.Infinite
                NumberAnimation { to: 0.25; duration: 700; easing.type: Easing.InOutSine }
                NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutSine }
            }
        }
        Text {
            anchors { left: lineIcon.right; leftMargin: 12; right: lineDetail.left; rightMargin: 10; verticalCenter: parent.verticalCenter }
            text: line.title
            color: Theme.text
            font.pixelSize: 14
            font.weight: Font.Medium
            elide: Text.ElideMiddle
        }
        Text {
            id: lineDetail
            anchors { right: lineAction.left; rightMargin: 6; verticalCenter: parent.verticalCenter }
            text: line.detail
            color: Theme.textFaint
            font.pixelSize: 12
        }
        IconButton {
            id: lineAction
            anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
            width: 32; height: 32
            icon: line.live ? "play" : "close"
            iconSize: 16
            showDot: false
            opacity: lineHover.hovered ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            onClicked: line.activated()
        }
    }

    component Hint: Text {
        Layout.fillWidth: true
        color: Theme.textDim
        font.pixelSize: 13
        wrapMode: Text.WordWrap
        lineHeight: 1.2
    }

    PageFlick {
        anchors.fill: parent

        Reveal {
            ColumnLayout {
                width: parent.width
                spacing: 4
                Text {
                    text: qsTr("Music sources")
                    color: Theme.text
                    font.family: Theme.fontDisplay
                    font.pixelSize: 38
                    font.weight: Font.Bold
                }
                Text {
                    text: qsTr("Connect the places Everplay should play music from. Everything ends up in one library.")
                    color: Theme.textDim
                    font.pixelSize: 15
                }
            }
        }

        Reveal {
            delay: 80
            // Two columns of cards in a wide window, one in a narrow one
            GridLayout {
                width: parent.width
                columns: width > 900 ? 2 : 1
                columnSpacing: 18
                rowSpacing: 18

                // ---- left column --------------------------------------------
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    Layout.alignment: Qt.AlignTop
                    spacing: 18

                    SourceCard {
                        source: page.src("local")
                        status: Library.scanning ? "connecting" : Library.folders.length > 0 ? "connected" : "disconnected"
                        detail: Library.folders.length === 0 ? ""
                                : qsTr("%n folders", "", Library.folders.length) + " · "
                                  + qsTr("%n tracks", "", Library.localCount)

                        Hint {
                            visible: Library.folders.length === 0
                                text: qsTr("No folder chosen yet. Add a folder and Everplay will scan it together with its subfolders.")
                        }

                        Repeater {
                            model: Library.folders
                            ListLine {
                                required property var modelData
                                icon: "folder"
                                iconColor: "#9fbfa8"
                                title: modelData.path
                                detail: qsTr("%n tracks", "", modelData.count)
                                onActivated: Library.removeFolder(modelData.path)
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            implicitHeight: Library.scanning ? 38 : 0
                            opacity: Library.scanning ? 1 : 0
                            clip: true
                            Behavior on implicitHeight { NumberAnimation { duration: 380; easing.type: Easing.OutQuint } }
                            Behavior on opacity { NumberAnimation { duration: 300 } }

                            Text {
                                text: Library.scanStatus
                                color: Theme.textDim
                                font.pixelSize: 12
                                font.features: { "tnum": 1 }
                            }
                            Rectangle {
                                id: scanTrack
                                anchors { left: parent.left; right: parent.right; bottom: parent.bottom; bottomMargin: 6 }
                                height: 6
                                radius: 3
                                color: Theme.alpha(Theme.text, 0.08)
                                clip: true

                                Rectangle {
                                    width: parent.width * Library.scanProgress
                                    height: parent.height
                                    radius: Theme.r(3)
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0; color: Qt.darker("#9fbfa8", 1.3) }
                                        GradientStop { position: 1; color: "#9fbfa8" }
                                    }
                                }
                                Rectangle {
                                    id: shimmer
                                    width: 90
                                    height: parent.height
                                    gradient: Gradient {
                                        orientation: Gradient.Horizontal
                                        GradientStop { position: 0; color: "#00ffffff" }
                                        GradientStop { position: 0.5; color: "#55ffffff" }
                                        GradientStop { position: 1; color: "#00ffffff" }
                                    }
                                    NumberAnimation on x {
                                        running: Library.scanning
                                        from: -90; to: scanTrack.width
                                        duration: 1100
                                        loops: Animation.Infinite
                                    }
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 10
                            PillButton {
                                kind: "secondary"
                                icon: "add"
                                text: qsTr("Add folder")
                                onClicked: Nav.addFolder()
                            }
                            PillButton {
                                kind: "ghost"
                                icon: "refresh"
                                text: Library.scanning ? qsTr("Scanning…") : qsTr("Scan again")
                                busy: Library.scanning
                                enabled: Library.folders.length > 0
                                onClicked: Library.rescan()
                            }
                        }
                    }

                    SourceCard {
                        source: page.src("youtube")
                        status: YouTube.loggingIn ? "connecting" : YouTube.connected ? "connected" : "disconnected"
                        detail: !YouTube.connected ? ""
                                : YouTube.syncing ? qsTr("Syncing with your account…")
                                : qsTr("%n liked tracks", "", YouTube.likedCount)
                                  + " · " + qsTr("%n playlists", "", YouTube.playlistCount)
                                  + (YouTube.lastSync !== "" ? " · " + YouTube.lastSync : "")

                        // Signed out
                        ColumnLayout {
                            visible: !YouTube.connected && !YouTube.loggingIn
                            Layout.fillWidth: true
                            spacing: 12

                            Hint {
                                text: YouTube.browserName !== ""
                                      ? qsTr("We'll open a separate %1 window with a clean profile. Sign in to your Google account there as usual. When YouTube Music appears, the window closes by itself and Everplay downloads your liked tracks and playlists.").arg(YouTube.browserName)
                                      : qsTr("Chrome, Edge or Brave was not found. You can paste the cookies manually.")
                            }
                            Flow {
                                Layout.fillWidth: true
                                spacing: 10
                                PillButton {
                                    icon: "person"
                                    text: qsTr("Sign in with the browser")
                                    enabled: YouTube.browserName !== ""
                                    onClicked: YouTube.login()
                                }
                                PillButton {
                                    kind: "ghost"
                                    icon: "key"
                                    text: manualLogin.visible ? qsTr("Hide") : qsTr("Paste cookies")
                                    onClicked: manualLogin.visible = !manualLogin.visible
                                }
                            }
                            ColumnLayout {
                                id: manualLogin
                                visible: false
                                Layout.fillWidth: true
                                spacing: 10
                                Hint {
                                    text: qsTr("Copy the Cookie header from a request to music.youtube.com (developer tools → Network) and paste it here.")
                                }
                                InputField {
                                    id: cookieField
                                    Layout.fillWidth: true
                                    icon: "key"
                                    echoMode: TextInput.Password
                                    placeholderText: "SAPISID=…; LOGIN_INFO=…; …"
                                }
                                PillButton {
                                    kind: "secondary"
                                    icon: "link"
                                    text: qsTr("Connect")
                                    enabled: cookieField.text.length > 0
                                    onClicked: {
                                        YouTube.loginWithCookies(cookieField.text)
                                        cookieField.text = ""
                                    }
                                }
                            }
                        }

                        // Signing in through the browser
                        RowLayout {
                            visible: YouTube.loggingIn
                            Layout.fillWidth: true
                            spacing: 14
                            Spinner { size: 22; color: Theme.warning }
                            Hint {
                                text: YouTube.loginStatus
                            }
                            PillButton {
                                kind: "ghost"
                                text: qsTr("Cancel")
                                onClicked: YouTube.cancelLogin()
                            }
                        }

                        // Signed in
                        RowLayout {
                            visible: YouTube.connected
                            Layout.fillWidth: true
                            spacing: 12

                            ArtCover {
                                Layout.preferredWidth: 44
                                Layout.preferredHeight: 44
                                radius: Theme.r(22)
                                decorated: false
                                colorA: "#cf9d9d"
                                colorB: "#5c3a42"
                                source: YouTube.accountPhoto
                                Icon {
                                    anchors.centerIn: parent
                                    visible: !parent.hasPicture
                                    name: "person"
                                    size: 22
                                    color: "#f4efe8"
                                }
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: YouTube.accountName || qsTr("YouTube Music account")
                                    color: Theme.text
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: text !== ""
                                    text: YouTube.accountHandle
                                    color: Theme.textDim
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                            }
                            Spinner {
                                visible: YouTube.syncing
                                size: 18
                                color: Theme.textDim
                            }
                            IconButton {
                                visible: !YouTube.syncing
                                icon: "refresh"
                                iconSize: 18
                                showDot: false
                                onClicked: YouTube.refresh()
                            }
                            PillButton {
                                kind: "ghost"
                                text: qsTr("Sign out")
                                onClicked: YouTube.logout()
                            }
                        }

                        // Several Google accounts in the browser, or brand channels: which one is used
                        ColumnLayout {
                            visible: YouTube.connected && YouTube.identities.length > 1
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                Layout.bottomMargin: 4
                                text: qsTr("Channel")
                                color: Theme.textDim
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                            }
                            Repeater {
                                model: YouTube.identities
                                Rectangle {
                                    id: identity
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    implicitHeight: 44
                                    radius: Theme.r(10)
                                    color: identityMouse.containsMouse ? Theme.alpha(Theme.text, 0.05) : "transparent"
                                    RowLayout {
                                        anchors { fill: parent; leftMargin: 8; rightMargin: 10 }
                                        spacing: 10
                                        ArtCover {
                                            Layout.preferredWidth: 28
                                            Layout.preferredHeight: 28
                                            radius: Theme.r(14)
                                            decorated: false
                                            colorA: "#cf9d9d"
                                            colorB: "#5c3a42"
                                            source: identity.modelData.photo
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Text {
                                                Layout.fillWidth: true
                                                text: identity.modelData.name
                                                color: Theme.text
                                                font.pixelSize: 13
                                                elide: Text.ElideRight
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: [identity.modelData.handle, identity.modelData.email].filter(x => x).join(" · ")
                                                color: Theme.textDim
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }
                                        }
                                        Icon {
                                            visible: identity.modelData.selected
                                            name: "check"
                                            size: 18
                                            color: Theme.accent
                                        }
                                    }
                                    MouseArea {
                                        id: identityMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: YouTube.selectIdentity(identity.index)
                                    }
                                }
                            }
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.topMargin: 4
                            height: 1
                            color: Qt.rgba(1, 1, 1, 0.06)
                        }

                        // Playback engine
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            Icon {
                                name: "playCircle"
                                size: 22
                                color: YouTube.engineReady ? Theme.success : Theme.textDim
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Playback engine (yt-dlp)")
                                    color: Theme.text
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: YouTube.engineDownloading ? qsTr("Downloading… %1%").arg(Math.round(YouTube.engineProgress * 100))
                                        : YouTube.engineReady ? qsTr("Ready") + (YouTube.engineVersion ? " · " + qsTr("version %1").arg(YouTube.engineVersion) : "")
                                        : qsTr("Needed to play tracks from YouTube — we'll download the official release from GitHub.")
                                    color: Theme.textDim
                                    font.pixelSize: 12
                                    wrapMode: Text.WordWrap
                                }
                            }
                            PillButton {
                                kind: YouTube.engineReady ? "ghost" : "primary"
                                icon: YouTube.engineReady ? "refresh" : "add"
                                text: YouTube.engineReady ? qsTr("Update") : qsTr("Download")
                                busy: YouTube.engineDownloading
                                onClicked: YouTube.downloadEngine()
                            }
                        }
                    }

                    SourceCard {
                        source: page.src("spotify")
                        status: Spotify.loggingIn ? "connecting" : Spotify.connected ? "connected" : "disconnected"
                        detail: !Spotify.connected ? ""
                                : Spotify.syncing ? qsTr("Syncing with your account…")
                                : qsTr("%n liked tracks", "", Spotify.likedCount)
                                  + " · " + qsTr("%n playlists", "", Spotify.playlistCount)
                                  + (Spotify.lastSync !== "" ? " · " + Spotify.lastSync : "")

                        // Signed out: sign-in through the browser, or an own Spotify app (Client ID)
                        ColumnLayout {
                            id: spotifySignIn
                            property bool ownApp: false
                            visible: !Spotify.connected && !Spotify.loggingIn
                            Layout.fillWidth: true
                            spacing: 12

                            Hint {
                                text: qsTr("Spotify doesn't let other apps play its music, so Everplay syncs your playlists and liked songs and plays each track from YouTube Music.")
                            }
                            Hint {
                                visible: !spotifySignIn.ownApp
                                text: YouTube.browserName !== ""
                                      ? qsTr("We'll open a separate %1 window with a clean profile. Sign in to Spotify there as usual — a free account is enough. When the web player appears, the window closes by itself and Everplay downloads your playlists and liked songs.").arg(YouTube.browserName)
                                      : qsTr("Signing in needs Google Chrome, Microsoft Edge or Brave.")
                            }
                            Flow {
                                visible: !spotifySignIn.ownApp
                                Layout.fillWidth: true
                                spacing: 10
                                PillButton {
                                    icon: "person"
                                    text: qsTr("Sign in with the browser")
                                    enabled: YouTube.browserName !== ""
                                    onClicked: Spotify.loginWithBrowser()
                                }
                                PillButton {
                                    kind: "ghost"
                                    icon: "key"
                                    text: qsTr("Use my own Spotify app")
                                    onClicked: spotifySignIn.ownApp = true
                                }
                            }

                            Hint {
                                visible: spotifySignIn.ownApp && !Spotify.builtInClientId
                                text: qsTr("With your own Spotify app (developer mode needs Spotify Premium): open the Spotify dashboard, create an app with the redirect URI %1 and the Web API checked, then paste its Client ID below.").arg(Spotify.redirectUri)
                            }
                            InputField {
                                id: spotifyClient
                                visible: spotifySignIn.ownApp
                                Layout.fillWidth: true
                                icon: "key"
                                placeholderText: qsTr("Spotify app Client ID")
                                text: Spotify.clientId
                                onEditingFinished: Spotify.clientId = text
                            }
                            Flow {
                                visible: spotifySignIn.ownApp
                                Layout.fillWidth: true
                                spacing: 10
                                PillButton {
                                    icon: "person"
                                    text: qsTr("Sign in to Spotify")
                                    enabled: spotifyClient.text.trim() !== ""
                                    onClicked: {
                                        Spotify.clientId = spotifyClient.text
                                        Spotify.login()
                                    }
                                }
                                PillButton {
                                    kind: "ghost"
                                    icon: "link"
                                    text: qsTr("Open the Spotify dashboard")
                                    onClicked: Qt.openUrlExternally("https://developer.spotify.com/dashboard")
                                }
                                PillButton {
                                    kind: "ghost"
                                    text: qsTr("Back")
                                    onClicked: spotifySignIn.ownApp = false
                                }
                            }
                        }

                        // Signing in
                        RowLayout {
                            visible: Spotify.loggingIn
                            Layout.fillWidth: true
                            spacing: 14
                            Spinner { size: 22; color: Theme.warning }
                            Hint {
                                text: Spotify.loginStatus !== "" ? Spotify.loginStatus
                                      : qsTr("Sign in in the browser and allow access — Everplay continues by itself.")
                            }
                            PillButton {
                                kind: "ghost"
                                text: qsTr("Cancel")
                                onClicked: Spotify.cancelLogin()
                            }
                        }

                        // Signed in
                        RowLayout {
                            visible: Spotify.connected
                            Layout.fillWidth: true
                            spacing: 12

                            ArtCover {
                                Layout.preferredWidth: 44
                                Layout.preferredHeight: 44
                                radius: Theme.r(22)
                                decorated: false
                                colorA: "#8fc79a"
                                colorB: "#2f4a36"
                                Icon {
                                    anchors.centerIn: parent
                                    name: "person"
                                    size: 22
                                    color: "#f4efe8"
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: Spotify.accountName || qsTr("Spotify account")
                                color: Theme.text
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Spinner {
                                visible: Spotify.syncing
                                size: 18
                                color: Theme.textDim
                            }
                            IconButton {
                                visible: !Spotify.syncing
                                icon: "refresh"
                                iconSize: 18
                                showDot: false
                                onClicked: Spotify.refresh()
                            }
                            PillButton {
                                kind: "ghost"
                                text: qsTr("Sign out")
                                onClicked: Spotify.logout()
                            }
                        }
                    }

                    SourceCard {
                        source: page.src("soundcloud")
                        status: SoundCloud.loggingIn ? "connecting" : SoundCloud.connected ? "connected" : "available"
                        detail: !SoundCloud.connected ? qsTr("Search works without an account")
                                : SoundCloud.syncing ? qsTr("Syncing with your account…")
                                : qsTr("%n liked tracks", "", SoundCloud.likedCount)
                                  + " · " + qsTr("%n playlists", "", SoundCloud.playlistCount)
                                  + (SoundCloud.lastSync !== "" ? " · " + SoundCloud.lastSync : "")

                        // Signed out: search and play right away; signing in adds the likes and playlists
                        ColumnLayout {
                            id: soundcloudSignIn
                            property bool manual: false
                            visible: !SoundCloud.connected && !SoundCloud.loggingIn
                            Layout.fillWidth: true
                            spacing: 12

                            Hint {
                                text: qsTr("SoundCloud tracks show up in Search and play straight from SoundCloud. Sign in to bring your liked tracks and playlists into the library.")
                            }
                            Hint {
                                visible: !soundcloudSignIn.manual
                                text: YouTube.browserName !== ""
                                      ? qsTr("We'll open a separate %1 window with a clean profile. Sign in to SoundCloud there as usual; once your feed appears, the window closes by itself.").arg(YouTube.browserName)
                                      : qsTr("Signing in needs Google Chrome, Microsoft Edge or Brave.")
                            }
                            Flow {
                                visible: !soundcloudSignIn.manual
                                Layout.fillWidth: true
                                spacing: 10
                                PillButton {
                                    icon: "person"
                                    text: qsTr("Sign in with the browser")
                                    enabled: YouTube.browserName !== ""
                                    onClicked: SoundCloud.login()
                                }
                                PillButton {
                                    kind: "ghost"
                                    icon: "key"
                                    text: qsTr("Paste a token")
                                    onClicked: soundcloudSignIn.manual = true
                                }
                            }
                            Hint {
                                visible: soundcloudSignIn.manual
                                text: qsTr("In a browser signed in to soundcloud.com, open the developer tools (F12) → Application → Cookies and copy the value of oauth_token.")
                            }
                            InputField {
                                id: soundcloudToken
                                visible: soundcloudSignIn.manual
                                Layout.fillWidth: true
                                icon: "key"
                                placeholderText: "oauth_token"
                                echoMode: TextInput.Password
                            }
                            Flow {
                                visible: soundcloudSignIn.manual
                                Layout.fillWidth: true
                                spacing: 10
                                PillButton {
                                    icon: "check"
                                    text: qsTr("Sign in")
                                    enabled: soundcloudToken.text.trim() !== ""
                                    onClicked: {
                                        SoundCloud.loginWithToken(soundcloudToken.text)
                                        soundcloudToken.text = ""
                                        soundcloudSignIn.manual = false
                                    }
                                }
                                PillButton {
                                    kind: "ghost"
                                    text: qsTr("Back")
                                    onClicked: soundcloudSignIn.manual = false
                                }
                            }
                        }

                        // Signing in
                        RowLayout {
                            visible: SoundCloud.loggingIn
                            Layout.fillWidth: true
                            spacing: 14
                            Spinner { size: 22; color: Theme.warning }
                            Hint { text: SoundCloud.loginStatus }
                            PillButton {
                                kind: "ghost"
                                text: qsTr("Cancel")
                                onClicked: SoundCloud.cancelLogin()
                            }
                        }

                        // Signed in
                        RowLayout {
                            visible: SoundCloud.connected
                            Layout.fillWidth: true
                            spacing: 12

                            ArtCover {
                                Layout.preferredWidth: 44
                                Layout.preferredHeight: 44
                                radius: Theme.r(22)
                                decorated: false
                                colorA: "#d9a27e"
                                colorB: "#5a3a28"
                                source: SoundCloud.accountPhoto
                                Icon {
                                    visible: SoundCloud.accountPhoto === ""
                                    anchors.centerIn: parent
                                    name: "person"
                                    size: 22
                                    color: "#f4efe8"
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: SoundCloud.accountName || qsTr("SoundCloud account")
                                color: Theme.text
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Spinner {
                                visible: SoundCloud.syncing
                                size: 18
                                color: Theme.textDim
                            }
                            IconButton {
                                visible: !SoundCloud.syncing
                                icon: "refresh"
                                iconSize: 18
                                showDot: false
                                onClicked: SoundCloud.refresh()
                            }
                            PillButton {
                                kind: "ghost"
                                text: qsTr("Sign out")
                                onClicked: SoundCloud.logout()
                            }
                        }
                    }
                }

                // ---- right column -------------------------------------------
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 100
                    Layout.alignment: Qt.AlignTop
                    spacing: 18

                    // FLAC files that may be shared freely — off until the user turns it on.
                    SourceCard {
                        source: page.src("openflac")
                        status: OpenMusic.enabled ? "connected" : "available"
                        detail: !OpenMusic.enabled ? qsTr("Off")
                                : OpenMusic.downloads.length > 0 ? qsTr("Saving %n files…", "", OpenMusic.downloads.length)
                                : qsTr("Shown in Search")

                        Hint {
                            text: qsTr("Everplay only looks where music may be shared: concerts taped with the bands' permission (Live Music Archive), netlabel releases and Creative Commons uploads on the Internet Archive, and freely licensed recordings on Wikimedia Commons. Most chart albums aren't there.")
                        }
                        SettingRow {
                            icon: "lossless"
                            title: qsTr("Search open FLAC sources")
                            description: qsTr("Results get their own section in Search, and play straight from the file")
                            Toggle { checked: OpenMusic.enabled; onToggled: c => OpenMusic.enabled = c }
                        }
                        SettingRow {
                            visible: OpenMusic.enabled
                            icon: "archive"
                            title: "Internet Archive"
                            description: qsTr("Live Music Archive concerts, netlabels and Creative Commons uploads")
                            Toggle { checked: OpenMusic.archiveEnabled; onToggled: c => OpenMusic.archiveEnabled = c }
                        }
                        SettingRow {
                            visible: OpenMusic.enabled
                            icon: "globe"
                            title: "Wikimedia Commons"
                            description: qsTr("Public-domain and freely licensed recordings, much of it classical")
                            Toggle { checked: OpenMusic.commonsEnabled; onToggled: c => OpenMusic.commonsEnabled = c }
                        }
                        ListLine {
                            visible: OpenMusic.enabled
                            icon: "download"
                            iconColor: "#9cc3c9"
                            title: OpenMusic.downloadFolder
                            detail: qsTr("Change")
                            onActivated: downloadFolderDialog.open()
                        }
                        Hint {
                            visible: OpenMusic.enabled
                            text: qsTr("Saved files go to this folder and join your library as local files. Use ⋯ → Save the FLAC file on a track, or save a whole concert from its page.")
                        }
                        Repeater {
                            model: OpenMusic.downloads
                            ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 6
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.title
                                        color: Theme.text
                                        font.pixelSize: 13
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        text: modelData.progress > 0 ? Math.round(modelData.progress * 100) + "%" : qsTr("Waiting")
                                        color: Theme.textDim
                                        font.pixelSize: 12
                                        font.features: { "tnum": 1 }
                                    }
                                }
                                Rectangle {
                                    Layout.fillWidth: true
                                    height: 4
                                    radius: 2
                                    color: Theme.alpha(Theme.text, 0.08)
                                    Rectangle {
                                        width: parent.width * modelData.progress
                                        height: parent.height
                                        radius: 2
                                        color: "#9cc3c9"
                                    }
                                }
                            }
                        }
                        PillButton {
                            visible: OpenMusic.downloads.length > 0
                            kind: "ghost"
                            icon: "close"
                            text: qsTr("Stop saving")
                            onClicked: OpenMusic.cancelDownloads()
                        }
                        FolderDialog {
                            id: downloadFolderDialog
                            title: qsTr("Where to save FLAC files")
                            onAccepted: OpenMusic.downloadFolder = String(selectedFolder)
                        }
                    }

                    SourceCard {
                        source: page.src("radio")

                        ListLine {
                            icon: "live"
                            iconColor: "#cdb68e"
                            live: true
                            title: qsTr("Ambient Waves FM — Night Jazz")
                            detail: "128 kbps"
                            onActivated: Player.playIndex(Library.search("", "radio", 1)[0])
                        }
                        ListLine {
                            icon: "live"
                            iconColor: "#cdb68e"
                            live: true
                            title: "Deep Focus Radio"
                            detail: "192 kbps"
                            onActivated: Nav.toast(qsTr("Radio streams are coming soon"), "radio")
                        }
                        ListLine {
                            icon: "live"
                            iconColor: "#cdb68e"
                            live: true
                            title: qsTr("Lo-Fi at Night")
                            detail: "128 kbps"
                            onActivated: Nav.toast(qsTr("Radio streams are coming soon"), "radio")
                        }
                        PillButton {
                            kind: "ghost"
                            icon: "add"
                            text: qsTr("Add station")
                            onClicked: Nav.toast(qsTr("Adding stations is coming soon"), "radio")
                        }
                    }

                    SourceCard {
                        source: page.src("stream")

                        Hint { text: qsTr("Play any audio stream — paste the address of a file or an M3U / PLS playlist.") }
                        InputField {
                            id: urlField
                            Layout.fillWidth: true
                            icon: "link"
                            placeholderText: "https://…/stream.mp3"
                        }
                        PillButton {
                            icon: "play"
                            text: qsTr("Play")
                            enabled: urlField.text.length > 0
                            onClicked: Nav.toast(qsTr("Playing from a URL is coming soon"), "link")
                        }
                    }
                }
            }
        }
    }
}
