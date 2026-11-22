#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QQuickItem>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;
class QFileSystemWatcher;
class QLocalServer;
class ShortcutPortal;

// The in-game overlay: an XMB-style menu (qml/overlay) over the game, opened with a global shortcut,
// by holding View + Menu on a pad, or with "everplay --overlay" (sent to the running app). It covers the
// game's monitor, takes the keyboard while open and gives the focus back when it closes.
// Windows: a low-level keyboard hook (games can switch RegisterHotKey off), XInput, and the focus moved
// by hand; a game in exclusive fullscreen is minimized while the menu is open.
// Linux (Wayland): the shortcut through the desktop portal (KDE, GNOME 48+, Hyprland) — or the command
// bound in the desktop's own shortcut settings; the pad from evdev (/dev/input); the window as a
// layer-shell overlay where the compositor has it (KDE, Hyprland, Sway; built with LayerShellQt),
// otherwise full screen.
class GameOverlay : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool open READ isOpen NOTIFY openChanged)
    Q_PROPERTY(QRect area READ area NOTIFY openChanged)               // the game's monitor (logical pixels)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY settingsChanged)
    Q_PROPERTY(bool padEnabled READ padEnabled WRITE setPadEnabled NOTIFY settingsChanged)
    // How the shortcut works here: "native" (Windows, set in Everplay), "portal" (the desktop owns it),
    // "command" (no portal: bind toggleCommand in the desktop's settings).
    Q_PROPERTY(QString hotkeyMode READ hotkeyMode NOTIFY settingsChanged)
    Q_PROPERTY(QString hotkey READ hotkey NOTIFY settingsChanged)     // "Ctrl+Shift+M"; "" while the desktop hasn't set one
    Q_PROPERTY(bool hotkeyTaken READ hotkeyTaken NOTIFY settingsChanged)   // another app has the hotkey (Windows)
    Q_PROPERTY(bool canConfigureHotkey READ canConfigureHotkey NOTIFY settingsChanged)   // the desktop's dialog
    Q_PROPERTY(QString toggleCommand READ toggleCommand CONSTANT)     // "everplay --overlay"
    Q_PROPERTY(bool padConnected READ padConnected NOTIFY padConnectedChanged)
    Q_PROPERTY(QQuickItem *view READ view WRITE setView NOTIFY viewChanged)   // the open menu (DevTour)

    // In-game lyrics overlay
    Q_PROPERTY(bool lyricsOpen READ isLyricsOpen NOTIFY lyricsOpenChanged)
    Q_PROPERTY(bool lyricsEnabled READ lyricsEnabled WRITE setLyricsEnabled NOTIFY settingsChanged)
    Q_PROPERTY(double lyricsDimming READ lyricsDimming WRITE setLyricsDimming NOTIFY settingsChanged)
    Q_PROPERTY(QString lyricsMode READ lyricsMode WRITE setLyricsMode NOTIFY settingsChanged)
    Q_PROPERTY(QString lyricsPosition READ lyricsPosition WRITE setLyricsPosition NOTIFY settingsChanged)
    Q_PROPERTY(bool lyricsHideNotes READ lyricsHideNotes WRITE setLyricsHideNotes NOTIFY settingsChanged)
    Q_PROPERTY(bool lyricsHidePunctuation READ lyricsHidePunctuation WRITE setLyricsHidePunctuation NOTIFY settingsChanged)
    Q_PROPERTY(QString lyricsHotkey READ lyricsHotkey NOTIFY settingsChanged)
    Q_PROPERTY(bool lyricsHotkeyTaken READ lyricsHotkeyTaken NOTIFY settingsChanged)

