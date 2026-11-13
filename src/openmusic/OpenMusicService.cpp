#include "OpenMusicService.h"

#include "AppPaths.h"
#include "LibraryScanner.h"
#include "TagLibPath.h"
#include "youtube/YouTubeService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>
#include <memory>

namespace {

const QByteArray kUserAgent = QByteArrayLiteral("Everplay/" EVERPLAY_VERSION " (desktop music player)");

// What is searched on the Internet Archive: items with FLAC files from the Live Music Archive (taped
// with the bands' permission), netlabels, and community uploads that state a free licence.
const char *kArchiveScope =
    "format:Flac AND (mediatype:etree OR (mediatype:audio AND (collection:netlabels"
    " OR (collection:(opensource_audio OR audio_music) AND licenseurl:*))))";

constexpr int kAlbumResults = 18;
constexpr int kItemsWithTracks = 5;     // search results whose files are listed as tracks
constexpr int kTracksPerItem = 20;      // of an item matched by its name, not by the track's title
constexpr int kTrackResults = 80;
constexpr int kItemTrackLimit = 300;

QString stateFile() { return AppPaths::dataDir() + QStringLiteral("/open-music.json"); }

// Lowercase, without accents: "Frédéric" is found by "frederic".
QString folded(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_D).toLower();
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed)
        if (c.category() != QChar::Mark_NonSpacing)
            out += c;
    return out;
}

QStringList wordsOf(const QString &query)
{
    static const QRegularExpression split(QStringLiteral("[^\\w]+"), QRegularExpression::UseUnicodePropertiesOption);
    return folded(query).split(split, Qt::SkipEmptyParts);
}

bool hasAll(const QString &text, const QStringList &words)
{
    if (words.isEmpty())
        return false;
    const QString f = folded(text);
    return std::all_of(words.cbegin(), words.cend(), [&](const QString &w) { return f.contains(w); });
}

// The query as plain words for the Internet Archive's Lucene syntax.
QString luceneWords(const QString &query)
{
    static const QRegularExpression special(QStringLiteral("[+\\-&|!(){}\\[\\]^\"~*?:\\\\/]"));
    return QString(query).replace(special, QStringLiteral(" ")).simplified();
}

// A field that is either a string or a list of strings.
QString textOf(const QJsonValue &v)
{
    if (v.isArray()) {
        QStringList parts;
        for (const QJsonValue &p : v.toArray())
            if (const QString s = p.toString().trimmed(); !s.isEmpty())
                parts << s;
        return parts.join(QStringLiteral(", "));
    }
    if (v.isDouble())
        return QString::number(v.toDouble());
    return v.toString().trimmed();
}

int yearOf(const QString &date)
{
    static const QRegularExpression year(QStringLiteral("\\b(1[89]\\d\\d|20\\d\\d)\\b"));
    const auto m = year.match(date);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

// "323.81", "05:23" or "1:05:23" -> seconds
int lengthOf(const QString &text)
{
    if (!text.contains(QChar(':')))
        return int(text.toDouble());
    int seconds = 0;
    for (const QString &part : text.split(QChar(':')))
        seconds = seconds * 60 + int(part.toDouble());
    return seconds;
}

QString stripHtml(QString html)
{
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    html.replace(tags, QStringLiteral(" "));
    html.replace(QLatin1String("&amp;"), QLatin1String("&")).replace(QLatin1String("&quot;"), QLatin1String("\""))
        .replace(QLatin1String("&#039;"), QLatin1String("'")).replace(QLatin1String("&nbsp;"), QLatin1String(" "))
        .replace(QLatin1String("&lt;"), QLatin1String("<")).replace(QLatin1String("&gt;"), QLatin1String(">"));
    return html.simplified();
}

// A readable name for a licence URL ("…/licenses/by-nc-sa/3.0/us/" -> "CC BY-NC-SA 3.0").
QString licenseName(const QString &url)
{
    if (url.isEmpty())
        return {};
    if (url.contains(QLatin1String("publicdomain/zero")))
        return QStringLiteral("CC0");
    if (url.contains(QLatin1String("publicdomain")))
        return QCoreApplication::translate("OpenMusicService", "Public domain");
    static const QRegularExpression cc(QStringLiteral("licenses/([a-z\\-]+)/([0-9.]+)"));
    if (const auto m = cc.match(url); m.hasMatch())
        return QStringLiteral("CC ") + m.captured(1).toUpper() + QChar(' ') + m.captured(2);
    return url;
}

QString encodedPath(const QString &name)
{
    QStringList parts;
    for (const QString &part : name.split(QChar('/')))
        parts << QString::fromLatin1(QUrl::toPercentEncoding(part));
    return parts.join(QChar('/'));
}

// A file or folder name that works on every system.
QString safeName(QString name, int max = 80)
{
    static const QRegularExpression bad(QStringLiteral("[<>:\"/\\\\|?*\\x00-\\x1f]"));
    name.replace(bad, QStringLiteral("_"));
    name = name.simplified().left(max);
    while (name.endsWith(QChar('.')) || name.endsWith(QChar(' ')))
        name.chop(1);
    return name.isEmpty() ? QStringLiteral("_") : name;
}

// Tags the saved file is missing (Wikimedia Commons files often have none) come from the source's data;
// tags the file already has stay. Runs on a worker thread.
void fillTags(const QString &path, const QJsonObject &tags)
{
    TagLib::FileRef file(TagLibPath(path).get());
    if (file.isNull() || !file.file())
        return;
    TagLib::PropertyMap props = file.file()->properties();
    bool changed = false;
    for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
        const QString value = it.value().toVariant().toString();
        const TagLib::String key(it.key().toStdString(), TagLib::String::UTF8);
        if (value.isEmpty() || value == QLatin1String("0") || (props.contains(key) && !props[key].isEmpty()))
            continue;
        props.replace(key, TagLib::StringList(TagLib::String(value.toStdString(), TagLib::String::UTF8)));
        changed = true;
    }
    if (changed) {
        file.file()->setProperties(props);
        file.save();
    }
}

} // namespace

