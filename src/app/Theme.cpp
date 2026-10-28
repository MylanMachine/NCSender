#include "Theme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>

#include <cmath>

using nc::TransferState;

namespace Theme {

namespace {

const char* const kStyleSheet = R"(
QMainWindow, QDialog { background: #eef1f5; }

QMenuBar { background: #ffffff; border-bottom: 1px solid #e3e7ed; }
QMenuBar::item { padding: 5px 10px; background: transparent; }
QMenuBar::item:selected { background: #e8eef7; border-radius: 4px; }
QMenu { background: #ffffff; border: 1px solid #d7dde5; padding: 4px; }
QMenu::item { padding: 6px 28px 6px 20px; border-radius: 4px; }
QMenu::item:selected { background: #e8eef7; color: #1d2530; }
QMenu::separator { height: 1px; background: #e3e7ed; margin: 4px 8px; }

QToolBar { background: #ffffff; border: none; border-bottom: 1px solid #d7dde5; padding: 4px 8px; spacing: 2px; }
QToolBar::separator { width: 1px; background: #e3e7ed; margin: 6px 8px; }
QToolBar QToolButton { padding: 5px 10px; border-radius: 6px; color: #1d2530; }
QToolBar QToolButton:hover { background: #e8eef7; }
QToolBar QToolButton:pressed { background: #d6e2f3; }
QToolBar QToolButton:disabled { color: #a3acb8; }

QStatusBar { background: #ffffff; border-top: 1px solid #d7dde5; color: #5b6675; }
QStatusBar::item { border: none; }

QFrame#machineCard { background: #ffffff; border: 1px solid #d7dde5; border-radius: 10px; }
QLabel#cardTitle { font-size: 13pt; font-weight: 600; color: #1d2530; }
QLabel[muted="true"] { color: #5b6675; }
QLabel#fieldLabel { color: #5b6675; font-weight: 600; }

QLineEdit, QComboBox {
    background: #ffffff; border: 1px solid #c5cdd8; border-radius: 6px;
    padding: 5px 8px; min-height: 18px; selection-background-color: #1f6feb;
}
QLineEdit:focus, QComboBox:focus, QComboBox:on { border-color: #1f6feb; }
QLineEdit:disabled, QComboBox:disabled { background: #f3f5f8; color: #7b8592; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 26px; border: none; }
QComboBox::down-arrow { image: url(:/images/chevron.png); width: 12px; height: 8px; }
QComboBox::down-arrow:disabled { image: url(:/images/chevron-disabled.png); }
QComboBox QAbstractItemView { border: 1px solid #d7dde5; selection-background-color: #e8eef7; selection-color: #1d2530; outline: 0; }

QPushButton {
    background: #ffffff; border: 1px solid #c5cdd8; border-radius: 6px;
    padding: 6px 14px; color: #1d2530;
}
QPushButton:hover { background: #f2f6fb; border-color: #9fb0c4; }
QPushButton:pressed { background: #e3eaf3; }
QPushButton:disabled { color: #a3acb8; background: #f6f7f9; border-color: #e3e7ed; }
QPushButton[size="big"] { min-height: 30px; padding: 6px 18px; font-weight: 600; }
QPushButton[role="primary"] { background: #1f6feb; border-color: #1f6feb; color: #ffffff; }
QPushButton[role="primary"]:hover { background: #1a5fd0; border-color: #1a5fd0; }
QPushButton[role="primary"]:pressed { background: #154ea8; }
QPushButton[role="primary"]:disabled { background: #b4cbf0; border-color: #b4cbf0; color: #f2f6fd; }
QPushButton[role="drip"] { background: #0f8a7e; border-color: #0f8a7e; color: #ffffff; }
QPushButton[role="drip"]:hover { background: #0c766c; border-color: #0c766c; }
QPushButton[role="drip"]:pressed { background: #09625a; }
QPushButton[role="drip"]:disabled { background: #a9d4cf; border-color: #a9d4cf; color: #f1f8f7; }
QPushButton[role="danger"] { color: #c62828; border-color: #e6b0b0; }
QPushButton[role="danger"]:hover { background: #fdecec; border-color: #c62828; }
QPushButton[role="danger"]:pressed { background: #f8d7d7; }
QPushButton[role="danger"]:disabled { color: #a3acb8; background: #f6f7f9; border-color: #e3e7ed; }

QToolButton#followButton { border: 1px solid transparent; border-radius: 5px; padding: 3px 8px; color: #5b6675; }
QToolButton#followButton:hover { background: #eef2f7; }
QToolButton#followButton:checked { background: #e3edfd; color: #1f5fbf; border-color: #c4d7f7; }

QTabWidget::pane { border: 1px solid #d7dde5; border-radius: 6px; top: -1px; background: #ffffff; }
QTabBar::tab {
    background: transparent; padding: 6px 14px; margin-right: 2px; color: #5b6675;
    border: none; border-bottom: 2px solid transparent;
}
QTabBar::tab:hover { color: #1d2530; }
QTabBar::tab:selected { color: #1d2530; border-bottom: 2px solid #1f6feb; font-weight: 600; }

QPlainTextEdit { border: none; background: #ffffff; selection-background-color: #cfe0fb; selection-color: #1d2530; }

QProgressBar { border: none; border-radius: 5px; background: #e6eaf0; min-height: 10px; max-height: 10px; }
QProgressBar::chunk { border-radius: 5px; background: #1f6feb; }

QTableWidget {
    background: #ffffff; border: 1px solid #d7dde5; border-radius: 8px;
    gridline-color: transparent; alternate-background-color: #f7f9fb;
    selection-background-color: #dce8fb; selection-color: #1d2530;
}
QTableWidget::item { padding: 0 6px; border: none; }
QHeaderView::section {
    background: #f3f5f8; border: none; border-bottom: 1px solid #d7dde5;
    padding: 6px 8px; color: #5b6675; font-weight: 600;
}
QTableCornerButton::section { background: #f3f5f8; border: none; }

QGroupBox {
    background: #ffffff; border: 1px solid #d7dde5; border-radius: 8px;
    margin-top: 16px; padding: 12px 8px 8px 8px;
}
QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; color: #1d2530; font-weight: 600; }
QListWidget { background: #ffffff; border: 1px solid #d7dde5; border-radius: 8px; padding: 4px; outline: 0; }
QListWidget::item { padding: 6px 8px; border-radius: 5px; }
QListWidget::item:selected { background: #e3edfd; color: #1d2530; }

QSplitter::handle { background: transparent; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: #c8d0db; border-radius: 4px; min-height: 30px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:horizontal { background: #c8d0db; border-radius: 4px; min-width: 30px; margin: 2px; }
QScrollBar::handle:hover { background: #aab5c3; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QToolTip { background: #1d2530; color: #ffffff; border: none; padding: 6px; }
)";

QPen strokePen(const QColor& c, qreal width = 6.0)
{
    return QPen(c, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

// Draws one icon on a 64 x 64 canvas.
void paintIcon(QPainter& p, Icon which, const QColor& c)
{
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    p.setPen(strokePen(c));
    switch (which) {
    case Icon::Send:
    case Icon::Receive: {
        QPainterPath tray;
        tray.moveTo(10, 38);
        tray.lineTo(10, 54);
        tray.lineTo(54, 54);
        tray.lineTo(54, 38);
        p.drawPath(tray);
        if (which == Icon::Send) {
            p.drawLine(QPointF(32, 42), QPointF(32, 8));
            p.drawPolyline(QPolygonF({QPointF(19, 21), QPointF(32, 8), QPointF(45, 21)}));
        } else {
            p.drawLine(QPointF(32, 8), QPointF(32, 42));
            p.drawPolyline(QPolygonF({QPointF(19, 29), QPointF(32, 42), QPointF(45, 29)}));
        }
        break;
    }
    case Icon::Drip: {
        QPainterPath drop;
        drop.moveTo(32, 5);
        drop.cubicTo(38, 16, 52, 28, 52, 40);
        drop.cubicTo(52, 51, 43, 59, 32, 59);
        drop.cubicTo(21, 59, 12, 51, 12, 40);
        drop.cubicTo(12, 28, 26, 16, 32, 5);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPath(drop);
        break;
    }
    case Icon::Stop:
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(14, 14, 36, 36), 6, 6);
        break;
    case Icon::Folder: {
        QPainterPath f;
        f.moveTo(8, 50);
        f.lineTo(8, 14);
        f.lineTo(25, 14);
        f.lineTo(31, 21);
        f.lineTo(56, 21);
        f.lineTo(56, 50);
        f.closeSubpath();
        p.setPen(strokePen(c, 5));
        p.drawPath(f);
        break;
    }
    case Icon::Refresh: {
        const QRectF r(12, 12, 40, 40);
        const double start = 70.0;
        const double span = 285.0;
        p.drawArc(r, int(start * 16), int(span * 16));
        // Arrow head at the start of the arc, pointing clockwise.
        const double a = start * 3.14159265358979 / 180.0;
        const QPointF tip0(32 + 20 * std::cos(a), 32 - 20 * std::sin(a));
        const QPointF dir(std::sin(a), std::cos(a));
        const QPointF n(-dir.y(), dir.x());
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF({tip0 + dir * 10, tip0 - dir * 3 + n * 9, tip0 - dir * 3 - n * 9}));
        break;
    }
    case Icon::Profiles: {
        const double ys[] = {16, 32, 48};
        const double knobs[] = {22, 42, 30};
        p.setPen(strokePen(c, 5));
        for (double y : ys)
            p.drawLine(QPointF(8, y), QPointF(56, y));
        p.setBrush(Qt::white);
        for (int i = 0; i < 3; ++i)
            p.drawEllipse(QPointF(knobs[i], ys[i]), 6, 6);
        break;
    }
    case Icon::Add:
        p.drawLine(QPointF(32, 12), QPointF(32, 52));
        p.drawLine(QPointF(12, 32), QPointF(52, 32));
        break;
    case Icon::Remove:
        p.drawLine(QPointF(12, 32), QPointF(52, 32));
        break;
    }
}

QPixmap iconPixmap(Icon which, const QColor& color)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paintIcon(p, which, color);
    return pm;
}

} // namespace

void apply(QApplication& app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette pal;
    pal.setColor(QPalette::Window, Color::window);
    pal.setColor(QPalette::WindowText, Color::text);
    pal.setColor(QPalette::Base, Color::card);
    pal.setColor(QPalette::AlternateBase, QColor(0xf7, 0xf9, 0xfb));
    pal.setColor(QPalette::Text, Color::text);
    pal.setColor(QPalette::Button, Color::card);
    pal.setColor(QPalette::ButtonText, Color::text);
    pal.setColor(QPalette::Highlight, Color::accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::ToolTipBase, Color::text);
    pal.setColor(QPalette::ToolTipText, Qt::white);
    pal.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x94, 0xa3));
    pal.setColor(QPalette::Link, Color::accent);
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor(0x8a, 0x94, 0xa3));
    pal.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x8a, 0x94, 0xa3));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0xa3, 0xac, 0xb8));
    app.setPalette(pal);

    QFont f = app.font();
    if (QFontDatabase::families().contains(QStringLiteral("Segoe UI")))
        f.setFamily(QStringLiteral("Segoe UI"));
    f.setPointSizeF(10);
    app.setFont(f);

    app.setStyleSheet(QString::fromLatin1(kStyleSheet));
}

QFont codeFont()
{
    const QStringList families = QFontDatabase::families();
    QFont f;
    for (const char* name : {"Cascadia Mono", "Consolas", "Courier New"}) {
        if (families.contains(QLatin1String(name))) {
            f = QFont(QLatin1String(name));
            break;
        }
    }
    if (f.family().isEmpty() || !families.contains(f.family()))
        f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    f.setStyleHint(QFont::Monospace);
    f.setPointSizeF(10);
    return f;
}

QIcon icon(Icon which, const QColor& color, const QColor& disabledColor)
{
    QIcon i;
    i.addPixmap(iconPixmap(which, color), QIcon::Normal);
    i.addPixmap(iconPixmap(which, disabledColor.isValid() ? disabledColor : QColor(0xa3, 0xac, 0xb8)),
                QIcon::Disabled);
    return i;
}

StateColors stateColors(TransferState s)
{
    switch (s) {
    case TransferState::Idle: return {Color::muted, QColor(0xee, 0xf1, 0xf5), Color::accent};
    case TransferState::Opening:
    case TransferState::Sending:
    case TransferState::Receiving:
    case TransferState::Finishing: return {QColor(0x15, 0x58, 0xc0), QColor(0xe3, 0xed, 0xfd), Color::accent};
    case TransferState::WaitingForXon:
    case TransferState::Paused: return {QColor(0x9a, 0x5b, 0x00), QColor(0xfd, 0xf1, 0xdc), QColor(0xe0, 0x9b, 0x1f)};
    case TransferState::Cancelled: return {QColor(0x8a, 0x55, 0x00), QColor(0xf6, 0xee, 0xe2), QColor(0xc9, 0xa2, 0x6b)};
    case TransferState::Done: return {QColor(0x1e, 0x6b, 0x25), QColor(0xe3, 0xf3, 0xe5), QColor(0x2e, 0x9d, 0x45)};
    case TransferState::Error: return {QColor(0xb0, 0x1f, 0x1f), QColor(0xfd, 0xe7, 0xe7), Color::danger};
    }
    return {Color::muted, Color::window, Color::accent};
}

} // namespace Theme
