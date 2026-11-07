#include "Lyrics.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <optional>

bool Lyrics::isEmpty() const
{
    return std::none_of(lines.cbegin(), lines.cend(), [](const LyricLine &l) { return !l.text.isEmpty(); });
}

int Lyrics::wordCount() const
{
    int n = 0;
    for (const LyricLine &l : lines)
        n += int(l.text.split(QChar(' '), Qt::SkipEmptyParts).size());
    return n;
}

QString Lyrics::plainText() const
{
    QStringList out;
    for (const LyricLine &l : lines)
        out << l.text;
    return out.join(QChar('\n'));
}

QJsonObject Lyrics::toJson() const
{
    QJsonArray arr;
    for (const LyricLine &l : lines) {
        QJsonArray line { double(l.start), double(l.end), l.text };
        if (!l.words.isEmpty()) {
            QJsonArray words;   // flat: start, end, from, length, ...
            for (const LyricWord &w : l.words)
                words << double(w.start) << double(w.end) << w.from << w.length;
            line.append(words);
        }
        arr.append(line);
    }
    QJsonObject o { { "lines", arr }, { "synced", synced }, { "source", source }, { "origin", origin }, { "wordTimed", wordTimed } };
    if (!engine.isEmpty())
        o.insert("engine", engine);
    return o;
}

Lyrics Lyrics::fromJson(const QJsonObject &o)
{
    Lyrics out;
    for (const QJsonValue &v : o.value("lines").toArray()) {
        const QJsonArray a = v.toArray();
        LyricLine line { qint64(a.at(0).toDouble(-1)), qint64(a.at(1).toDouble(-1)), a.at(2).toString(), {} };
        const QJsonArray words = a.at(3).toArray();
        for (qsizetype i = 0; i + 3 < words.size(); i += 4)
            line.words << LyricWord { qint64(words.at(i).toDouble(-1)), qint64(words.at(i + 1).toDouble(-1)),
                                      words.at(i + 2).toInt(), words.at(i + 3).toInt() };
        out.lines << line;
    }
    out.wordTimed = o.value("wordTimed").toBool();
    out.synced = o.value("synced").toBool();
    out.source = o.value("source").toString();
    out.origin = o.value("origin").toString();
    out.engine = o.value("engine").toString();
    return out;
}