OpenMusicService *OpenMusicService::instance()
{
    static OpenMusicService *inst = new OpenMusicService;
    return inst;
}

OpenMusicService *OpenMusicService::create(QQmlEngine *, QJSEngine *)
{
    OpenMusicService *s = instance();
    QJSEngine::setObjectOwnership(s, QJSEngine::CppOwnership);
    return s;
}

bool OpenMusicService::isOpenSource(const QString &source)
{
    return source == QLatin1String("archive") || source == QLatin1String("commons");
}

OpenMusicService::OpenMusicService(QObject *parent)
    : QObject(parent)
{
    auto *settings = AppPaths::settings();
    m_enabled = settings->value("openmusic/enabled", false).toString() == QLatin1String("true");
    m_archive = settings->value("openmusic/archive", true).toString() != QLatin1String("false");
    m_commons = settings->value("openmusic/commons", true).toString() != QLatin1String("false");
    m_folder = settings->value("openmusic/folder").toString();
    m_net.setTransferTimeout(30000);

    // "ia:" / "wc:" streams go through the same resolver (cache, signals) as YouTube's.
    for (const char *prefix : { "ia:", "wc:" }) {
        YouTubeService::instance()->resolver()->setExternal(QLatin1String(prefix),
                                                            [this](const QString &id, StreamResolver::Done done) {
            resolveStream(id, std::move(done));
        });
    }
    connect(qApp, &QCoreApplication::aboutToQuit, this, &OpenMusicService::cancelDownloads);
    loadState();
}

// ---- settings ---------------------------------------------------------------------

void OpenMusicService::setEnabled(bool on)
{
    if (m_enabled == on)
        return;
    m_enabled = on;
    AppPaths::settings()->setValue("openmusic/enabled", on);
    if (!on) {
        ++m_serial;   // a search still running is dropped
        m_pending = 0;
        m_search.clear();
        m_albumResults.clear();
        emit searchingChanged();
        emit albumResultsChanged();
        ++m_resultsRevision;
        emit resultsChanged();
    }
    emit settingsChanged();
}

void OpenMusicService::setArchiveEnabled(bool on)
{
    if (m_archive == on)
        return;
    m_archive = on;
    AppPaths::settings()->setValue("openmusic/archive", on);
    m_query.clear();   // the next search runs again with the new choice
    emit settingsChanged();
}

void OpenMusicService::setCommonsEnabled(bool on)
{
    if (m_commons == on)
        return;
    m_commons = on;
    AppPaths::settings()->setValue("openmusic/commons", on);
    m_query.clear();
    emit settingsChanged();
}

QString OpenMusicService::downloadFolder() const
{
    if (!m_folder.isEmpty())
        return m_folder;
    QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (music.isEmpty())
        music = QDir::homePath();
    return QDir::cleanPath(music + QStringLiteral("/Everplay FLAC"));
}

void OpenMusicService::setDownloadFolder(const QString &folder)
{
    const QString path = folder.startsWith(QLatin1String("file:")) ? QUrl(folder).toLocalFile() : folder;
    m_folder = path.isEmpty() ? QString() : QDir::cleanPath(path);
    AppPaths::settings()->setValue("openmusic/folder", m_folder);
    emit settingsChanged();
}

// ---- network ----------------------------------------------------------------------

void OpenMusicService::getJson(const QUrl &url, JsonCallback callback)
{
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setRawHeader("Accept", "application/json");
    QNetworkReply *reply = m_net.get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError) {
            callback({}, status == 429 ? tr("too many requests — try again in a moment") : reply->errorString());
            return;
        }
        // An item with hundreds of files is a few hundred kB: parsed on a worker thread.
        auto *watcher = new QFutureWatcher<QJsonObject>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [watcher, callback] {
            callback(watcher->result(), {});
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([data = reply->readAll()] {
            return QJsonDocument::fromJson(data).object();
        }));
    });
}

