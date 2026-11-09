#include <QScreen>
#include <atomic>
#include <memory>
#include "AppPaths.h"
#include "Language.h"
#include "Library.h"
#include "online/OnlineService.h"
#include "platform/GameOverlay.h"

#include <QAtomicInt>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QQmlApplicationEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

#ifdef Q_OS_ANDROID
#  include "platform/AndroidSupport.h"
#  include <QtWebView/QtWebView>
#endif

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

#ifdef Q_OS_ANDROID
    QtWebView::initialize();   // the in-app sign-in page (before the application object)
#endif
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

    engine.loadFromModule("Everplay", "Main");
    const bool perfDebug = qEnvironmentVariableIsSet("EVERPLAY_PERF_DEBUG");
    if (perfDebug)
        qInfo("perf: the interface was built %lld ms after start", startClock.elapsed());
    QObject::connect(GameOverlay::instance(), &GameOverlay::linkReceived, OnlineService::instance(), &OnlineService::openLink);
    if (!link.isEmpty())
        QTimer::singleShot(1200, OnlineService::instance(), [link] { OnlineService::instance()->openLink(link); });
#ifdef Q_OS_ANDROID
    // everplay:// links from a browser or a chat app come as intents, through QDesktopServices.
    QDesktopServices::setUrlHandler(QStringLiteral("everplay"), OnlineService::instance(), "openUrl");
    // The local library: the system Music folder once the storage permission is granted.
    AndroidSupport::requestAudioPermission([](bool granted) {
        if (!granted)
            return;
        Library *library = Library::instance();
        if (library->folders().isEmpty())
            library->addFolder(QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::MusicLocation)));
        else
            library->rescan(false);
    });
#endif
    if (overlayRequest)
        QTimer::singleShot(1500, GameOverlay::instance(), &GameOverlay::show);
    else if (lyricsOverlayRequest)
        QTimer::singleShot(1500, GameOverlay::instance(), &GameOverlay::showLyrics);

    // After a GPU reset the scene graph rebuilds its textures from the images' memory copies, which
    // QSG_TRANSIENT_IMAGES dropped — so the images are loaded again (Theme.imageEpoch changes their URLs).
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (QObject *theme = engine.singletonInstance<QObject *>("Everplay", "Theme");
        window && theme && qgetenv("QSG_TRANSIENT_IMAGES") == "1") {
        QObject::connect(window, &QQuickWindow::sceneGraphInvalidated, theme, [theme] {
            theme->setProperty("imageEpoch", theme->property("imageEpoch").toInt() + 1);
        }, Qt::QueuedConnection);
    }

    // Diagnostics (EVERPLAY_PERF_DEBUG=1): reports when the GUI thread was blocked — audio samples go
    // through it, so a stall over ~200 ms is heard as a gap.
    QTimer stallProbe;
    QElapsedTimer stallClock;
    if (perfDebug && window) {
        auto *once = new QMetaObject::Connection;
        *once = QObject::connect(window, &QQuickWindow::frameSwapped, window, [once, startClock] {
            qInfo("perf: first frame on screen %lld ms after start", startClock.elapsed());
            QObject::disconnect(*once);
            delete once;
        });
    }
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
                qInfo("perf: GUI thread blocked for %lld ms", gap);
        });
        stallClock.start();
        stallProbe.start();
    }
    return app.exec();
}
