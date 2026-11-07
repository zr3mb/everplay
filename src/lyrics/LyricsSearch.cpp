#include "LyricsSearch.h"

#include "LrcLib.h"
#include "soundcloud/SoundCloudService.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace {

// The lyrics sites answer browsers; an honest but browser-like agent keeps them from serving a stub.
const QByteArray kBrowserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                                 "Chrome/130.0 Safari/537.36";
constexpr int kTimeoutMs = 15000;

QString sourceLabel(const QString &name)
{
    return QCoreApplication::translate("LyricsService", "Source: %1").arg(name);
}

// The inner HTML of the element whose opening tag contains `at`, nested elements of the same tag included.
QString innerElement(const QString &html, qsizetype at, const QString &tag = QStringLiteral("div"))
{
    const qsizetype open = html.lastIndexOf(QChar('<') + tag, at);
    const qsizetype contentStart = open < 0 ? -1 : html.indexOf(QChar('>'), at);
    if (contentStart < 0)
        return {};
    const QString openTag = QChar('<') + tag, closeTag = QStringLiteral("</") + tag + QChar('>');
    int depth = 1;
    qsizetype pos = contentStart + 1;
    while (depth > 0) {
        const qsizetype nextOpen = html.indexOf(openTag, pos);
        const qsizetype nextClose = html.indexOf(closeTag, pos);
        if (nextClose < 0)
            return {};
        if (nextOpen >= 0 && nextOpen < nextClose) {
            ++depth;
            pos = nextOpen + openTag.size();
        } else {
            if (--depth == 0)
                return html.mid(contentStart + 1, nextClose - contentStart - 1);
            pos = nextClose + closeTag.size();
        }
    }
    return {};
}

QString decodeEntities(QString text)
{
    static const QRegularExpression numeric(QStringLiteral("&#(x?)([0-9a-fA-F]+);"));
    QString out;
    qsizetype last = 0;
    for (auto it = numeric.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        bool ok = false;
        const uint code = m.captured(2).toUInt(&ok, m.captured(1).isEmpty() ? 10 : 16);
        if (ok)
            out += QString::fromUcs4(reinterpret_cast<const char32_t *>(&code), 1);
        last = m.capturedEnd();
    }
    out += text.mid(last);
    out.replace(QLatin1String("&quot;"), QLatin1String("\""))
       .replace(QLatin1String("&apos;"), QLatin1String("'"))
       .replace(QLatin1String("&lt;"), QLatin1String("<"))
       .replace(QLatin1String("&gt;"), QLatin1String(">"))
       .replace(QLatin1String("&nbsp;"), QLatin1String(" "))
       .replace(QLatin1String("&amp;"), QLatin1String("&"));
    return out;
}

QString htmlToText(QString html)
{
    static const QRegularExpression br(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags(QStringLiteral("<[^>]+>"));
    static const QRegularExpression sourceBreaks(QStringLiteral("[\\r\\n]+\\s*"));
    html.replace(sourceBreaks, QStringLiteral(" "));   // line breaks in the page source mean nothing in HTML
    html.replace(br, QStringLiteral("\n"));
    html.remove(tags);
    return decodeEntities(html);
}

// A line that names a part of the song instead of being sung: "[Chorus]", "Refren:", "Zwrotka 2: <rapper>".
bool isSectionHeader(const QString &line)
{
    static const QRegularExpression bracketed(QStringLiteral("^\\s*\\[[^\\]]*\\]\\s*$"));
    static const QRegularExpression named(
        QStringLiteral("^\\s*[\\[\\(]?\\s*(ref(ren)?|zwrotka|chorus|verse|hook|intro|outro|bridge|pre-?chorus|"
                       "post-?chorus|interlude|tekst|lyrics|słowa)\\b\\s*\\d*\\s*[\\]\\)]?\\s*([:：-]\\s*.{0,40})?$"),
        QRegularExpression::CaseInsensitiveOption);
    return bracketed.match(line).hasMatch() || named.match(line).hasMatch();
}

// Lyrics from plain text: section headers become verse breaks.
Lyrics textToLyrics(const QString &text, const QString &source)
{
    QStringList lines;
    for (const QString &line : text.split(QChar('\n')))
        lines << (isSectionHeader(line) ? QString() : line);
    Lyrics l = Lyrics::fromPlainText(lines.join(QChar('\n')));
    if (l.wordCount() < 12)
        return {};
    l.source = source;
    l.origin = QStringLiteral("web");
    return l;
}

bool sameTitle(const QString &found, const QString &wanted)
{
    const QString a = LyricsSearch::simple(LyricsSearch::cleanTitle(found));
    const QString b = LyricsSearch::simple(wanted);
    if (a.isEmpty() || b.isEmpty())
        return false;
    return a == b || (b.size() >= 4 && a.startsWith(b)) || (a.size() >= 4 && b.startsWith(a))
           || (b.size() >= 8 && a.contains(b));
}

bool sameArtist(const QString &found, const QString &wanted)
{
    const QString a = QChar(' ') + LyricsSearch::simple(found) + QChar(' ');
    const QString b = LyricsSearch::simple(wanted);
    if (b.isEmpty())
        return false;
    // The whole name, or its first word when it's distinctive enough ("Diho RaZ" for "Diho").
    const QString first = b.section(QChar(' '), 0, 0);
    return a.contains(QChar(' ') + b + QChar(' ')) || (first.size() >= 4 && a.contains(QChar(' ') + first + QChar(' ')));
}

} // namespace