// ---- search -----------------------------------------------------------------------

void OpenMusicService::search(const QString &query)
{
    const QString q = query.simplified();
    if (!m_enabled || q.size() < 2 || (!m_archive && !m_commons))
        return;
    if (q == m_query && m_pending == 0)
        return;   // the same search again: its results are still there
    m_query = q;
    const quint64 serial = ++m_serial;
    m_archiveFound.clear();
    m_commonsFound.clear();
    m_search.clear();
    m_albumResults.clear();
    emit albumResultsChanged();
    m_pending = (m_archive ? 1 : 0) + (m_commons ? 1 : 0);
    emit searchingChanged();
    if (m_archive)
        searchArchive(q, serial);
    if (m_commons)
        searchCommons(q, serial);
}

void OpenMusicService::finishSearch(quint64 serial)
{
    if (serial != m_serial || m_pending == 0)
        return;
    // Both sources' tracks in one list: title matches first, then the artist's, then the album's.
    const QStringList words = wordsOf(m_query);
    struct Entry { int score; int order; QString id; };
    QList<Entry> entries;
    int order = 0;
    for (const QStringList *found : { &m_archiveFound, &m_commonsFound }) {
        for (const QString &id : *found) {
            const Track &t = m_tracks[id];
            const int score = hasAll(t.title, words) ? 3 : hasAll(t.artist, words) ? 2 : hasAll(t.album, words) ? 1 : 0;
            entries << Entry { score, order++, id };
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.score > b.score; });
    m_search.clear();
    for (const Entry &e : std::as_const(entries))
        if (m_search.size() < kTrackResults)
            m_search << e.id;
    publish(m_search);
    --m_pending;
    emit searchingChanged();
}

void OpenMusicService::searchArchiveItems(const QString &fields, int rows,
                                          std::function<void(const QJsonArray &docs, const QString &error)> done)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), QStringLiteral("(%1) AND %2").arg(fields, QLatin1String(kArchiveScope)));
    for (const char *field : { "identifier", "title", "creator", "date", "year", "downloads", "format" })
        q.addQueryItem(QStringLiteral("fl[]"), QLatin1String(field));
    q.addQueryItem(QStringLiteral("rows"), QString::number(rows));
    q.addQueryItem(QStringLiteral("sort[]"), QStringLiteral("downloads desc"));
    q.addQueryItem(QStringLiteral("output"), QStringLiteral("json"));
    QUrl url(QStringLiteral("https://archive.org/advancedsearch.php"));
    url.setQuery(q);
    getJson(url, [done](const QJsonObject &json, const QString &error) {
        done(json.value("response").toObject().value("docs").toArray(), error);
    });
}

void OpenMusicService::searchArchive(const QString &query, quint64 serial)
{
    const QString words = luceneWords(query);
    if (words.isEmpty()) {
        finishSearch(serial);
        return;
    }
    const QString named = QStringLiteral("title:(%1) OR creator:(%1) OR subject:(%1)").arg(words);
    searchArchiveItems(named, 24, [this, words, serial](QJsonArray docs, const QString &error) {
        if (serial != m_serial)
            return;
        if (!error.isEmpty())
            emit notice(tr("Internet Archive: %1").arg(error), QStringLiteral("close"));
        auto withDocs = [this, serial](const QJsonArray &docs) {
            if (serial != m_serial)
                return;
            QStringList itemIds;
            m_albumResults.clear();
            for (const QJsonValue &v : docs) {
                const QJsonObject d = v.toObject();
                const QString id = d.value("identifier").toString();
                if (id.isEmpty() || itemIds.contains(id))
                    continue;
                itemIds << id;
                const QString creator = textOf(d.value("creator"));
                const int year = d.value("year").toInt() > 0 ? d.value("year").toInt() : yearOf(textOf(d.value("date")));
                QStringList subtitle;
                if (!creator.isEmpty())
                    subtitle << creator;
                if (year > 0)
                    subtitle << QString::number(year);
                // Without a picture of its own an item's tile is a drawn waveform: the card draws its own cover then.
                const bool art = textOf(d.value("format")).contains(QLatin1String("JPEG"));
                if (m_albumResults.size() < kAlbumResults)
                    m_albumResults << QVariantMap { { "id", id }, { "title", textOf(d.value("title")) },
                                                    { "subtitle", subtitle.join(QStringLiteral(" · ")) },
                                                    { "cover", art ? QStringLiteral("https://archive.org/services/img/") + id : QString() },
                                                    { "source", "archive" } };
            }
            emit albumResultsChanged();
            // The first few items' files become tracks; the rest show as albums to open.
            const QStringList withTracks = itemIds.mid(0, kItemsWithTracks);
            if (withTracks.isEmpty()) {
                finishSearch(serial);
                return;
            }
            auto left = std::make_shared<int>(int(withTracks.size()));
            for (const QString &itemId : withTracks) {
                loadItem(itemId, [this, serial, left, withTracks](const QStringList &, const QString &) {
                    if (--*left > 0 || serial != m_serial)
                        return;
                    // An item found by its name (the artist, the album) gives its tracks; one found by its
                    // description (a concert whose setlist has the song) only the tracks named like the query.
                    const QStringList words = wordsOf(m_query);
                    for (const QString &item : withTracks) {
                        const QVariantMap info = m_items.value(item);
                        const bool named = hasAll(info.value("creator").toString() + QChar(' ') + info.value("title").toString(), words);
                        int fromItem = 0;
                        for (const QString &id : m_collections.value(item)) {
                            const Track &t = m_tracks[id];
                            const bool titled = hasAll(t.title, words);
                            if (titled || ((named || hasAll(t.artist, words)) && fromItem < kTracksPerItem)) {
                                m_archiveFound << id;
                                fromItem += titled ? 0 : 1;
                            }
                        }
                    }
                    finishSearch(serial);
                });
            }
        };
        if (docs.size() >= 8) {
            withDocs(docs);
            return;
        }
        // Few items carry the words in their name: a song is looked for in the concerts' setlists.
        searchArchiveItems(QStringLiteral("description:(%1)").arg(words), 12,
                           [this, docs, serial, withDocs](const QJsonArray &more, const QString &) mutable {
            if (serial != m_serial)
                return;
            for (const QJsonValue &v : more)
                docs.append(v);
            withDocs(docs);
        });
    });
}

