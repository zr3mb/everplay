#include <QDateTime>
#include <QScreen>
#include <atomic>
#include <functional>
#include <memory>
#include "AppPaths.h"
#include "Language.h"
#include "online/OnlineService.h"
#include "platform/EarlyGraphics.h"
#include "platform/GameOverlay.h"

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QQmlApplicationEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QQmlProperty>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

namespace {

// Covers from the web are kept on disk, so a list scrolled again or the app opened again shows them without
// downloading them anew. QML asks for an access manager per thread (the images load on their own); each gets
// its own directory, as two disk caches mustn't share one.
class CachedNetworkFactory : public QQmlNetworkAccessManagerFactory
{
public:
    QNetworkAccessManager *create(QObject *parent) override
    {
        auto *manager = new QNetworkAccessManager(parent);
        auto *cache = new QNetworkDiskCache(manager);
        cache->setCacheDirectory(AppPaths::dataDir() + QStringLiteral("/image-cache/") + QString::number(m_made.fetchAndAddRelaxed(1)));
        cache->setMaximumCacheSize(150LL * 1024 * 1024);
        manager->setCache(cache);
        return manager;
    }

private:
    QAtomicInt m_made;
};

} // namespace

int main(int argc, char *argv[])
{
    QElapsedTimer startClock;   // EVERPLAY_PERF_DEBUG: how long the start takes
    startClock.start();
    EarlyGraphics::start();     // the window's GPU device, made while everything below runs
    // Images (covers) give up their memory copy once they're uploaded to the GPU, and skip the shared
    // texture atlas — covers are drawn from a mipmapped copy of their own, so the atlas only kept a
    // second one. Variables set by the user win. Not on Android: it drops the GPU's copies whenever the app
    // goes to the background, and every cover would be loaded again each time it comes back.
#ifndef Q_OS_ANDROID
    if (!qEnvironmentVariableIsSet("QSG_TRANSIENT_IMAGES"))
        qputenv("QSG_TRANSIENT_IMAGES", "1");
#endif
    if (!qEnvironmentVariableIsSet("QSG_ATLAS_SIZE_LIMIT"))
        qputenv("QSG_ATLAS_SIZE_LIMIT", "1");
    // Everplay plays audio only, but Qt's FFmpeg backend probes every hardware video decoder (CUDA,
    // Vulkan, QSV…) the first time it loads, which held the start for well over a second. An unknown
    // device name leaves the lists empty, so nothing is probed.
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", "none");
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_ENCODING_HW_DEVICE_TYPES", "none");

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Everplay"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Everplay"));
    QGuiApplication::setOrganizationName(QStringLiteral("Everplay"));
    QGuiApplication::setApplicationVersion(QStringLiteral(EVERPLAY_VERSION));
    // Wayland/X11: ties the window to the everplay.desktop file (icon, name in the panel, MPRIS).
    QGuiApplication::setDesktopFileName(QStringLiteral("everplay"));
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/brand/icon-256.png")));
    AppPaths::adoptFormerName();   // before anything reads the data folder

    // The "Basic" style allows fully restyling the controls in QML.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

#ifdef Q_OS_WIN
    QFont font(QStringLiteral("Segoe UI Variable Text"));
    font.setHintingPreference(QFont::PreferNoHinting);
    QGuiApplication::setFont(font);
#endif

    // "everplay --overlay" / "--lyrics-overlay": the running Everplay opens or closes its overlay;
    // with none running, this one starts and opens it.
    const bool overlayRequest = app.arguments().contains(QStringLiteral("--overlay"));
    const bool lyricsOverlayRequest = app.arguments().contains(QStringLiteral("--lyrics-overlay"));
    if (overlayRequest && GameOverlay::sendToRunning(QStringLiteral("overlay")))
        return 0;
    if (lyricsOverlayRequest && GameOverlay::sendToRunning(QStringLiteral("lyrics-overlay")))
        return 0;
    // "everplay everplay://r/<code>" (the system opens links this way): a running Everplay opens it.
    QString link;
    for (const QString &arg : app.arguments().mid(1))
        if ((arg.startsWith(QLatin1String("everplay:")) || arg.startsWith(QLatin1String("nokturn:")) || arg.startsWith(QLatin1String("http"))) && OnlineService::isLink(arg))
            link = arg;
    if (!link.isEmpty() && GameOverlay::sendToRunning(QStringLiteral("open ") + link))
        return 0;

    Language::instance()->apply();

    QQmlApplicationEngine engine;
    static CachedNetworkFactory networkFactory;
    engine.setNetworkAccessManagerFactory(&networkFactory);
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);

    // Only the window is made here, so it comes on screen at once; the interface (Shell.qml) is built into
    // it after its first frame.
    engine.loadFromModule("Everplay", "Main");
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    const bool perfDebug = qEnvironmentVariableIsSet("EVERPLAY_PERF_DEBUG");
    if (perfDebug && window) {
        qInfo("perf: window shown %lld ms after start", startClock.elapsed());
        // From the render thread, as it happens (queued, it would wait for the GUI thread). The first frame is
        // never drawn before this: the window's first expose comes after Main.qml is made.
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [startClock] {
            qInfo("perf: first frame on screen %lld ms after start", startClock.elapsed());
        }, Qt::ConnectionType(Qt::DirectConnection | Qt::SingleShotConnection));
    }
    // The window is shown but not drawn yet: it gets its background and its GPU device now. While the device
    // is still being made, the services the interface uses are (Theme makes Player, Library, Plugins…).
    EarlyGraphics::prepare(window, [&engine] { engine.singletonInstance<QObject *>("Everplay", "Theme"); });