struct LyricsSearch::Search
{
    std::function<void(const Lyrics &)> done;
    int pending = 0;
    bool finished = false;
    Lyrics best;
    int bestRank = 1 << 30;

    void offer(int rank, const Lyrics &l)
    {
        if (!l.isEmpty() && rank < bestRank) {
            best = l;
            bestRank = rank;
        }
    }
    void finish()
    {
        if (finished)
            return;
        finished = true;
        done(best);
    }
};

LyricsSearch::LyricsSearch(QObject *parent)
    : QObject(parent)
{
}

QString LyricsSearch::simple(const QString &text)
{
    const QString decomposed = text.toLower().normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c == QChar(0x0142))   // ł doesn't decompose
            out += QChar('l');
        else if (c.isLetterOrNumber())
            out += c;
        else if (c.category() != QChar::Mark_NonSpacing)
            out += QChar(' ');
    }
    return out.simplified();
}

QString LyricsSearch::cleanTitle(const QString &title)
{
    // Re-uploads: slowed, sped up, rebassed for car audio ("[33Hz]"), bass boosted, extended mixes.
    static const QRegularExpression brackets(
        QStringLiteral("\\s*[\\(\\[][^\\)\\]]*(slowed|reverb|sped|nightcore|bass|boost|rebass|hz|extended|radio edit|"
                       "\\bhq\\b|\\b8d\\b|remix|rmx|version|wersja|lyrics?|tekst|teledysk|prod|official|video|audio|"
                       "clip|visuali[sz]er|explicit|premiere|premiera)[^\\)\\]]*[\\)\\]]"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tail(
        QStringLiteral("\\s*(\\||//|\\s/\\s|\\bprod\\.|\\bslowed\\b|\\bsped up\\b|\\bbass ?boosted\\b|\\brebass(ed)?\\b|\\bnightcore\\b|#).*$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression hertz(QStringLiteral("\\b\\d+(\\s*-\\s*\\d+)*\\s*hz\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression number(QStringLiteral("^\\d{1,2}\\.\\s+"));
    QString t = title;
    t.remove(brackets);
    t.remove(tail);
    t.remove(hertz);
    t.remove(number);
    t = LrcLib::cleanTitle(t).simplified();
    while (t.endsWith(QChar('-')) || t.endsWith(QChar('"')))
        t.chop(1);
    while (t.startsWith(QChar('"')))
        t.remove(0, 1);
    t = t.trimmed();
    return t.isEmpty() ? title.trimmed() : t;
}

QString LyricsSearch::lyricsInDescription(const QString &description)
{
    // A line of lyrics: short, without links, handles, hashtags, timestamps or "Credit: name".
    static const QRegularExpression link(QStringLiteral("https?://|www\\.|@\\w|#\\w|©|℗|^\\d{1,2}:\\d{2}|^\\d+[.)]\\s"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression credit(QStringLiteral("^[^:：]{1,40}[:：]"));

    QString text = description;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    const QStringList lines = text.split(QChar('\n'));

    QStringList best, current;
    int bestCount = 0, count = 0;
    auto close = [&] {
        while (!current.isEmpty() && current.last().isEmpty())
            current.removeLast();
        if (count > bestCount) {
            best = current;
            bestCount = count;
        }
        current.clear();
        count = 0;
    };
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || isSectionHeader(line)) {
            if (!current.isEmpty() && !current.last().isEmpty())
                current << QString();   // a verse break inside the block
            continue;
        }
        const bool lyric = line.size() <= 90 && !link.match(line).hasMatch() && !credit.match(line).hasMatch()
                           && line.count(QChar(' ')) < 18;
        if (!lyric) {
            close();
            continue;
        }
        current << line;
        ++count;
    }
    close();
    if (bestCount < 10)
        return {};
    int words = 0;
    for (const QString &l : std::as_const(best))
        words += int(l.split(QChar(' '), Qt::SkipEmptyParts).size());
    return words >= 40 ? best.join(QChar('\n')) : QString();
}

void LyricsSearch::get(const QUrl &url, std::function<void(const QByteArray &, int)> done, const QByteArray &post,
                       const QList<std::pair<QByteArray, QByteArray>> &headers)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kBrowserAgent);
    request.setTransferTimeout(10000);
    for (const auto &[name, value] : headers)
        request.setRawHeader(name, value);
    QNetworkReply *reply;
    if (post.isNull()) {
        reply = m_net.get(request);
    } else {
        request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
        reply = m_net.post(request, post);
    }
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        reply->deleteLater();
        done(reply->readAll(), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
    });
}

void LyricsSearch::fetch(const Query &query, std::function<void(const Lyrics &)> done)
{
    // What to look for: a "Artist - Title" upload title first (the channel is often not the artist),
    // then the track's own artist and title.
    QList<std::pair<QString, QString>> candidates;
    auto add = [&candidates](QString artist, const QString &title) {
        static const QRegularExpression brackets(QStringLiteral("[\\(\\[][^\\)\\]]*[\\)\\]]"));
        artist = LrcLib::firstArtist(artist.remove(brackets).remove(QStringLiteral(" - Topic")).simplified());
        const QString name = cleanTitle(title);
        if (simple(artist).isEmpty() || simple(name).isEmpty())
            return;
        const std::pair<QString, QString> c { artist, name };
        if (!candidates.contains(c))
            candidates << c;
    };
    static const QRegularExpression dash(QStringLiteral("\\s[-–—]\\s"));
    static const QRegularExpression number(QStringLiteral("^\\d{1,2}\\.\\s+"));
    QString title = query.title;
    title.remove(number);
    if (const auto m = dash.match(title); m.hasMatch())
        add(title.left(m.capturedStart()), title.mid(m.capturedEnd()));
    add(query.artist, title);

    auto search = std::make_shared<Search>();
    search->done = std::move(done);
    auto collect = [search](int rank) {
        return [search, rank](const Lyrics &l) {
            search->offer(rank, l);
            if (--search->pending == 0)
                search->finish();
        };
    };
    // Ranks: timed lyrics first, then the sites by how reliable their texts are; a description last.
    for (int c = 0; c < candidates.size(); ++c) {
        const auto &[artist, name] = candidates.at(c);
        search->pending += 4;
        netEase(artist, name, query.durationSec, [search, c](const Lyrics &l) {
            search->offer((l.synced ? 0 : 30) + c, l);
            if (--search->pending == 0)
                search->finish();
        });
        tekstowo(artist, name, collect(10 + c));
        genius(artist, name, collect(20 + c));
        lyricsOvh(artist, name, collect(40 + c));
    }
    if (!query.videoId.isEmpty()) {
        ++search->pending;
        youTubeDescription(query.videoId, collect(50));
    }
    if (!query.soundCloudId.isEmpty()) {
        ++search->pending;
        auto finish = collect(50);
        SoundCloudService::instance()->fetchDescription(query.soundCloudId, [finish](const QString &description) {
            const QString text = lyricsInDescription(description);
            finish(text.isEmpty() ? Lyrics() : textToLyrics(text, QCoreApplication::translate("LyricsService", "Source: the track description on SoundCloud")));
        });
    }
    if (search->pending == 0) {
        search->finish();
        return;
    }
    // A site that hangs doesn't hold the lyrics back.
    QTimer::singleShot(kTimeoutMs, this, [search] { search->finish(); });
}

// ---- sources --------------------------------------------------------------------

void LyricsSearch::netEase(const QString &artist, const QString &title, int durationSec, std::function<void(const Lyrics &)> done)
{
    QUrl url(QStringLiteral("https://music.163.com/api/search/get"));
    url.setQuery(QUrlQuery { { QStringLiteral("s"), artist + QChar(' ') + title }, { QStringLiteral("type"), QStringLiteral("1") },
                             { QStringLiteral("limit"), QStringLiteral("6") } });
    const QList<std::pair<QByteArray, QByteArray>> headers { { "Referer", "https://music.163.com" } };
    get(url, [this, artist, title, durationSec, done, headers](const QByteArray &body, int status) {
        if (status != 200) {
            done({});
            return;
        }
        for (const QJsonValue &v : QJsonDocument::fromJson(body).object().value("result").toObject().value("songs").toArray()) {
            const QJsonObject song = v.toObject();
            QStringList artists;
            for (const QJsonValue &a : song.value("artists").toArray())
                artists << a.toObject().value("name").toString();
            if (!sameTitle(song.value("name").toString(), title) || !sameArtist(artists.join(QChar(' ')), artist))
                continue;
            const double songSec = song.value("duration").toDouble() / 1000.0;
            const bool sameCut = durationSec > 0 && std::abs(songSec - durationSec) <= 3;
            QUrl lyricUrl(QStringLiteral("https://music.163.com/api/song/lyric"));
            lyricUrl.setQuery(QUrlQuery { { QStringLiteral("id"), QString::number(qint64(song.value("id").toDouble())) },
                                          { QStringLiteral("lv"), QStringLiteral("1") } });
            get(lyricUrl, [done, sameCut](const QByteArray &body, int status) {
                const QString lrc = status == 200 ? QJsonDocument::fromJson(body).object().value("lrc").toObject().value("lyric").toString()
                                                  : QString();
                // "纯音乐" = an instrumental; the lines before the lyrics credit the writers ("作词 : …").
                if (lrc.contains(QStringLiteral("纯音乐"))) {
                    done({});
                    return;
                }
                static const QRegularExpression credits(
                    QStringLiteral("^\\[[^\\]]*\\]\\s*(作词|作曲|编曲|制作人|混音|母带|和声|吉他|贝斯|鼓|录音|监制|出品|词|曲|"
                                   "producer|composer|lyricist|written by|mixed|mastered|arranged)[^:：]*[:：].*$"),
                    QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
                QString cleaned = lrc;
                cleaned.remove(credits);
                Lyrics l = Lyrics::fromLrc(cleaned);
                if (l.wordCount() < 12) {
                    done({});
                    return;
                }
                if (!sameCut || !l.synced) {
                    // Another cut of the track: its timing would be wrong, the text is still right.
                    for (LyricLine &line : l.lines)
                        line.start = line.end = -1;
                    l.synced = false;
                }
                l.source = sourceLabel(QStringLiteral("NetEase Cloud Music"));
                l.origin = QStringLiteral("web");
                done(l);
            }, {}, { { "Referer", "https://music.163.com" } });
            return;
        }
        done({});
    }, {}, headers);
}

void LyricsSearch::tekstowo(const QString &artist, const QString &title, std::function<void(const Lyrics &)> done)
{
    QUrl url(QStringLiteral("https://www.tekstowo.pl/szukaj"));
    url.setQuery(QUrlQuery { { QStringLiteral("wykonawca"), artist }, { QStringLiteral("tytul"), title } });
    get(url, [this, artist, title, done](const QByteArray &body, int status) {
        if (status != 200) {
            done({});
            return;
        }
        // Songs are at /<artist-slug>/<title-slug>.
        const QString artistSlug = simple(artist).replace(QChar(' '), QChar('-'));
        const QString titleSlug = simple(title).replace(QChar(' '), QChar('-'));
        static const QRegularExpression link(QStringLiteral("href=\"/([a-z0-9-]+)/([a-z0-9-]+)\""));
        QString path;
        for (auto it = link.globalMatch(QString::fromUtf8(body)); it.hasNext() && path.isEmpty();) {
            const auto m = it.next();
            if (m.captured(1).startsWith(artistSlug.left(8)) && m.captured(2).contains(titleSlug.left(12)))
                path = m.captured(0).mid(6).chopped(1);
        }
        if (path.isEmpty()) {
            done({});
            return;
        }
        get(QUrl(QStringLiteral("https://www.tekstowo.pl") + path), [done](const QByteArray &body, int status) {
            const QString page = QString::fromUtf8(body);
            const qsizetype at = status == 200 ? page.indexOf(QLatin1String("class=\"inner-text\"")) : -1;
            done(at < 0 ? Lyrics() : textToLyrics(htmlToText(innerElement(page, at)), sourceLabel(QStringLiteral("Tekstowo.pl"))));
        });
    });
}

void LyricsSearch::genius(const QString &artist, const QString &title, std::function<void(const Lyrics &)> done)
{
    QUrl url(QStringLiteral("https://genius.com/api/search/multi"));
    url.setQuery(QUrlQuery { { QStringLiteral("per_page"), QStringLiteral("5") }, { QStringLiteral("q"), artist + QChar(' ') + title } });
    get(url, [this, artist, title, done](const QByteArray &body, int status) {
        QString page;
        if (status == 200) {
            const QJsonArray sections = QJsonDocument::fromJson(body).object().value("response").toObject().value("sections").toArray();
            for (const QJsonValue &s : sections) {
                if (s.toObject().value("type").toString() != QLatin1String("song") || !page.isEmpty())
                    continue;
                for (const QJsonValue &h : s.toObject().value("hits").toArray()) {
                    const QJsonObject r = h.toObject().value("result").toObject();
                    const QString artists = r.value("primary_artist").toObject().value("name").toString() + QChar(' ')
                                            + r.value("artist_names").toString();
                    if (sameTitle(r.value("title").toString(), title) && sameArtist(artists, artist)) {
                        page = r.value("url").toString();
                        break;
                    }
                }
            }
        }
        if (page.isEmpty()) {
            done({});
            return;
        }
        get(QUrl(page), [done](const QByteArray &body, int status) {
            if (status != 200) {
                done({});
                return;
            }
            // The lyrics come in several containers; headers and ads inside them are marked to be left out.
            const QString html = QString::fromUtf8(body);
            QStringList parts;
            const QString marker = QStringLiteral("data-lyrics-container=\"true\"");
            for (qsizetype at = html.indexOf(marker); at >= 0; at = html.indexOf(marker, at + marker.size())) {
                QString inner = innerElement(html, at);
                const QString exclude = QStringLiteral("data-exclude-from-selection=\"true\"");
                for (qsizetype x = inner.indexOf(exclude); x >= 0; x = inner.indexOf(exclude)) {
                    const qsizetype open = inner.lastIndexOf(QStringLiteral("<div"), x);
                    const qsizetype contentStart = inner.indexOf(QChar('>'), x) + 1;
                    const qsizetype end = contentStart + innerElement(inner, x).size() + 6;   // + "</div>"
                    if (open < 0 || contentStart <= 0 || end > inner.size())
                        break;
                    inner.remove(open, end - open);
                }
                parts << htmlToText(inner);
            }
            done(textToLyrics(parts.join(QChar('\n')), sourceLabel(QStringLiteral("Genius"))));
        });
    });
}

void LyricsSearch::lyricsOvh(const QString &artist, const QString &title, std::function<void(const Lyrics &)> done)
{
    const QUrl url(QStringLiteral("https://api.lyrics.ovh/v1/") + QString::fromLatin1(QUrl::toPercentEncoding(artist))
                   + QChar('/') + QString::fromLatin1(QUrl::toPercentEncoding(title)));
    get(url, [done](const QByteArray &body, int status) {
        QString text = status == 200 ? QJsonDocument::fromJson(body).object().value("lyrics").toString() : QString();
        // Some answers start with a French title line ("Paroles de la chanson … par …").
        if (text.startsWith(QLatin1String("Paroles de la chanson")))
            text = text.section(QChar('\n'), 1);
        done(textToLyrics(text, sourceLabel(QStringLiteral("lyrics.ovh"))));
    });
}

void LyricsSearch::youTubeDescription(const QString &videoId, std::function<void(const Lyrics &)> done)
{
    // The YouTube Music client doesn't return the description; the website's player request does.
    const QJsonObject body {
        { "context", QJsonObject { { "client", QJsonObject { { "clientName", "WEB" }, { "clientVersion", "2.20251101.00.00" },
                                                             { "hl", "en" } } } } },
        { "videoId", videoId },
    };
    get(QUrl(QStringLiteral("https://www.youtube.com/youtubei/v1/player?prettyPrint=false")),
        [done](const QByteArray &body, int status) {
        const QString description = status == 200
            ? QJsonDocument::fromJson(body).object().value("videoDetails").toObject().value("shortDescription").toString()
            : QString();
        const QString text = lyricsInDescription(description);
        done(text.isEmpty() ? Lyrics()
                            : textToLyrics(text, QCoreApplication::translate("LyricsService", "Source: the video description on YouTube")));
    }, QJsonDocument(body).toJson(QJsonDocument::Compact));
}