void OpenMusicService::searchCommons(const QString &query, quint64 serial)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("action"), QStringLiteral("query"));
    q.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    q.addQueryItem(QStringLiteral("formatversion"), QStringLiteral("2"));
    q.addQueryItem(QStringLiteral("generator"), QStringLiteral("search"));
    q.addQueryItem(QStringLiteral("gsrnamespace"), QStringLiteral("6"));
    q.addQueryItem(QStringLiteral("gsrsearch"), query + QStringLiteral(" filemime:audio/x-flac"));
    q.addQueryItem(QStringLiteral("gsrlimit"), QStringLiteral("24"));
    q.addQueryItem(QStringLiteral("prop"), QStringLiteral("imageinfo"));
    q.addQueryItem(QStringLiteral("iiprop"), QStringLiteral("url|size|metadata|extmetadata"));
    q.addQueryItem(QStringLiteral("iiextmetadatafilter"), QStringLiteral("Artist|ObjectName|LicenseShortName|DateTimeOriginal"));
    QUrl url(QStringLiteral("https://commons.wikimedia.org/w/api.php"));
    url.setQuery(q);
    getJson(url, [this, serial](const QJsonObject &json, const QString &error) {
        if (serial != m_serial)
            return;
        if (!error.isEmpty())
            emit notice(tr("Wikimedia Commons: %1").arg(error), QStringLiteral("close"));
        QJsonArray pages = json.value("query").toObject().value("pages").toArray();
        QList<QJsonObject> sorted;
        for (const QJsonValue &v : pages)
            sorted << v.toObject();
        std::sort(sorted.begin(), sorted.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value("index").toInt() < b.value("index").toInt();
        });
        static const QRegularExpression code(QStringLiteral("^[A-Z0-9]{2,6}(-[A-Z0-9]{1,4})? - "));
        for (const QJsonObject &page : std::as_const(sorted)) {
            const QString fileTitle = page.value("title").toString();   // "File:Name.flac"
            const QJsonArray infos = page.value("imageinfo").toArray();
            const QString name = fileTitle.mid(fileTitle.indexOf(QChar(':')) + 1);
            if (infos.isEmpty() || !name.endsWith(QLatin1String(".flac"), Qt::CaseInsensitive))
                continue;
            const QJsonObject info = infos.at(0).toObject();
            const QJsonObject ext = info.value("extmetadata").toObject();
            auto extValue = [&](const char *key) { return stripHtml(ext.value(QLatin1String(key)).toObject().value("value").toString()); };

            Track t;
            t.id = QStringLiteral("wc:") + name;
            QUrl file(info.value("url").toString());
            file.setQuery(QString());
            t.path = file.toString();
            QString title = extValue("ObjectName");
            if (title.isEmpty())
                title = name.chopped(5);
            title.remove(code);   // an archive's code in front ("PDP-CH - …")
            // "Composer - Piece (Performer, year)": the composer is the artist, as with a classical album.
            QString artist = extValue("Artist");
            if (const qsizetype dash = title.indexOf(QLatin1String(" - ")); dash > 0 && dash <= 40) {
                artist = title.left(dash).trimmed();
                title = title.mid(dash + 3).trimmed();
            } else if (artist.size() > 60) {
                artist = artist.section(QChar(','), 0, 1).trimmed();
            }
            t.title = title;
            t.artist = artist.isEmpty() ? Scanner::unknownArtist() : artist;
            t.duration = int(info.value("duration").toDouble());
            for (const QJsonValue &m : info.value("metadata").toArray()) {
                const QJsonObject o = m.toObject();
                if (t.duration <= 0 && o.value("name").toString() == QLatin1String("playtime_seconds"))
                    t.duration = int(o.value("value").toDouble());
            }
            t.year = yearOf(extValue("DateTimeOriginal"));
            t.source = QStringLiteral("commons");
            t.quality = QStringLiteral("FLAC");
            t.transient = true;
            m_licenses.insert(t.id, extValue("LicenseShortName"));
            m_pages.insert(t.id, QStringLiteral("https://commons.wikimedia.org/wiki/")
                                     + QString::fromLatin1(QUrl::toPercentEncoding(QString(fileTitle).replace(QChar(' '), QChar('_')), ":")));
            m_commonsFound << storeTrack(t).id;
        }
        finishSearch(serial);
    });
}