public:
    static GameOverlay *instance();
    static GameOverlay *create(QQmlEngine *, QJSEngine *);
    ~GameOverlay() override;

    // "everplay --overlay" / "--lyrics-overlay": hands the request to a running Everplay; false when none is running.
    static bool sendToRunning(const QString &command = QStringLiteral("overlay"));

    bool supported() const;
    bool isOpen() const { return m_open; }
    QRect area() const { return m_area; }
    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    bool padEnabled() const { return m_padEnabled; }
    void setPadEnabled(bool on);
    QString hotkeyMode() const;
    QString hotkey() const;
    bool hotkeyTaken() const { return m_hotkeyTaken; }
    bool canConfigureHotkey() const;
    QString toggleCommand() const;
    bool padConnected() const { return m_padConnected; }
    QQuickItem *view() const { return m_view; }
    void setView(QQuickItem *view);

    bool isLyricsOpen() const { return m_lyricsOpen; }
    bool lyricsEnabled() const { return m_lyricsEnabled; }
    void setLyricsEnabled(bool on);
    double lyricsDimming() const { return m_lyricsDimming; }
    void setLyricsDimming(double dimming);
    QString lyricsMode() const { return m_lyricsMode; }
    void setLyricsMode(const QString &mode);
    QString lyricsPosition() const { return m_lyricsPosition; }
    void setLyricsPosition(const QString &position);
    bool lyricsHideNotes() const { return m_lyricsHideNotes; }
    void setLyricsHideNotes(bool on);
    bool lyricsHidePunctuation() const { return m_lyricsHidePunctuation; }
    void setLyricsHidePunctuation(bool on);
    QString lyricsHotkey() const;
    bool lyricsHotkeyTaken() const { return m_lyricsHotkeyTaken; }

    Q_INVOKABLE void show();
    Q_INVOKABLE void hide();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void showLyrics();
    Q_INVOKABLE void hideLyrics();
    Q_INVOKABLE void toggleLyrics();
    // Before the window is first shown: the layer-shell role on Wayland.
    Q_INVOKABLE void prepare(QWindow *window);
    // The overlay window takes the keyboard (the game had it). Called once the window is visible.
    Q_INVOKABLE void activate(QWindow *window);
    // Click-through HUD overlay that does not take focus.
    Q_INVOKABLE void activateLyrics(QWindow *window);
    // A key and modifiers from a QML key event; false if it can't be a global hotkey (Windows).
    Q_INVOKABLE bool setHotkey(int key, int modifiers);
    Q_INVOKABLE void resetHotkey();
    Q_INVOKABLE bool setLyricsHotkey(int key, int modifiers);
    Q_INVOKABLE void resetLyricsHotkey();
    Q_INVOKABLE void configureHotkey();
    Q_INVOKABLE void copyToClipboard(const QString &text);
    // Sends a pad action as if it came from the pad (DevTour).
    Q_INVOKABLE void simulate(const QString &action) { emit padInput(action); }

signals:
    void openChanged();
    void lyricsOpenChanged();
    void settingsChanged();
    void padConnectedChanged();
    void viewChanged();
    // "left", "right", "up", "down", "accept", "back", "playPause", "previousTrack", "nextTrack"
    void padInput(const QString &action);
    // A everplay:// link opened while Everplay was already running ("open <link>" on the local socket).
    void linkReceived(const QString &link);

private:
    explicit GameOverlay(QObject *parent = nullptr);
    void registerHotkey();
    void unregisterHotkey();
    void listen();
    void pollPad();
    bool readPad(unsigned &buttons, int &x, int &y);   // the platform's pad, false = none
    void closePad();
    void findPadAsync();   // Linux
    void saveSettings();
    void updateArea();

    bool m_open = false;
    bool m_enabled = true;
    bool m_padEnabled = true;
    bool m_hotkeyTaken = false;
    bool m_hotkeyRegistered = false;
    bool m_padConnected = false;
    int m_key = 0;          // Qt::Key
    int m_modifiers = 0;    // Qt::KeyboardModifiers
    QRect m_area;
    quintptr m_previous = 0;   // the window that had the focus (the game, Windows)
    QPointer<QQuickItem> m_view;
    QLocalServer *m_server = nullptr;
    ShortcutPortal *m_portal = nullptr;   // Linux
    QString m_activationToken;            // from the portal: lets the window take the focus (Wayland)
    bool m_layerShell = false;

    // Lyrics overlay
    bool m_lyricsOpen = false;
    bool m_lyricsEnabled = true;
    double m_lyricsDimming = 0.20;
    QString m_lyricsMode = QStringLiteral("karaoke");
    QString m_lyricsPosition = QStringLiteral("bottom");
    bool m_lyricsHideNotes = false;
    bool m_lyricsHidePunctuation = false;
    int m_lyricsKey = 0;
    int m_lyricsModifiers = 0;
    bool m_lyricsHotkeyTaken = false;
    bool m_lyricsHotkeyRegistered = false;

    // Pad
    QTimer m_padTimer;
    int m_padSlot = -1;            // Windows: the XInput user index; -1 = none
    int m_padFd = -1;              // Linux: the evdev device
    unsigned m_evButtons = 0, m_evHat = 0;
    int m_evX = 0, m_evY = 0;      // the left stick, ±32767, up positive
    int m_absMin[2] = { -32768, -32768 }, m_absMax[2] = { 32767, 32767 };
    qint64 m_nextPadScan = 0;      // looking for a pad is slow — not every tick
    bool m_padScanning = false;    // Linux: a look through /dev/input is running in the background
    bool m_inputChanged = true;    // Linux: /dev/input changed since the last look
    QFileSystemWatcher *m_inputWatcher = nullptr;
    unsigned m_lastButtons = 0;
    QString m_held;                // the direction being held (repeats)
    qint64 m_heldNext = 0;
    qint64 m_comboSince = -1;      // View + Menu held since (ms)
    bool m_comboFired = false;
    QElapsedTimer m_clock;
};
