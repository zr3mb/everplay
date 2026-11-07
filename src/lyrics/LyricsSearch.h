#pragma once

#include "Lyrics.h"

#include <QNetworkAccessManager>
#include <QObject>

#include <functional>

// Lyrics from the web, for tracks YouTube Music, the file and LRCLIB have none for — before anyone has to
// recognize them from the recording. Every source is asked at once, and the best answer wins:
//   1. timed lyrics (LRC) from NetEase Cloud Music, QQ Music and Kugou — many Western and Asian tracks;
//   2. Musixmatch;
//   3. Tekstowo.pl: the largest collection of Polish lyrics;
//   4. Genius;
//   5. lyrics.ovh (an open lookup by artist and title);
//   6. the description of the video on YouTube or of the track on SoundCloud, where artists often put the lyrics.
// A result has to match the artist and the title; the timing of LRC lyrics is kept only when the
// duration matches too (slowed or sped-up uploads get the text alone, for alignment to time).
class LyricsSearch : public QObject
{
    Q_OBJECT

public:
    struct Query
    {
        QString artist;
        QString title;
        int durationSec = 0;
        QString videoId;        // YouTube: the description is checked too
        QString soundCloudId;   // "sc:<id>": the SoundCloud description
    };

    explicit LyricsSearch(QObject *parent = nullptr);

    // Empty lyrics = nothing found.
    void fetch(const Query &query, std::function<void(const Lyrics &)> done);

    // The block of lyrics in a description, "" when it has none (links, credits and hashtags aren't lyrics).
    static QString lyricsInDescription(const QString &description);
    // "Title (slowed + reverb) [33Hz] (Official Video)" -> "Title".
    static QString cleanTitle(const QString &title);
    // Lower case without diacritics, letters and digits separated by single spaces.
    static QString simple(const QString &text);

private:
    struct Search;

    void get(const QUrl &url, std::function<void(const QByteArray &body, int status)> done,
             const QByteArray &post = {}, const QList<std::pair<QByteArray, QByteArray>> &headers = {});
    void netEase(const QString &artist, const QString &title, int durationSec, std::function<void(const Lyrics &)> done);
    void qqMusic(const QString &artist, const QString &title, int durationSec, std::function<void(const Lyrics &)> done);
    void kugou(const QString &artist, const QString &title, int durationSec, std::function<void(const Lyrics &)> done);
    void musixmatch(const QString &artist, const QString &title, std::function<void(const Lyrics &)> done);
    void tekstowo(const QString &artist, const QString &title, std::function<void(const Lyrics &)> done);
    void genius(const QString &artist, const QString &title, std::function<void(const Lyrics &)> done);
    void lyricsOvh(const QString &artist, const QString &title, std::function<void(const Lyrics &)> done);
    void youTubeDescription(const QString &videoId, std::function<void(const Lyrics &)> done);

    QNetworkAccessManager m_net;
};