// ---- items (albums and concerts on the Internet Archive) ----------------------------

void OpenMusicService::loadItem(const QString &itemId, std::function<void(const QStringList &, const QString &)> done)
{
    if (m_collections.contains(itemId)) {
        done(m_collections.value(itemId), {});
        return;
    }
    m_itemWaiters[itemId] << std::move(done);
    if (m_loadingItems.contains(itemId))
        return;
    m_loadingItems.insert(itemId);
    getJson(QUrl(QStringLiteral("https://archive.org/metadata/") + QString::fromLatin1(QUrl::toPercentEncoding(itemId))),
            [this, itemId](const QJsonObject &json, const QString &error) {
        m_loadingItems.remove(itemId);
        QStringList ids;
        if (error.isEmpty())
            ids = storeItem(itemId, json);
        m_collections.insert(itemId, ids);
        for (const auto &waiter : m_itemWaiters.take(itemId))
            waiter(ids, error);
    });
}

QStringList OpenMusicService::storeItem(const QString &itemId, const QJsonObject &json)
{
    const QJsonObject meta = json.value("metadata").toObject();
    const QString creator = textOf(meta.value("creator"));
    const QString album = textOf(meta.value("title"));
    const QString date = textOf(meta.value("date"));
    const int year = yearOf(date.isEmpty() ? textOf(meta.value("year")) : date);
    const QString licenseUrl = textOf(meta.value("licenseurl"));
    const bool liveArchive = textOf(meta.value("collection")).contains(QLatin1String("etree"))
                             || meta.value("mediatype").toString() == QLatin1String("etree");
    const QString license = !licenseUrl.isEmpty() ? licenseName(licenseUrl)
                            : liveArchive ? tr("Shared with the band's permission (Live Music Archive)") : QString();
    const QString page = QStringLiteral("https://archive.org/details/") + itemId;
    // A cover only when the uploader added a picture; the item's tile is otherwise a drawn waveform.
    bool art = false;
    for (const QJsonValue &v : json.value("files").toArray()) {
        const QJsonObject f = v.toObject();
        const QString format = f.value("format").toString();
        art = art || (f.value("source").toString() == QLatin1String("original")
                      && (format == QLatin1String("JPEG") || format == QLatin1String("PNG")));
    }
    const QString cover = art ? QStringLiteral("https://archive.org/services/img/") + itemId : QString();

    struct File { int trackNo; QString name; Track track; };
    QList<File> files;
    qint64 bytes = 0;
    bool allNumbered = true;
    for (const QJsonValue &v : json.value("files").toArray()) {
        const QJsonObject f = v.toObject();
        const QString name = f.value("name").toString();
        if (!name.endsWith(QLatin1String(".flac"), Qt::CaseInsensitive) || files.size() >= kItemTrackLimit)
            continue;
        Track t;
        t.id = QStringLiteral("ia:") + itemId + QChar('/') + name;
        t.path = QStringLiteral("https://archive.org/download/") + encodedPath(itemId) + QChar('/') + encodedPath(name);
        t.title = textOf(f.value("title"));
        if (t.title.isEmpty())
            t.title = QFileInfo(name).completeBaseName().replace(QChar('_'), QChar(' ')).simplified();
        t.artist = textOf(f.value("creator"));
        if (t.artist.isEmpty())
            t.artist = textOf(f.value("artist"));
        if (t.artist.isEmpty())
            t.artist = creator.isEmpty() ? Scanner::unknownArtist() : creator;
        t.album = textOf(f.value("album"));
        if (t.album.isEmpty())
            t.album = album;
        t.albumArtist = creator;
        t.trackNo = textOf(f.value("track")).section(QChar('/'), 0, 0).toInt();
        t.duration = lengthOf(textOf(f.value("length")));
        t.year = year;
        t.cover = cover;
        t.source = QStringLiteral("archive");
        t.quality = f.value("format").toString().contains(QLatin1String("24")) ? QStringLiteral("FLAC · 24-bit")
                                                                                : QStringLiteral("FLAC");
        t.transient = true;
        allNumbered = allNumbered && t.trackNo > 0;
        bytes += textOf(f.value("size")).toLongLong();
        m_licenses.insert(t.id, license);
        m_pages.insert(t.id, page);
        files << File { t.trackNo, name, t };
    }
    // In the item's order: by track number when every file has one, otherwise by file name (d1t01, d1t02…).
    std::stable_sort(files.begin(), files.end(), [allNumbered](const File &a, const File &b) {
        return allNumbered && a.trackNo != b.trackNo ? a.trackNo < b.trackNo : a.name < b.name;
    });
    QStringList ids;
    for (const File &f : std::as_const(files))
        ids << storeTrack(f.track).id;
    m_items.insert(itemId, QVariantMap { { "title", album }, { "creator", creator }, { "date", date },
                                         { "license", license }, { "licenseUrl", licenseUrl }, { "page", page },
                                         { "bytes", double(bytes) }, { "count", int(ids.size()) } });
    return ids;
}

