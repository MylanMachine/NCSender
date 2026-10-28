#include "DataSource.h"

#include "ProgramFormatter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace nc {

MemoryDataSource::MemoryDataSource(QByteArray data, QString description)
    : m_data(std::move(data))
    , m_description(std::move(description))
{
}

QByteArray MemoryDataSource::read(qint64 maxBytes)
{
    const qint64 n = qMin<qint64>(maxBytes, m_data.size() - m_pos);
    if (n <= 0)
        return {};
    QByteArray chunk = m_data.mid(m_pos, n);
    m_pos += n;
    return chunk;
}

// ---------------------------------------------------------------- streaming

StreamingFileSource::LineReader::LineReader(QFile* file, qint64 blockSize)
    : m_file(file)
    , m_blockSize(qMax<qint64>(1, blockSize))
{
}

bool StreamingFileSource::LineReader::fill()
{
    if (m_pos > 0) {
        m_buf.remove(0, m_pos);
        m_scan -= m_pos;
        m_pos = 0;
    }
    // The first block must be long enough to recognise a BOM.
    const qint64 want = m_first ? qMax<qint64>(m_blockSize, 3) : m_blockSize;
    QByteArray block(want, Qt::Uninitialized);
    const qint64 got = m_file->read(block.data(), want);
    if (got < 0) {
        m_failed = true;
        m_eof = true;
        return false;
    }
    if (got == 0) {
        m_eof = true;
        return false;
    }
    block.truncate(got);
    if (m_first) {
        m_first = false;
        if (block.startsWith("\xEF\xBB\xBF"))
            block.remove(0, 3);
    }
    block.replace('\x1A', QByteArray());
    m_buf.append(block);
    return true;
}

bool StreamingFileSource::LineReader::next(QByteArray* line)
{
    for (;;) {
        if (m_failed)
            return false; // never hand out a line cut short by a read error
        const qsizetype n = m_buf.size();
        qsizetype i = m_scan;
        while (i < n && m_buf.at(i) != '\r' && m_buf.at(i) != '\n')
            ++i;
        // A CR needs the two bytes after it to tell CR, CR LF and CR CR LF apart.
        const bool needLookahead = i < n && m_buf.at(i) == '\r' && i + 2 >= n && !m_eof;
        if (i < n && !needLookahead) {
            *line = m_buf.mid(m_pos, i - m_pos);
            qsizetype end = i;
            if (m_buf.at(i) == '\r') {
                if (i + 2 < n && m_buf.at(i + 1) == '\r' && m_buf.at(i + 2) == '\n')
                    end = i + 2;
                else if (i + 1 < n && m_buf.at(i + 1) == '\n')
                    end = i + 1;
            }
            m_pos = m_scan = end + 1;
            return true;
        }
        m_scan = i;
        if (m_eof) {
            if (m_pos < n) {
                *line = m_buf.mid(m_pos);
                m_pos = m_scan = n;
                return true;
            }
            return false;
        }
        fill();
    }
}

