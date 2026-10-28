#include "ProgramFormatter.h"

namespace nc::ProgramFormatter {

// Ctrl+Z has no meaning in an NC program; old editors leave it at the end,
// sometimes followed by a line ending.
QByteArray cleaned(const QByteArray& raw)
{
    QByteArray data = raw;
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);
    data.replace('\x1A', QByteArray());
    return data;
}

void PercentPlanner::addLine(const QByteArray& line)
{
    const int i = m_count++;
    const QByteArray t = line.trimmed();
    if (t.isEmpty())
        return;
    if (m_first < 0)
        m_first = i;
    m_last = i;
    if (t.startsWith('%')) {
        if (m_firstPercent < 0)
            m_firstPercent = i;
        ++m_percentCount;
    }
}

PercentPlanner::Plan PercentPlanner::plan() const
{
    Plan p;
    if (m_first < 0) {
        p.emptyProgram = true;
    } else if (m_percentCount == 0) {
        // No % anywhere: wrap the whole program.
        p.openBefore = m_first;
        p.closeAfter = m_last;
    } else if (m_percentCount == 1) {
        if (m_firstPercent < m_last) {
            // The one % opens the program (there may be header comments
            // above it, which the control skips); add the closing one.
            p.closeAfter = m_last;
        } else {
            // The one % is at the end: it closes the program, add an opening one.
            p.openBefore = m_first;
        }
    }
    // Two or more % lines: the file already has its start and end, leave it alone.
    return p;
}

QList<QByteArray> splitLines(const QByteArray& raw)
{
    QList<QByteArray> lines;
    qsizetype start = 0;
    const qsizetype n = raw.size();
    for (qsizetype i = 0; i < n; ++i) {
        const char c = raw.at(i);
        if (c == '\r' || c == '\n') {
            lines.append(raw.mid(start, i - start));
            if (c == '\r') {
                // CR LF, and the CR CR LF some old editors write, are one line break.
                if (i + 2 < n && raw.at(i + 1) == '\r' && raw.at(i + 2) == '\n')
                    i += 2;
                else if (i + 1 < n && raw.at(i + 1) == '\n')
                    ++i;
            }
            start = i + 1;
        }
    }
    if (start < n)
        lines.append(raw.mid(start));
    return lines;
}

bool hasProgramContent(const QByteArray& raw)
{
    for (const char c : cleaned(raw)) {
        if (static_cast<uchar>(c) > 0x20 && c != '\x7f')
            return true;
    }
    return false;
}

QByteArray format(const QByteArray& raw, const MachineProfile& profile)
{
    const QByteArray data = cleaned(raw);
    QList<QByteArray> lines = splitLines(data);

    PercentPlanner::Plan plan;
    if (profile.ensurePercent) {
        PercentPlanner planner;
        for (const QByteArray& line : lines)
            planner.addLine(line);
        plan = planner.plan();
        if (plan.emptyProgram)
            lines = {QByteArrayLiteral("%"), QByteArrayLiteral("%")};
    }

    const QByteArray eol = lineEndingBytes(profile.lineEnding);
    QByteArray out;
    out.reserve(data.size() + lines.size() * 2 + profile.leaderCount + profile.trailerCount + 8);
    if (profile.leaderCount > 0)
        out.append(QByteArray(profile.leaderCount, static_cast<char>(profile.leaderByte & 0xFF)));
    for (int i = 0; i < lines.size(); ++i) {
        if (i == plan.openBefore)
            out.append('%').append(eol);
        out.append(lines.at(i));
        out.append(eol);
        if (i == plan.closeAfter)
            out.append('%').append(eol);
    }
    if (profile.trailerCount > 0)
        out.append(QByteArray(profile.trailerCount, static_cast<char>(profile.trailerByte & 0xFF)));
    return out;
}

QList<qint64> lineEndOffsets(const QList<qint64>& lineLengths, const PercentPlanner::Plan& plan,
                             const MachineProfile& profile)
{
    const qint64 eol = lineEndingBytes(profile.lineEnding).size();
    const bool percent = profile.ensurePercent && !plan.emptyProgram;
    QList<qint64> ends;
    ends.reserve(lineLengths.size());
    qint64 off = qMax(0, profile.leaderCount);
    for (int i = 0; i < lineLengths.size(); ++i) {
        if (percent && i == plan.openBefore)
            off += 1 + eol;
        off += lineLengths.at(i) + eol;
        ends.append(off);
        if (percent && i == plan.closeAfter)
            off += 1 + eol;
    }
    return ends;
}

ProgramStats stats(const QByteArray& raw)
{
    ProgramStats s;
    s.bytes = raw.size();
    // Counted without building the line list, so it is cheap on big files.
    const qsizetype n = raw.size();
    qsizetype i = 0;
    bool lineOpen = false;
    while (i < n) {
        const char c = raw.at(i);
        if (c == '\r' || c == '\n') {
            ++s.lines;
            lineOpen = false;
            if (c == '\r') {
                if (i + 2 < n && raw.at(i + 1) == '\r' && raw.at(i + 2) == '\n')
                    i += 2;
                else if (i + 1 < n && raw.at(i + 1) == '\n')
                    ++i;
            }
        } else {
            lineOpen = true;
        }
        ++i;
    }
    if (lineOpen)
        ++s.lines;
    return s;
}

} // namespace nc::ProgramFormatter