void OpenMusicService::ensureCollection(const QString &itemId)
{
    if (itemId.isEmpty() || m_opened.contains(itemId) || m_loadingItems.contains(itemId))
        return;
    // Loaded for a search already, only its matching tracks were shown: now all of them join the model.
    m_opened.insert(itemId);
    loadItem(itemId, [this, itemId](const QStringList &ids, const QString &error) {
        if (!error.isEmpty()) {
            m_collections.remove(itemId);   // a search may try again; this page stays empty for now
            emit notice(tr("Internet Archive: %1").arg(error), QStringLiteral("close"));
        }
        // Later, not inside the page's binding that asked for it (an item loaded before answers at once).
        QTimer::singleShot(0, this, [this, ids] { publish(ids); });
    });
}

// ---- tracks -------------------------------------------------------------------------

Track OpenMusicService::storeTrack(Track t)
{
    t.seed = Scanner::stableSeed(t.id);
    const auto existing = m_tracks.constFind(t.id);
    if (existing != m_tracks.cend()) {
        t.colorA = existing->colorA;   // computed from the cover meanwhile
        t.colorB = existing->colorB;
        t.transient = existing->transient && t.transient;
    } else {
        const auto [a, b] = Scanner::fallbackPalette(t.album.isEmpty() ? t.id : t.album);
        t.colorA = a;
        t.colorB = b;
    }
    m_tracks.insert(t.id, t);
    return t;
}

void OpenMusicService::publish(const QStringList &ids)
{
    QList<Track> added;
    for (const QString &id : ids) {
        const auto it = m_tracks.constFind(id);
        if (it == m_tracks.cend())
            continue;
        Track t = *it;
        t.liked = m_liked.contains(id);
        added << t;
    }
    if (!added.isEmpty())
        emit tracksAdded(added);
    ++m_resultsRevision;
    emit resultsChanged();
}

QList<Track> OpenMusicService::tracks() const
{
    // Order: liked, search results, history, opened albums.
    QList<Track> out;
    QSet<QString> seen;
    auto add = [&](const QString &id) {
        if (seen.contains(id))
            return;
        const auto it = m_tracks.constFind(id);
        if (it == m_tracks.cend())
            return;
        seen.insert(id);
        Track t = *it;
        t.liked = m_liked.contains(id);
        out << t;
    };
    for (const QString &id : m_liked)
        add(id);
    for (const QString &id : m_search)
        add(id);
    for (const QString &id : m_history)
        add(id);
    for (const QStringList &ids : m_collections)
        for (const QString &id : ids)
            add(id);
    return out;
}

void OpenMusicService::setHistory(const QList<Track> &tracks)
{
    m_history.clear();
    for (Track t : tracks) {
        if (!m_tracks.contains(t.id)) {
            t.transient = true;
            m_tracks.insert(t.id, t);
        }
        m_history << t.id;
    }
    publish(m_history);
}

void OpenMusicService::setTrackLiked(const Track &track, bool liked)
{
    if (!isOpenSource(track.source))
        return;
    Track t = m_tracks.value(track.id, track);
    t.transient = !liked;
    t.liked = liked;
    m_tracks.insert(t.id, t);
    m_liked.removeAll(t.id);
    if (liked)
        m_liked.prepend(t.id);
    saveState();
    emit dataChanged();
}

