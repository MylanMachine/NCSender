#pragma once

#include <QPlainTextEdit>

class QTimer;

// Read-only program view: line numbers, G-code colouring and a marker on the
// line currently being sent.
//
// Colouring is applied only to the lines on screen, as they scroll into
// view, so a multi-megabyte drip feed program opens instantly (a
// QSyntaxHighlighter would colour every line up front).
class CodeView : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeView(QWidget* parent = nullptr);

    // 0-based line to mark, or -1 for none. With follow on, the view scrolls
    // to keep it in sight.
    void setActiveLine(int line);
    int activeLine() const { return m_active; }
    void setFollow(bool on) { m_follow = on; }

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    friend class CodeGutter;

    int gutterWidth() const;
    void paintGutter(QPaintEvent* event);
    void onUpdateRequest(const QRect& rect, int dy);
    void colourVisibleLines();
    void scrollToActive();

    QWidget* m_gutter = nullptr;
    QTimer* m_colourTimer = nullptr;
    int m_active = -1;
    bool m_follow = true;
};
