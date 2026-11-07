#include "Lyrics.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

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

Lyrics Lyrics::fromPlainText(const QString &text)
{
    Lyrics out;
    QString normalized = text;
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
