#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace nc {

enum class Parity { None, Even, Odd, Mark, Space };
enum class StopBits { One, OneAndHalf, Two };
enum class FlowControl { None, XonXoff, RtsCts };
enum class LineEnding { CR, LF, CRLF };

// ASCII device-control codes used by Fanuc and other RS232 controls.
constexpr char kXon = 0x11;  // DC1: "ready to receive"
constexpr char kDc2 = 0x12;  // DC2: punch on
constexpr char kXoff = 0x13; // DC3: "stop sending"
constexpr char kDc4 = 0x14;  // DC4: punch off

// Stable strings used in profiles.json.
QString toString(Parity v);
QString toString(StopBits v);
QString toString(FlowControl v);
QString toString(LineEnding v);

Parity parityFromString(const QString& s, Parity fallback = Parity::Even);
StopBits stopBitsFromString(const QString& s, StopBits fallback = StopBits::Two);
FlowControl flowControlFromString(const QString& s, FlowControl fallback = FlowControl::XonXoff);
LineEnding lineEndingFromString(const QString& s, LineEnding fallback = LineEnding::CRLF);

// Labels for the UI.
QString displayName(Parity v);
QString displayName(StopBits v);
QString displayName(FlowControl v);
QString displayName(LineEnding v);
QChar parityLetter(Parity v);

QByteArray lineEndingBytes(LineEnding v);

// Bits on the wire per character (start + data + parity + stop), used to
// estimate transmit time.
double bitsPerCharacter(int dataBits, Parity parity, StopBits stopBits);

const QList<qint32>& standardBaudRates();
const QList<Parity>& allParities();
const QList<StopBits>& allStopBits();
const QList<FlowControl>& allFlowControls();
const QList<LineEnding>& allLineEndings();

} // namespace nc
