#pragma once

#include "TrackModel.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <functional>

class QQmlEngine;
class QJSEngine;

// A track's radio: tracks like it, for "Start radio" and for what plays after the queue ends
// (PlayerController). YouTube Music's radio of the track — found by search for local files and Spotify
// tracks — or SoundCloud's related tracks for a SoundCloud one. Songs heard in the last days go to the
// end, so a radio brings something new first.
class Radio : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)

public:
    using Done = std::function<void(const QStringList &ids, const QString &error)>;

    static Radio *instance();
    static Radio *create(QQmlEngine *, QJSEngine *);

    bool loading() const { return m_pending > 0; }
    // The station of `seed` (without the seed itself). `avoid`: ids that mustn't come again (the queue so far).
    void tracksFor(const Track &seed, const QStringList &avoid, Done done);
    QStringList stationIds(const QString &seedId) const { return m_stations.value(seedId); }

signals:
    void loadingChanged();

private:
    explicit Radio(QObject *parent = nullptr);
    void youTubeRadio(const Track &seed, const QString &videoId, const QStringList &avoid, Done done);
    void finish(const Track &seed, QList<Track> tracks, const QStringList &avoid, Done done);
    void begin();
    void end();

    QHash<QString, QStringList> m_stations;   // seed id -> station
    int m_pending = 0;
};
