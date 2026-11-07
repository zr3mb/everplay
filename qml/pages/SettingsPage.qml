import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts
import Everplay

Item {
    id: page
    property var args: ({})
    property int quality: 2

    component Heading: Text {
        Layout.topMargin: 6
        Layout.bottomMargin: 4
        color: Theme.text
        font.family: Theme.fontDisplay
        font.pixelSize: 20
        font.weight: Font.DemiBold
    }

    component Key: Rectangle {
        property string label: ""
        implicitWidth: keyText.implicitWidth + 16
        implicitHeight: 26
        radius: Theme.r(7)
        color: Theme.alpha(Theme.text, 0.07)
        border.width: 1
        border.color: Theme.alpha(Theme.text, 0.1)
        Text {
            id: keyText
            anchors.centerIn: parent
            text: parent.label
            color: Theme.text
            font.pixelSize: 12
            font.weight: Font.DemiBold
        }
    }

    // Nav.navigate("settings", { section: "account" }) opens the page at the Everplay account.
    Timer {
        running: page.args.section === "account"
        interval: 250
        onTriggered: flick.contentY = Math.max(0, Math.min(accountRow.mapToItem(flick.contentItem, 0, 0).y - 60,
                                                            flick.contentHeight - flick.height))
    }

    PageFlick {
        id: flick
        anchors.fill: parent
        spacing: 26

        Reveal {
            Text {
                text: qsTr("Settings")
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
                spacing: 8
                Heading { text: qsTr("Appearance") }
                ThemePicker {
                    Layout.fillWidth: true
                    Layout.bottomMargin: 6
                }
                SettingRow {
                    icon: "grid"
                    title: qsTr("Corners")
                    description: qsTr("The shape of panels and covers: soft and rounded, or crisper corners with round buttons.")
                    SegmentedTabs {
                        model: [qsTr("Rounded"), qsTr("Semi-angular")]
                        currentIndex: Theme.semi ? 1 : 0
                        onActivated: i => Theme.shape = i === 1 ? "semi" : "rounded"
                    }
                }
                SettingRow {
                    icon: "image"
                    title: qsTr("Background image")
                    description: Theme.wallpaper !== ""
                        ? decodeURIComponent(Theme.wallpaper.replace(/^.*[\/]/, ""))
                        : qsTr("Your own picture behind the interface — in any theme.")
                    Row {
                        spacing: 8
                        PillButton {
                            visible: Theme.wallpaper !== ""
                            kind: "ghost"
                            text: qsTr("Remove")
                            fontSize: 13
                            implicitHeight: 36
                            onClicked: Theme.wallpaper = ""
                        }
                        PillButton {
                            kind: "secondary"
                            icon: "image"
                            text: Theme.wallpaper !== "" ? qsTr("Change") : qsTr("Choose a picture")
                            fontSize: 13
                            implicitHeight: 36
                            onClicked: wallpaperDialog.open()
                        }
                    }
                }
                SettingRow {
                    visible: Theme.wallpaper !== ""
                    icon: "tune"
                    title: qsTr("Background dimming")
                    description: qsTr("How much the picture is darkened so the text stays readable.")
                    ProgressSlider {
                        width: 170
                        value: Theme.wallpaperDim
                        labelFor: function(v) { return Math.round(v * 100) + "%" }
                        onMoved: v => Theme.wallpaperDim = Math.round(v * 100) / 100
                    }
                }
                SettingRow {
                    icon: "refresh"
                    title: qsTr("First-run wizard")
                    description: qsTr("Go through choosing the layout, colors, music sources, transitions and integrations again.")
                    PillButton {
                        kind: "secondary"
                        icon: "refresh"
                        text: qsTr("Run the wizard")
                        fontSize: 13
                        implicitHeight: 36
                        onClicked: Nav.startOnboarding()
                    }
                }
                SettingRow {
                    icon: "chat"
                    title: qsTr("Language")
                    description: qsTr("Interface language. “System” follows the language of your operating system.")
                    SegmentedTabs {
                        model: [qsTr("System"), "English", "Polski"]
                        currentIndex: ["system", "en", "pl"].indexOf(Language.language)
                        onActivated: i => Language.language = ["system", "en", "pl"][i]
                    }
                }
                SettingRow {
                    icon: "image"
                    title: qsTr("Dynamic accent color")
                    description: Theme.preset.fixedAccent
                        ? qsTr("The %1 theme keeps its own colors.").arg(Theme.preset.name)
                        : qsTr("Interface colors smoothly adapt to the cover of the playing track.")
                    Toggle {
                        enabled: !Theme.preset.fixedAccent
                        opacity: enabled ? 1 : 0.4
                        checked: Theme.dynamicAccent
                        onToggled: c => Theme.dynamicAccent = c
                    }
                }
                SettingRow {
                    icon: "album"
                    title: qsTr("Live background")
                    description: qsTr("Soft light behind the interface, gently pulsing with the music.")
                    Toggle { checked: Theme.ambient; onToggled: c => Theme.ambient = c }
                }
                SettingRow {
                    icon: "menuOpen"
                    title: qsTr("Compact sidebar")
                    description: qsTr("Show only icons in the sidebar.")
                    Toggle { checked: Nav.sidebarCompact; onToggled: c => Nav.sidebarCompact = c }
                }
            }
        }

        Reveal {
            delay: 120
            ColumnLayout {
                width: parent.width
                spacing: 8
                Heading { text: qsTr("Playback") }
                SettingRow {
                    icon: "tune"
                    title: qsTr("Smooth transitions (crossfade)")
                    description: Player.crossfade > 0
                        ? qsTr("The end of a track overlaps the start of the next one. Playlists with “Mix” turned on use their own settings.")
                        : qsTr("Off — tracks play one after another. You can also turn on Mix for a single playlist.")
                    Row {
                        spacing: 14
                        ProgressSlider {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 170
                            value: Player.crossfade / 12
                            labelFor: function(v) { return Math.round(v * 12) === 0 ? qsTr("off") : qsTr("%1 s").arg(Math.round(v * 12)) }
                            onMoved: v => Player.crossfade = Math.round(v * 12)
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 34
                            text: Player.crossfade === 0 ? qsTr("off") : qsTr("%1 s").arg(Player.crossfade)
                            color: Theme.textDim
                            font.pixelSize: 13
                            font.features: { "tnum": 1 }
                        }
                    }
                }
                SettingRow {
                    visible: Player.crossfade > 0
                    icon: "mix"
                    title: qsTr("Transition style")
                    description: Player.mixStyleDescription(Player.crossfadeStyle)
                    SegmentedTabs {
                        model: Player.mixStyles.map(s => Player.mixStyleName(s))
                        currentIndex: Player.mixStyles.indexOf(Player.crossfadeStyle)
                        onActivated: i => Player.crossfadeStyle = Player.mixStyles[i]
                    }
                }
                SettingRow {
                    icon: "next"
                    title: qsTr("Smart transitions")
                    description: qsTr("The mix starts when the music actually ends: it skips silence at the end and start of tracks and detects fade-outs recorded in the track.")
                    Toggle { checked: Player.smartTransitions; onToggled: c => Player.smartTransitions = c }
                }
                SettingRow {
                    icon: "queue"
                    title: qsTr("Keep playing (autoplay)")
                    description: qsTr("When a playlist ends, the music continues with the next playlist; after an album or search results — with your library.")
                    Toggle { checked: Player.autoplay; onToggled: c => Player.autoplay = c }
                }
                SettingRow {
                    icon: "bolt"
                    title: qsTr("Keep played tracks on disk")
                    description: Player.cacheLimitMB > 0
                        ? qsTr("Tracks from YouTube start at once the next time, without the network; they show a bolt next to the title. The ones you play most stay longest. In use: %1 MB.").arg(Player.cacheUsedMB)
                        : qsTr("Off — every track is downloaded again when played.")
                    RowLayout {
                        spacing: 10
                        PillButton {
                            visible: Player.cacheUsedMB > 0
                            kind: "secondary"
                            text: qsTr("Clear")
                            fontSize: 13
                            implicitHeight: 36
                            onClicked: Player.clearCache()
                        }
                        SegmentedTabs {
                            model: [qsTr("Off"), "1 GB", "2 GB", "5 GB"]
                            currentIndex: [0, 1024, 2048, 5120].indexOf(Player.cacheLimitMB)
                            onActivated: i => Player.cacheLimitMB = [0, 1024, 2048, 5120][i]
                        }
                    }
                }
                SettingRow {
                    icon: "volumeUp"
                    title: qsTr("Volume normalization")
                    description: qsTr("Evens out the loudness of tracks from different sources.")
                    Toggle { checked: true }
                }
                SettingRow {
                    icon: "queue"
                    title: qsTr("Gapless playback")
                    description: qsTr("Live albums and mixes play without silence between tracks.")
                    Toggle { checked: false }
                }
            }
        }

        // The in-game overlay (GameOverlay, qml/overlay) — Windows only.
        Reveal {
            delay: 135
            visible: GameOverlay.supported
            ColumnLayout {
                width: parent.width
                spacing: 8
                Heading { text: qsTr("In-game overlay") }
                SettingRow {
                    icon: "gamepad"
                    title: qsTr("Music menu over games")
                    description: qsTr("A menu over the dimmed game to pause, skip, change the volume or switch the playlist without leaving it. Games in exclusive full screen are minimized while it's open — borderless or windowed mode works best.")
                    Row {
                        spacing: 12
                        PillButton {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: GameOverlay.enabled
                            kind: "ghost"
                            text: qsTr("Try it")
                            onClicked: GameOverlay.show()
                        }
                        Toggle {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: GameOverlay.enabled
                            onToggled: c => GameOverlay.enabled = c
                        }
                    }
                }
                SettingRow {
                    visible: GameOverlay.enabled && GameOverlay.hotkeyMode === "native"
                    icon: "key"
                    title: qsTr("Shortcut")
                    description: hotkeyField.recording ? qsTr("Press the new shortcut (with Ctrl or Alt, or an F key). Esc cancels.")
                               : hotkeyField.rejected ? qsTr("That can't be a shortcut — add Ctrl or Alt, or use an F key.")
                               : GameOverlay.hotkeyTaken ? qsTr("Another app already uses %1 — pick a different shortcut.").arg(GameOverlay.hotkey)
                               : qsTr("Opens and closes the menu from any game. Click to change it.")
                    Row {
                        spacing: 12
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: GameOverlay.hotkey !== "Ctrl+Shift+M" && !hotkeyField.recording
                            text: qsTr("Reset")
                            color: resetMouse.containsMouse ? Theme.text : Theme.accent
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            MouseArea {
                                id: resetMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: GameOverlay.resetHotkey()
                            }
                        }
                        // The shortcut as a key cap; click, then press the new one.
                        Rectangle {
                            id: hotkeyField
                            property bool recording: false
                            property bool rejected: false
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(130, hotkeyText.implicitWidth + 28)
                            height: 34
                            radius: Theme.r(9)
                            color: recording ? Theme.alpha(Theme.accent, 0.22) : Theme.alpha(Theme.text, keyMouse.containsMouse ? 0.1 : 0.06)
                            border.width: 1
                            border.color: recording ? Theme.accent : GameOverlay.hotkeyTaken ? Theme.danger : Theme.alpha(Theme.text, 0.14)
                            onActiveFocusChanged: if (!activeFocus) recording = false
                            Text {
                                id: hotkeyText
                                anchors.centerIn: parent
                                text: hotkeyField.recording ? qsTr("Press keys…") : GameOverlay.hotkey
                                color: Theme.text
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            MouseArea {
                                id: keyMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    hotkeyField.rejected = false
                                    hotkeyField.recording = true
                                    hotkeyField.forceActiveFocus()
                                }
                            }
                            Keys.onPressed: event => {
                                if (!recording)
                                    return
                                event.accepted = true
                                if (event.key === Qt.Key_Escape) {
                                    recording = false
                                    return
                                }
                                // Modifiers alone: wait for the key.
                                if ([Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_AltGr].indexOf(event.key) >= 0)
                                    return
                                rejected = !GameOverlay.setHotkey(event.key, event.modifiers)
                                recording = false
                            }
                        }
                    }
                }
                // Linux: the desktop owns the shortcut (portal) …
                SettingRow {
                    visible: GameOverlay.enabled && GameOverlay.hotkeyMode === "portal"
                    icon: "key"
                    title: qsTr("Shortcut")
                    description: GameOverlay.hotkey !== ""
                        ? qsTr("Set in your desktop's shortcut settings — it opens and closes the menu from any game.")
                        : qsTr("Your desktop asks once whether Everplay may have a shortcut. On Hyprland, add to hyprland.conf: %1")
                              .arg("bind = CTRL SHIFT, M, global, everplay:overlay")
                    Row {
                        spacing: 12
                        PillButton {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: GameOverlay.canConfigureHotkey
                            kind: "ghost"
                            text: qsTr("Change")
                            onClicked: GameOverlay.configureHotkey()
                        }
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: GameOverlay.hotkey !== ""
                            width: portalKey.implicitWidth + 28
                            height: 34
                            radius: Theme.r(9)
                            color: Theme.alpha(Theme.text, 0.06)
                            border.width: 1
                            border.color: Theme.alpha(Theme.text, 0.14)
                            Text {
                                id: portalKey
                                anchors.centerIn: parent
                                text: GameOverlay.hotkey
                                color: Theme.text
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }
                // … or there's no portal: a command for the desktop's own shortcut settings.
                SettingRow {
                    visible: GameOverlay.enabled && GameOverlay.hotkeyMode === "command"
                    icon: "key"
                    title: qsTr("Shortcut")
                    description: qsTr("Your desktop doesn't let apps add shortcuts. Add one in its keyboard settings that runs: %1")
                                 .arg(GameOverlay.toggleCommand)
                    PillButton {
                        kind: "ghost"
                        text: qsTr("Copy command")
                        onClicked: {
                            GameOverlay.copyToClipboard(GameOverlay.toggleCommand)
                            Nav.toast(qsTr("Command copied"), "check")
                        }
                    }
                }
                SettingRow {
                    visible: GameOverlay.enabled
                    icon: "gamepad"
                    title: qsTr("Xbox controller")
                    description: (GameOverlay.padConnected ? qsTr("Controller connected.") : qsTr("No controller connected right now.")) + " "
                                 + qsTr("Hold View + Menu to open the menu; d-pad or stick to move, A selects, B goes back, Y pauses, LB / RB skip tracks.")
                    Toggle { checked: GameOverlay.padEnabled; onToggled: c => GameOverlay.padEnabled = c }
                }
                SettingRow {
                    icon: "lyrics"
                    title: qsTr("In-game lyrics overlay")
                    description: qsTr("Displays song lyrics directly over games while playing with click-through transparency. Won't intercept mouse or keyboard inputs.")
                    Row {
                        spacing: 12
                        PillButton {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: GameOverlay.lyricsEnabled
                            kind: "ghost"
                            text: qsTr("Try it")
                            onClicked: GameOverlay.showLyrics()
                        }
                        Toggle {
                            anchors.verticalCenter: parent.verticalCenter
                            checked: GameOverlay.lyricsEnabled
                            onToggled: c => GameOverlay.lyricsEnabled = c
                        }
                    }
                }
                SettingRow {
                    visible: GameOverlay.lyricsEnabled
                    icon: "tune"
                    title: qsTr("Lyrics display style")
                    description: GameOverlay.lyricsMode === "adhd"
                        ? qsTr("ADHD: shows only the single word currently being sung, for maximum focus without distractions.")
                        : (GameOverlay.lyricsMode === "sections"
                            ? qsTr("Sections: shows verse stanzas in clear blocks, highlighting the active singing line.")
                            : qsTr("Karaoke: active line prominent in the center with letter-by-letter timing and next-line preview."))
                    SegmentedTabs {
                        model: [qsTr("Karaoke"), qsTr("Sections"), qsTr("ADHD")]
                        currentIndex: GameOverlay.lyricsMode === "adhd" ? 2 : (GameOverlay.lyricsMode === "sections" ? 1 : 0)
                        onActivated: i => GameOverlay.lyricsMode = (i === 2 ? "adhd" : (i === 1 ? "sections" : "karaoke"))
                    }
                }
                SettingRow {
                    visible: GameOverlay.lyricsEnabled
                    icon: "eye"
                    title: qsTr("Background dimming")
                    description: qsTr("Slight screen darkening over the game to make lyrics easy to read: %1%.").arg(Math.round(GameOverlay.lyricsDimming * 100))
                    Row {
                        spacing: 12
                        anchors.verticalCenter: parent.verticalCenter
                        ProgressSlider {
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 160
                            value: GameOverlay.lyricsDimming / 0.80
                            onMoved: v => GameOverlay.lyricsDimming = Math.round(v * 0.80 * 100) / 100
                            labelFor: v => Math.round(v * 80) + "%"
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: Math.round(GameOverlay.lyricsDimming * 100) + "%"
                            color: Theme.textDim
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            width: 36
                        }
                    }
                }
                SettingRow {
                    visible: GameOverlay.lyricsEnabled
                    icon: "menuOpen"
                    title: qsTr("Screen position")
                    description: qsTr("Where the lyrics HUD appears on your monitor.")
                    SegmentedTabs {
                        model: [qsTr("Bottom"), qsTr("Center"), qsTr("Top")]
                        currentIndex: ["bottom", "center", "top"].indexOf(GameOverlay.lyricsPosition) >= 0
                            ? ["bottom", "center", "top"].indexOf(GameOverlay.lyricsPosition) : 0
                        onActivated: i => GameOverlay.lyricsPosition = ["bottom", "center", "top"][i]
                    }
                }
                SettingRow {
                    visible: GameOverlay.lyricsEnabled && GameOverlay.hotkeyMode === "native"
                    icon: "key"
                    title: qsTr("Lyrics shortcut")
                    description: lyricsHotkeyField.recording ? qsTr("Press the new shortcut (with Ctrl or Alt, or an F key). Esc cancels.")
                               : lyricsHotkeyField.rejected ? qsTr("That can't be a shortcut — add Ctrl or Alt, or use an F key.")
                               : GameOverlay.lyricsHotkeyTaken ? qsTr("Another app already uses %1 — pick a different shortcut.").arg(GameOverlay.lyricsHotkey)
                               : qsTr("Toggles the in-game lyrics overlay on and off while playing.")
                    Row {
                        spacing: 12
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: GameOverlay.lyricsHotkey !== "Ctrl+Shift+L" && !lyricsHotkeyField.recording
                            text: qsTr("Reset")
                            color: lyricsResetMouse.containsMouse ? Theme.text : Theme.accent
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            MouseArea {
                                id: lyricsResetMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: GameOverlay.resetLyricsHotkey()
                            }
                        }
                        Rectangle {
                            id: lyricsHotkeyField
                            property bool recording: false
                            property bool rejected: false
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(130, lyricsHotkeyText.implicitWidth + 28)
                            height: 34
                            radius: Theme.r(9)
                            color: recording ? Theme.alpha(Theme.accent, 0.22) : Theme.alpha(Theme.text, lyricsKeyMouse.containsMouse ? 0.1 : 0.06)
                            border.width: 1
                            border.color: recording ? Theme.accent : GameOverlay.lyricsHotkeyTaken ? Theme.danger : Theme.alpha(Theme.text, 0.14)
                            onActiveFocusChanged: if (!activeFocus) recording = false
                            Text {
                                id: lyricsHotkeyText
                                anchors.centerIn: parent
                                text: lyricsHotkeyField.recording ? qsTr("Press keys…") : GameOverlay.lyricsHotkey
                                color: Theme.text
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            MouseArea {
                                id: lyricsKeyMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    lyricsHotkeyField.rejected = false
                                    lyricsHotkeyField.recording = true
                                    lyricsHotkeyField.forceActiveFocus()
                                }
                            }
                            Keys.onPressed: event => {
                                if (!recording)
                                    return
                                event.accepted = true
                                if (event.key === Qt.Key_Escape) {
                                    recording = false
                                    return
                                }
                                if ([Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta, Qt.Key_AltGr].indexOf(event.key) >= 0)
                                    return
                                rejected = !GameOverlay.setLyricsHotkey(event.key, event.modifiers)
                                recording = false
                            }
                        }
                    }
                }
            }
        }

        Reveal {
            delay: 150
            ColumnLayout {
                width: parent.width
                spacing: 8
                Heading { text: qsTr("Lyrics") }
                SettingRow {
                    icon: "lyrics"
                    title: qsTr("Lyrics from YouTube Music")
                    description: qsTr("Lyrics of the playing track (synced when available) in the “Now playing” panel and in full-screen mode. For local files the track is looked up by artist and title; as a fallback, an .lrc file next to the track or lyrics stored in the tags are used.")
                }
                SettingRow {
                    icon: "lyrics"
                    title: qsTr("Letter-by-letter highlight")
                    description: qsTr("The line being sung lights up letter by letter, karaoke style. Off: the whole line lights up at once, and words aren't timed.")
                    Toggle { checked: Lyrics.karaoke; onToggled: c => Lyrics.karaoke = c }
                }
                SettingRow {
                    icon: "clock"
                    title: qsTr("Automatic timing (alpha)")
                    description: qsTr("Lyrics without timestamps get them, and synced lyrics get every word timed: Everplay matches the lyrics to the singing in the recording (CrispASR, locally on your computer). The result is remembered.")
                        + " " + (Lyrics.sttUsesGpu
                                 ? qsTr("On %1 the vocals are first separated from the music, which makes the timing much more accurate; it takes up to a minute per track.").arg(Lyrics.sttGpuName)
                                 : qsTr("With a graphics card the vocals are first separated from the music, which makes the timing much more accurate. Without one it takes a few seconds per track."))
                    Toggle {
                        enabled: Lyrics.hardwareOk
                        opacity: enabled ? 1 : 0.4
                        checked: Lyrics.autoAlign
                        onToggled: c => Lyrics.autoAlign = c
                    }
                }
                SettingRow {
                    icon: Lyrics.sttComplete ? "check" : "add"
                    title: Lyrics.sttComplete ? qsTr("Lyric timing is ready") : qsTr("Download components")
                    description: Lyrics.sttDownloading
                        ? qsTr("Downloading: %1… %2%").arg(Lyrics.sttDownloadLabel).arg(Math.round(Lyrics.sttProgress * 100))
                        : [qsTr("CrispASR program: %1").arg(Lyrics.sttEngineReady ? qsTr("installed")
                               : Lyrics.sttCanDownloadEngine ? qsTr("missing (official release from GitHub, %1)").arg(Lyrics.sttEngineSize)
                               : qsTr("missing — install CrispASR (the crispasr command)")),
                           qsTr("alignment model: %1").arg(Lyrics.sttAlignerReady ? qsTr("downloaded") : qsTr("missing (%1, Hugging Face)").arg(Lyrics.sttAlignerSize)),
                           qsTr("vocal separation model: %1").arg(Lyrics.sttSeparatorReady ? qsTr("downloaded")
                               : !Lyrics.sttEngineReady ? qsTr("with a graphics card (%1, Hugging Face)").arg(Lyrics.sttSeparatorSize)
                               : Lyrics.sttUsesGpu ? qsTr("missing (%1, Hugging Face)").arg(Lyrics.sttSeparatorSize)
                               : qsTr("not needed without a graphics card"))
                          ].join(" · ") + ". " + qsTr("Files are verified with SHA-256.")
                    Row {
                        spacing: 8
                        PillButton {
                            visible: Lyrics.sttDownloading
                            kind: "ghost"
                            icon: "close"
                            text: qsTr("Cancel")
                            onClicked: Lyrics.cancelSttDownload()
                        }
                        PillButton {
                            visible: !Lyrics.sttDownloading && !Lyrics.sttComplete && (Lyrics.sttEngineReady || Lyrics.sttCanDownloadEngine)
                            kind: "primary"
                            icon: "add"
                            text: qsTr("Download")
                            onClicked: Lyrics.downloadSttComponents()
                        }
                        PillButton {
                            visible: !Lyrics.sttDownloading && !Lyrics.sttEngineReady && !Lyrics.sttCanDownloadEngine
                            kind: "ghost"
                            icon: "refresh"
                            text: qsTr("Check again")
                            onClicked: Lyrics.redetectStt()
                        }
                        PillButton {
                            visible: !Lyrics.sttDownloading && (Lyrics.sttAlignerReady || Lyrics.sttSeparatorReady)
                            kind: "ghost"
                            text: qsTr("Remove")
                            onClicked: Lyrics.removeSttComponents()
                        }
                    }
                }
                SettingRow {
                    visible: Lyrics.sttEngineReady
                    icon: "equalizer"
                    title: qsTr("Graphics card")
                    description: Lyrics.sttUsesGpu
                        ? qsTr("%1: the vocals are separated on it before timing, and lyrics recognition runs on it too.").arg(Lyrics.sttGpuName)
                        : qsTr("None found (CrispASR looks for one through Vulkan). Timing runs on the CPU without separating the vocals, so it's less accurate.")
                }
                // Shown with the hardware for it: the button in the lyrics of a track without any needs it.
                SettingRow {
                    visible: Lyrics.recognizeHardwareOk
                    icon: "tune"
                    title: qsTr("Lyrics recognition model")
                    description: qsTr("For tracks nobody has lyrics for, Everplay can write them down from the recording (test feature). A larger model recognizes singing better but runs slower.") + " "
                        + Lyrics.sttModel + ": " + Lyrics.modelSize(Lyrics.sttModel) + " · "
                        + (Lyrics.sttModelReady ? qsTr("downloaded") : qsTr("not downloaded")) + " · "
                        + (Lyrics.sttModel === Lyrics.recommendedModel ? qsTr("recommended for your hardware") : qsTr("recommended: %1").arg(Lyrics.recommendedModel))
                    Row {
                        spacing: 8
                        SegmentedTabs {
                            model: Lyrics.sttModels.map(m => m.charAt(0).toUpperCase() + m.slice(1))
                            currentIndex: Lyrics.sttModels.indexOf(Lyrics.sttModel)
                            onActivated: i => Lyrics.sttModel = Lyrics.sttModels[i]
                        }
                        PillButton {
                            visible: !Lyrics.sttDownloading && !Lyrics.sttModelReady
                            kind: "primary"
                            icon: "add"
                            text: qsTr("Download")
                            onClicked: Lyrics.downloadSttModel()
                        }
                        PillButton {
                            visible: !Lyrics.sttDownloading && Lyrics.sttModelReady
                            kind: "ghost"
                            text: qsTr("Remove")
                            onClicked: Lyrics.removeSttModel(Lyrics.sttModel)
                        }
                    }
                }
                // Results of whisper, which recognized the singing before forced alignment
                SettingRow {
                    visible: Lyrics.whisperLyricsCount > 0
                    icon: "refresh"
                    title: qsTr("Lyrics from the old speech recognition")
                    description: qsTr("%n track(s) have lyrics recognized by whisper or timing made by it, often with mistakes. Remove them: the lyrics are looked up again, and with automatic timing on, timed anew.", "", Lyrics.whisperLyricsCount)
                    Component.onCompleted: Lyrics.countWhisperLyrics()
                    PillButton {
                        kind: "ghost"
                        text: qsTr("Remove")
                        onClicked: Lyrics.removeWhisperLyrics()
                    }
                }
            }
        }

        Reveal {
            delay: 165
            ColumnLayout {
                width: parent.width
                spacing: 8
                Heading { text: "Discord" }
                SettingRow {
                    icon: "chat"
                    title: qsTr("Show what you're listening to on Discord")
                    description: qsTr("A “Listening” status with the title, artist, cover and a progress bar. Works with the Discord app running on this computer. Status: %1.").arg(Discord.status)
                    Toggle { checked: Discord.enabled; onToggled: c => Discord.enabled = c }
                }
                SettingRow {
                    visible: Discord.enabled
                    icon: "key"
                    title: qsTr("Discord application ID")
                    description: Discord.builtInClientId && Discord.clientId !== ""
                        ? qsTr("Everplay's built-in ID. You can enter your own to change the name shown in the status.")
                        : qsTr("Create an application at discord.com/developers/applications (its name will show as “Listening to <name>”), copy the “Application ID” and paste it here. Optionally add an image named “everplay” under Rich Presence → Art Assets — it becomes the cover for files without a cover online.")
                    InputField {
                        width: 230
                        implicitHeight: 38
                        placeholderText: qsTr("e.g. 1234567890123456789")
                        text: Discord.clientId
                        inputMethodHints: Qt.ImhDigitsOnly
                        onEditingFinished: Discord.clientId = text
                    }
                }
                SettingRow {
                    visible: Discord.enabled
                    icon: "pause"
                    title: qsTr("Show while paused too")
                    description: qsTr("By default the status disappears when the music is paused (like Spotify).")
                    Toggle { checked: Discord.showWhenPaused; onToggled: c => Discord.showWhenPaused = c }
                }
            }
        }

        Reveal {
            delay: 172
            ColumnLayout {
                width: parent.width
                spacing: 8
                Heading { text: qsTr("Online") }
                SettingRow {
                    id: accountRow
                    icon: "person"
                    title: Online.login !== "" ? qsTr("Everplay account: %1").arg(Online.login) : qsTr("Everplay account")
                    description: Online.login !== ""
                                 ? qsTr("Signed in. Your library, playlists and folders are the same on every device signed in to this account.")
                                 : Online.registered
                                   ? qsTr("This account has no password yet, so it lives only on this device — lose the device and its shared playlists are gone. Set a login and a password to keep it safe and sign in on your phone, laptop or in the browser.")
                                   : qsTr("Sign in on every device — computer, laptop, phone, browser — for the same library and playlists everywhere.")
                    Row {
                        spacing: 8
                        PillButton {
                            visible: Online.login === ""
                            text: Online.registered ? qsTr("Set a password") : qsTr("Create an account")
                            fontSize: 13
                            implicitHeight: 36
                            onClicked: Nav.account("create")
                        }
                        PillButton {
                            visible: Online.login === ""
                            kind: "ghost"
                            text: qsTr("Sign in")
                            fontSize: 13
                            implicitHeight: 36
                            onClicked: Nav.account("signin")
                        }
                        PillButton {
                            visible: Online.login !== ""
                            kind: "ghost"
                            text: qsTr("Change the password")
                            fontSize: 13
                            implicitHeight: 36
                            onClicked: Nav.account("password")
                        }
                        PillButton {
                            visible: Online.login !== ""
                            kind: "ghost"
                            text: qsTr("Sign out")
                            fontSize: 13
                            implicitHeight: 36
                            onClicked: Online.signOut()
                        }
                    }
                }
                SettingRow {
                    visible: Online.login !== ""
                    icon: "devices"
                    title: qsTr("Signed-in devices")
                    description: qsTr("Sign out a device you don't use or don't recognise.")
                    Component.onCompleted: Online.refreshAccount()
                }
                Repeater {
                    model: Online.login !== "" ? Online.devices : []
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.leftMargin: 52
                        implicitHeight: 52
                        radius: Theme.r(14)
                        color: Qt.rgba(1, 1, 1, 0.03)
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 16
                            anchors.rightMargin: 10
                            spacing: 10
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text {
                                    Layout.fillWidth: true
                                    text: (modelData.name || qsTr("Unknown device")) + (modelData.current ? "  ·  " + qsTr("this device") : "")
                                    color: modelData.current ? Theme.accent : Theme.text
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: qsTr("Last used %1").arg(new Date(modelData.seen).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                                    color: Theme.textDim
                                    font.pixelSize: 11
                                }
                            }
                            PillButton {
                                visible: !modelData.current
                                kind: "ghost"
                                text: qsTr("Sign out")
                                fontSize: 12
                                implicitHeight: 32
                                onClicked: Online.signOutDevice(modelData.id)
                            }
                        }
                    }
                }
                SettingRow {
                    visible: Online.login !== ""
                    icon: "key"
                    title: qsTr("Recovery key")
                    description: qsTr("Sets a new password if you forget it. Lost the old one? Get a new key.")
                    PillButton {
                        kind: "ghost"
                        text: qsTr("New key")
                        fontSize: 13
                        implicitHeight: 36
                        onClicked: Nav.account("newkey")
                    }
                }
                SettingRow {
                    icon: "person"
                    title: qsTr("Your name")
                    description: qsTr("What others see in listening rooms and shared playlists.")
                    InputField {
                        width: 230
                        implicitHeight: 38
                        maximumLength: 40
                        text: Online.displayName
                        onEditingFinished: Online.displayName = text
                    }
                }
                SettingRow {
                    icon: "sources"
                    title: qsTr("Server")
                    description: qsTr("Links, shared playlists and listening rooms go through this server. %1").arg(
                        Online.connected ? qsTr("Connected.") : Online.registered ? qsTr("Signed in.") : qsTr("Not used yet."))
                    InputField {
                        width: 260
                        implicitHeight: 38
                        text: Online.serverUrl
                        onEditingFinished: Online.serverUrl = text
                    }
                }
                SettingRow {
                    icon: "heart"
                    title: qsTr("Same library on every device")
                    description: qsTr("YouTube Music likes and what you play go to your Everplay account, so every device signed in to it — computer, phone, browser — shows the same Liked and Recently played.")
                    Toggle { checked: Online.librarySync; onToggled: c => Online.librarySync = c }
                }
                SettingRow {
                    id: pairRow
                    // the code is valid for ten minutes; the row counts down
                    property int minutesLeft: Online.pairCode === "" ? 0 : Math.ceil((Online.pairExpires - clock.now) / 60000)
                    icon: "link"
                    title: qsTr("Link another device")
                    description: pairRow.minutesLeft > 0
                                 ? qsTr("Enter this code on the other device. Valid for %1 more min.").arg(pairRow.minutesLeft)
                                 : qsTr("Show a code and enter it on another device (the app on a computer or phone, or the browser version): it gets this account, with its library, playlists and rooms.")
                    Timer {
                        id: clock
                        property double now: Date.now()
                        interval: 15000
                        repeat: true
                        running: Online.pairCode !== ""
                        triggeredOnStart: true
                        onTriggered: now = Date.now()
                    }
                    Text {
                        visible: pairRow.minutesLeft > 0
                        text: Online.pairCode.slice(0, 4) + "-" + Online.pairCode.slice(4)
                        color: Theme.accent
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                        font.letterSpacing: 2
                        font.features: { "tnum": 1 }
                    }
                    PillButton {
                        visible: pairRow.minutesLeft <= 0
                        kind: "ghost"
                        text: qsTr("Show a code")
                        fontSize: 13
                        implicitHeight: 36
                        onClicked: Online.createPairCode()
                    }
                }
                SettingRow {
                    icon: "key"
                    title: qsTr("I have a code")
                    description: qsTr("From another device: this one signs in to that device's Everplay account and brings along what it has here (likes, shared playlists).")
                    Row {
                        spacing: 8
                        InputField {
                            id: codeField
                            width: 140
                            implicitHeight: 38
                            maximumLength: 9
                            placeholderText: "XXXX-XXXX"
                            onAccepted: linkButton.clicked()
                        }
                        PillButton {
                            id: linkButton
                            text: qsTr("Link")
                            fontSize: 13
                            implicitHeight: 38
                            enabled: codeField.text.replace(/[^A-Za-z0-9]/g, "").length === 8
                            onClicked: {
                                Online.redeemPairCode(codeField.text)
                                codeField.text = ""
                            }
                        }
                    }
                }
                SettingRow {
                    visible: Online.registered
                    icon: "close"
                    title: qsTr("Delete the online account")
                    description: qsTr("Removes your name, your share links and the shared playlists you own from the server.")
                    PillButton {
                        id: forgetButton
                        property bool confirm: false
                        kind: "ghost"
                        text: confirm ? qsTr("Click again to delete") : qsTr("Delete")
                        fontSize: 13
                        implicitHeight: 36
                        onClicked: {
                            if (!confirm) {
                                confirm = true
                                return
                            }
                            confirm = false
                            Online.forgetAccount()
                        }
                    }
                }
            }
        }

        Reveal {
            delay: 180
            ColumnLayout {
                width: parent.width
                spacing: 8
                Heading { text: qsTr("Stream quality") }
                SettingRow {
                    icon: "playCircle"
                    title: qsTr("YouTube Music and radio")
                    description: [qsTr("Lowest data usage."), qsTr("Balanced quality."), qsTr("Highest available quality (up to 256 kbps)."), qsTr("Lossless when the source allows it.")][page.quality]
                    SegmentedTabs {
                        model: [qsTr("Data saver"), qsTr("Normal"), qsTr("High"), qsTr("Lossless")]
                        currentIndex: page.quality
                        onActivated: i => page.quality = i
                    }
                }
            }
        }

        Reveal {
            delay: 240
            ColumnLayout {
                width: parent.width
                spacing: 12
                Heading { text: qsTr("Keyboard shortcuts") }
                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 30
                    rowSpacing: 12

                    Repeater {
                        model: [
                            { keys: [qsTr("Space")], label: qsTr("Play / pause") },
                            { keys: ["Ctrl", "→"], label: qsTr("Next track") },
                            { keys: ["Ctrl", "←"], label: qsTr("Previous track") },
                            { keys: ["Ctrl", "K"], label: qsTr("Search") },
                            { keys: ["F11"], label: qsTr("Full-screen mode") },
                            { keys: ["Alt", "←"], label: qsTr("Back") }
                        ]
                        RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: parent.modelData.keys
                                Key {
                                    required property var modelData
                                    label: modelData
                                }
                            }
                            Text {
                                Layout.leftMargin: 8
                                Layout.fillWidth: true
                                text: parent.modelData.label
                                color: Theme.textDim
                                font.pixelSize: 13
                            }
                        }
                    }
                }
            }
        }

        Reveal {
            delay: 300
            RowLayout {
                width: parent.width
                spacing: 14
                LogoMark {}
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text { text: "Everplay " + Updater.currentVersion; color: Theme.text; font.pixelSize: 14; font.weight: Font.DemiBold }
                    Text { text: "C++20 · Qt 6 · Qt Quick"; color: Theme.textFaint; font.pixelSize: 12 }
                }
                PillButton {
                    text: Updater.available ? qsTr("Update to %1").arg(Updater.latestVersion) : qsTr("Check for updates")
                    kind: "secondary"
                    icon: "refresh"
                    busy: Updater.state === "checking"
                    onClicked: Updater.check(true)
                }
            }
        }
    }

    FileDialog {
        id: wallpaperDialog
        title: qsTr("Choose a background picture")
        nameFilters: [qsTr("Images (*.jpg *.jpeg *.png *.webp *.bmp *.gif)")]
        onAccepted: Theme.wallpaper = selectedFile.toString()
    }
}
