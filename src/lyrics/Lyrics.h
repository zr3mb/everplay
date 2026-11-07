#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>

// Lyrics of a track: lines with (optional) timestamps in milliseconds.
// An empty line (text == "") is a break between verses — it has no time of its own.

// A word of a line: where it is in the text and when it's sung (ms; -1 = not known).
struct LyricWord
{
    qint64 start = -1;
    qint64 end = -1;
    int from = 0;     // characters into the line's text
    int length = 0;
};

struct LyricLine
{
    qint64 start = -1;   // ms; -1 = no timestamp
    qint64 end = -1;
    QString text;
    QList<LyricWord> words;   // times from speech recognition; empty = only the line's time is known

    // Every word of the text with a time: the known ones kept, the rest spread by their letters
    // over the gaps (or over the line, sung at a typical pace). Needs the line's start.
    QList<LyricWord> timedWords() const;
};

struct Lyrics
{
    QList<LyricLine> lines;
    bool synced = false;    // every non-empty line has a timestamp
    QString source;         // e.g. "Source: LyricFind" (footer from YouTube Music)
    QString origin;         // "youtube" | "local" | "stt" (timing aligned by speech recognition) | "stt-text" (recognized)
    bool wordTimed = false; // the words' times come from speech recognition, not an estimate
    // What made the "stt" timing or the "stt-text" lyrics: "aligner" (forced alignment, from 0.25.1 on),
    // "whisper" (speech recognition); empty in older entries.
    QString engine;

    bool isEmpty() const;
    int wordCount() const;
    QString plainText() const;

    QJsonObject toJson() const;
    static Lyrics fromJson(const QJsonObject &o);

    // Plain text -> lines without timestamps (keeps empty lines as breaks, trims the excess).
    static Lyrics fromPlainText(const QString &text);
    // LRC format: "[mm:ss.xx]text", several tags per line, [offset:±ms]. No tags -> plain text.
    static Lyrics fromLrc(const QString &text);
    // Fills in "end" from the start of the next line.
    void fillEnds(qint64 durationMs = -1);
    // "Synced" lyrics whose lines all start at the same moment (YouTube sends some with no times at all)
    // aren't really timed — they become plain lyrics.
    bool hasRealTiming() const;
    void dropFakeTiming();
};
