#include "TransferTypes.h"

namespace nc {

QString toString(TransferDirection d)
{
    return d == TransferDirection::Send ? QStringLiteral("Send") : QStringLiteral("Receive");
}

QString toString(TransferState s)
{
    switch (s) {
    case TransferState::Idle: return QStringLiteral("Idle");
    case TransferState::Opening: return QStringLiteral("Opening");
    case TransferState::WaitingForXon: return QStringLiteral("Waiting for XON");
    case TransferState::Sending: return QStringLiteral("Sending");
    case TransferState::Paused: return QStringLiteral("Paused");
    case TransferState::Receiving: return QStringLiteral("Receiving");
    case TransferState::Finishing: return QStringLiteral("Finishing");
    case TransferState::Done: return QStringLiteral("Done");
    case TransferState::Cancelled: return QStringLiteral("Cancelled");
    case TransferState::Error: return QStringLiteral("Error");
    }
    return {};
}

QString toString(TransferOutcome o)
{
    switch (o) {
    case TransferOutcome::Success: return QStringLiteral("Success");
    case TransferOutcome::Cancelled: return QStringLiteral("Cancelled");
    case TransferOutcome::Failed: return QStringLiteral("Failed");
    }
    return {};
}

void registerTransferMetaTypes()
{
    static const bool once = [] {
        qRegisterMetaType<nc::TransferDirection>();
        qRegisterMetaType<nc::TransferState>();
        qRegisterMetaType<nc::TransferOutcome>();
        qRegisterMetaType<nc::TransferResult>();
        return true;
    }();
    Q_UNUSED(once);
}

} // namespace nc