void OpenMusicService::setTrackColors(const QString &trackId, const QColor &a, const QColor &b)
{
    auto it = m_tracks.find(trackId);
    if (it == m_tracks.end())
        return;
    it->colorA = a;
    it->colorB = b;
}

QString OpenMusicService::pageUrl(const QString &trackId) const
{
    if (const QString page = m_pages.value(trackId); !page.isEmpty())
        return page;
    if (trackId.startsWith(QLatin1String("ia:")))
        return QStringLiteral("https://archive.org/details/") + trackId.mid(3).section(QChar('/'), 0, 0);
    if (trackId.startsWith(QLatin1String("wc:")))
        return QStringLiteral("https://commons.wikimedia.org/wiki/File:") + trackId.mid(3);
    return {};
}

void OpenMusicService::resolveStream(const QString &trackId, std::function<void(const QUrl &, const QString &)> done)
{
    QString path = m_tracks.value(trackId).path;
    if (path.isEmpty() && trackId.startsWith(QLatin1String("ia:"))) {
        const QString rest = trackId.mid(3);
        path = QStringLiteral("https://archive.org/download/") + encodedPath(rest);
    } else if (path.isEmpty() && trackId.startsWith(QLatin1String("wc:"))) {
        path = QStringLiteral("https://commons.wikimedia.org/wiki/Special:FilePath/") + QString::fromLatin1(QUrl::toPercentEncoding(trackId.mid(3)));
    }
    // The file's own address: nothing to look up, but the answer comes like the others' (asynchronously).
    QTimer::singleShot(0, this, [path, done = std::move(done)] {
        if (path.isEmpty())
            done({}, tr("the file isn't known"));
        else
            done(QUrl(path), {});
    });
}

// ---- saving files ---------------------------------------------------------------------

QString OpenMusicService::targetPath(const Track &t) const
{
    QString dir = downloadFolder() + QChar('/') + safeName(t.albumArtist.isEmpty() ? t.artist : t.albumArtist);
    if (!t.album.isEmpty())
        dir += QChar('/') + safeName(t.album, 100);
    const QString number = t.trackNo > 0 ? QStringLiteral("%1 ").arg(t.trackNo, 2, 10, QChar('0')) : QString();
    return dir + QChar('/') + safeName(number + t.title, 120) + QStringLiteral(".flac");
}

QVariantList OpenMusicService::downloads() const
{
    QVariantList out;
    const QStringList order = m_running.isEmpty() ? m_queue : QStringList { m_running } + m_queue;
    for (const QString &id : order) {
        const Job &j = m_jobs[id];
        out << QVariantMap { { "id", id }, { "title", j.title },
                             { "progress", j.total > 0 ? double(j.received) / double(j.total) : 0.0 } };
    }
    return out;
}

void OpenMusicService::download(const QString &trackId)
{
    const auto it = m_tracks.constFind(trackId);
    if (it == m_tracks.cend() || m_jobs.contains(trackId))
        return;
    const QString target = targetPath(*it);
    if (QFileInfo::exists(target)) {
        if (m_jobs.isEmpty())
            emit notice(tr("“%1” is already in your library").arg(it->title), QStringLiteral("check"));
        return;
    }
    Job job;
    job.trackId = trackId;
    job.title = it->title;
    job.url = QUrl(it->path);
    job.target = target;
    job.tags = QJsonObject { { "TITLE", it->title }, { "ARTIST", it->artist }, { "ALBUM", it->album },
                             { "ALBUMARTIST", it->albumArtist }, { "TRACKNUMBER", it->trackNo },
                             { "DATE", it->year }, { "LICENSE", m_licenses.value(trackId) },
                             { "COMMENT", pageUrl(trackId) } };
    const bool first = m_jobs.isEmpty();
    m_jobs.insert(trackId, job);
    m_queue << trackId;
    if (first) {
        m_batchSaved = 0;
        m_batchFailed = 0;
    }
    emit downloadsChanged();
    startNextDownload();
}

void OpenMusicService::downloadCollection(const QString &itemId)
{
    const QStringList ids = m_collections.value(itemId);
    int queued = 0;
    for (const QString &id : ids) {
        const auto it = m_tracks.constFind(id);
        if (it == m_tracks.cend() || m_jobs.contains(id) || QFileInfo::exists(targetPath(*it)))
            continue;
        download(id);
        ++queued;
    }
    if (queued == 0 && !ids.isEmpty())
        emit notice(tr("These files are already in your library"), QStringLiteral("check"));
    else if (queued > 0)
        emit notice(tr("Saving %n FLAC files to your library…", "", queued), QStringLiteral("download"));
}

void OpenMusicService::cancelDownloads()
{
    // The jobs go first, so the aborted reply finds none and reports nothing.
    const QPointer<QNetworkReply> running = m_jobs.value(m_running).reply;
    m_queue.clear();
    m_jobs.clear();
    m_running.clear();
    if (running)
        running->abort();
    emit downloadsChanged();
}