std::unique_ptr<StreamingFileSource> StreamingFileSource::open(const QString& path, const MachineProfile& profile,
                                                               QString* error)
{
    auto fail = [&](const QString& msg) -> std::unique_ptr<StreamingFileSource> {
        if (error)
            *error = msg;
        return nullptr;
    };

    std::unique_ptr<StreamingFileSource> s(new StreamingFileSource);
    const QFileInfo fi(path);
    s->m_shownPath = QDir::toNativeSeparators(fi.absoluteFilePath());
    s->m_description = fi.fileName();
    s->m_file.setFileName(path);
    if (!s->m_file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot open %1: %2").arg(s->m_shownPath, s->m_file.errorString()));

    s->m_eol = lineEndingBytes(profile.lineEnding);
    if (profile.leaderCount > 0)
        s->m_leader = QByteArray(profile.leaderCount, static_cast<char>(profile.leaderByte & 0xFF));
    const qint64 eolSize = s->m_eol.size();

    // First pass: plan the % lines, and measure the output without building it.
    ProgramFormatter::PercentPlanner planner;
    qint64 linesBytes = 0;    // every line plus its line ending
    qint64 lastBlockEnd = 0;  // linesBytes just after the last program block
    int lastBlockLine = -1;
    bool content = false;
    {
        LineReader reader(&s->m_file);
        QByteArray line;
        for (int i = 0; reader.next(&line); ++i) {
            planner.addLine(line);
            linesBytes += line.size() + eolSize;
            const QByteArray t = line.trimmed();
            if (!t.isEmpty() && !t.startsWith('%')) {
                lastBlockLine = i;
                lastBlockEnd = linesBytes;
            }
            if (!content)
                content = ProgramFormatter::hasProgramContent(line);
        }
        if (reader.failed())
            return fail(QStringLiteral("Error reading %1: %2").arg(s->m_shownPath, s->m_file.errorString()));
    }
    if (!content)
        return fail(QStringLiteral("File has no program in it (only blank lines): %1").arg(s->m_shownPath));

    if (profile.ensurePercent) {
        const auto plan = planner.plan();
        s->m_openBefore = plan.openBefore;
        s->m_closeAfter = plan.closeAfter;
    }
    const qint64 percentLine = 1 + eolSize;
    s->m_total = s->m_leader.size() + linesBytes + (s->m_openBefore >= 0 ? percentLine : 0)
                 + (s->m_closeAfter >= 0 ? percentLine : 0);
    if (lastBlockLine >= 0) {
        qint64 blockEnd = s->m_leader.size() + lastBlockEnd;
        if (s->m_openBefore >= 0 && s->m_openBefore <= lastBlockLine)
            blockEnd += percentLine;
        if (s->m_closeAfter >= 0 && s->m_closeAfter < lastBlockLine)
            blockEnd += percentLine;
        s->m_endTail = s->m_total - blockEnd;
    }

    if (!s->m_file.seek(0))
        return fail(QStringLiteral("Error reading %1: %2").arg(s->m_shownPath, s->m_file.errorString()));
    s->m_reader = std::make_unique<LineReader>(&s->m_file);
    return s;
}

bool StreamingFileSource::refill()
{
    m_pending.clear();
    m_pendingPos = 0;
    if (m_exhausted)
        return false;
    if (!m_leaderDone) {
        m_leaderDone = true;
        if (!m_leader.isEmpty()) {
            m_pending = m_leader;
            return true;
        }
    }
    QByteArray line;
    if (m_reader->next(&line)) {
        if (m_line == m_openBefore)
            m_pending.append('%').append(m_eol);
        m_pending.append(line).append(m_eol);
        if (m_line == m_closeAfter)
            m_pending.append('%').append(m_eol);
        ++m_line;
        return true;
    }
    if (m_reader->failed()) {
        m_error = QStringLiteral("Error reading %1 part way through the program: %2")
                      .arg(m_shownPath, m_file.errorString());
        return false;
    }
    m_exhausted = true;
    m_file.close();
    return false;
}

QByteArray StreamingFileSource::read(qint64 maxBytes)
{
    QByteArray out;
    while (out.size() < maxBytes) {
        if (m_pendingPos >= m_pending.size() && !refill())
            break;
        const qint64 n = qMin<qint64>(maxBytes - out.size(), m_pending.size() - m_pendingPos);
        out.append(m_pending.constData() + m_pendingPos, n);
        m_pendingPos += n;
    }
    // Look ahead now, so atEnd() is already true after the last bytes.
    if (m_pendingPos >= m_pending.size() && m_error.isEmpty())
        refill();
    m_pos += out.size();
    return out;
}

// ---------------------------------------------------------------- factory

std::unique_ptr<DataSource> createDataSource(const SendRequest& request, const MachineProfile& profile,
                                             QString* error, qint64* fileSize)
{
    auto fail = [&](const QString& msg) -> std::unique_ptr<DataSource> {
        if (error)
            *error = msg;
        return nullptr;
    };

    const QString path = request.filePath.trimmed();
    if (path.isEmpty())
        return fail(QStringLiteral("No program file selected."));

    const QFileInfo fi(path);
    const QString shown = QDir::toNativeSeparators(fi.absoluteFilePath());
    if (!fi.exists())
        return fail(QStringLiteral("File not found: %1").arg(shown));
    if (!fi.isFile())
        return fail(QStringLiteral("Not a file: %1").arg(shown));
    if (fileSize)
        *fileSize = fi.size();
    if (fi.size() == 0)
        return fail(QStringLiteral("File is empty: %1").arg(shown));
    if (request.mode == SendRequest::Mode::DripFeed)
        return StreamingFileSource::open(path, profile, error);
    if (fi.size() > kMaxNormalSendBytes) {
        return fail(QStringLiteral("File is too large for a normal send (%1 MB). Use Drip Feed for files this size.")
                        .arg(fi.size() / (1024 * 1024)));
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Cannot open %1: %2").arg(shown, f.errorString()));
    const QByteArray raw = f.readAll();
    if (f.error() != QFileDevice::NoError)
        return fail(QStringLiteral("Error reading %1: %2").arg(shown, f.errorString()));
    if (!ProgramFormatter::hasProgramContent(raw))
        return fail(QStringLiteral("File has no program in it (only blank lines): %1").arg(shown));

    return std::make_unique<MemoryDataSource>(ProgramFormatter::format(raw, profile), fi.fileName());
}

} // namespace nc
