#pragma once

#include "TrackModel.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QStringList>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

class QQmlEngine;
class QJSEngine;

// "Recommended for you" on the home page. The taste comes from what the user listens to (tracks played
// long enough count), what they liked and which artists fill their library; a few tracks that say the
// most about it start YouTube Music radios, and the tracks the library doesn't have yet are the picks.
// The rows are in Library.recommendedRows().
class Recommendations : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(QStringList basedOn READ basedOn NOTIFY changed)   // the artists the picks started from
    // Tracks to start a radio from on the home page: [{ row, title, artist, cover }] (rows in Library.tracks).
    Q_PROPERTY(QVariantList radioSeeds READ radioSeeds NOTIFY changed)

public:
    static Recommendations *instance();
    static Recommendations *create(QQmlEngine *, QJSEngine *);

    bool loading() const { return m_pending > 0; }
    QStringList basedOn() const { return m_basedOn; }

    QVariantList radioSeeds() const;
    // A pick by an artist the user hasn't heard or saved yet ("New artists for you").
    Q_INVOKABLE bool isNewArtist(const QString &trackId) const { return m_fresh.contains(trackId); }
    // Ids and song keys ("artist|title", looseKey) of what was listened to after `sinceMs`.
    QSet<QString> heardSince(qint64 sinceMs) const;

    Q_INVOKABLE void refresh();
    // The track has played long enough to count as listened to (PlayerController).
    void notePlayed(const Track &t);

    static QString looseKey(const QString &text);   // lower case, letters and digits only

signals:
    void changed();

private:
    explicit Recommendations(QObject *parent = nullptr);

    struct Play { QString id, artist, title; int duration = 0; int count = 0; qint64 last = 0; };
    struct Seed { QString artist, title, videoId; int duration = 0; };

    QList<Seed> pickSeeds() const;
    void pickRadioSeeds();
    void startSeed(const Seed &seed, int slot, quint64 serial);
    void finishSeed(int slot, const QList<Track> &radio, quint64 serial);
    void publish();
    void maybeRefresh();
    void load();
    void save() const;

    QHash<QString, Play> m_plays;     // track id -> how often and when it was listened to
    QList<QList<Track>> m_radios;     // per seed, while loading
    QStringList m_seedArtists;
    QList<Track> m_picks;
    QStringList m_basedOn;
    QSet<QString> m_fresh;            // picks by artists new to the user
    QHash<QString, qint64> m_shown;   // song key -> when it was first offered
    QStringList m_radioSeeds;         // track ids
    int m_playsSinceRefresh = 0;
    QDateTime m_generated;
    int m_pending = 0;
    quint64 m_serial = 0;
    QElapsedTimer m_lastAttempt;
    QTimer m_saveTimer;
    QTimer m_retryTimer;
};