void OpenMusicService::startNextDownload()
{
    if (!m_running.isEmpty() || m_queue.isEmpty())
        return;
    const QString id = m_queue.takeFirst();
    m_running = id;
    Job &job = m_jobs[id];
    QDir().mkpath(QFileInfo(job.target).absolutePath());
    job.file = new QFile(job.target + QStringLiteral(".part"), this);
    if (!job.file->open(QIODevice::WriteOnly)) {
        const QString error = job.file->errorString();
        delete std::exchange(job.file, nullptr);
        finishDownload(id, error);
        return;
    }
    if (m_jobs.size() == 1 && m_batchSaved == 0 && m_batchFailed == 0)
        emit notice(tr("Saving “%1” to your library…").arg(job.title), QStringLiteral("download"));
    QNetworkRequest request(job.url);
    request.setRawHeader("User-Agent", kUserAgent);
    request.setTransferTimeout(60000);
    QNetworkReply *reply = m_net.get(request);
    job.reply = reply;
    QFile *file = job.file;
    connect(reply, &QNetworkReply::readyRead, this, [reply, file] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, id](qint64 received, qint64 total) {
        const auto it = m_jobs.find(id);
        if (it == m_jobs.end())
            return;
        const bool step = total > 0 && received * 100 / total != it->received * 100 / std::max<qint64>(1, it->total);
        it->received = received;
        it->total = total;
        if (step)
            emit downloadsChanged();
    });
    connect(reply, &QNetworkReply::finished, this, [this, id, reply, file] {
        reply->deleteLater();
        file->write(reply->readAll());
        file->close();
        const QString partial = file->fileName();
        file->deleteLater();
        const auto it = m_jobs.find(id);
        if (reply->error() != QNetworkReply::NoError || it == m_jobs.end()) {
            QFile::remove(partial);
            if (it != m_jobs.end())
                finishDownload(id, reply->errorString());
            return;
        }
        const QString target = it->target;
        QFile::remove(target);
        if (!QFile::rename(partial, target)) {
            QFile::remove(partial);
            finishDownload(id, tr("couldn't write %1").arg(QDir::toNativeSeparators(target)));
            return;
        }
        // Missing tags are written on a worker thread: for a FLAC file that can mean rewriting all of it.
        auto *watcher = new QFutureWatcher<void>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, id, target] {
            watcher->deleteLater();
            m_lastSaved = m_jobs.value(id).title;
            emit fileSaved(QFileInfo(target).absolutePath());
            finishDownload(id, {});
        });
        watcher->setFuture(QtConcurrent::run(fillTags, target, it->tags));
    });
}

void OpenMusicService::finishDownload(const QString &trackId, const QString &error)
{
    const QString title = m_jobs.value(trackId).title;
    m_jobs.remove(trackId);
    if (m_running == trackId)
        m_running.clear();
    if (error.isEmpty())
        ++m_batchSaved;
    else {
        ++m_batchFailed;
        emit notice(tr("Couldn't save “%1”: %2").arg(title, error), QStringLiteral("close"));
    }
    emit downloadsChanged();
    if (m_jobs.isEmpty() && m_batchSaved > 0) {
        emit notice(m_batchSaved == 1 ? tr("Saved to your library: %1").arg(m_lastSaved)
                                      : tr("%n FLAC files saved to your library", "", m_batchSaved),
                    QStringLiteral("check"));
    }
    startNextDownload();
}

// ---- state ----------------------------------------------------------------------------

void OpenMusicService::loadState()
{
    QFile f(stateFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue &v : root.value("liked").toArray()) {
        const QJsonObject o = v.toObject();
        Track t = YouTubeService::trackFromJson(o);
        if (!isOpenSource(t.source))
            continue;
        t.year = o.value("year").toInt();
        t.trackNo = o.value("track").toInt();
        t.albumArtist = o.value("albumArtist").toString();
        t.liked = true;
        m_tracks.insert(t.id, t);
        m_liked << t.id;
        m_licenses.insert(t.id, o.value("license").toString());
        if (const QString page = o.value("page").toString(); !page.isEmpty())
            m_pages.insert(t.id, page);
    }
}

void OpenMusicService::saveState() const
{
    QJsonArray liked;
    for (const QString &id : m_liked) {
        const Track t = m_tracks.value(id);
        QJsonObject o = YouTubeService::trackJson(t);
        o.insert("year", t.year);
        o.insert("track", t.trackNo);
        o.insert("albumArtist", t.albumArtist);
        o.insert("license", m_licenses.value(id));
        o.insert("page", m_pages.value(id));
        liked.append(o);
    }
    QSaveFile f(stateFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(QJsonObject { { "liked", liked } }).toJson(QJsonDocument::Compact));
        f.commit();
    }
}
