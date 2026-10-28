#include "CodeView.h"

#include "Theme.h"

#include <QPainter>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>
#include <QTimer>

namespace {

constexpr int kColoured = 1; // QTextBlock::userState once a line has been coloured

QColor wordColour(QChar letter)
{
    switch (letter.toUpper().toLatin1()) {
    case 'N': return QColor(0x9a, 0xa4, 0xb1);
    case 'G': return QColor(0x1f, 0x5f, 0xbf);
    case 'M': return QColor(0xa1, 0x2d, 0x84);
    case 'O': return QColor(0x6f, 0x42, 0xc1);
    case 'T':
    case 'S':
    case 'F':
    case 'H':
    case 'D': return QColor(0xb2, 0x5e, 0x00);
    case 'X':
    case 'Y':
    case 'Z':
    case 'A':
    case 'B':
    case 'C':
    case 'I':
    case 'J':
    case 'K':
    case 'R':
    case 'U':
    case 'V':
    case 'W':
    case 'Q':
    case 'P':
    case 'L':
    case 'E': return QColor(0x0b, 0x6e, 0x75);
    default: return QColor();
    }
}

// Colour ranges for one line of a Fanuc-style program. Colours only, so the
// line's layout never changes width.
QList<QTextLayout::FormatRange> colourLine(const QString& text)
{
    QList<QTextLayout::FormatRange> ranges;
    auto add = [&](int start, int length, const QColor& c) {
        if (length <= 0 || !c.isValid())
            return;
        QTextLayout::FormatRange r;
        r.start = start;
        r.length = length;
        r.format.setForeground(c);
        ranges.append(r);
    };

    const int n = int(text.size());
    int i = 0;
    while (i < n && text.at(i).isSpace())
        ++i;
    if (i < n && text.at(i) == QLatin1Char('%')) {
        add(i, n - i, Theme::Color::danger);
        return ranges;
    }

    const QColor comment(0x2f, 0x7d, 0x4f);
    while (i < n) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('(')) {
            int end = int(text.indexOf(QLatin1Char(')'), i));
            end = end < 0 ? n : end + 1;
            add(i, end - i, comment);
            i = end;
        } else if (c == QLatin1Char(';')) {
            add(i, n - i, comment);
            break;
        } else if (c.isLetter() || c == QLatin1Char(':')) {
            int end = i + 1;
            while (end < n) {
                const QChar d = text.at(end);
                if (!(d.isDigit() || d == QLatin1Char('.') || d == QLatin1Char('-') || d == QLatin1Char('+')
                      || d == QLatin1Char('#') || d == QLatin1Char(' ')))
                    break;
                ++end;
            }
            while (end > i + 1 && text.at(end - 1) == QLatin1Char(' '))
                --end;
            add(i, end - i, c == QLatin1Char(':') ? wordColour(QLatin1Char('O')) : wordColour(c));
            i = end;
        } else {
            ++i;
        }
    }
    return ranges;
}

} // namespace

class CodeGutter : public QWidget
{
public:
    explicit CodeGutter(CodeView* view)
        : QWidget(view)
        , m_view(view)
    {
    }
    QSize sizeHint() const override { return QSize(m_view->gutterWidth(), 0); }

protected:
    void paintEvent(QPaintEvent* event) override { m_view->paintGutter(event); }

private:
    CodeView* m_view;
};

CodeView::CodeView(QWidget* parent)
    : QPlainTextEdit(parent)
{
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(Theme::codeFont());
    setFrameShape(QFrame::NoFrame);

    m_gutter = new CodeGutter(this);
    m_colourTimer = new QTimer(this);
    m_colourTimer->setSingleShot(true);
    m_colourTimer->setInterval(0);
    connect(m_colourTimer, &QTimer::timeout, this, &CodeView::colourVisibleLines);

    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] {
        setViewportMargins(gutterWidth(), 0, 0, 0);
        if (m_active >= blockCount())
            setActiveLine(-1);
    });
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeView::onUpdateRequest);
    // Lines that change (live received data) are coloured again.
    connect(document(), &QTextDocument::contentsChange, this, [this](int pos, int, int added) {
        QTextBlock block = document()->findBlock(pos);
        const QTextBlock last = document()->findBlock(pos + added);
        while (block.isValid()) {
            block.setUserState(-1);
            if (block == last)
                break;
            block = block.next();
        }
        m_colourTimer->start();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, m_colourTimer, qOverload<>(&QTimer::start));
    setViewportMargins(gutterWidth(), 0, 0, 0);
}