#ifdef Q_OS_WIN
    // When the first Qt::Window is made, Qt's Windows plugin asks Direct3D 9 which GPU drives the main
    // display (a workaround for an old AMD driver bug, QTBUG-50371), and that took ~140 ms before the
    // window could appear. Main.qml makes it as a dialog, which skips the check — to Windows it's the
    // same frameless window — and here, once it exists, it becomes a plain window again. (After the
    // device above: the change redraws the window at once.)
    if (window && window->handle() && window->type() == Qt::Dialog)
        window->setFlags((window->flags() & ~Qt::WindowType_Mask) | Qt::Window);
#endif
    // Runs `run` `delay` ms after the interface is built (Main.qml's "ready").
    const auto afterInterface = [window](int delay, const std::function<void()> &run) {
        if (!window)
            return;
        auto *timer = new QTimer(window);
        timer->setSingleShot(true);
        timer->setInterval(delay);
        QObject::connect(timer, &QTimer::timeout, timer, [timer, run] {
            run();
            timer->deleteLater();
        });
        if (window->property("ready").toBool())
            timer->start();
        else
            QQmlProperty(window, QStringLiteral("ready")).connectNotifySignal(timer, SLOT(start()));
    };
    if (perfDebug)
        afterInterface(0, [startClock] { qInfo("perf: the interface was built %lld ms after start", startClock.elapsed()); });
    QObject::connect(GameOverlay::instance(), &GameOverlay::linkReceived, OnlineService::instance(), &OnlineService::openLink);
    if (!link.isEmpty())
        afterInterface(1200, [link] { OnlineService::instance()->openLink(link); });
    if (overlayRequest)
        afterInterface(1500, [] { GameOverlay::instance()->show(); });
    else if (lyricsOverlayRequest)
        afterInterface(1500, [] { GameOverlay::instance()->showLyrics(); });

    // After a GPU reset the scene graph rebuilds its textures from the images' memory copies, which
    // QSG_TRANSIENT_IMAGES dropped — so the images are loaded again (Theme.imageEpoch changes their URLs).
    // Theme is looked up then: it may not be made yet here.
    if (window && qgetenv("QSG_TRANSIENT_IMAGES") == "1") {
        QObject::connect(window, &QQuickWindow::sceneGraphInvalidated, &engine, [&engine] {
            if (QObject *theme = engine.singletonInstance<QObject *>("Everplay", "Theme"))
                theme->setProperty("imageEpoch", theme->property("imageEpoch").toInt() + 1);
        }, Qt::QueuedConnection);
    }

    // Diagnostics (EVERPLAY_PERF_DEBUG=1): reports when the GUI thread was blocked — audio samples go
    // through it, so a stall over ~200 ms is heard as a gap.
    QTimer stallProbe;
    QElapsedTimer stallClock;
    // EVERPLAY_FPS_DEBUG=1: frames drawn per second (only seconds with frames: an idle window draws none),
    // next to the refresh rate of the screen the window is on.
    QTimer fpsTimer;
    if (qEnvironmentVariableIsSet("EVERPLAY_FPS_DEBUG") && window) {
        auto frames = std::make_shared<std::atomic<int>>(0);   // frameSwapped comes from the render thread
        QObject::connect(window, &QQuickWindow::frameSwapped, window, [frames] { ++*frames; }, Qt::DirectConnection);
        fpsTimer.setInterval(1000);
        QObject::connect(&fpsTimer, &QTimer::timeout, window, [window, frames] {
            const int n = frames->exchange(0);
            if (n > 0)
                qInfo("fps: %d (screen %s, %.0f Hz)", n, qPrintable(window->screen()->name()), window->screen()->refreshRate());
        });
        fpsTimer.start();
    }
    if (perfDebug) {
        stallProbe.setInterval(10);
        stallProbe.setTimerType(Qt::PreciseTimer);
        QObject::connect(&stallProbe, &QTimer::timeout, [&stallClock] {
            const qint64 gap = stallClock.restart();
            if (gap > 60)
                qInfo("perf: [%.1f s] GUI thread blocked for %lld ms", QDateTime::currentMSecsSinceEpoch() % 100000 / 1000.0, gap);
        });
        stallClock.start();
        stallProbe.start();
    }
    return app.exec();
}
