#pragma once

#include "TransferTypes.h"

#include <QColor>
#include <QFont>
#include <QIcon>

class QApplication;

// The app's look: one light theme on Qt's Fusion style, so it looks the same
// on every Windows version and in the static single-exe build. Icons are
// drawn in code because the static build has no SVG support.
namespace Theme {

// Fusion style, palette, fonts and the application style sheet.
void apply(QApplication& app);

// Monospaced font for program text.
QFont codeFont();

enum class Icon { Send, Receive, Drip, Stop, Folder, Refresh, Profiles, Add, Remove };

// A crisp icon in the given colour, with a greyed version for disabled buttons.
QIcon icon(Icon which, const QColor& color, const QColor& disabledColor = QColor());

// Colours for a transfer state: text, a light badge background, and the
// progress bar fill.
struct StateColors
{
    QColor text;
    QColor background;
    QColor bar;
};
StateColors stateColors(nc::TransferState state);

// Palette shared with widgets that paint themselves.
namespace Color {
inline const QColor window{0xee, 0xf1, 0xf5};
inline const QColor card{0xff, 0xff, 0xff};
inline const QColor border{0xd7, 0xdd, 0xe5};
inline const QColor text{0x1d, 0x25, 0x30};
inline const QColor muted{0x5b, 0x66, 0x75};
inline const QColor faint{0x9a, 0xa4, 0xb1};
inline const QColor accent{0x1f, 0x6f, 0xeb};
inline const QColor drip{0x0f, 0x8a, 0x7e};
inline const QColor danger{0xc6, 0x28, 0x28};
inline const QColor success{0x2e, 0x7d, 0x32};
inline const QColor warning{0xb2, 0x6a, 0x00};
inline const QColor gutter{0xf3, 0xf5, 0xf8};
inline const QColor activeLine{0xff, 0xf1, 0xbf};
} // namespace Color

} // namespace Theme