namespace {

// "x2", "(2x)", "[x3]", "*2", "2 razy" — how many times something is sung; 0 when the text isn't a mark.
int repeatMark(const QString &text)
{
    static const QRegularExpression mark(
        QStringLiteral("^[\\[\\(]?\\s*(?:x\\s?(\\d)|(\\d)\\s?(?:x|razy)|\\*\\s?(\\d))\\s*[\\]\\)]?\\s*:?$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = mark.match(text.trimmed());
    if (!m.hasMatch())
        return 0;
    const int n = (m.captured(1) + m.captured(2) + m.captured(3)).toInt();
    return n >= 2 && n <= 8 ? n : 0;
}

struct SectionHeader
{
    bool chorus = false;
    int times = 1;
    QString sung;   // lyrics written on the header's own line ("Ref.: <the first line>")
};

std::optional<SectionHeader> sectionHeader(const QString &line)
{
    static const QRegularExpression named(
        QStringLiteral("^[\\[\\(]?\\s*(pre-?chorus|post-?chorus|chorus|refrain|refren|ref|hook|verse|zwrotka|intro|outro|"
                       "bridge|interlude|przej[sś]cie|przedrefren|tekst|lyrics|s[lł]owa|instrumental)\\.?\\s*\\d*\\.?"
                       "\\s*([\\[\\(]?\\s*(?:x\\s?\\d|\\d\\s?x)\\s*[\\]\\)]?)?\\s*[\\]\\)]?\\s*(?:[:：\\-–]\\s*(.*))?$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression bracketed(QStringLiteral("^\\[[^\\]]*\\]$"));
    static const QRegularExpression chorusWord(QStringLiteral("chorus|refrain|refren|\\bref\\b|hook"),
                                               QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression inlineMark(QStringLiteral("\\b(?:x\\s?(\\d)|(\\d)\\s?x)\\b"),
                                               QRegularExpression::CaseInsensitiveOption);
    const QString t = line.trimmed();
    SectionHeader h;
    if (const auto m = named.match(t); m.hasMatch()) {
        const QString name = m.captured(1).toLower();
        h.chorus = name == u"chorus" || name == u"refrain" || name == u"refren" || name == u"ref" || name == u"hook";
        if (const int n = repeatMark(m.captured(2)))
            h.times = n;
        QString rest = m.captured(3).trimmed();
        while (rest.endsWith(QChar(']')) || rest.endsWith(QChar(')')))
            rest.chop(1);
        rest = rest.trimmed();
        if (const int n = repeatMark(rest))
            h.times = n;
        else if (rest.split(QChar(' '), Qt::SkipEmptyParts).size() >= 4)
            h.sung = rest;   // a short rest names who sings it, a longer one is sung
        return h;
    }
    if (bracketed.match(t).hasMatch()) {
        h.chorus = chorusWord.match(t).hasMatch();
        if (const auto m = inlineMark.match(t); m.hasMatch())
            h.times = std::clamp((m.captured(1) + m.captured(2)).toInt(), 1, 8);
        return h;
    }
    return std::nullopt;
}

// Guitar chords written over or after the words ("Am  C  G", "<a line>     E     gis").
bool isChord(const QString &token)
{
    static const QRegularExpression chord(
        QStringLiteral("^[A-Ha-h](?:is|es|#|b)?(?:m|maj|min|dim|aug|sus)?\\d{0,2}(?:/[A-Ha-h](?:is|es|#|b)?)?$"));
    return chord.match(token).hasMatch();
}

QString dropChords(const QString &line)
{
    const QStringList tokens = line.split(QChar(' '), Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return line;
    const bool allChords = std::all_of(tokens.cbegin(), tokens.cend(), isChord);
    if (allChords && (tokens.size() >= 2 || tokens.first().size() >= 2))
        return {};
    // Chords after the words are set apart by a wide gap.
    static const QRegularExpression tail(QStringLiteral("^(.*?\\S)\\s{3,}(\\S.*)$"));
    if (const auto m = tail.match(line); m.hasMatch()) {
        const QStringList after = m.captured(2).split(QChar(' '), Qt::SkipEmptyParts);
        if (std::all_of(after.cbegin(), after.cend(), isChord))
            return m.captured(1);
    }
    return line;
}

} // namespace

bool Lyrics::isSectionHeader(const QString &line)
{
    return sectionHeader(line).has_value();
}

QString Lyrics::tidyText(const QString &text)
{
    static const QRegularExpression lineMark(
        QStringLiteral("^(.*\\S)\\s+[\\[\\(]?\\s*(?:x\\s?(\\d)|(\\d)\\s?(?:x|razy)|\\*\\s?(\\d))\\s*[\\]\\)]?$"),
        QRegularExpression::CaseInsensitiveOption);

    struct Stanza
    {
        bool headed = false;
        bool chorus = false;
        int times = 1;
        QStringList lines;
    };
    QStringList out, chorusLines, lastLines;
    Stanza cur;
    auto close = [&] {
        if (!cur.headed && cur.lines.isEmpty())
            return;
        QStringList lines = cur.lines;
        if (cur.chorus) {
            if (lines.isEmpty())
                lines = chorusLines;   // "Ref." alone: the chorus as written the first time
            else if (chorusLines.isEmpty())
                chorusLines = lines;
        }
        if (!lines.isEmpty()) {
            for (int i = 0; i < cur.times; ++i)
                out << lines;
            out << QString();
            lastLines = lines;
        }
        cur = Stanza();
    };

    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QChar('\r'), QChar('\n'));
    for (const QString &raw : normalized.split(QChar('\n'))) {
        const QString trimmed = raw.trimmed();
        const QString line = dropChords(trimmed);
        if (line.isEmpty() && !trimmed.isEmpty())
            continue;   // a line of chords
        if (line.isEmpty()) {
            // A header alone before a blank line: a chorus named again, or a part whose words come after the gap.
            if (cur.headed && cur.lines.isEmpty() && !(cur.chorus && !chorusLines.isEmpty()))
                continue;
            close();
            continue;
        }
        if (const int n = repeatMark(line)) {
            // Under a verse: the verse is sung n times; after a gap, the verse before it.
            if (!cur.lines.isEmpty() || cur.headed) {
                cur.times = n;
            } else if (!lastLines.isEmpty()) {
                if (!out.isEmpty() && out.last().isEmpty())
                    out.removeLast();
                for (int i = 1; i < n; ++i)
                    out << lastLines;
                out << QString();
            }
            continue;
        }
        if (const auto h = sectionHeader(line)) {
            close();
            cur.headed = true;
            cur.chorus = h->chorus;
            cur.times = h->times;
            if (!h->sung.isEmpty())
                cur.lines << h->sung;
            continue;
        }
        if (const auto m = lineMark.match(line); m.hasMatch()) {
            const int n = std::clamp((m.captured(2) + m.captured(3) + m.captured(4)).toInt(), 1, 8);
            for (int i = 0; i < n; ++i)
                cur.lines << m.captured(1);
            continue;
        }
        cur.lines << line;
    }
    close();
    return out.join(QChar('\n'));
}

Lyrics Lyrics::fromPlainText(const QString &text, bool tidy)
{
    Lyrics out;
    QString normalized = tidy ? tidyText(text) : text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(QChar('\r'), QChar('\n'));
    bool lastEmpty = true;   // no empty lines at the start and no double breaks
    for (const QString &raw : normalized.split(QChar('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() && lastEmpty)
            continue;
        out.lines << LyricLine { -1, -1, line };
        lastEmpty = line.isEmpty();
    }
    while (!out.lines.isEmpty() && out.lines.last().text.isEmpty())
        out.lines.removeLast();
    return out;
}

Lyrics Lyrics::fromLrc(const QString &text)
{
    static const QRegularExpression timeTag(QStringLiteral("\\[(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?\\]"));
    static const QRegularExpression metaTag(QStringLiteral("^\\[([a-zA-Z]+):\\s*(.*)\\]$"));

    struct Stamped { qint64 at; QString text; };
    QList<Stamped> stamped;
    qint64 offset = 0;
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    for (const QString &raw : normalized.split(QChar('\n'))) {
        const QString line = raw.trimmed();
        if (const auto meta = metaTag.match(line); meta.hasMatch() && !timeTag.match(line).hasMatch()) {
            if (meta.captured(1).compare(QLatin1String("offset"), Qt::CaseInsensitive) == 0)
                offset = meta.captured(2).toLongLong();
            continue;
        }
        QList<qint64> times;
        int pos = 0;
        auto m = timeTag.match(line, pos, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        while (m.hasMatch()) {
            const QString frac = m.captured(3);
            qint64 ms = (m.captured(1).toLongLong() * 60 + m.captured(2).toLongLong()) * 1000;
            if (!frac.isEmpty())
                ms += frac.toLongLong() * (frac.size() == 1 ? 100 : frac.size() == 2 ? 10 : 1);
            times << ms;
            pos = m.capturedEnd();
            m = timeTag.match(line, pos, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        }
        if (times.isEmpty())
            continue;
        // Word tags of the "enhanced LRC" version (<mm:ss.xx>) aren't needed here.
        static const QRegularExpression wordTags(QStringLiteral("<\\d{1,3}:\\d{1,2}(?:[.:]\\d{1,3})?>"));
        const QString body = line.mid(pos).remove(wordTags).trimmed();
        for (qint64 t : std::as_const(times))
            stamped << Stamped { std::max<qint64>(0, t - offset), body };
    }

    if (stamped.isEmpty())
        return fromPlainText(text);

    std::stable_sort(stamped.begin(), stamped.end(), [](const Stamped &a, const Stamped &b) { return a.at < b.at; });
    Lyrics out;
    out.synced = true;
    for (const Stamped &s : std::as_const(stamped)) {
        // An empty line with a timestamp means an instrumental break in LRC.
        if (s.text.isEmpty() && (out.lines.isEmpty() || out.lines.last().text.isEmpty()))
            continue;
        out.lines << LyricLine { s.text.isEmpty() ? -1 : s.at, -1, s.text, {} };
    }
    out.fillEnds();
    return out;
}

QList<LyricWord> LyricLine::timedWords() const
{
    // The words of the text, with the known times where they are.
    QList<LyricWord> out;
    for (int i = 0; i < text.size();) {
        while (i < text.size() && text.at(i).isSpace())
            ++i;
        const int from = i;
        while (i < text.size() && !text.at(i).isSpace())
            ++i;
        if (i > from)
            out << LyricWord { -1, -1, from, i - from };
    }
    // A heard word is held at most this long: recognition often stretches the last word before a pause
    // over the whole silence, and the highlight would then crawl through it.
    const auto longest = [](int letters) { return std::max<qint64>(1500, qint64(letters) * 300); };
    for (LyricWord &w : out)
        for (const LyricWord &known : words)
            if (known.from == w.from && known.start >= 0) {
                w.start = known.start;
                w.end = std::clamp(known.end, known.start + 80, known.start + longest(w.length));
            }
    if (start < 0 || out.isEmpty())
        return out;

    // Unknown runs share the time between the known neighbours by letters, but never slower than a slow
    // singer: across a pause (an instrumental break, a breath between phrases) the words go at a sung
    // pace next to the word they belong with instead of being spread over the silence.
    constexpr qint64 kPerLetter = 75;       // a typical pace
    constexpr qint64 kSlowestLetter = 180;  // a drawn-out one
    for (int i = 0; i < out.size();) {
        if (out[i].start >= 0) {
            ++i;
            continue;
        }
        int j = i;
        int letters = 0;
        while (j < out.size() && out[j].start < 0)
            letters += out[j++].length + 1;
        const qint64 slowest = letters * kSlowestLetter + 400;
        qint64 from = i > 0 ? out[i - 1].end : start;
        qint64 to = j < out.size() ? out[j].start : -1;
        if (to < 0) {
            // The end of the line: the words take part of the time left, as fast as typical at the least.
            const qint64 sung = letters * kPerLetter + 250;
            const qint64 room = end > from ? end - from : sung;
            to = from + std::min(room, std::clamp(room * 6 / 10, sung, slowest));
        } else if (to - from > slowest) {
            // A long gap between heard words: the first words of a line lead into the first heard one,
            // the others follow the word before them.
            if (i == 0)
                from = to - slowest;
            else
                to = from + slowest;
        }
        to = std::max(to, from + 80 * (j - i));
        qint64 at = from;
        for (int k = i; k < j; ++k) {
            const qint64 span = (to - from) * (out[k].length + 1) / std::max(1, letters);
            out[k].start = at;
            out[k].end = at + span;
            at += span;
        }
        i = j;
    }
    return out;
}

bool Lyrics::hasRealTiming() const
{
    QSet<qint64> starts;
    int textLines = 0;
    for (const LyricLine &l : lines) {
        if (l.text.isEmpty())
            continue;
        ++textLines;
        if (l.start >= 0)
            starts.insert(l.start);
    }
    return starts.size() >= std::min(textLines, 3);
}

void Lyrics::dropFakeTiming()
{
    if (!synced || hasRealTiming())
        return;
    for (LyricLine &l : lines) {
        l.start = l.end = -1;
        l.words.clear();
    }
    synced = false;
    wordTimed = false;
}

void Lyrics::fillEnds(qint64 durationMs)
{
    qint64 next = durationMs;
    for (qsizetype i = lines.size() - 1; i >= 0; --i) {
        LyricLine &l = lines[i];
        if (l.text.isEmpty() || l.start < 0)
            continue;
        if (l.end < 0 || (next >= 0 && l.end > next))
            l.end = next >= 0 ? next : l.start + 5000;
        next = l.start;
    }
}