int CodeView::gutterWidth() const
{
    int digits = 1;
    for (int max = qMax(1, blockCount()); max >= 10; max /= 10)
        ++digits;
    digits = qMax(digits, 3);
    return 20 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeView::onUpdateRequest(const QRect& rect, int dy)
{
    if (dy)
        m_gutter->scroll(0, dy);
    else
        m_gutter->update(0, rect.y(), m_gutter->width(), rect.height());
    m_colourTimer->start();
}

void CodeView::resizeEvent(QResizeEvent* event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_gutter->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
    m_colourTimer->start();
}

void CodeView::paintGutter(QPaintEvent* event)
{
    QPainter p(m_gutter);
    p.fillRect(event->rect(), Theme::Color::gutter);
    p.setPen(Theme::Color::border);
    p.drawLine(m_gutter->width() - 1, event->rect().top(), m_gutter->width() - 1, event->rect().bottom());

    QFont normal = font();
    QFont bold = font();
    bold.setBold(true);
    QTextBlock block = firstVisibleBlock();
    int number = block.blockNumber();
    qreal top = blockBoundingGeometry(block).translated(contentOffset()).top();
    while (block.isValid() && top <= event->rect().bottom()) {
        const qreal height = blockBoundingRect(block).height();
        if (block.isVisible() && top + height >= event->rect().top()) {
            const QRectF r(0, top, m_gutter->width() - 8, height);
            if (number == m_active) {
                p.fillRect(QRectF(0, top, m_gutter->width() - 1, height), Theme::Color::activeLine);
                p.fillRect(QRectF(0, top, 3, height), Theme::Color::warning);
                p.setFont(bold);
                p.setPen(Theme::Color::text);
            } else {
                p.setFont(normal);
                p.setPen(Theme::Color::faint);
            }
            p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, QString::number(number + 1));
        }
        block = block.next();
        top += height;
        ++number;
    }
}

void CodeView::colourVisibleLines()
{
    // Colouring a line re-lays it out, which asks for another repaint and
    // re-runs this through the timer; that pass finds nothing left to do.
    const int bottom = viewport()->height();
    QTextBlock block = firstVisibleBlock();
    qreal top = block.isValid() ? blockBoundingGeometry(block).translated(contentOffset()).top() : 0;
    while (block.isValid() && top <= bottom) {
        top += blockBoundingRect(block).height();
        if (block.userState() != kColoured) {
            block.setUserState(kColoured);
            block.layout()->setFormats(colourLine(block.text()));
            document()->markContentsDirty(block.position(), block.length());
        }
        block = block.next();
    }
}

void CodeView::setActiveLine(int line)
{
    if (line >= blockCount())
        line = -1;
    if (line == m_active)
        return;
    m_active = line;

    QList<QTextEdit::ExtraSelection> sel;
    if (line >= 0) {
        QTextEdit::ExtraSelection s;
        s.format.setBackground(Theme::Color::activeLine);
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        s.cursor = QTextCursor(document()->findBlockByNumber(line));
        sel.append(s);
    }
    setExtraSelections(sel);
    if (line >= 0 && m_follow)
        scrollToActive();
    m_gutter->update();
}

void CodeView::scrollToActive()
{
    // With wrapping off, the scroll bar counts lines. Only move when the line
    // gets near an edge, so the view steps along instead of jittering.
    QScrollBar* sb = verticalScrollBar();
    const int lineHeight = qMax(1, fontMetrics().lineSpacing());
    const int visible = qMax(1, viewport()->height() / lineHeight);
    const int first = sb->value();
    if (m_active >= first + 2 && m_active <= first + visible - 3)
        return;
    sb->setValue(qMax(0, m_active - visible / 3));
}
