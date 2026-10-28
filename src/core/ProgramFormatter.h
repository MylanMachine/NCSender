#pragma once

#include "MachineProfile.h"

#include <QByteArray>
#include <QList>

namespace nc {

struct ProgramStats
{
    qint64 bytes = 0;
    int lines = 0;
};

// Turns a program file on disk into the exact bytes that go out on the wire.
namespace ProgramFormatter {

// Decides where format() adds % lines (step 3 below). Feed it every line of
// the file in order, then ask for the plan. Shared by format() and the drip
// feed source, which streams the file and cannot hold every line at once.
class PercentPlanner
{
public:
    struct Plan
    {
        int openBefore = -1;   // add a % line before this line index (-1 = none)
        int closeAfter = -1;   // add a % line after this line index (-1 = none)
        bool emptyProgram = false; // no non-blank lines: the output is just two % lines
    };

    void addLine(const QByteArray& line);
    Plan plan() const;

private:
    int m_count = 0;
    int m_first = -1;
    int m_last = -1;
    int m_percentCount = 0;
    int m_firstPercent = -1;
};

// Removes a UTF-8 BOM and every DOS end-of-file (Ctrl+Z) byte.
QByteArray cleaned(const QByteArray& raw);

// Splits on CR LF, LF or lone CR. A trailing line ending does not add an
// empty last line.
QList<QByteArray> splitLines(const QByteArray& raw);

// True if the file contains anything other than whitespace, a BOM or
// end-of-file characters.
bool hasProgramContent(const QByteArray& raw);

// Steps:
//  1. strip a UTF-8 BOM and DOS end-of-file (Ctrl+Z) characters
//  2. normalize every line ending to the profile's line ending
//  3. if ensurePercent, by counting lines that start with %:
//       none -> add % before the first and after the last non-blank line
//       one  -> it is the opening % if program lines follow it (header
//               comments above it are left alone), so add a closing %;
//               otherwise it is the closing %, so add an opening one
//       two or more -> leave the file as it is
//  4. add leader characters before and trailer characters after
QByteArray format(const QByteArray& raw, const MachineProfile& profile);

ProgramStats stats(const QByteArray& raw);

// For each line of splitLines(cleaned(raw)), given as its length, the byte
// offset in format()'s output just past that line's line ending. Lets the UI
// turn "bytes sent" into "line being sent".
QList<qint64> lineEndOffsets(const QList<qint64>& lineLengths, const PercentPlanner::Plan& plan,
                             const MachineProfile& profile);

} // namespace ProgramFormatter
} // namespace nc
