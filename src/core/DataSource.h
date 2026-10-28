#pragma once

#include "MachineProfile.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <memory>

namespace nc {

// Where the bytes for a send come from. The transfer worker never touches
// files directly. It only pulls bytes from a DataSource when the serial port
// has room, so flow control and pacing live in the worker while the content
// is decided here.
//
// The worker's pull loop also allows:
//   - totalSize() may return -1 if the size is unknown
//   - read() may return an empty array while !atEnd() to mean "nothing ready
//     yet, ask again shortly"; the worker retries on a short timer
class DataSource
{
public:
    virtual ~DataSource() = default;

    virtual qint64 totalSize() const = 0; // -1 if unknown
    virtual qint64 position() const = 0;
    virtual bool atEnd() const = 0;
    virtual QByteArray read(qint64 maxBytes) = 0;
    virtual QString description() const = 0;

    // Bytes at the very end of the stream that come after the program's last
    // block: the closing % line and its line ending. A control that stops
    // reading once it has the last block never takes these (drip feed).
    virtual qint64 endTailBytes() const { return 0; }
    // Set if reading failed part way through (e.g. a network share dropped).
    virtual QString error() const { return {}; }
};

// A fully formatted program held in memory (normal send).
class MemoryDataSource final : public DataSource
{
public:
    MemoryDataSource(QByteArray data, QString description);

    qint64 totalSize() const override { return m_data.size(); }
    qint64 position() const override { return m_pos; }
    bool atEnd() const override { return m_pos >= m_data.size(); }
    QByteArray read(qint64 maxBytes) override;
    QString description() const override { return m_description; }

private:
    QByteArray m_data;
    qint64 m_pos = 0;
    QString m_description;
};

// Drip feed: streams the file from disk and formats it line by line as the
// worker asks for bytes, so a program of any size goes out without being held
// in memory. The output is exactly ProgramFormatter::format() without the
// trailer: a control running the program stops reading at the closing %, so
// trailer characters after it would sit in the port and be the first thing
// sent on the next Cycle Start.
//
// The file is read twice: once when it is opened, to plan the % lines and
// work out the exact size, then again while streaming.
class StreamingFileSource final : public DataSource
{
public:
    // Returns nullptr and sets *error if the file can't be read or has no
    // program in it.
    static std::unique_ptr<StreamingFileSource> open(const QString& path, const MachineProfile& profile,
                                                     QString* error);

    qint64 totalSize() const override { return m_total; }
    qint64 position() const override { return m_pos; }
    bool atEnd() const override { return m_exhausted && m_pendingPos >= m_pending.size(); }
    QByteArray read(qint64 maxBytes) override;
    QString description() const override { return m_description; }
    qint64 endTailBytes() const override { return m_endTail; }
    QString error() const override { return m_error; }

    // Splits a file into lines exactly like ProgramFormatter::splitLines()
    // on the cleaned file (BOM and Ctrl+Z removed), reading it in blocks.
    class LineReader
    {
    public:
        explicit LineReader(QFile* file, qint64 blockSize = 64 * 1024);
        bool next(QByteArray* line); // false at the end of the file or on a read error
        bool failed() const { return m_failed; }

    private:
        bool fill();

        QFile* m_file;
        qint64 m_blockSize;
        QByteArray m_buf;
        qsizetype m_pos = 0;  // start of the next line in m_buf
        qsizetype m_scan = 0; // where to resume looking for a line break
        bool m_eof = false;
        bool m_failed = false;
        bool m_first = true;
    };

private:
    StreamingFileSource() = default;
    bool refill();

    QFile m_file;
    std::unique_ptr<LineReader> m_reader;
    QString m_description;
    QString m_shownPath;
    QByteArray m_eol;
    QByteArray m_leader;
    int m_openBefore = -1;
    int m_closeAfter = -1;
    int m_line = 0;
    bool m_leaderDone = false;
    bool m_exhausted = false;
    QByteArray m_pending;
    qsizetype m_pendingPos = 0;
    qint64 m_pos = 0;
    qint64 m_total = 0;
    qint64 m_endTail = 0;
    QString m_error;
};

struct SendRequest
{
    enum class Mode { Normal, DripFeed };

    QString filePath;
    Mode mode = Mode::Normal;
};

// Largest file accepted for a normal (in-memory) send. Drip feed has no limit.
constexpr qint64 kMaxNormalSendBytes = 64LL * 1024 * 1024;

// Loads and formats the requested file. Returns nullptr and sets *error on
// failure (file not found, unreadable, empty, too large for a normal send).
std::unique_ptr<DataSource> createDataSource(const SendRequest& request, const MachineProfile& profile,
                                             QString* error, qint64* fileSize = nullptr);

} // namespace nc
