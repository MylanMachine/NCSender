#include "SerialSettings.h"

namespace nc {

QString toString(Parity v)
{
    switch (v) {
    case Parity::None: return QStringLiteral("none");
    case Parity::Even: return QStringLiteral("even");
    case Parity::Odd: return QStringLiteral("odd");
    case Parity::Mark: return QStringLiteral("mark");
    case Parity::Space: return QStringLiteral("space");
    }
    return QStringLiteral("even");
}

QString toString(StopBits v)
{
    switch (v) {
    case StopBits::One: return QStringLiteral("1");
    case StopBits::OneAndHalf: return QStringLiteral("1.5");
    case StopBits::Two: return QStringLiteral("2");
    }
    return QStringLiteral("2");
}

QString toString(FlowControl v)
{
    switch (v) {
    case FlowControl::None: return QStringLiteral("none");
    case FlowControl::XonXoff: return QStringLiteral("xonxoff");
    case FlowControl::RtsCts: return QStringLiteral("rtscts");
    }
    return QStringLiteral("xonxoff");
}

QString toString(LineEnding v)
{
    switch (v) {
    case LineEnding::CR: return QStringLiteral("CR");
    case LineEnding::LF: return QStringLiteral("LF");
    case LineEnding::CRLF: return QStringLiteral("CRLF");
    }
    return QStringLiteral("CRLF");
}

Parity parityFromString(const QString& s, Parity fallback)
{
    const QString t = s.trimmed().toLower();
    if (t == QLatin1String("none") || t == QLatin1String("n")) return Parity::None;
    if (t == QLatin1String("even") || t == QLatin1String("e")) return Parity::Even;
    if (t == QLatin1String("odd") || t == QLatin1String("o")) return Parity::Odd;
    if (t == QLatin1String("mark") || t == QLatin1String("m")) return Parity::Mark;
    if (t == QLatin1String("space") || t == QLatin1String("s")) return Parity::Space;
    return fallback;
}

StopBits stopBitsFromString(const QString& s, StopBits fallback)
{
    const QString t = s.trimmed();
    if (t == QLatin1String("1")) return StopBits::One;
    if (t == QLatin1String("1.5")) return StopBits::OneAndHalf;
    if (t == QLatin1String("2")) return StopBits::Two;
    return fallback;
}

FlowControl flowControlFromString(const QString& s, FlowControl fallback)
{
    QString t = s.trimmed().toLower();
    t.remove(QLatin1Char('/'));
    t.remove(QLatin1Char(' '));
    if (t == QLatin1String("none") || t == QLatin1String("off")) return FlowControl::None;
    if (t == QLatin1String("xonxoff") || t == QLatin1String("software")) return FlowControl::XonXoff;
    if (t == QLatin1String("rtscts") || t == QLatin1String("hardware")) return FlowControl::RtsCts;
    return fallback;
}

LineEnding lineEndingFromString(const QString& s, LineEnding fallback)
{
    QString t = s.trimmed().toUpper();
    t.remove(QLatin1Char(' '));
    t.remove(QLatin1Char('+'));
    if (t == QLatin1String("CR")) return LineEnding::CR;
    if (t == QLatin1String("LF")) return LineEnding::LF;
    if (t == QLatin1String("CRLF")) return LineEnding::CRLF;
    return fallback;
}

QString displayName(Parity v)
{
    switch (v) {
    case Parity::None: return QStringLiteral("None");
    case Parity::Even: return QStringLiteral("Even");
    case Parity::Odd: return QStringLiteral("Odd");
    case Parity::Mark: return QStringLiteral("Mark");
    case Parity::Space: return QStringLiteral("Space");
    }
    return {};
}

QString displayName(StopBits v)
{
    return toString(v);
}

QString displayName(FlowControl v)
{
    switch (v) {
    case FlowControl::None: return QStringLiteral("None");
    case FlowControl::XonXoff: return QStringLiteral("XON/XOFF (software)");
    case FlowControl::RtsCts: return QStringLiteral("RTS/CTS (hardware)");
    }
    return {};
}

QString displayName(LineEnding v)
{
    switch (v) {
    case LineEnding::CR: return QStringLiteral("CR");
    case LineEnding::LF: return QStringLiteral("LF");
    case LineEnding::CRLF: return QStringLiteral("CR LF");
    }
    return {};
}

QChar parityLetter(Parity v)
{
    switch (v) {
    case Parity::None: return QLatin1Char('N');
    case Parity::Even: return QLatin1Char('E');
    case Parity::Odd: return QLatin1Char('O');
    case Parity::Mark: return QLatin1Char('M');
    case Parity::Space: return QLatin1Char('S');
    }
    return QLatin1Char('?');
}

QByteArray lineEndingBytes(LineEnding v)
{
    switch (v) {
    case LineEnding::CR: return QByteArrayLiteral("\r");
    case LineEnding::LF: return QByteArrayLiteral("\n");
    case LineEnding::CRLF: return QByteArrayLiteral("\r\n");
    }
    return QByteArrayLiteral("\r\n");
}

double bitsPerCharacter(int dataBits, Parity parity, StopBits stopBits)
{
    double bits = 1.0 + dataBits;
    if (parity != Parity::None)
        bits += 1.0;
    switch (stopBits) {
    case StopBits::One: bits += 1.0; break;
    case StopBits::OneAndHalf: bits += 1.5; break;
    case StopBits::Two: bits += 2.0; break;
    }
    return bits;
}

const QList<qint32>& standardBaudRates()
{
    static const QList<qint32> v{110, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    return v;
}

const QList<Parity>& allParities()
{
    static const QList<Parity> v{Parity::None, Parity::Even, Parity::Odd, Parity::Mark, Parity::Space};
    return v;
}

const QList<StopBits>& allStopBits()
{
    static const QList<StopBits> v{StopBits::One, StopBits::OneAndHalf, StopBits::Two};
    return v;
}

const QList<FlowControl>& allFlowControls()
{
    static const QList<FlowControl> v{FlowControl::XonXoff, FlowControl::RtsCts, FlowControl::None};
    return v;
}

const QList<LineEnding>& allLineEndings()
{
    static const QList<LineEnding> v{LineEnding::CRLF, LineEnding::LF, LineEnding::CR};
    return v;
}

} // namespace nc
