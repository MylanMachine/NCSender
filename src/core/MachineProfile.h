#pragma once

#include "SerialSettings.h"

#include <QJsonObject>
#include <QString>

namespace nc {

// Everything needed to talk to one machine control. Plain value type: a copy
// is handed to the transfer worker at the start of each transfer, so editing
// profiles never affects a transfer that is already running.
struct MachineProfile
{
    QString name = QStringLiteral("New Machine");

    // Port settings
    QString portName = QStringLiteral("COM1");
    qint32 baudRate = 9600;
    int dataBits = 7;
    Parity parity = Parity::Even;
    StopBits stopBits = StopBits::Two;
    FlowControl flowControl = FlowControl::XonXoff;

    // Sending
    LineEnding lineEnding = LineEnding::CRLF;
    int leaderByte = 0;      // character code sent before the program (0 = NUL tape feed)
    int leaderCount = 0;     // number of leader characters, 0 = none
    int trailerByte = 0;
    int trailerCount = 0;
    bool ensurePercent = true; // add % at start/end if the file is missing them
    int charDelayMs = 0;     // pause after every character, 0 = full speed
    bool waitForXon = false; // hold the transfer until the control sends XON (DC1)
    int xonTimeoutSec = 60;  // 0 = wait until cancelled
    QString programFolder;

    // Receiving (punch from the control)
    QString receiveFolder;   // empty = <programFolder>\received, or <app folder>\received
    QString receiveExtension = QStringLiteral("nc");
    int receiveStartTimeoutSec = 0;  // waiting for the first byte; 0 = until cancelled
    int receiveIdleTimeoutSec = 10;  // silence after data started; 0 = stop only on closing %
    bool stripReceivedControlChars = true; // drop NUL leader and DC1-DC4 codes from the saved file

    QJsonObject toJson() const;
    static MachineProfile fromJson(const QJsonObject& o);

    // Returns an empty string if the profile is usable, otherwise a message.
    QString validate() const;

    // One-line description such as "COM3  9600 7-E-2  XON/XOFF  CR LF".
    QString summary() const;

    QString effectiveReceiveFolder() const;
};

} // namespace nc
