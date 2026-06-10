#include "PythonCodeEditor.h"
#include "PythonHighlighter.h"   // single source of truth for the completion-popup colours
#include "FrameBufferReader.h"   // the position picker shows the live rendered frame
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QTextBlock>
#include <QToolTip>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QCompleter>
#include <QStandardItemModel>
#include <QStandardItem>
#include <QAbstractItemView>
#include <QScrollBar>
#include <QScrollArea>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QSet>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QApplication>
#include <QTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QMouseEvent>
#include <QColor>
#include <QLinearGradient>
#include <QImage>
#include <QRegularExpression>
#include <functional>
#include <QWheelEvent>
#include <QEnterEvent>
#include <QEvent>

// Shared, lazily-built member-completion table (the engine API is global).
QMap<QString, QMap<QString, MemberInfo>> PythonCodeEditor::s_members;
QMap<QString, QString>                   PythonCodeEditor::s_globalTypes;
QMap<QString, MemberInfo>                PythonCodeEditor::s_classInfo;
bool                                     PythonCodeEditor::s_tableBuilt = false;

// Completion-popup row colour for a category, pulled from the syntax highlighter so the
// two never drift apart (see PythonHighlighter's static colour accessors).
static QColor colorForKind(CompletionKind k) {
    switch (k) {
    case CompletionKind::Class:    return PythonHighlighter::classColor();
    case CompletionKind::Callable: return PythonHighlighter::callableColor();
    case CompletionKind::Variable:
    default:                       return PythonHighlighter::variableColor();
    }
}

// The docstring body (and the footer hint) render in dotim3 ("Dotimatrix", a lighter weight)
// rather than the editor's dotim5, because for a block of prose the lighter face is easier on
// the eye. They share the signature's *size* though (see setBoxFont), so the whole box reads
// at one size. We resolve the family exactly the way main.cpp does (load the .ttf, take its
// reported family) so we get whatever name dotim3.ttf registers on any platform; cached after
// the first lookup. Returns "" if the font can't be loaded (caller then leaves the face as-is).
static QString docBodyFamily() {
    static QString family;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const int id = QFontDatabase::addApplicationFont(
            QStringLiteral(":/resources/fonts/dotim3.ttf"));
        if (id != -1) {
            const QStringList fams = QFontDatabase::applicationFontFamilies(id);
            if (!fams.isEmpty()) family = fams.first();
        }
    }
    return family;
}

// ===================== Documentation / signature box ===================
//
// A small frameless panel that floats beside the completion popup (and at the mouse
// when hovering a method). The top line is the signature "name(params) -> ReturnType";
// below it is the docstring, clipped to a few lines with a More/Less toggle (a clickable
// affordance when shown on hover, an "F1" hint when shown beside the completer popup so
// it never has to steal the popup's keys/focus).
//
// It is intentionally NOT a QObject subclass with Q_OBJECT (only inherited virtuals and
// plain methods are used), so it needs no moc pass and can live entirely in this .cpp
// without touching the build files. It never activates, so the editor keeps its caret.
class CompletionDocBox : public QFrame {
public:
    explicit CompletionDocBox(PythonCodeEditor *editor)
        : QFrame(editor), m_editor(editor) {
        // Window flags are chosen per show() (see applyWindowMode): a non-activating
        // Qt::ToolTip beside the completer popup (so it can't dismiss that Qt::Popup),
        // and a clickable Qt::Tool for hover. WA_ShowWithoutActivating keeps editor focus.
        applyWindowMode(false);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setFocusPolicy(Qt::NoFocus);
        setFrameShape(QFrame::NoFrame);
        setObjectName("neoCompletionDocBox");
        setStyleSheet(
            "#neoCompletionDocBox { background:#FBFBF4; border:1px solid #B7B7C4;"
            "  border-radius:4px; }"
            "QLabel { color:#1A1A28; background:transparent; }");

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(10, 8, 10, 8);
        lay->setSpacing(5);

        // Optional error section, pinned to the TOP of the box: when hovering a token that
        // has both a Python error and API docs, the error reads first and the signature +
        // description sit underneath it (separated by a thin rule).
        m_err = new QLabel(this);
        m_err->setTextFormat(Qt::PlainText);
        m_err->setWordWrap(true);
        m_err->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_err->setStyleSheet("color:#C0282D; background:transparent;");   // error red
        m_err->hide();

        m_sep = new QFrame(this);
        m_sep->setFrameShape(QFrame::HLine);
        m_sep->setStyleSheet("color:#D9C9C9; background:transparent;");
        m_sep->hide();

        m_sig = new QLabel(this);
        m_sig->setTextFormat(Qt::RichText);
        m_sig->setWordWrap(true);
        // No font-family here: the signature inherits the editor typeface applied via
        // setBoxFont() (a stylesheet font-family would override that). The bold name comes
        // from the <b> tags in the rich-text signature, not from a stylesheet.

        // The docstring lives inside a QScrollArea so a long description can be scrolled
        // (in addition to the F1/click expand toggle). m_desc is owned by the scroll area
        // via setWidget(), not parented to the frame directly. We size it by hand in
        // relayout() instead of setWidgetResizable(true), which mis-measures a word-wrapped
        // QLabel's height and clips the final line.
        m_desc = new QLabel;                       // parented by m_descScroll->setWidget()
        m_desc->setTextFormat(Qt::PlainText);      // preserves the docstring's line breaks
        m_desc->setWordWrap(true);
        m_desc->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_desc->setStyleSheet("color:#595959; background:transparent;");   // medium-dark grey, softer than the signature

        m_descScroll = new QScrollArea(this);
        m_descScroll->setWidget(m_desc);
        m_descScroll->setWidgetResizable(false);
        m_descScroll->setFrameShape(QFrame::NoFrame);
        m_descScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_descScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_descScroll->viewport()->setAutoFillBackground(false);   // let the cream box show through
        // Transparent viewport + a slim, unobtrusive scrollbar that matches the box.
        m_descScroll->setStyleSheet(
            "QScrollArea { background:transparent; border:none; }"
            "QScrollBar:vertical { background:transparent; width:8px; margin:0px; }"
            "QScrollBar::handle:vertical { background:#C8C8D0; border-radius:4px;"
            "  min-height:24px; }"
            "QScrollBar::handle:vertical:hover { background:#B0B0BC; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0px; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "  background:transparent; }");

        m_footer = new QLabel(this);
        m_footer->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_footer->setStyleSheet("color:#3A6EA5;");

        lay->addWidget(m_err);
        lay->addWidget(m_sep);
        lay->addWidget(m_sig);
        lay->addWidget(m_descScroll);
        lay->addWidget(m_footer);
    }

    // Render every line (signature, docstring, footer) in the given typeface so the box
    // matches the editor rather than the system UI font. Each child label gets it
    // explicitly: setting the font on the frame alone wouldn't reach labels whose font Qt
    // has already resolved.
    void setBoxFont(const QFont &f) {
        // One size for the whole box: the description and footer copy the editor font (so
        // their size matches the signature exactly, whether it's point- or pixel-based) and
        // only swap the family to dotim3 (lighter, on-theme). The signature keeps dotim5.
        setFont(f);
        m_sig->setFont(f);
        QFont body = f;
        const QString fam = docBodyFamily();
        if (!fam.isEmpty()) body.setFamily(fam);
        m_err->setFont(body);     // error reads in the description face + size, as requested
        m_desc->setFont(body);
        m_footer->setFont(body);
    }

    // Fill the box for one member (resets to the collapsed state). Methods show
    // "name(params) -> ReturnType"; attributes show "name : Type". The docstring (if any)
    // goes underneath.
    void setContent(const QString &name, const MemberInfo &mi,
                    const QString &errorMsg = QString()) {
        // Error section (pinned to the top). Populated only on hover, when the hovered line
        // carries a Python error; the completer-popup path always passes an empty message.
        if (errorMsg.isEmpty()) {
            m_err->hide();
        } else {
            m_err->setText(errorMsg);
            m_err->show();
        }

        // Signature + description. A pure-error hover (no API token) passes an empty name,
        // so we hide the signature and leave the description blank.
        if (name.isEmpty()) {
            m_sig->hide();
            m_fullDoc.clear();
        } else {
            QString sig;
            if (mi.isMethod) {
                sig = "<b>" + name.toHtmlEscaped() + "</b>(" + mi.params.toHtmlEscaped() + ")";
                if (!mi.returnType.isEmpty())
                    sig += " -&gt; " + mi.returnType.toHtmlEscaped();
            } else {
                sig = "<b>" + name.toHtmlEscaped() + "</b>";
                if (!mi.attrType.isEmpty()) sig += " : " + mi.attrType.toHtmlEscaped();
            }
            m_sig->setText(sig);
            m_sig->show();
            m_fullDoc = mi.doc;
        }

        // The rule only earns its place between an error and an API entry.
        m_sep->setVisible(!errorMsg.isEmpty() && !name.isEmpty());

        m_desc->setText(m_fullDoc);
        m_expanded = false;
        relayout();
    }

    // Show to the right of the completion popup (flip to its left if there's no room).
    // Non-interactive: the popup owns the keyboard, so expansion is via the editor's F1.
    void showRightOf(const QRect &popupGlobalRect) {
        m_interactive = false;
        applyWindowMode(false);
        m_mode = RightOf;
        m_popupRect = popupGlobalRect;
        m_refPoint = popupGlobalRect.center();
        relayout();
        reposition();
        if (!isVisible()) show();
        raise();
    }

    // Show near the mouse (hover mode). Interactive: clicking toggles More/Less.
    void showNear(const QPoint &globalPos) {
        m_interactive = true;
        applyWindowMode(true);
        m_mode = Near;
        m_nearPos = globalPos;
        m_refPoint = globalPos;
        relayout();
        reposition();
        if (!isVisible()) show();
        raise();
    }

    void toggleExpanded() {
        if (!m_truncated) return;
        m_expanded = !m_expanded;
        relayout();
        reposition();
    }

    bool isInteractive() const { return m_interactive; }
    bool isTruncated()   const { return m_truncated; }

    // Scroll the description by a wheel notch. Used when the box sits beside the completer
    // popup (RightOf mode): that popup holds a mouse grab, so the wheel never reaches the box
    // directly — the editor catches it on the popup and forwards it here.
    void wheelScroll(const QPoint &angleDelta, const QPoint &pixelDelta) {
        if (!m_descScroll || !m_descScroll->isVisible()) return;
        QScrollBar *sb = m_descScroll->verticalScrollBar();
        if (!sb) return;
        int dy = pixelDelta.y();
        if (dy == 0) {   // classic wheel: angleDelta is in 1/8 degree, 120 == one notch
            const int lineH = qMax(1, m_desc->fontMetrics().lineSpacing());
            dy = qRound(angleDelta.y() / 120.0 * 3 * lineH);   // ~3 lines per notch
        }
        sb->setValue(sb->value() - dy);
    }

    // Is this (top-level) box currently under the given global point? Lets the editor decide
    // whether a wheel event delivered to the popup actually belongs to the box.
    bool containsGlobal(const QPoint &globalPt) const {
        return isVisible() && geometry().contains(globalPt);
    }

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (m_interactive && m_truncated) { toggleExpanded(); e->accept(); return; }
        QFrame::mousePressEvent(e);
    }
    void enterEvent(QEnterEvent *e) override {
        if (m_interactive && m_editor) m_editor->docBoxHoverChanged(true);
        QFrame::enterEvent(e);
    }
    void leaveEvent(QEvent *e) override {
        if (m_interactive && m_editor) m_editor->docBoxHoverChanged(false);
        QFrame::leaveEvent(e);
    }

private:
    enum Mode { RightOf, Near };

    // Pick window flags for the current mode. Beside the completer popup we use a
    // non-activating Qt::ToolTip: tooltips are designed to layer over Qt::Popup windows,
    // so showing it can't make the completion list disappear. On hover (no popup in play)
    // we use a Qt::Tool that stays out of the focus chain but still accepts clicks for
    // the More/Less toggle. Re-creating the native window only happens on a real change.
    void applyWindowMode(bool interactive) {
        const Qt::WindowFlags f = interactive
            ? (Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
               Qt::WindowDoesNotAcceptFocus)
            : (Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        if (windowFlags() == f) return;
        const bool wasVisible = isVisible();
        setWindowFlags(f);
        setAttribute(Qt::WA_ShowWithoutActivating, true);   // cleared by setWindowFlags
        if (wasVisible) show();
    }

    // Recompute sizes: the doc area shrink-wraps the text so there's never empty space below
    // it. Collapsed clamps to the collapsed band (the rest scrolls); expanded shows the whole
    // docstring (clamped only by the screen height). Does not move the window.
    void relayout() {
        const int contentW = kBoxWidth - 22;  // box width minus margins + 1px border each side
        const int sbw = 10;                    // room reserved for the slim scrollbar (8px + slack)
        const int textW = contentW - sbw;      // word-wrap width for the docstring label
        // Measure the wrapped text height straight from the font metrics rather than the
        // label's heightForWidth()/sizeHint(): those cache against the widget's last applied
        // fixed size, so after a tall (scrolling) box every later box — even a one-liner —
        // inherited that large height and wrongly hit the collapsed cap. This depends only
        // on the wrap width and the text.
        int fullH = 0;
        if (!m_fullDoc.isEmpty()) {
            const QRect br = m_desc->fontMetrics().boundingRect(
                QRect(0, 0, textW, 1000000), Qt::TextWordWrap, m_fullDoc);
            fullH = br.height();
        }
        const int lineH = m_desc->fontMetrics().lineSpacing();
        const int collapsedCap = lineH * kCollapsedLines;
        // "Truncated" (and so scrollable / footer-worthy) whenever the doc overflows the
        // collapsed band. Collapsed clamps to that band; expanding reveals the whole text.
        m_truncated = fullH > collapsedCap + 2;

        if (m_fullDoc.isEmpty()) {
            m_descScroll->hide();
            m_footer->hide();
            setFixedWidth(kBoxWidth);
            adjustSize();
            return;
        }

        m_desc->setFixedWidth(textW);
        m_desc->setFixedHeight(fullH);           // full content height; the scroll area clips it
        m_descScroll->show();
        updateFooter();

        // Collapsed: shrink to the text but never past the collapsed band. Expanded: the
        // entire docstring, no line cap.
        int visibleH = m_expanded ? fullH : qMin(fullH, collapsedCap);
        m_descScroll->setFixedHeight(visibleH);
        setFixedWidth(kBoxWidth);
        adjustSize();

        // Safety net for a very long docstring: an expanded box must still fit on screen, so
        // if the full text would push it past the available height, trim the scroll viewport
        // by the overflow (the remainder then scrolls).
        if (m_expanded) {
            QScreen *scr = QGuiApplication::screenAt(m_refPoint);
            if (!scr) scr = QGuiApplication::primaryScreen();
            const int availH = scr ? scr->availableGeometry().height() : 0;
            if (availH > 0 && height() > availH) {
                visibleH = qMax(collapsedCap, visibleH - (height() - availH));
                m_descScroll->setFixedHeight(visibleH);
                adjustSize();
            }
        }
    }

    void updateFooter() {
        if (!m_truncated) { m_footer->hide(); return; }
        // Build the arrows from explicit code points (U+25B4 up / U+25BE down) rather than
        // UTF-8 source bytes, so they render correctly regardless of the compiler's
        // execution charset (MinGW vs MSVC vs clang).
        if (m_interactive) {
            m_footer->setText(m_expanded ? (QStringLiteral("Less ") + QChar(0x25B4))
                                         : (QStringLiteral("More ") + QChar(0x25BE)));
            m_footer->setCursor(Qt::PointingHandCursor);
        } else {
            m_footer->setText(m_expanded ? QStringLiteral("F1: less")
                                         : QStringLiteral("F1: more"));
            m_footer->unsetCursor();
        }
        m_footer->show();
    }

    // Place the (already-sized) box relative to its anchor, clamped onto the screen.
    void reposition() {
        QScreen *scr = QGuiApplication::screenAt(m_refPoint);
        if (!scr) scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);
        const QSize sz = size();
        int x, y;
        if (m_mode == RightOf) {
            x = m_popupRect.right() + kGap;
            if (x + sz.width() > avail.right())
                x = m_popupRect.left() - kGap - sz.width();   // no room on the right -> left
            y = m_popupRect.top();
        } else {
            x = m_nearPos.x() + 14;
            y = m_nearPos.y() + 18;
            if (x + sz.width()  > avail.right())  x = m_nearPos.x() - 14 - sz.width();
            if (y + sz.height() > avail.bottom()) y = m_nearPos.y() - 18 - sz.height();
        }
        x = qBound(avail.left(), x, qMax(avail.left(), avail.right()  - sz.width()));
        y = qBound(avail.top(),  y, qMax(avail.top(),  avail.bottom() - sz.height()));
        move(x, y);
    }

    static constexpr int kBoxWidth = 510;        // 1.5x the original 340 for wider, easier-to-read docs
    static constexpr int kCollapsedLines = 6;    // max doc lines before the collapsed box scrolls
    static constexpr int kGap = 6;

    PythonCodeEditor *m_editor = nullptr;
    QLabel *m_err = nullptr;               // optional error message, pinned above the signature
    QFrame *m_sep = nullptr;               // thin rule between the error and the API entry
    QLabel *m_sig = nullptr;
    QLabel *m_desc = nullptr;
    QScrollArea *m_descScroll = nullptr;   // wraps m_desc so a long docstring scrolls
    QLabel *m_footer = nullptr;
    QString m_fullDoc;
    bool m_expanded = false;
    bool m_truncated = false;
    bool m_interactive = false;
    Mode  m_mode = RightOf;
    QRect m_popupRect;
    QPoint m_nearPos;
    QPoint m_refPoint;
};

// ===================== Parameter-list / docstring parsing =====================
//
// Helpers shared by the parameter-input panel: split a parameter or argument list on
// top-level commas (respecting brackets and quotes), break a signature into named
// fields, locate the live arguments of a call (with document offsets), and pull
// per-parameter descriptions out of a docstring's "Parameters:" block.

// One documented parameter, parsed from a cleaned signature (self/cls already dropped).
// annotation is the text after ':' (may be empty); defaultVal the text after '=' (empty
// when the parameter has no default — shown as the input box's grey placeholder).
struct ParamInfo {
    QString name;
    QString annotation;
    QString defaultVal;
};

// One top-level argument of a call, with absolute document offsets so a value can be
// rewritten in place. keyword/name are set for the "name = value" form; valStart/valEnd
// bound the value text (the whole token for a positional argument).
struct ArgSpan {
    int start = 0, end = 0;        // [start,end) of the trimmed argument text
    bool keyword = false;
    QString name;
    int valStart = 0, valEnd = 0;  // [valStart,valEnd) of the value portion
};

// Index of the first '=' at bracket depth 0 that is a real assignment (not ==/!=/<=/>=/:=),
// or -1. Separates a parameter's default and a keyword argument's value.
static int topLevelEq(const QString &s) {
    int depth = 0;
    QChar quote;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == quote) quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) { quote = c; continue; }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) { ++depth; continue; }
        if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) { if (depth > 0) --depth; continue; }
        if (depth == 0 && c == QLatin1Char('=')) {
            const QChar prev = (i > 0) ? s.at(i - 1) : QChar();
            const QChar next = (i + 1 < s.size()) ? s.at(i + 1) : QChar();
            if (next == QLatin1Char('=')) { ++i; continue; }        // ==
            if (prev == QLatin1Char('!') || prev == QLatin1Char('<') ||
                prev == QLatin1Char('>') || prev == QLatin1Char(':') ||
                prev == QLatin1Char('=')) continue;                 // !=, <=, >=, :=
            return i;
        }
    }
    return -1;
}

// Split on commas at bracket depth 0 and outside any quote, so tuples/lists/dicts and
// strings containing commas stay intact. Pieces are trimmed; empty pieces dropped.
static QStringList splitTopLevel(const QString &s) {
    QStringList out;
    int depth = 0, start = 0;
    QChar quote;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == quote) quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) { quote = c; continue; }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) { ++depth; continue; }
        if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) { if (depth > 0) --depth; continue; }
        if (c == QLatin1Char(',') && depth == 0) {
            const QString piece = s.mid(start, i - start).trimmed();
            if (!piece.isEmpty()) out << piece;
            start = i + 1;
        }
    }
    const QString last = s.mid(start).trimmed();
    if (!last.isEmpty()) out << last;
    return out;
}

// Walk the argument list of the call whose "(" is at openParen in docText. Returns one
// ArgSpan per top-level argument (offsets absolute in docText); *closeOut receives the
// matching ")" index, or -1 if the call isn't closed. Tracks strings and "#" comments so
// commas/brackets inside them don't split arguments.
static QList<ArgSpan> scanArgs(const QString &docText, int openParen, int *closeOut, int endLimit = -1) {
    QList<ArgSpan> args;
    *closeOut = -1;
    const int hardEnd = docText.size();
    // When the caller knows the call's ")" (an anchored cursor), bound the scan to it so a
    // value containing an unbalanced "(" can't run the scan past the call — and make us
    // rewrite — the rest of the document.
    const int n = (endLimit >= 0 && endLimit <= hardEnd) ? endLimit : hardEnd;
    int depth = 1, argStart = openParen + 1;
    QChar quote;

    auto flush = [&](int end) {
        int s = argStart, e = end;
        while (s < e && docText.at(s).isSpace()) ++s;
        while (e > s && docText.at(e - 1).isSpace()) --e;
        if (e <= s) return;                       // empty piece (e.g. trailing comma)
        ArgSpan a;
        a.start = s; a.end = e;
        int k = s;
        while (k < e && (docText.at(k).isLetterOrNumber() || docText.at(k) == QLatin1Char('_'))) ++k;
        int p = k;
        while (p < e && docText.at(p).isSpace()) ++p;
        if (k > s && p < e && docText.at(p) == QLatin1Char('=') &&
            (p + 1 >= e || docText.at(p + 1) != QLatin1Char('='))) {
            a.keyword = true;
            a.name = docText.mid(s, k - s);
            int v = p + 1;
            while (v < e && docText.at(v).isSpace()) ++v;
            a.valStart = v; a.valEnd = e;
        } else {
            a.valStart = s; a.valEnd = e;
        }
        args << a;
    };

    int i = openParen + 1;
    for (; i < n; ++i) {
        const QChar c = docText.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\')) { ++i; continue; }
            if (c == quote) quote = QChar();
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) { quote = c; continue; }
        if (c == QLatin1Char('#')) {                          // comment in a multi-line call
            while (i < n && docText.at(i) != QLatin1Char('\n')) ++i;
            continue;
        }
        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) { ++depth; continue; }
        if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
            --depth;
            if (depth == 0) { *closeOut = i; break; }
            continue;
        }
        if (c == QLatin1Char(',') && depth == 1) {
            flush(i);
            argStart = i + 1;
        }
    }
    if (*closeOut < 0 && endLimit >= 0) *closeOut = endLimit;   // bounded: ")" sits at endLimit
    flush((*closeOut >= 0) ? *closeOut : n);
    return args;
}

// Break a cleaned parameter list into ordered fields. Skips *args / **kwargs / bare "*"
// and positional-only "/" markers (not fillable as keyword arguments).
static QList<ParamInfo> parseParams(const QString &paramStr) {
    QList<ParamInfo> out;
    const QStringList parts = splitTopLevel(paramStr);
    for (const QString &raw : parts) {
        const QString p = raw.trimmed();
        if (p.isEmpty() || p == QLatin1String("*") || p == QLatin1String("/")) continue;
        if (p.startsWith(QLatin1Char('*'))) continue;         // *args / **kwargs
        QString namePart = p, defPart;
        const int eq = topLevelEq(p);
        if (eq >= 0) { namePart = p.left(eq).trimmed(); defPart = p.mid(eq + 1).trimmed(); }
        QString name = namePart, ann;
        const int colon = namePart.indexOf(QLatin1Char(':'));
        if (colon >= 0) { name = namePart.left(colon).trimmed(); ann = namePart.mid(colon + 1).trimmed(); }
        if (name.isEmpty()) continue;
        ParamInfo pi;
        pi.name = name;
        pi.annotation = ann;
        pi.defaultVal = defPart;
        out << pi;
    }
    return out;
}

// Pull per-parameter descriptions out of a docstring's "Parameters:" (or "Args:") block.
// Recognises "name (type): desc", "name: desc" and "name -- desc"; folds in more-indented
// continuation lines; stops at the next section header or a dedent out of the block. The
// docstring is already dedented by extractDocstring(), so indentation is reliable.
static QMap<QString, QString> parseParamDocs(const QString &doc) {
    QMap<QString, QString> out;
    if (doc.isEmpty()) return out;
    const QStringList lines = doc.split(QLatin1Char('\n'));
    static const QRegularExpression nonSpaceRe(QStringLiteral("\\S"));
    static const QRegularExpression headerRe(
        QStringLiteral("^\\s*(Parameters|Params|Args|Arguments)\\s*:?\\s*$"));
    static const QRegularExpression otherHeaderRe(
        QStringLiteral("^\\s*(Returns?|Yields?|Raises?|Examples?|Notes?|See Also|References|Attributes)\\s*:?\\s*$"));
    static const QRegularExpression entryRe(
        QStringLiteral("^(\\s*)([A-Za-z_]\\w*)\\s*(?:\\(([^)]*)\\))?\\s*(?::|--)\\s*(.*)$"));

    int i = 0;
    for (; i < lines.size(); ++i)
        if (headerRe.match(lines.at(i)).hasMatch()) break;
    if (i >= lines.size()) return out;
    const int headerIndent = qMax(0, lines.at(i).indexOf(nonSpaceRe));
    ++i;

    QString curName;
    QStringList curDesc;
    int entryIndent = -1;
    auto commit = [&]() {
        if (!curName.isEmpty())
            out.insert(curName, curDesc.join(QLatin1Char(' ')).simplified());
        curName.clear();
        curDesc.clear();
    };
    for (; i < lines.size(); ++i) {
        const QString &ln = lines.at(i);
        if (ln.trimmed().isEmpty()) continue;
        const int indent = ln.indexOf(nonSpaceRe);
        if (indent <= headerIndent) break;               // dedented out of the block
        if (otherHeaderRe.match(ln).hasMatch()) break;    // a following section header
        // Field tags like "@color" / "@position" may sit anywhere on the entry line (before or
        // after the colon). Pull EVERY "@tag" out so the entry regex parses cleanly, then
        // re-attach them to the description as markers the panel reads (and shows stripped).
        static const QRegularExpression tagRe(QStringLiteral("@\\w+"));
        QString lnClean = ln;
        QString tagPrefix;
        {
            QStringList tags;
            QRegularExpressionMatchIterator tit = tagRe.globalMatch(ln);
            while (tit.hasNext()) tags << tit.next().captured(0);
            if (!tags.isEmpty()) {
                lnClean = QString(ln).remove(tagRe);
                tagPrefix = tags.join(QLatin1Char(' ')) + QLatin1Char(' ');
            }
        }
        const QRegularExpressionMatch em = entryRe.match(lnClean);
        if (em.hasMatch() && (entryIndent < 0 || em.captured(1).length() <= entryIndent)) {
            commit();
            entryIndent = em.captured(1).length();
            curName = em.captured(2);
            QString d = em.captured(4).trimmed();
            if (!tagPrefix.isEmpty()) d = tagPrefix + d;
            if (!d.isEmpty()) curDesc << d;
        } else if (!curName.isEmpty()) {
            curDesc << lnClean.trimmed();                 // continuation of the current entry
        }
    }
    commit();
    return out;
}

// ===================== Inline HSV colour picker ===================
//
// A small pop-up colour picker for a tuple parameter tagged @color: a 2D saturation/value
// square plus a hue slider, exactly like a standard picker. Both sub-widgets are NoFocus and
// handle the mouse directly, so interacting with the picker never moves keyboard focus — which
// is what keeps the parameter panels open while you use it. All colour maths goes through
// QColor (HSV<->RGB), so there's no hand-rolled conversion. Plain widgets (no Q_OBJECT): the
// owner sets std::function callbacks, so no moc is needed.

// 2D field: x = saturation (0..255), y = value/brightness (255 at top .. 0 at bottom).
class SVField : public QWidget {
public:
    explicit SVField(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(170, 150);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::CrossCursor);
    }
    void setHue(int h) { m_h = h; update(); }
    void setSV(int s, int v) { m_s = qBound(0, s, 255); m_v = qBound(0, v, 255); update(); }
    int sat() const { return m_s; }
    int val() const { return m_v; }
    std::function<void(int, int)> onChange;   // (sat, val)

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const QRect r = rect();
        // Base hue at full S/V, then white->transparent across X (saturation) and
        // transparent->black down Y (value).
        p.fillRect(r, QColor::fromHsv(m_h, 255, 255));
        QLinearGradient sat(r.topLeft(), r.topRight());
        sat.setColorAt(0, QColor(255, 255, 255, 255));
        sat.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillRect(r, sat);
        QLinearGradient val(r.topLeft(), r.bottomLeft());
        val.setColorAt(0, QColor(0, 0, 0, 0));
        val.setColorAt(1, QColor(0, 0, 0, 255));
        p.fillRect(r, val);
        // Marker.
        const int x = int(m_s / 255.0 * (width() - 1));
        const int y = int((1.0 - m_v / 255.0) * (height() - 1));
        p.setPen(QPen(m_v > 128 ? Qt::black : Qt::white, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPoint(x, y), 5, 5);
    }
    void mousePressEvent(QMouseEvent *e) override { apply(e->pos()); }
    void mouseMoveEvent(QMouseEvent *e) override { apply(e->pos()); }

private:
    void apply(QPoint pos) {
        m_s = qBound(0, int(pos.x() * 255.0 / qMax(1, width() - 1)), 255);
        m_v = qBound(0, int((1.0 - pos.y() * 1.0 / qMax(1, height() - 1)) * 255), 255);
        update();
        if (onChange) onChange(m_s, m_v);
    }
    int m_h = 0, m_s = 0, m_v = 255;
};

// Vertical hue bar (0..359, red at top through the spectrum back to red at the bottom).
class HueSlider : public QWidget {
public:
    explicit HueSlider(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedSize(16, 150);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
    }
    void setHue(int h) { m_h = qBound(0, h, 359); update(); }
    int hue() const { return m_h; }
    std::function<void(int)> onChange;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        QLinearGradient g(rect().topLeft(), rect().bottomLeft());
        for (int i = 0; i <= 6; ++i)
            g.setColorAt(i / 6.0, QColor::fromHsv((360 * i / 6) % 360, 255, 255));
        p.fillRect(rect(), g);
        const int y = int(m_h / 359.0 * (height() - 1));
        p.setPen(QPen(Qt::black, 1.5));
        p.drawLine(0, y, width(), y);
        p.setPen(QPen(Qt::white, 1));
        p.drawLine(0, y + 1, width(), y + 1);
    }
    void mousePressEvent(QMouseEvent *e) override { apply(e->pos().y()); }
    void mouseMoveEvent(QMouseEvent *e) override { apply(e->pos().y()); }

private:
    void apply(int y) {
        m_h = qBound(0, int(y * 359.0 / qMax(1, height() - 1)), 359);
        update();
        if (onChange) onChange(m_h);
    }
    int m_h = 0;
};

// The pop-up frame: SV square + hue slider, floated like the parameter panel (a non-activating
// tool window). setRgb() seeds it from the fields; onColorChanged reports edits back as RGB.
class ColorPicker : public QFrame {
public:
    explicit ColorPicker(QWidget *editor) : QFrame(editor) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setObjectName("neoColorPicker");
        setStyleSheet("#neoColorPicker { background:#FBFBF4; border:1px solid #B7B7C4; border-radius:4px; }");
        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(10, 10, 10, 10);
        lay->setSpacing(8);
        m_sv = new SVField(this);
        m_hue = new HueSlider(this);
        lay->addWidget(m_sv);
        lay->addWidget(m_hue);
        m_sv->onChange  = [this](int, int) { emitColor(); };
        m_hue->onChange = [this](int h) { m_sv->setHue(h); emitColor(); };
    }

    // Seed the picker from RGB (0..255) without emitting (avoids feedback while syncing).
    void setRgb(int r, int g, int b) {
        QColor c(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
        int h, s, v;
        c.getHsv(&h, &s, &v);
        if (h < 0) h = m_hue->hue();        // achromatic: keep the current hue
        m_hue->setHue(h);
        m_sv->setHue(h);
        m_sv->setSV(s, v);
    }

    std::function<void(int, int, int)> onColorChanged;   // (r, g, b)

private:
    void emitColor() {
        const QColor c = QColor::fromHsv(m_hue->hue(), m_sv->sat(), m_sv->val());
        if (onColorChanged) onColorChanged(c.red(), c.green(), c.blue());
    }
    SVField *m_sv = nullptr;
    HueSlider *m_hue = nullptr;
};

// ===================== Live-frame position picker ===================
//
// A pop-up for a tuple parameter tagged @position: it shows the current rendered preview frame
// and lets you drag a point to choose an (x, y). A checkbox toggles Absolute vs Relative.
// Absolute writes pixels scaled by the dx preview factor — int(X*dx) — so the same point lands
// correctly at preview and full render. Relative writes the point as a fraction of
// renderer.width()/height() — int(renderer.width()*f) — softly snapping to halves, thirds and
// quarters. The frame sub-widget is NoFocus, so dragging never moves keyboard focus (keeping
// the parameter panels open). Plain widgets (no Q_OBJECT); callbacks via std::function.

extern FrameBufferReader *g_frameBuffer;   // main.cpp: the shared preview frame buffer
extern int g_previewFrameIndex;            // main.cpp: index of the frame currently previewed

// The frame display with a draggable crosshair; reports the point as normalised (0..1) coords.
class FrameView : public QWidget {
public:
    explicit FrameView(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(280, 170);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::CrossCursor);
    }
    void setPoint(double nx, double ny) { m_nx = qBound(0.0, nx, 1.0); m_ny = qBound(0.0, ny, 1.0); update(); }
    double nx() const { return m_nx; }
    double ny() const { return m_ny; }
    void setRelative(bool r) { m_relative = r; update(); }
    std::function<void(double, double)> onChange;   // normalised (nx, ny)

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(18, 18, 26));
        const QImage img = currentFrame();
        QRect dst = rect();
        if (!img.isNull()) {
            const QSize sz = img.size().scaled(rect().size(), Qt::KeepAspectRatio);
            dst = QRect(QPoint((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz);
            p.drawImage(dst, img);
        }
        m_dst = dst;
        if (m_relative) {   // guide lines at the snap fractions
            p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::DashLine));
            const double g[] = { 1.0 / 3, 0.5, 2.0 / 3 };
            for (double f : g) {
                p.drawLine(dst.left() + int(f * dst.width()), dst.top(),
                           dst.left() + int(f * dst.width()), dst.bottom());
                p.drawLine(dst.left(), dst.top() + int(f * dst.height()),
                           dst.right(), dst.top() + int(f * dst.height()));
            }
        }
        const int cx = dst.left() + int(m_nx * dst.width());
        const int cy = dst.top() + int(m_ny * dst.height());
        p.setPen(QPen(Qt::black, 3)); p.drawLine(cx - 9, cy, cx + 9, cy); p.drawLine(cx, cy - 9, cx, cy + 9);
        p.setPen(QPen(Qt::white, 1)); p.drawLine(cx - 9, cy, cx + 9, cy); p.drawLine(cx, cy - 9, cx, cy + 9);
    }
    void mousePressEvent(QMouseEvent *e) override { apply(e->pos()); }
    void mouseMoveEvent(QMouseEvent *e) override { apply(e->pos()); }

private:
    static QImage currentFrame() {
        if (!g_frameBuffer || !g_frameBuffer->isOpen()) return QImage();
        g_frameBuffer->refreshHeader();
        const FrameBufferReader::Header &h = g_frameBuffer->header();
        if (h.magic != FrameBufferReader::MAGIC || h.frame_count == 0 || h.width == 0 || h.height == 0)
            return QImage();
        const uint32_t idx = qBound(0u, (uint32_t)g_previewFrameIndex, h.frame_count - 1);
        const uint8_t *data = g_frameBuffer->frameData(idx);
        if (!data) return QImage();
        return QImage(data, (int)h.width, (int)h.height, (int)(h.width * h.channels),
                      QImage::Format_RGB888).copy();   // copy: the mmap may change under us
    }
    void apply(QPoint pos) {
        if (m_dst.width() <= 0 || m_dst.height() <= 0) return;
        double nx = double(pos.x() - m_dst.left()) / m_dst.width();
        double ny = double(pos.y() - m_dst.top()) / m_dst.height();
        nx = qBound(0.0, nx, 1.0); ny = qBound(0.0, ny, 1.0);
        if (m_relative) { nx = snap(nx); ny = snap(ny); }
        m_nx = nx; m_ny = ny; update();
        if (onChange) onChange(nx, ny);
    }
    static double snap(double v) {   // soft snap to halves, thirds, quarters
        const double targets[] = { 0.0, 0.25, 1.0 / 3, 0.5, 2.0 / 3, 0.75, 1.0 };
        for (double t : targets) if (qAbs(v - t) < 0.025) return t;
        return v;
    }
    double m_nx = 0.5, m_ny = 0.5;
    bool m_relative = false;
    QRect m_dst;
};

// The pop-up frame: the frame view + an Absolute/Relative checkbox.
class PosPicker : public QFrame {
public:
    explicit PosPicker(QWidget *editor) : QFrame(editor) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setObjectName("neoPosPicker");
        setStyleSheet("#neoPosPicker { background:#FBFBF4; border:1px solid #B7B7C4; border-radius:4px; }"
                      "QCheckBox { color:#1A1A28; background:transparent; }");
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(10, 10, 10, 10);
        lay->setSpacing(6);
        m_view = new FrameView(this);
        lay->addWidget(m_view);
        m_relBox = new QCheckBox(QStringLiteral("Relative (fraction of renderer size)"), this);
        m_relBox->setFocusPolicy(Qt::NoFocus);   // clicking must not move focus / close panels
        lay->addWidget(m_relBox);
        m_view->onChange = [this](double nx, double ny) {
            if (onPosChanged) onPosChanged(nx, ny, m_relBox->isChecked());
        };
        QObject::connect(m_relBox, &QCheckBox::toggled, m_relBox, [this](bool on) {
            m_view->setRelative(on);
            if (onPosChanged) onPosChanged(m_view->nx(), m_view->ny(), on);   // re-emit in the new form
        });
    }
    void setState(double nx, double ny, bool relative) {
        QSignalBlocker b(m_relBox);
        m_relBox->setChecked(relative);
        m_view->setRelative(relative);
        m_view->setPoint(nx, ny);
    }
    void setBoxFont(const QFont &f) { m_relBox->setFont(f); }
    std::function<void(double, double, bool)> onPosChanged;   // nx, ny, relative

private:
    FrameView *m_view = nullptr;
    QCheckBox *m_relBox = nullptr;
};

// ===================== Parameter-input panel ===================
//
// A focusable, frameless panel floated to the right of the caret while it sits inside a
// documented call's parentheses. It shows the call's signature, then one row per
// parameter: the parameter name + type, an input box (placeholder = the default), and the
// parameter's description from the docstring. Typing in a box calls back into the editor
// (applyParamEdit) which writes "name=value" into the parens. Unlike CompletionDocBox this
// panel ACCEPTS focus (its line edits need it); WA_ShowWithoutActivating keeps the editor
// caret when it first appears, and the user clicks/Tabs in to fill the boxes.
//
// Plain QFrame (no Q_OBJECT): line-edit signals are wired with functor connect(), so no moc
// pass is needed and it lives entirely in this .cpp. It is a friend of PythonCodeEditor so
// edits route straight to applyParamEdit()/hideParamPanel().
class ParamPanel : public QFrame {
public:
    explicit ParamPanel(PythonCodeEditor *editor)
        : QFrame(editor), m_editor(editor) {
        // No WindowStaysOnTopHint: it would sit above the field's own autocomplete popup and
        // doc box, hiding them. A Tool window already floats above its parent editor window,
        // which is all the panel needs.
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_ShowWithoutActivating, true);   // showing must not steal editor focus
        setFrameShape(QFrame::NoFrame);
        setObjectName("neoParamPanel");
        setStyleSheet(
            "#neoParamPanel { background:#FBFBF4; border:1px solid #B7B7C4; border-radius:4px; }"
            "QLabel { color:#1A1A28; background:transparent; }"
            "QPlainTextEdit { background:#FFFFFF; border:1px solid #C8C8D0; border-radius:3px; }"
            "QPlainTextEdit:focus { border:1px solid #3A6EA5; }");

        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(kMargin, kMargin, kMargin, kMargin);
        lay->setSpacing(kTitleGap);

        m_title = new QLabel(this);
        m_title->setTextFormat(Qt::RichText);
        m_title->setWordWrap(true);
        lay->addWidget(m_title);

        m_rowsHost = new QWidget;                 // parented via m_scroll->setWidget()
        m_rowsLay = new QVBoxLayout(m_rowsHost);
        m_rowsLay->setContentsMargins(0, 0, 0, 0);
        m_rowsLay->setSpacing(10);
        m_rowsLay->addStretch(1);                 // keeps rows top-aligned

        m_scroll = new QScrollArea(this);
        m_scroll->setWidget(m_rowsHost);
        m_scroll->setWidgetResizable(false);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        m_scroll->viewport()->setAutoFillBackground(false);
        m_scroll->setStyleSheet(
            "QScrollArea { background:transparent; border:none; }"
            "QScrollBar:vertical { background:transparent; width:8px; margin:0px; }"
            "QScrollBar::handle:vertical { background:#C8C8D0; border-radius:4px; min-height:24px; }"
            "QScrollBar::handle:vertical:hover { background:#B0B0BC; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0px; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }");
        lay->addWidget(m_scroll);

        // Inline colour picker for @color tuple parameters. Parented to the editor (lives with
        // it); shown beside the panel whenever an r/g/b field of a colour row holds focus.
        m_colorPicker = new ColorPicker(m_editor);
        m_colorPicker->onColorChanged = [this](int r, int g, int b) {
            if (m_pickerRow < 0 || m_pickerRow >= m_rows.size()) return;
            const Row &row = m_rows.at(m_pickerRow);
            if (row.edits.size() < 3) return;
            m_pickerSyncing = true;                       // setFieldText blocks signals anyway
            setFieldText(row.edits.at(0), QString::number(r));
            setFieldText(row.edits.at(1), QString::number(g));
            setFieldText(row.edits.at(2), QString::number(b));
            m_pickerSyncing = false;
            refreshRow(m_pickerRow);
            m_editor->applyParamEdit(QString(), QString(), false);   // rewrite the call once
        };
        // Show/hide & rebind the picker as focus moves between colour fields (functor connect,
        // so ParamPanel needs no Q_OBJECT). The picker's own widgets are NoFocus, so clicking
        // it never fires this — focus stays on the field and every panel stays open.
        // Inline position picker for @position tuple parameters: shows the live frame and a
        // draggable point. Dragging rewrites the x/y fields (absolute int(X*dx), or relative
        // int(renderer.width()*f)); the Relative checkbox toggles the form.
        m_posPicker = new PosPicker(m_editor);
        m_posPicker->onPosChanged = [this](double nx, double ny, bool relative) {
            if (m_posRow < 0 || m_posRow >= m_rows.size()) return;
            const Row &row = m_rows.at(m_posRow);
            if (row.edits.size() < 2) return;
            QString xs, ys;
            if (relative) {
                xs = QStringLiteral("int(renderer.width() * %1)").arg(QString::number(nx, 'g', 4));
                ys = QStringLiteral("int(renderer.height() * %1)").arg(QString::number(ny, 'g', 4));
            } else {
                const long X = qRound(nx * m_posFw / m_posDx);   // full-resolution pixel
                const long Y = qRound(ny * m_posFh / m_posDx);
                xs = QStringLiteral("int(%1 * dx)").arg(X);
                ys = QStringLiteral("int(%1 * dx)").arg(Y);
            }
            m_posSyncing = true;
            setFieldText(row.edits.at(0), xs);
            setFieldText(row.edits.at(1), ys);
            m_posSyncing = false;
            refreshRow(m_posRow);
            m_editor->applyParamEdit(QString(), QString(), false);
        };

        // Show/hide & rebind whichever picker matches the focused field (functor connect, so
        // ParamPanel needs no Q_OBJECT). The pickers' own widgets are NoFocus, so clicking them
        // never fires this — focus stays on the field and every panel stays open.
        QObject::connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
            if (!isVisible()) return;
            if (!now) return;            // transient null while a picker window activates — keep it
            const int crow = colorRowOf(now);
            if (crow >= 0) showPickerFor(crow);
            else if (m_colorPicker) m_colorPicker->hide();
            const int prow = posRowOf(now);
            if (prow >= 0) showPosPickerFor(prow);
            else if (m_posPicker) m_posPicker->hide();
        });
    }

    // Render the box in the editor typeface (signature in the editor face; the parameter
    // rows and descriptions in the lighter dotim3 body face, like the doc box).
    void setBoxFont(const QFont &f) {
        setFont(f);
        m_sigFont = f;
        m_bodyFont = f;
        const QString fam = docBodyFamily();
        if (!fam.isEmpty()) m_bodyFont.setFamily(fam);
        m_title->setFont(f);
        if (m_posPicker) m_posPicker->setBoxFont(m_bodyFont);
        // Live rows (if any) are re-fonted on the next buildFor().
    }

    // (Re)build the rows for one call. Returns false (and shows nothing) when there are no
    // fillable parameters. docs maps a parameter name to its description (may be empty).
    bool buildFor(const QString &name, const MemberInfo & /*mi*/,
                  const QList<ParamInfo> &params, const QMap<QString, QString> &docs) {
        if (params.isEmpty()) return false;

        // Title is just the class/method name — the parameters are listed in full below, so
        // the signature line doesn't repeat them.
        m_title->setText(QStringLiteral("<b>") + name.toHtmlEscaped() + QStringLiteral("</b>"));

        // Nested fields resolve names against the same script the top panel does, so flatten
        // the context to the root editor (m_editor's own context when m_editor is itself a field).
        PythonCodeEditor *ctx = (m_editor->m_valueFieldMode && m_editor->m_contextEditor)
                                    ? m_editor->m_contextEditor : m_editor;

        clearRows();
        for (const ParamInfo &pi : params) {
            const int kind = kindForAnnotation(pi.annotation);   // 0 plain, 1 str, 2 tuple

            // A tuple documented with the @color tag becomes a colour field: r/g/b labels plus
            // the inline picker. The tag is stripped from the description that's shown.
            QString desc = docs.value(pi.name);
            const bool isColor = (kind == 2) && desc.contains(QStringLiteral("@color"));
            const bool isPos   = (kind == 2) && desc.contains(QStringLiteral("@position"));
            desc.remove(QStringLiteral("@color"));
            desc.remove(QStringLiteral("@position"));
            desc = desc.simplified();

            QWidget *row = new QWidget(m_rowsHost);
            auto *rl = new QVBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            rl->setSpacing(2);

            // Header line: the parameter name (+ type / optional) on the left, and for an
            // interpreted field (str / tuple / colour / position / bool) a right-aligned "plain"
            // toggle. Checking it swaps the interpreted widget(s) for one verbatim code field whose
            // text goes into the call exactly as typed — no quotes, no parens, no comma-splitting.
            const bool interpretive = (kind != 0);   // kind 0 is already a verbatim expression
            auto *headerLine = new QWidget(row);
            auto *hh = new QHBoxLayout(headerLine);
            hh->setContentsMargins(0, 0, 0, 0);
            hh->setSpacing(6);
            auto *h = new QLabel(headerLine);       // text (name + type + optional) set by refreshRow
            h->setTextFormat(Qt::RichText);
            h->setFont(m_sigFont);
            hh->addWidget(h, 1);
            QCheckBox *plainToggle = nullptr;
            if (interpretive) {
                plainToggle = new QCheckBox(QStringLiteral("plain"), headerLine);
                plainToggle->setFont(m_bodyFont);
                plainToggle->setStyleSheet("color:#9A9AA8; background:transparent;");
                plainToggle->setFocusPolicy(Qt::NoFocus);   // clicking mustn't steal field focus
                hh->addWidget(plainToggle, 0, Qt::AlignRight | Qt::AlignTop);
            }
            rl->addWidget(headerLine);

            Row r;
            r.name = pi.name;
            r.kind = kind;
            r.isColor = isColor;
            r.isPos = isPos;
            r.header = h;
            r.container = row;
            r.plainToggle = plainToggle;
            r.annotation = pi.annotation;
            r.defaultVal = pi.defaultVal;
            r.optional = !pi.defaultVal.isEmpty();   // a default value => the argument is optional

            if (kind == 3) {
                // Bool: a checkbox. Its text mirrors the value (True/False), updated in refreshRow.
                auto *cb = new QCheckBox(row);
                cb->setFont(m_bodyFont);
                cb->setStyleSheet("color:#1A1A28; background:transparent;");
                rl->addWidget(cb);
                r.checkbox = cb;
                r.inputWidgets.append(cb);
            } else if (kind == 2) {
                // Tuple sub-fields: colour rows show r/g/b; position rows show x/y (2 only);
                // any other tuple shows x/y/z.
                static const char axisXYZ[3] = { 'x', 'y', 'z' };
                static const char axisRGB[3] = { 'r', 'g', 'b' };
                const char *axis = isColor ? axisRGB : axisXYZ;
                const int n = isPos ? 2 : 3;
                for (int a = 0; a < n; ++a) {
                    QWidget *sub = new QWidget(row);
                    auto *hl = new QHBoxLayout(sub);
                    hl->setContentsMargins(0, 0, 0, 0);
                    hl->setSpacing(6);
                    auto *lbl = new QLabel(QString(QChar::fromLatin1(axis[a])), sub);
                    lbl->setFont(m_bodyFont);
                    lbl->setStyleSheet("color:#9A9AA8; background:transparent;");
                    hl->addWidget(lbl);
                    PythonCodeEditor *e = makeFieldEditor(sub, ctx, /*plain*/false, QString());
                    hl->addWidget(e, 1);
                    rl->addWidget(sub);
                    r.edits.append(e);
                    r.inputWidgets.append(sub);
                }
            } else {
                // Plain expression, or a str literal (plain-text field). A str default is shown
                // unquoted as the placeholder, since the field edits the unquoted text.
                QString ph = pi.defaultVal;
                if (kind == 1 && ph.size() >= 2 &&
                    ((ph.startsWith(QLatin1Char('"')) && ph.endsWith(QLatin1Char('"'))) ||
                     (ph.startsWith(QLatin1Char('\'')) && ph.endsWith(QLatin1Char('\'')))))
                    ph = ph.mid(1, ph.size() - 2);
                PythonCodeEditor *e = makeFieldEditor(row, ctx, /*plain*/kind == 1, ph);
                rl->addWidget(e);
                r.edits.append(e);
                r.inputWidgets.append(e);
            }

            // Verbatim "plain" field (interpreted rows only): hidden until the toggle is checked,
            // then it replaces the interpreted widget(s) and its text is written through unchanged.
            if (interpretive) {
                PythonCodeEditor *pe = makeFieldEditor(row, ctx, /*plain*/false, QString());
                pe->setVisible(false);
                rl->addWidget(pe);
                r.plainEdit = pe;
            }

            if (!desc.isEmpty()) {
                auto *descLbl = new QLabel(desc, row);
                descLbl->setTextFormat(Qt::PlainText);
                descLbl->setWordWrap(true);
                descLbl->setFont(m_bodyFont);
                descLbl->setStyleSheet("color:#7A7A86; background:transparent;");
                rl->addWidget(descLbl);
            }

            m_rowsLay->insertWidget(m_rowsLay->count() - 1, row);   // before the trailing stretch
            m_rows.append(r);
            const int idx = m_rows.size() - 1;
            refreshRow(idx);                         // initial name colour + optional marker

            // QPlainTextEdit has no textEdited, so we use textChanged (which also fires on
            // programmatic changes) and rely on setValues() blocking signals so only genuine
            // user edits reach here. Recolour the name, sync the colour picker if this is the
            // bound colour row, then rewrite the call.
            for (PythonCodeEditor *e : r.edits) {
                QObject::connect(e, &QPlainTextEdit::textChanged, e, [this, idx]() {
                    refreshRow(idx);
                    const Row &cr = m_rows.at(idx);
                    if (!m_pickerSyncing && m_pickerRow == idx && m_colorPicker &&
                        m_colorPicker->isVisible() && cr.isColor && cr.edits.size() >= 3) {
                        m_colorPicker->setRgb(fieldInt(cr.edits.at(0)), fieldInt(cr.edits.at(1)),
                                              fieldInt(cr.edits.at(2)));
                    }
                    if (!m_posSyncing && m_posRow == idx && m_posPicker &&
                        m_posPicker->isVisible() && cr.isPos && cr.edits.size() >= 2) {
                        bool rx = false, ry = false;
                        const double nx = parseAxis(cr.edits.at(0)->toPlainText(), m_posFw, &rx);
                        const double ny = parseAxis(cr.edits.at(1)->toPlainText(), m_posFh, &ry);
                        m_posPicker->setState(nx, ny, rx || ry);
                    }
                    m_editor->applyParamEdit(QString(), QString(), false);
                });
            }
            if (r.checkbox) {   // bool row: toggling rewrites the call (omitting at the default)
                QObject::connect(r.checkbox, &QCheckBox::toggled, r.checkbox, [this, idx](bool) {
                    refreshRow(idx);
                    m_editor->applyParamEdit(QString(), QString(), false);
                });
            }
            // The verbatim field writes straight through; recolour the name + rewrite on each edit.
            if (r.plainEdit) {
                QObject::connect(r.plainEdit, &QPlainTextEdit::textChanged, r.plainEdit, [this, idx]() {
                    refreshRow(idx);
                    m_editor->applyParamEdit(QString(), QString(), false);
                });
            }
            // The "plain" toggle swaps a row between its interpreted widget(s) and the verbatim field.
            if (r.plainToggle) {
                QObject::connect(r.plainToggle, &QCheckBox::toggled, r.plainToggle, [this, idx](bool on) {
                    setRowPlain(idx, on);
                });
            }
        }
        return true;
    }

    // Set field texts to mirror the live arguments (called when the user edits in the editor,
    // not the panel). Signals are blocked so this never triggers write-back. The incoming
    // value is the code form ("hi" / (1, 2, 3) / expr); it's decoded into the field(s) per
    // type. A parameter absent from vals is cleared (its argument was removed in the editor).
    void setValues(const QMap<QString, QString> &vals) {
        for (int i = 0; i < m_rows.size(); ++i) {
            setRowFromCode(i, vals.value(m_rows.at(i).name));
            refreshRow(i);                        // name colour follows the in-use state
        }
    }

    // Code form of every field, keyed by parameter name (empty fields omitted). The write-back
    // rebuilds the whole argument list from this; str/tuple fields are encoded to source here.
    QMap<QString, QString> fieldValues() const {
        QMap<QString, QString> v;
        for (int i = 0; i < m_rows.size(); ++i) {
            const QString code = encodeRow(i);
            if (!code.isEmpty()) v.insert(m_rows.at(i).name, code);
        }
        return v;
    }

    // Close everything the fields themselves opened (nested panels AND their autofill/doc
    // pop-ups) plus the colour picker, so hiding this panel doesn't leave orphaned pop-ups.
    void hideNestedPanels() {
        if (m_colorPicker) m_colorPicker->hide();
        if (m_posPicker) m_posPicker->hide();
        m_pickerRow = -1;
        m_posRow = -1;
        for (int i = 0; i < m_rows.size(); ++i) {
            for (PythonCodeEditor *e : m_rows.at(i).edits) e->closeAllPopups();
            if (m_rows.at(i).plainEdit) m_rows.at(i).plainEdit->closeAllPopups();
        }
    }

    void showAt(const QPoint &globalCaret) {
        m_anchor = globalCaret;
        relayout();
        reposition();
        if (!isVisible()) show();
        raise();
    }

protected:
    // Esc closes every pop-up (the whole panel tree, autofill, and doc boxes) and returns
    // focus to the root script editor.
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape) {
            PythonCodeEditor *root = m_editor->m_contextEditor ? m_editor->m_contextEditor : m_editor;
            root->closeAllPopups();
            root->setFocus();
            e->accept();
            return;
        }
        QFrame::keyPressEvent(e);
    }

private:
    void clearRows() {
        // Delete every row widget immediately (not deleteLater) so the layout and the size
        // measurement in relayout() don't transiently include stale rows. The trailing stretch
        // is the only layout item that remains. Safe here: clearRows() is only reached from
        // buildFor(), never from a field's own signal handler. Close any nested pop-ups and
        // block signals first so a stray textChanged during teardown can't reach write-back.
        if (m_colorPicker) m_colorPicker->hide();
        if (m_posPicker) m_posPicker->hide();
        m_pickerRow = -1;
        m_posRow = -1;
        for (int i = 0; i < m_rows.size(); ++i) {
            for (PythonCodeEditor *e : m_rows.at(i).edits) {
                e->closeAllPopups();
                e->blockSignals(true);
            }
            if (m_rows.at(i).plainEdit) {
                m_rows.at(i).plainEdit->closeAllPopups();
                m_rows.at(i).plainEdit->blockSignals(true);
            }
            if (m_rows.at(i).checkbox)    m_rows.at(i).checkbox->blockSignals(true);
            if (m_rows.at(i).plainToggle) m_rows.at(i).plainToggle->blockSignals(true);
            delete m_rows.at(i).container;
        }
        m_rows.clear();
    }

    // Recolour a row's name. Only OPTIONAL parameters grey out while unused; required ones
    // always read dark. Optional parameters also carry an italic "(optional)" marker.
    void refreshRow(int i) {
        const Row &r = m_rows.at(i);
        bool filled;
        if (r.plain) {
            filled = r.plainEdit && !r.plainEdit->toPlainText().trimmed().isEmpty();
        } else if (r.kind == 3) {
            // A bool is "in use" (dark) when it will be emitted — i.e. it differs from its
            // default, or it's required. Mirror the value as the checkbox's own text.
            const bool checked = r.checkbox && r.checkbox->isChecked();
            const bool hasDefault = !r.defaultVal.isEmpty();
            const bool defChecked = (r.defaultVal.trimmed() == QLatin1String("True"));
            filled = !(hasDefault && checked == defChecked);
            if (r.checkbox)
                r.checkbox->setText(checked ? QStringLiteral("True") : QStringLiteral("False"));
        } else {
            filled = false;
            for (PythonCodeEditor *e : r.edits)
                if (!e->toPlainText().trimmed().isEmpty()) { filled = true; break; }
        }
        const bool grey = r.optional && !filled;
        QString html = QStringLiteral("<b><span style=\"color:%1;\">%2</span></b>")
                           .arg(grey ? QStringLiteral("#9A9AA8") : QStringLiteral("#1A1A28"),
                                r.name.toHtmlEscaped());
        if (!r.annotation.isEmpty())
            html += QStringLiteral(" <span style=\"color:#9A9AA8;\">: %1</span>")
                        .arg(r.annotation.toHtmlEscaped());
        if (r.optional)
            html += QStringLiteral(" <span style=\"color:#9A9AA8; font-style:italic;\">(optional)</span>");
        r.header->setText(html);
    }

    // ---- field-kind helpers ------------------------------------------------------------
    // Map a parameter's annotation to a field kind: 1 = str (plain-text, auto-quoted),
    // 2 = tuple (x/y/z sub-fields), 3 = bool (checkbox), 0 = plain expression.
    static int kindForAnnotation(const QString &annotation) {
        const QString base = annotation.section(QLatin1Char('['), 0, 0).trimmed().toLower();
        if (base == QLatin1String("str")) return 1;
        if (base == QLatin1String("tuple")) return 2;
        if (base == QLatin1String("bool")) return 3;
        return 0;
    }

    // Create one configured single-line field editor.
    PythonCodeEditor *makeFieldEditor(QWidget *parent, PythonCodeEditor *ctx, bool plain,
                                      const QString &placeholder) {
        auto *e = new PythonCodeEditor(parent);
        e->enableValueFieldMode(ctx, plain);
        e->setEditorFont(m_sigFont);
        if (!placeholder.isEmpty()) e->setPlaceholderText(placeholder);
        return e;
    }

    static void setFieldText(PythonCodeEditor *e, const QString &t) {
        if (e->toPlainText() == t) return;
        QSignalBlocker b(e);
        e->setPlainText(t);
    }

    // Encode a row's field(s) into the source written between the call's parens: str ->
    // "quoted", tuple -> (a, b, c), plain -> the expression verbatim. "" when unused.
    // Takes the row index (not a Row&) so Row needn't be declared before this point.
    QString encodeRow(int i) const {
        const Row &r = m_rows.at(i);
        if (r.plain)                             // "plain": written exactly as typed, no encoding
            return r.plainEdit ? r.plainEdit->toPlainText().trimmed() : QString();
        if (r.kind == 3) {                       // bool: True/False, omitted when at the default
            const bool checked = r.checkbox && r.checkbox->isChecked();
            const bool hasDefault = !r.defaultVal.isEmpty();
            const bool defChecked = (r.defaultVal.trimmed() == QLatin1String("True"));
            if (hasDefault && checked == defChecked) return QString();
            return checked ? QStringLiteral("True") : QStringLiteral("False");
        }
        if (r.kind == 1) {                       // str
            QString t = r.edits.at(0)->toPlainText().trimmed();
            if (t.isEmpty()) return QString();
            if (t.size() >= 2 &&
                ((t.startsWith(QLatin1Char('"'))  && t.endsWith(QLatin1Char('"'))) ||
                 (t.startsWith(QLatin1Char('\'')) && t.endsWith(QLatin1Char('\'')))))
                return t;                         // already a quoted literal — leave as-is
            t.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
            t.replace(QLatin1Char('"'),  QStringLiteral("\\\""));
            return QLatin1Char('"') + t + QLatin1Char('"');
        }
        if (r.kind == 2) {                       // tuple
            QStringList parts;
            for (PythonCodeEditor *e : r.edits) {
                const QString t = e->toPlainText().trimmed();
                if (!t.isEmpty()) parts << t;
            }
            if (parts.isEmpty()) return QString();
            return QLatin1Char('(') + parts.join(QStringLiteral(", ")) + QLatin1Char(')');
        }
        return r.edits.at(0)->toPlainText().trimmed();   // plain
    }

    // Decode an argument's source into a row's field(s): the inverse of encodeRow.
    void setRowFromCode(int i, const QString &code) {
        const Row &r = m_rows.at(i);
        if (r.plain) {                           // verbatim field mirrors the raw argument text
            if (r.plainEdit) setFieldText(r.plainEdit, code.trimmed());
            return;
        }
        if (r.kind == 3) {                       // bool: check from the arg, else from the default
            const QString t = code.trimmed();
            const bool checked = t.isEmpty() ? (r.defaultVal.trimmed() == QLatin1String("True"))
                                             : (t.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ||
                                                t == QLatin1String("1"));
            if (r.checkbox && r.checkbox->isChecked() != checked) {
                QSignalBlocker b(r.checkbox);
                r.checkbox->setChecked(checked);
            }
            return;
        }
        if (r.kind == 1) {                       // str: show the unquoted text
            QString t = code.trimmed();
            if (t.size() >= 2 &&
                ((t.startsWith(QLatin1Char('"'))  && t.endsWith(QLatin1Char('"'))) ||
                 (t.startsWith(QLatin1Char('\'')) && t.endsWith(QLatin1Char('\'')))))
                t = t.mid(1, t.size() - 2);
            t.replace(QStringLiteral("\\\""), QStringLiteral("\""));
            t.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
            setFieldText(r.edits.at(0), t);
        } else if (r.kind == 2) {                // tuple: split into x / y / z
            QString t = code.trimmed();
            if (t.size() >= 2 && t.startsWith(QLatin1Char('(')) && t.endsWith(QLatin1Char(')')))
                t = t.mid(1, t.size() - 2);
            const QStringList parts = splitTopLevel(t);
            for (int a = 0; a < r.edits.size(); ++a)
                setFieldText(r.edits.at(a), a < parts.size() ? parts.at(a).trimmed() : QString());
        } else {                                 // plain
            setFieldText(r.edits.at(0), code);
        }
    }

    // Toggle a row between its interpreted widget(s) and the single verbatim code field. Entering
    // plain seeds that field with the current encoded value (so nothing is lost); leaving it parses
    // the text back into the interpreted widget(s). Either way the call is rewritten and the panel
    // re-measured (the row's height changes), and any picker bound to the row is dismissed.
    void setRowPlain(int i, bool on) {
        if (i < 0 || i >= m_rows.size()) return;
        Row &r = m_rows[i];
        if (!r.plainEdit || r.plain == on) return;
        if (on) {
            const QString code = encodeRow(i);          // r.plain still false -> interpreted form
            r.plain = true;
            setFieldText(r.plainEdit, code);
            if (m_colorPicker && m_pickerRow == i) { m_colorPicker->hide(); m_pickerRow = -1; }
            if (m_posPicker   && m_posRow == i)    { m_posPicker->hide();   m_posRow = -1; }
        } else {
            const QString code = r.plainEdit->toPlainText();
            r.plain = false;
            setRowFromCode(i, code);                     // r.plain now false -> interpreted decode
        }
        for (QWidget *w : r.inputWidgets) w->setVisible(!on);
        r.plainEdit->setVisible(on);
        refreshRow(i);
        relayout();                                      // the row's height changed
        reposition();
        m_editor->applyParamEdit(QString(), QString(), false);
    }

    // ---- colour-picker helpers ---------------------------------------------------------
    static int fieldInt(PythonCodeEditor *e) {
        bool ok = false;
        const int n = e->toPlainText().trimmed().toInt(&ok);
        return ok ? n : 0;
    }

    // Row index whose colour field is w, or -1. Used to show/bind the picker on focus.
    int colorRowOf(QWidget *w) const {
        if (!w) return -1;
        for (int i = 0; i < m_rows.size(); ++i) {
            if (!m_rows.at(i).isColor || m_rows.at(i).plain) continue;   // plain row: no picker
            for (PythonCodeEditor *e : m_rows.at(i).edits)
                if (e == w) return i;
        }
        return -1;
    }

    // Bind the picker to colour row i, seed it from the r/g/b fields, and float it beside the
    // panel. (No-op if the row isn't a 3-field colour row.)
    void showPickerFor(int i) {
        if (!m_colorPicker || i < 0 || i >= m_rows.size()) return;
        const Row &r = m_rows.at(i);
        if (!r.isColor || r.edits.size() < 3) return;
        m_pickerRow = i;
        m_colorPicker->setRgb(fieldInt(r.edits.at(0)), fieldInt(r.edits.at(1)), fieldInt(r.edits.at(2)));
        const QPoint tr = mapToGlobal(QPoint(width(), 0));
        m_colorPicker->move(tr.x() + kGap, tr.y());
        if (!m_colorPicker->isVisible()) m_colorPicker->show();
        m_colorPicker->raise();
    }

    // ---- position-picker helpers -------------------------------------------------------
    int posRowOf(QWidget *w) const {
        if (!w) return -1;
        for (int i = 0; i < m_rows.size(); ++i) {
            if (!m_rows.at(i).isPos || m_rows.at(i).plain) continue;   // plain row: no picker
            for (PythonCodeEditor *e : m_rows.at(i).edits)
                if (e == w) return i;
        }
        return -1;
    }

    // Cache the preview frame dimensions and dx for this picking session.
    void refreshPosMetrics() {
        m_posFw = 640; m_posFh = 480; m_posDx = 1.0;
        if (g_frameBuffer && g_frameBuffer->isOpen()) {
            g_frameBuffer->refreshHeader();
            const FrameBufferReader::Header &h = g_frameBuffer->header();
            if (h.magic == FrameBufferReader::MAGIC && h.width > 0 && h.height > 0) {
                m_posFw = h.width; m_posFh = h.height;
            }
        }
        QFile f(QStringLiteral("settings.txt"));
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            if (lines.size() > 3) {
                bool ok = false;
                const double d = lines.at(3).trimmed().toDouble(&ok);
                if (ok && d > 0) m_posDx = d;
            }
        }
    }

    // Parse one axis field's code to a normalised fraction (0..1); sets *rel if it's the
    // renderer-relative form. Understands the two forms the picker writes plus a bare pixel.
    double parseAxis(const QString &code, double frameDim, bool *rel) const {
        static const QRegularExpression relRe(
            QStringLiteral("renderer\\.(?:width|height)\\s*\\(\\s*\\)\\s*\\*\\s*([0-9.]+)"));
        QRegularExpressionMatch m = relRe.match(code);
        if (m.hasMatch()) { *rel = true; return qBound(0.0, m.captured(1).toDouble(), 1.0); }
        static const QRegularExpression absRe(QStringLiteral("([0-9.]+)\\s*\\*\\s*dx"));
        m = absRe.match(code);
        if (m.hasMatch()) {
            *rel = false;
            const double R = m.captured(1).toDouble();
            return frameDim > 0 ? qBound(0.0, R * m_posDx / frameDim, 1.0) : 0.5;
        }
        bool ok = false;
        const double v = code.trimmed().toDouble(&ok);   // a bare pixel value
        if (ok) { *rel = false; return frameDim > 0 ? qBound(0.0, v / frameDim, 1.0) : 0.5; }
        *rel = false;
        return 0.5;
    }

    // Bind the position picker to row i, seed it from the x/y fields, float it beside the panel.
    void showPosPickerFor(int i) {
        if (!m_posPicker || i < 0 || i >= m_rows.size()) return;
        const Row &r = m_rows.at(i);
        if (!r.isPos || r.edits.size() < 2) return;
        m_posRow = i;
        refreshPosMetrics();
        bool relX = false, relY = false;
        const double nx = parseAxis(r.edits.at(0)->toPlainText(), m_posFw, &relX);
        const double ny = parseAxis(r.edits.at(1)->toPlainText(), m_posFh, &relY);
        m_posPicker->setState(nx, ny, relX || relY);
        const QPoint tr = mapToGlobal(QPoint(width(), 0));
        m_posPicker->move(tr.x() + kGap, tr.y());
        if (!m_posPicker->isVisible()) m_posPicker->show();
        m_posPicker->raise();
    }

    // Fix the width, lay the rows out at that width to measure their wrapped height, then
    // cap the scroll viewport so a long signature still fits on screen (the rest scrolls).
    void relayout() {
        setFixedWidth(kPanelWidth);
        const int sbw = 10;
        const int hostW = kPanelWidth - 2 * kMargin - sbw;
        m_rowsHost->setFixedWidth(hostW);
        m_rowsHost->adjustSize();
        int hostH = m_rowsHost->sizeHint().height();
        if (hostH < 0) hostH = m_rowsHost->height();
        m_rowsHost->setFixedHeight(hostH);

        QScreen *scr = QGuiApplication::screenAt(m_anchor);
        if (!scr) scr = QGuiApplication::primaryScreen();
        const int capH = scr ? int(scr->availableGeometry().height() * 0.80) : 700;
        const int chrome = m_title->sizeHint().height() + 2 * kMargin + kTitleGap + 2;
        int scrollH = hostH;
        if (scrollH + chrome > capH) scrollH = qMax(80, capH - chrome);
        m_scroll->setFixedHeight(scrollH);
        adjustSize();
    }

    // Float just to the right of the caret; flip left if there's no room; clamp on-screen.
    void reposition() {
        QScreen *scr = QGuiApplication::screenAt(m_anchor);
        if (!scr) scr = QGuiApplication::primaryScreen();
        const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1920, 1080);
        const QSize sz = size();
        int x = m_anchor.x() + kGap;
        int y = m_anchor.y();
        if (x + sz.width() > avail.right()) x = m_anchor.x() - kGap - sz.width();   // flip left
        x = qBound(avail.left(), x, qMax(avail.left(), avail.right() - sz.width()));
        y = qBound(avail.top(), y, qMax(avail.top(), avail.bottom() - sz.height()));
        move(x, y);
    }

    static constexpr int kPanelWidth = 360;
    static constexpr int kMargin = 10;
    static constexpr int kTitleGap = 6;
    static constexpr int kGap = 8;

    // One parameter row: its name; the field kind (0 plain, 1 str, 2 tuple); whether it's a
    // @color tuple (r/g/b + picker); the input field(s) (one for plain/str, three for tuple);
    // the row container widget (deleted on rebuild); the header label (name + type + optional
    // marker, recoloured by use); the type annotation; and whether the argument is optional.
    struct Row {
        QString name;
        int kind = 0;
        bool isColor = false;
        bool isPos = false;                // @position tuple (x/y + frame picker)
        QList<PythonCodeEditor *> edits;   // empty for a bool row
        QCheckBox *checkbox = nullptr;     // the input for a bool row
        QString defaultVal;                // the parameter's default (to omit bools at default)
        QWidget *container = nullptr;
        QLabel *header = nullptr;
        QString annotation;
        bool optional = false;
        bool plain = false;                       // "plain" toggle on: the field is written verbatim
        QCheckBox *plainToggle = nullptr;         // the right-aligned "plain" checkbox (interpreted rows)
        QList<QWidget *> inputWidgets;            // interpreted input widget(s), hidden while plain
        PythonCodeEditor *plainEdit = nullptr;    // single verbatim code field, shown while plain
    };

    PythonCodeEditor *m_editor = nullptr;
    QLabel *m_title = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_rowsHost = nullptr;
    QVBoxLayout *m_rowsLay = nullptr;
    QList<Row> m_rows;          // one per documented parameter, in signature order
    ColorPicker *m_colorPicker = nullptr;   // shared inline picker for @color rows
    int m_pickerRow = -1;                   // row the colour picker is currently bound to
    bool m_pickerSyncing = false;           // guard: colour picker is driving the fields
    PosPicker *m_posPicker = nullptr;       // shared frame picker for @position rows
    int m_posRow = -1;                      // row the position picker is bound to
    bool m_posSyncing = false;              // guard: position picker is driving the fields
    double m_posFw = 640, m_posFh = 480, m_posDx = 1.0;   // cached frame dims + dx for the session
    QFont m_sigFont;
    QFont m_bodyFont;
    QPoint m_anchor;
};

PythonCodeEditor::PythonCodeEditor(QWidget *parent)
    : SearchTextEdit(parent), lineNumberArea(new LineNumberArea(this)) {
    // Connect signals to update line number area and highlight the current line
    connect(this, &SearchTextEdit::blockCountChanged, this, &PythonCodeEditor::updateLineNumberAreaWidth);
    connect(this, &SearchTextEdit::updateRequest, this, &PythonCodeEditor::updateLineNumberArea);
    connect(this, &SearchTextEdit::cursorPositionChanged, this, &PythonCodeEditor::highlightCurrentLine);

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();

    // Completion popup: a QListView (so it scrolls and accepts mouse clicks for free)
    // driven by a QStandardItemModel we refill per context. We use QStandardItemModel
    // (not QStringListModel) so each row can carry its own foreground colour, matching the
    // syntax highlighting. Case-insensitive so "ge" matches "get_frame"; UnsortedModel
    // because we sort the candidates ourselves.
    m_completionModel = new QStandardItemModel(this);
    m_completer = new QCompleter(this);
    m_completer->setModel(m_completionModel);
    m_completer->setWidget(this);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setModelSorting(QCompleter::UnsortedModel);
    m_completer->setWrapAround(false);
    connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated),
            this, &PythonCodeEditor::insertCompletion);

    // Documentation/signature box: shown to the right of the popup for the highlighted
    // entry, and at the mouse when hovering a method. It lives as long as the editor.
    m_docBox = new CompletionDocBox(this);
    connect(m_completer, QOverload<const QString &>::of(&QCompleter::highlighted),
            this, &PythonCodeEditor::showCompletionDocFor);
    // The completer hides its popup itself in several cases (Esc, pick, click-away); a
    // Hide event-filter is the one place that catches them all, so we hide the doc box too.
    // We also filter the popup's viewport for pointer events: hover-to-highlight (MouseMove
    // changes the current row) and wheel-forwarding (so the wheel scrolls the doc box that
    // sits beside the popup, which otherwise swallows it via its mouse grab). Mouse tracking
    // must be on so MouseMove arrives with no button held.
    m_completer->popup()->installEventFilter(this);
    m_completer->popup()->viewport()->installEventFilter(this);
    m_completer->popup()->setMouseTracking(true);
    m_completer->popup()->viewport()->setMouseTracking(true);

    // Hover doc: a short grace period before hiding so the pointer can travel from the
    // method onto the box (to click "More ▾") without it disappearing.
    m_docHideTimer = new QTimer(this);
    m_docHideTimer->setSingleShot(true);
    m_docHideTimer->setInterval(220);
    connect(m_docHideTimer, &QTimer::timeout, this, [this]() {
        if (!m_docBoxHovered && m_docBox) m_docBox->hide();
    });
    viewport()->setMouseTracking(true);   // so viewportEvent sees MouseMove for hover tracking

    // Parameter-input panel: pops up to the right of the caret when it's inside a documented
    // call's parentheses, listing each parameter with an input box that writes "name=value"
    // back into the call. Driven by the same cursorPositionChanged the highlight uses.
    m_paramPanel = new ParamPanel(this);
    connect(this, &SearchTextEdit::cursorPositionChanged, this, &PythonCodeEditor::updateParamPanel);

    // Grace period before hiding, mirroring the hover doc: lets focus settle (e.g. moving
    // from the editor into a field) without the panel vanishing. The panel stays open while
    // it (or one of its fields) holds focus.
    m_paramHideTimer = new QTimer(this);
    m_paramHideTimer->setSingleShot(true);
    m_paramHideTimer->setInterval(220);
    connect(m_paramHideTimer, &QTimer::timeout, this, [this]() {
        if (!m_paramPanel) return;
        if (focusInPanelTree(QApplication::focusWidget())) return;   // user is filling a field
        hideParamPanel();
    });
    // Close the panel when focus leaves both the editor and the panel (clicked another
    // widget / window). Returning focus into the editor keeps it (the caret re-syncs).
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        if (!m_paramPanel || !m_paramPanel->isVisible()) return;
        // A null "now" is the transient that fires while one of our tool windows activates
        // (e.g. clicking a field in a nested panel). Treating it as "focus left" would slam
        // every panel shut, so ignore it and wait for the real target. focusInPanelTree walks
        // the QObject parent chain so nested-panel fields (separate tool windows) still count.
        if (!now) return;
        if (now == this) return;
        if (focusInPanelTree(now)) return;
        hideParamPanel();
    });
}

int PythonCodeEditor::lineNumberAreaWidth() const {
    if (m_valueFieldMode) return 0;   // value fields show no line-number gutter
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    return 3 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void PythonCodeEditor::setEditorFont(const QFont &font) {
    SearchTextEdit::setEditorFont(font);

    // Set the same font for the line number area
    if (lineNumberArea) {
        lineNumberArea->setFont(font);
    }

    // Update the line number area width since font metrics may have changed
    updateLineNumberAreaWidth(0);

    // Match the autocomplete popup and the signature/doc box to the editor's typeface, so
    // the suggestions and signatures read in the same font as the code rather than the
    // default system UI font. Done here (the single place the editor font is set) so they
    // follow any later font change automatically.
    if (m_completer && m_completer->popup())
        m_completer->popup()->setFont(font);
    if (m_docBox)
        m_docBox->setBoxFont(font);
    if (m_paramPanel)
        m_paramPanel->setBoxFont(font);

    // A value field is exactly one text line tall; size it to the (now known) font.
    if (m_valueFieldMode)
        setFixedHeight(fontMetrics().lineSpacing() + 12);
}

void PythonCodeEditor::enableValueFieldMode(PythonCodeEditor *contextEditor, bool plainText) {
    m_valueFieldMode = true;
    m_plainField = plainText;
    m_contextEditor = contextEditor;
    if (lineNumberArea) lineNumberArea->hide();
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // NOTE: not setTabChangesFocus — Tab is handled in keyPressEvent so it can first accept an
    // open autofill suggestion, and only move to the next field when no popup is showing.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    updateLineNumberAreaWidth(0);             // gutter is now 0 wide
    // Code fields get the editor's syntax highlighting (the highlighter is owned by the
    // document, so it's freed with this editor). A plain-text field (e.g. a str argument) is
    // taken verbatim, so it gets neither highlighting nor autocomplete.
    if (!plainText) new PythonHighlighter(document());
    // Height is finalised in setEditorFont(), once the field font is applied.
}

void PythonCodeEditor::toggleComment(QTextCursor &cursor) {
    cursor.beginEditBlock();

    int startBlock = cursor.selectionStart();
    int endBlock = cursor.selectionEnd();

    QTextBlock start = document()->findBlock(startBlock);
    QTextBlock end = document()->findBlock(endBlock);

    for (QTextBlock block = start; block.isValid() && block.position() <= end.position(); block = block.next()) {
        QString text = block.text();
        if (text.trimmed().startsWith("#")) {
            int index = text.indexOf('#');
            cursor.setPosition(block.position() + index);
            cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor);
            cursor.removeSelectedText();
        } else {
            cursor.setPosition(block.position());
            cursor.insertText("#");
        }
    }

    cursor.endEditBlock();
}

void PythonCodeEditor::indentSelection(QTextCursor &cursor, bool unindent) {
    cursor.beginEditBlock();

    int startBlock = cursor.selectionStart();
    int endBlock = cursor.selectionEnd();

    QTextBlock start = document()->findBlock(startBlock);
    QTextBlock end = document()->findBlock(endBlock);

    for (QTextBlock block = start; block.isValid() && block.position() <= end.position(); block = block.next()) {
        QString text = block.text();
        cursor.setPosition(block.position());

        if (unindent) {
            if (text.startsWith("    ")) {
                cursor.movePosition(QTextCursor::Right, QTextCursor::KeepAnchor, 4);
                cursor.removeSelectedText();
            }
        } else {
            cursor.insertText("    ");
        }
    }

    cursor.endEditBlock();
}

void PythonCodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event) {
    QPainter painter(lineNumberArea);
    painter.fillRect(event->rect(),  QColor(205, 205, 220));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = static_cast<int>(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + static_cast<int>(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const QString number = QString::number(blockNumber + 1);
            // Make an error line's number stand out in the gutter: bold + red.
            const bool hasError = m_errors.contains(blockNumber + 1);
            QFont f = lineNumberArea->font();
            f.setBold(hasError);
            painter.setFont(f);
            painter.setPen(hasError ? QColor(220, 0, 0) : QColor(25, 25, 40));
            painter.drawText(0, top, lineNumberArea->width(), fontMetrics().height(),
                             Qt::AlignRight, number);
        }

        block = block.next();
        top = bottom;
        bottom = top + static_cast<int>(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

void PythonCodeEditor::resizeEvent(QResizeEvent *event) {
    SearchTextEdit::resizeEvent(event);
    QRect cr = contentsRect();
    lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void PythonCodeEditor::updateLineNumberAreaWidth(int /* newBlockCount */) {
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void PythonCodeEditor::updateLineNumberArea(const QRect &rect, int dy) {
    if (dy) {
        lineNumberArea->scroll(0, dy);
    } else {
        lineNumberArea->update(0, rect.y(), lineNumberArea->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);
    }
}

void PythonCodeEditor::highlightCurrentLine() {
    QList<QTextEdit::ExtraSelection> extraSelections;

    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        QColor lineColor = QColor(QColor(245, 245, 255));

        selection.format.setBackground(lineColor);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        extraSelections.append(selection);
    }

    setExtraSelections(extraSelections);
}

void PythonCodeEditor::setErrors(const QMap<int, LineError> &errors) {
    m_errors = errors;
    viewport()->update();       // repaint so paintEvent (re)draws the underlines
    lineNumberArea->update();   // repaint the gutter so error numbers turn red
}

void PythonCodeEditor::clearErrors() {
    if (m_errors.isEmpty()) return;
    m_errors.clear();
    viewport()->update();
    lineNumberArea->update();
}

// ===================== Member autocomplete ==============================
//
// We expose, on ".", an alphabetical list of the methods available on the object
// to the left of the dot. The list of classes -> methods is parsed straight from
// the engine's own Python sources (so it always matches the real API and updates
// automatically when fields/filters/classes are added), and inheritance is
// resolved so e.g. a Blur filter also offers Filter's methods.

void PythonCodeEditor::invalidateApiTable() { s_tableBuilt = false; }

void PythonCodeEditor::ensureApiTable() {
    if (!s_tableBuilt) buildApiTable();
}

// True if a parameter list (the text between "(" and ")") expects an argument from
// the caller — i.e. it has any parameter beyond a leading self/cls. Used to decide
// whether the caret lands inside the auto-inserted "()".
static bool methodTakesArgs(const QString &paramStr) {
    const QString p = paramStr.trimmed();
    if (p.isEmpty()) return false;
    QStringList parts = p.split(',');               // a yes/no answer doesn't need real arg parsing
    const QString first = parts.first().trimmed();
    if (first == "self" || first == "cls" ||
        first.startsWith("self:") || first.startsWith("cls:") ||
        first.startsWith("self ") || first.startsWith("cls "))
        parts.removeFirst();
    for (const QString &r : parts)
        if (!r.trimmed().isEmpty()) return true;
    return false;
}

// The parameter list as the user should see it in the signature box: the raw text
// between "(" and ")" with a leading self/cls (and its annotation/comma) stripped, but
// otherwise verbatim so default values and type hints keep the author's formatting.
static QString cleanParams(const QString &paramStr) {
    QString p = paramStr.trimmed();
    if (p.isEmpty()) return QString();
    static const QRegularExpression selfRe(R"(^(?:self|cls)\b\s*(?::[^,=]*)?\s*(?:,\s*)?)");
    const QRegularExpressionMatch m = selfRe.match(p);
    if (m.hasMatch()) p = p.mid(m.capturedLength());
    return p.trimmed();
}

// Pull a method's docstring out of the source lines. Starts scanning at the line after
// the def (defIdx); if the first non-blank body line opens a triple-quoted string, the
// (dedented) contents become the doc. *lastIdx is set to the index of the docstring's
// final line — equal to defIdx when there is no docstring — so the caller can skip past
// the consumed lines without re-parsing the string body as code.
static QString extractDocstring(const QStringList &lines, int defIdx, int *lastIdx) {
    *lastIdx = defIdx;
    int i = defIdx + 1;
    while (i < lines.size() && lines.at(i).trimmed().isEmpty()) ++i;   // skip blank lines
    if (i >= lines.size()) return QString();

    const QString firstTrim = lines.at(i).trimmed();
    static const QRegularExpression openRe(R"(^[rRbBuUfF]{0,2}("""|'''))");
    const QRegularExpressionMatch om = openRe.match(firstTrim);
    if (!om.hasMatch()) return QString();   // first statement isn't a docstring
    const QString quote = om.captured(1);
    const QString firstContent = firstTrim.mid(om.capturedLength());

    QStringList body;
    int closeLine;
    const int closeInFirst = firstContent.indexOf(quote);
    if (closeInFirst >= 0) {                 // single-line docstring: """text"""
        body << firstContent.left(closeInFirst);
        closeLine = i;
    } else {
        body << firstContent;
        closeLine = lines.size() - 1;        // default to EOF if unterminated
        for (int j = i + 1; j < lines.size(); ++j) {
            const int c = lines.at(j).indexOf(quote);
            if (c >= 0) { body << lines.at(j).left(c); closeLine = j; break; }
            body << lines.at(j);
        }
    }
    *lastIdx = closeLine;

    // Dedent the continuation lines by their common leading whitespace (the first line
    // sat right after the quotes, so it's just trimmed), then drop blank top/bottom lines.
    int common = -1;
    for (int k = 1; k < body.size(); ++k) {
        const QString &l = body.at(k);
        if (l.trimmed().isEmpty()) continue;
        int ws = 0;
        while (ws < l.size() && (l.at(ws) == ' ' || l.at(ws) == '\t')) ++ws;
        if (common < 0 || ws < common) common = ws;
    }
    if (common < 0) common = 0;

    QStringList out;
    out << body.first().trimmed();
    for (int k = 1; k < body.size(); ++k)
        out << body.at(k).mid(qMin(common, body.at(k).size()));
    while (!out.isEmpty() && out.first().trimmed().isEmpty()) out.removeFirst();
    while (!out.isEmpty() && out.last().trimmed().isEmpty())  out.removeLast();
    return out.join('\n');
}

void PythonCodeEditor::buildApiTable() {
    s_members.clear();
    s_globalTypes.clear();
    s_classInfo.clear();
    // Well-known globals the user's script gets via "from neovere import *".
    s_globalTypes.insert("renderer", "NonlinearRenderer");

    // Per-class own members + declared base classes (inheritance resolved below).
    QMap<QString, QMap<QString, MemberInfo>> ownMembers;
    QMap<QString, QStringList> bases;

    // Built-in engine API + any custom classes the user has added on disk. These mirror
    // the three documentation categories, so the popup updates exactly when the docs do.
    QStringList sources;
    const QStringList cats = { "classes", "fields", "filters" };
    for (const QString &cat : cats)
        sources << (":/resources/code/" + cat + ".py");
    for (const QString &cat : cats) {
        QDir d("classes/" + cat);
        d.setNameFilters(QStringList() << "*.py");
        d.setFilter(QDir::Files);
        const QStringList files = d.entryList();
        for (const QString &f : files)
            sources << d.absoluteFilePath(f);
    }
    // Finally the runtime module itself (written to the working dir and imported by the
    // engine). It is the authoritative source for classes the category files don't split out
    // — e.g. the static-method utilities Audio_Analyzer / Color_Picker / Media_Optimizer — so
    // calls on those get a parameter panel too. Parsed LAST and only for classes no earlier
    // source defined, so the curated category signatures/docstrings stay authoritative.
    const QString fallbackSource = QStringLiteral("neovere.py");
    sources << fallbackSource;

    // Same class/def shapes as documentPython()'s regexes, but we also capture
    // indentation (so a method is only attributed to a class when it's a *direct*
    // child), the parameter list + return annotation, and public "self.x = ..." attrs.
    static const QRegularExpression classRe(R"(^(\s*)class\s+(\w+)\s*(?:\(([^)]*)\))?\s*:)");
    static const QRegularExpression defRe(R"(^(\s*)def\s+(\w+)\s*\((.*)$)");
    static const QRegularExpression attrRe(R"(^(\s+)self\.(\w+)\s*=\s*(.*)$)");
    static const QRegularExpression ctorRhsRe(R"(^([A-Za-z_]\w*)\s*\()");
    static const QRegularExpression identRe(R"(^[A-Za-z_]\w*$)");

    for (const QString &path : sources) {
        const bool isFallback = (path == fallbackSource);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QStringList lines;
        {
            QTextStream in(&file);
            while (!in.atEnd()) lines << in.readLine();
        }
        file.close();

        QString curClass;     // class whose body we're currently inside ("" = none)
        int classIndent = -1; // indentation of that class's "class" keyword

        for (int li = 0; li < lines.size(); ++li) {
            const QString line = lines.at(li);

            const QRegularExpressionMatch cm = classRe.match(line);
            if (cm.hasMatch()) {
                const QString name = cm.captured(2);
                classIndent = cm.captured(1).length();
                // The fallback module repeats the category-file classes; keep those curated
                // definitions and take only classes no earlier source defined. Clearing
                // curClass makes the member loop skip this duplicate class's body.
                if (isFallback && ownMembers.contains(name)) {
                    curClass.clear();
                    continue;
                }
                curClass = name;
                if (!ownMembers.contains(name)) ownMembers.insert(name, {});

                // Seed the class's constructor/doc record. A class is callable, so the doc
                // box treats it like a method (returnType left empty => no "-> ..."). The
                // docstring right under "class X:" becomes the description; __init__ (below)
                // fills in the parameter list.
                int classDocLast = li;
                const QString cdoc = extractDocstring(lines, li, &classDocLast);
                MemberInfo &cinfo = s_classInfo[name];
                cinfo.isMethod = true;
                if (!cdoc.isEmpty()) cinfo.doc = cdoc;
                li = classDocLast;   // don't re-parse the class docstring as body code

                QStringList bl;
                const QString inside = cm.captured(3);
                if (!inside.isEmpty()) {
                    const QStringList parts = inside.split(',');
                    for (QString b : parts) {
                        b = b.trimmed();
                        if (b.isEmpty() || b.contains('=')) continue; // skip metaclass=… etc.
                        if (b.contains('.')) b = b.section('.', -1);  // module.Qualified -> Qualified
                        if (identRe.match(b).hasMatch()) bl << b;
                    }
                }
                bases.insert(name, bl);
                continue;
            }

            if (curClass.isEmpty()) continue;   // only parse members while inside a class body

            const QRegularExpressionMatch dm = defRe.match(line);
            if (dm.hasMatch()) {
                const int di = dm.captured(1).length();
                if (di <= classIndent) {           // dedented out of the class body
                    curClass.clear();
                    classIndent = -1;
                    continue;
                }
                if (di == classIndent + 4) {       // direct method (codebase uses 4-space indent)
                    const QString mname = dm.captured(2);
                    const bool isCtor = (mname == "__init__");
                    if (isCtor || !mname.startsWith('_')) {  // public methods + the constructor
                        // A signature may span several lines (one parameter per line, as
                        // FEllipse/FText do). Walk forward counting parentheses until the
                        // parameter list's "(" closes; sigEndLi is that closing line, so the
                        // docstring is searched after the real signature end instead of being
                        // mistaken for a parameter line (which left those ctors blank).
                        QString acc = dm.captured(3);   // text after the opening "("
                        int sigEndLi = li;
                        int depth = 1, closeIdx = -1, scanFrom = 0;
                        for (;;) {
                            for (int k = scanFrom; k < acc.size(); ++k) {
                                const QChar ch = acc.at(k);
                                if (ch == '(') ++depth;
                                else if (ch == ')' && --depth == 0) { closeIdx = k; break; }
                            }
                            if (closeIdx >= 0 || sigEndLi + 1 >= lines.size()) break;
                            scanFrom = acc.size() + 1;   // resume past the newline we append
                            acc += QLatin1Char('\n');
                            acc += lines.at(++sigEndLi);
                        }
                        // Parameter list flattened to one line (so a multi-line signature reads
                        // like the single-line ones) plus the optional "-> ReturnType".
                        QString params = (closeIdx >= 0) ? acc.left(closeIdx) : acc;
                        static const QRegularExpression wsRe(R"(\s+)");
                        params = params.replace(wsRe, " ").trimmed();
                        if (params.endsWith(',')) params = params.chopped(1).trimmed();
                        QString ret;
                        if (closeIdx >= 0) {
                            const int arrow = acc.indexOf("->", closeIdx);
                            if (arrow >= 0) {
                                const int colon = acc.indexOf(':', arrow);
                                ret = (colon >= 0 ? acc.mid(arrow + 2, colon - arrow - 2)
                                                  : acc.mid(arrow + 2)).trimmed();
                            }
                        }
                        int docLast = sigEndLi;
                        const QString doc = extractDocstring(lines, sigEndLi, &docLast);
                        if (isCtor) {
                            // Feed the constructor's signature into the class record so
                            // "ClassName(" completes with the right parens + doc box. Keep
                            // the class docstring if we already have one; else use __init__'s.
                            MemberInfo &cinfo = s_classInfo[curClass];
                            cinfo.isMethod  = true;
                            cinfo.params    = cleanParams(params);
                            cinfo.takesArgs = methodTakesArgs(params);
                            if (cinfo.doc.isEmpty()) cinfo.doc = doc;
                            cinfo.paramDoc = doc;   // __init__'s docstring drives the param panel
                        } else {
                            MemberInfo mi;
                            mi.isMethod  = true;
                            mi.takesArgs = methodTakesArgs(params);
                            mi.returnType = ret;
                            mi.params = cleanParams(params);
                            mi.doc = doc;
                            mi.paramDoc = doc;
                            ownMembers[curClass].insert(mname, mi);
                        }
                        li = docLast;   // skip docstring lines so their text isn't parsed as code
                    }
                }
                // deeper than one level => nested function inside a method; ignore.
                continue;
            }

            // Public instance attribute: "self.name = <rhs>" anywhere in the class body.
            const QRegularExpressionMatch am = attrRe.match(line);
            if (am.hasMatch() && am.captured(1).length() > classIndent) {
                const QString aname = am.captured(2);
                if (aname.startsWith('_')) continue;          // _protected / __private
                const QRegularExpressionMatch ctorM = ctorRhsRe.match(am.captured(3).trimmed());
                if (!ownMembers[curClass].contains(aname)) {
                    MemberInfo mi;
                    mi.isMethod = false;
                    if (ctorM.hasMatch()) mi.attrType = ctorM.captured(1);
                    ownMembers[curClass].insert(aname, mi);
                } else if (!ownMembers[curClass].value(aname).isMethod && ctorM.hasMatch()) {
                    // A later, clearer assignment (e.g. self.audio = None then
                    // self.audio = Audio(...)) — adopt the constructor-derived type.
                    MemberInfo mi = ownMembers[curClass].value(aname);
                    mi.attrType = ctorM.captured(1);
                    ownMembers[curClass].insert(aname, mi);
                }
                continue;
            }
        }
    }

    // Flatten inheritance: each class offers its own members plus every base's
    // (a child's own member overrides an inherited one of the same name).
    const QStringList classNames = ownMembers.keys();
    for (const QString &cls : classNames) {
        QMap<QString, MemberInfo> acc;
        QSet<QString> visited;
        QList<QString> stack;
        stack << cls;
        while (!stack.isEmpty()) {
            const QString c = stack.takeFirst();
            if (visited.contains(c)) continue;
            visited.insert(c);
            const QMap<QString, MemberInfo> &own = ownMembers[c];
            for (auto it = own.constBegin(); it != own.constEnd(); ++it)
                if (!acc.contains(it.key())) acc.insert(it.key(), it.value());
            for (const QString &b : bases.value(c)) stack << b;
        }
        s_members.insert(cls, acc);
    }

    s_tableBuilt = true;
}

QString PythonCodeEditor::nearestAssignmentRhs(const QString &name) const {
    // A value field resolves names against the whole script it belongs to (so e.g. "video."
    // knows video's type); a normal editor looks only at the text above the caret.
    const QString text = m_contextEditor ? m_contextEditor->toPlainText()
                                          : toPlainText().left(textCursor().position());
    QRegularExpression assignRe(
        QStringLiteral(R"((?m)^[ \t]*%1[ \t]*=[ \t]*(.+)$)")
            .arg(QRegularExpression::escape(name)));
    QString rhs;
    QRegularExpressionMatchIterator it = assignRe.globalMatch(text);
    while (it.hasNext()) rhs = it.next().captured(1).trimmed();  // keep the last one
    return rhs;
}

QString PythonCodeEditor::resolveReturnClass(const QString &typeStr) const {
    const QString t = typeStr.trimmed();
    if (t.isEmpty()) return QString();
    if (s_members.contains(t)) return t;               // exact, e.g. "Frame"
    // Pull the first known class name out of wrappers like Optional[Frame],
    // Union[Frame, None], List[Frame], or a 'Audio' forward reference.
    static const QRegularExpression wordRe(R"([A-Za-z_]\w*)");
    QRegularExpressionMatchIterator it = wordRe.globalMatch(t);
    while (it.hasNext()) {
        const QString w = it.next().captured(0);
        if (s_members.contains(w)) return w;
    }
    return QString();
}

QString PythonCodeEditor::inferType(const QString &expr, int depth) const {
    if (depth > 6) return QString();
    const QString e = expr.trimmed();
    if (e.isEmpty()) return QString();

    // <C> / <S> are a balanced () call group / [] subscript group whose contents may contain
    // ONE further level of nested () / [] — so a tuple or nested-call argument is spanned. The
    // old "[^()]*" couldn't cross a nested paren, so "Solid_Color((255,0,0))" wasn't recognised
    // as a constructor call and chained member resolution on it broke.
    static const QString callGrp = QStringLiteral(R"(\((?:[^()\[\]]|\([^()]*\)|\[[^\[\]]*\])*\))");
    static const QString subGrp  = QStringLiteral(R"(\[(?:[^()\[\]]|\([^()]*\)|\[[^\[\]]*\])*\])");

    // ---- head: media[...] | ClassName(...) | identifier ----
    static const QRegularExpression headMedia(
        QString(QStringLiteral(R"(^media\s*<S>)")).replace(QStringLiteral("<S>"), subGrp));
    static const QRegularExpression headCtor(
        QString(QStringLiteral(R"(^([A-Za-z_]\w*)\s*<C>)")).replace(QStringLiteral("<C>"), callGrp));
    static const QRegularExpression headIdent(R"(^[A-Za-z_]\w*)");

    QString curType;
    int i = 0;
    QRegularExpressionMatch m;
    if ((m = headMedia.match(e)).hasMatch()) {
        curType = "Video";                          // media[...] holds clips/images; clips dominate
        i = m.capturedLength();
    } else if ((m = headCtor.match(e)).hasMatch() && s_members.contains(m.captured(1))) {
        curType = m.captured(1);                    // ClassName(...) -> an instance
        i = m.capturedLength();
    } else if ((m = headIdent.match(e)).hasMatch()) {
        const QString id = m.captured(0);
        i = m.capturedLength();
        if (s_globalTypes.contains(id))   curType = s_globalTypes.value(id);
        else if (s_members.contains(id))  curType = id;   // a class name used directly ("Frame.")
        else {
            const QString rhs = nearestAssignmentRhs(id);
            curType = rhs.isEmpty() ? QString() : inferType(rhs, depth + 1);
        }
    } else {
        return QString();
    }

    // ---- trailing chain:  .name  ( call )?  [ sub ]?  ----
    static const QRegularExpression seg(
        QString(QStringLiteral(R"(^\s*\.\s*([A-Za-z_]\w*)\s*(<C>)?\s*(<S>)?)"))
            .replace(QStringLiteral("<C>"), callGrp).replace(QStringLiteral("<S>"), subGrp));
    while (i < e.length()) {
        const QRegularExpressionMatch sm = seg.match(e.mid(i));
        if (!sm.hasMatch()) break;
        if (curType.isEmpty() || !s_members.contains(curType) ||
            !s_members.value(curType).contains(sm.captured(1)))
            return QString();
        const MemberInfo mi = s_members.value(curType).value(sm.captured(1));
        const bool isCall = !sm.captured(2).isEmpty();
        if (isCall) {
            if (!mi.isMethod) return QString();
            curType = resolveReturnClass(mi.returnType);
        } else {
            // ".name" with no "()": a bare method reference has no useful type; an
            // attribute resolves to its (best-effort) class.
            curType = mi.isMethod ? QString() : resolveReturnClass(mi.attrType);
        }
        if (curType.isEmpty()) return QString();
        i += sm.capturedLength();
    }
    return curType;
}

QStringList PythonCodeEditor::completionsForContext(QString *prefixOut) {
    ensureApiTable();
    m_completionClass.clear();
    m_completionMeta.clear();
    const QTextCursor tc = textCursor();
    if (tc.hasSelection()) return {};

    const int cur = tc.position();
    const QTextBlock blk = tc.block();
    const int col = cur - blk.position();
    const QString left = blk.text().left(col);

    // ---- 1) Member ("receiver.") context ----------------------------------
    // Capture the receiver expression immediately left of the completion dot: a primary
    // (identifier / media[...] / ClassName(...)) followed by any number of ".name",
    // "(...)" or "[...]" segments — e.g. "renderer", "video.get_frame(f)", "video.audio".
    // Then the final "." and the partial member being typed.
    //
    // <G> is a balanced () or [] group whose contents may contain ONE further level of nested
    // () / [] — so a call/subscript argument that is itself a tuple or nested call is spanned.
    // The previous pattern used "[^()]*" for arguments, which cannot cross a nested paren, so a
    // member access on a constructor that takes a tuple — e.g. "Solid_Color((255,0,0))." — never
    // matched and offered no completions. (Calls whose arguments have no nested brackets still
    // matched, which is why this only seemed to break for the tuple-taking filter classes.)
    static const QString memberGrp = QStringLiteral(
        R"((?:\((?:[^()\[\]]|\([^()]*\)|\[[^\[\]]*\])*\)|\[(?:[^()\[\]]|\([^()]*\)|\[[^\[\]]*\])*\]))");
    static const QRegularExpression dotRe(
        QString(QStringLiteral(R"(((?:[A-Za-z_]\w*)\s*<G>?(?:\s*\.\s*[A-Za-z_]\w*\s*<G>?)*)\s*\.\s*([A-Za-z_]\w*|)$)"))
            .replace(QStringLiteral("<G>"), memberGrp));
    const QRegularExpressionMatch dm = dotRe.match(left);
    if (dm.hasMatch()) {
        const QString receiverExpr = dm.captured(1);
        const QString prefix       = dm.captured(2);
        if (prefixOut) *prefixOut = prefix;
        m_completionStart = cur - prefix.length();

        // Show ONLY the members of the receiver's class. If we can't determine the type,
        // show nothing (rather than every method of every class, or any variable name).
        const QString cls = inferType(receiverExpr, 0);
        if (cls.isEmpty() || !s_members.contains(cls)) return {};

        m_completionClass = cls;
        const QMap<QString, MemberInfo> &mm = s_members.value(cls);
        QStringList names = mm.keys();
        names.sort(Qt::CaseInsensitive);
        for (const QString &n : names) {
            const MemberInfo &mi = mm.value(n);
            CompletionItem ci;
            ci.kind      = mi.isMethod ? CompletionKind::Callable : CompletionKind::Variable;
            ci.takesArgs = mi.takesArgs;
            m_completionMeta.insert(n, ci);
        }
        return names;
    }

    // ---- 2) Identifier context (variables / classes / functions) ----------
    // A bare partial name being typed at the start of a line, in an expression, or inside
    // a call's arguments. Offer the in-scope identifiers, colour-coded by category.
    static const QRegularExpression identRe(R"(([A-Za-z_]\w*)$)");
    const QRegularExpressionMatch im = identRe.match(left);
    if (!im.hasMatch()) return {};
    const QString partial   = im.captured(1);
    const int partialStart  = im.capturedStart(1);

    // Suppress in cases where identifier suggestions would be wrong or noisy:
    //  - right after a "." we couldn't resolve to a class (it's member access, not a name);
    //  - while naming a brand-new function/class ("def foo", "class Foo");
    //  - inside a trailing line comment.
    if (partialStart > 0 && left.at(partialStart - 1) == QChar('.')) return {};
    static const QRegularExpression defNameRe(R"(^\s*(?:def|class)\s+\w*$)");
    if (defNameRe.match(left).hasMatch()) return {};
    if (left.contains(QChar('#'))) return {};

    if (prefixOut) *prefixOut = partial;
    m_completionStart = cur - partial.length();

    collectIdentifierCompletions();      // fills m_completionMeta with in-scope names
    QStringList names = m_completionMeta.keys();
    names.removeAll(partial);            // don't suggest the exact token already typed
    names.sort(Qt::CaseInsensitive);
    return names;
}

// Gather the identifiers in scope for the current document and record each one's category
// in m_completionMeta. Sources: the engine API (classes + known globals) and a light parse
// of the editor text (assignment targets, def/class names, function parameters, and
// for/with/except/import targets). First writer wins, so engine classes keep their colour
// even if a local later rebinds the same name.
void PythonCodeEditor::collectIdentifierCompletions() {
    auto add = [this](const QString &name, CompletionKind kind, bool takesArgs) {
        if (name.isEmpty()) return;
        if (m_completionMeta.contains(name)) return;   // first writer wins
        CompletionItem ci; ci.kind = kind; ci.takesArgs = takesArgs;
        m_completionMeta.insert(name, ci);
    };

    // Engine classes (red, auto-() from the captured __init__) and known globals
    // (variables, e.g. renderer/media/np).
    for (const QString &cls : s_members.keys())       add(cls, CompletionKind::Class, s_classInfo.value(cls).takesArgs);
    for (const QString &g   : s_globalTypes.keys())   add(g,   CompletionKind::Variable, false);
    add(QStringLiteral("media"), CompletionKind::Variable, false);
    add(QStringLiteral("np"),    CompletionKind::Variable, false);

    // Helper: split a comma-separated target list and add each bare identifier as a var.
    auto addTargets = [&](const QString &list) {
        static const QRegularExpression idOnly(R"(^[A-Za-z_]\w*$)");
        const QStringList parts = list.split(QChar(','));
        for (QString p : parts) {
            p = p.trimmed();
            // drop a trailing annotation / default ("x: int", "x = 5") and surrounding ()/*
            p = p.section(QChar(':'), 0, 0).section(QChar('='), 0, 0).trimmed();
            p.remove(QChar('(')); p.remove(QChar(')')); p.remove(QChar('*'));
            p = p.trimmed();
            if (idOnly.match(p).hasMatch()) add(p, CompletionKind::Variable, false);
        }
    };

    static const QRegularExpression classRe(R"(^\s*class\s+([A-Za-z_]\w*))");
    static const QRegularExpression defRe(R"(^\s*def\s+([A-Za-z_]\w*)\s*\(([^)]*))");
    static const QRegularExpression assignRe(R"(^\s*([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*(?::[^=]+)?=(?!=))");
    static const QRegularExpression forRe(R"(^\s*for\s+([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s+in\b)");
    static const QRegularExpression asRe(R"(\bas\s+([A-Za-z_]\w*))");
    static const QRegularExpression importRe(R"(^\s*import\s+(.+)$)");
    static const QRegularExpression fromImportRe(R"(^\s*from\s+\S+\s+import\s+(.+)$)");

    // Scan the script this editor belongs to (for a value field that's the parent editor's
    // document) as well as our own text, so a field offers the script's variables too.
    QList<QTextDocument *> scopeDocs;
    if (m_contextEditor) scopeDocs << m_contextEditor->document();
    scopeDocs << document();
    for (QTextDocument *scopeDoc : scopeDocs)
    for (QTextBlock b = scopeDoc->firstBlock(); b.isValid(); b = b.next()) {
        const QString line = b.text();

        {
            const QRegularExpressionMatch m = classRe.match(line);
            if (m.hasMatch()) add(m.captured(1), CompletionKind::Class, false);
        }
        {
            const QRegularExpressionMatch m = defRe.match(line);
            if (m.hasMatch()) {
                // function name is a callable; its params become local variables
                const QString params = m.captured(2);
                add(m.captured(1), CompletionKind::Callable, methodTakesArgs(params));
                addTargets(params);
            }
        }
        {
            const QRegularExpressionMatch m = assignRe.match(line);
            if (m.hasMatch()) addTargets(m.captured(1));
        }
        {
            const QRegularExpressionMatch m = forRe.match(line);
            if (m.hasMatch()) addTargets(m.captured(1));
        }

        // "as name" covers with/except/import-as; match every occurrence on the line.
        QRegularExpressionMatchIterator asIt = asRe.globalMatch(line);
        while (asIt.hasNext()) add(asIt.next().captured(1), CompletionKind::Variable, false);

        {
            const QRegularExpressionMatch m = importRe.match(line);
            if (m.hasMatch()) {
                const QStringList mods = m.captured(1).split(QChar(','));
                for (QString mod : mods) {
                    mod = mod.trimmed();
                    if (mod.contains(QStringLiteral(" as "))) continue;   // alias handled by asRe
                    mod = mod.section(QChar('.'), 0, 0).trimmed();        // top-level package name
                    static const QRegularExpression idOnly(R"(^[A-Za-z_]\w*$)");
                    if (idOnly.match(mod).hasMatch()) add(mod, CompletionKind::Variable, false);
                }
            }
        }
        {
            const QRegularExpressionMatch m = fromImportRe.match(line);
            if (m.hasMatch()) {
                QString names = m.captured(1);
                names.remove(QChar('(')); names.remove(QChar(')'));
                const QStringList parts = names.split(QChar(','));
                for (QString p : parts) {
                    p = p.trimmed();
                    if (p == QStringLiteral("*")) continue;
                    if (p.contains(QStringLiteral(" as "))) continue;     // alias handled by asRe
                    static const QRegularExpression idOnly(R"(^[A-Za-z_]\w*$)");
                    if (idOnly.match(p).hasMatch()) add(p, CompletionKind::Variable, false);
                }
            }
        }
    }
}

void PythonCodeEditor::updateCompletionPopup() {
    if (!m_completer) return;
    if (m_plainField) { m_completer->popup()->hide(); return; }   // literal-text field: no autocomplete

    QString prefix;
    const QStringList candidates = completionsForContext(&prefix);
    if (candidates.isEmpty()) { m_completer->popup()->hide(); if (m_docBox) m_docBox->hide(); return; }

    // Rebuild the model, colouring each row by its category (variable / class / callable)
    // so the popup mirrors the syntax highlighting. Rows are non-editable.
    m_completionModel->clear();
    for (const QString &name : candidates) {
        auto *item = new QStandardItem(name);
        item->setEditable(false);
        item->setForeground(colorForKind(m_completionMeta.value(name).kind));
        m_completionModel->appendRow(item);
    }
    m_completer->setCompletionPrefix(prefix);
    if (m_completer->completionCount() == 0) { m_completer->popup()->hide(); if (m_docBox) m_docBox->hide(); return; }

    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
    QRect cr = cursorRect();
    cr.setWidth(m_completer->popup()->sizeHintForColumn(0)
                + m_completer->popup()->verticalScrollBar()->sizeHint().width() + 8);
    m_completer->complete(cr);   // popup at the caret
    // The parameter panel is a tool window that can sit over the popup; force the autofill list
    // above it so suggestions are visible even while a documented call's panel is open.
    m_completer->popup()->raise();

    // Describe the highlighted (first) entry beside the list. Done explicitly because the
    // highlighted() signal isn't reliably emitted for a programmatic open on every Qt.
    const QString cur = m_completer->currentCompletion();
    if (!cur.isEmpty()) showCompletionDocFor(cur);
    else if (m_docBox)  m_docBox->hide();
}

void PythonCodeEditor::insertCompletion(const QString &completion) {
    // Replace the partial token between m_completionStart and the caret with the chosen
    // name. Callables (methods / functions) and classes (a constructor call) get "()" —
    // caret left inside when they take arguments, after the ")" when they don't; plain
    // variables are inserted bare. The category comes from m_completionMeta, so this works
    // for both the member ("receiver.") and the identifier context.
    if (m_completionStart < 0) return;
    QTextCursor tc = textCursor();
    const int cur = tc.position();
    if (m_completionStart > cur) { m_completionStart = -1; return; }

    const CompletionItem ci = m_completionMeta.value(completion);
    QString text = completion;
    bool caretInsideParens = false;
    // Callables (methods / functions) and classes (constructor call) both get "()" — caret
    // left inside when they take arguments, after the ")" when they don't.
    if (ci.kind == CompletionKind::Callable || ci.kind == CompletionKind::Class) {
        // Don't double up if the user already has a "(" right after the caret.
        const QChar next = (cur < document()->characterCount())
                               ? document()->characterAt(cur) : QChar();
        if (next != '(') {
            text += "()";
            caretInsideParens = ci.takesArgs;
        }
    }

    tc.setPosition(m_completionStart);
    tc.setPosition(cur, QTextCursor::KeepAnchor);
    tc.insertText(text);
    if (caretInsideParens) tc.movePosition(QTextCursor::Left);   // land between the ()
    setTextCursor(tc);
    m_completionStart = -1;
}

// ---- Documentation / signature box plumbing --------------------------------

// Show the doc box for the popup's highlighted entry, anchored to the right of the
// list. Also called once after the popup opens so the first row is described.
void PythonCodeEditor::showCompletionDocFor(const QString &member) {
    if (!m_docBox || !m_completer) return;
    if (!m_completer->popup()->isVisible()) { m_docBox->hide(); return; }

    // Pick the MemberInfo to describe: a member in the "receiver." context, or a class
    // constructor in the identifier context. Plain variables / functions have nothing to
    // show, so the box hides for them. (mm is a lifetime-extended copy; info points into
    // it or into the static s_classInfo, both valid for this call.)
    const QMap<QString, MemberInfo> &mm = s_members.value(m_completionClass);
    const MemberInfo *info = nullptr;
    if (!m_completionClass.isEmpty()) {
        auto it = mm.constFind(member);
        if (it != mm.constEnd()) info = &it.value();
    } else if (m_completionMeta.value(member).kind == CompletionKind::Class) {
        auto it = s_classInfo.constFind(member);
        if (it != s_classInfo.constEnd()) info = &it.value();
    }
    if (!info) { m_docBox->hide(); return; }

    m_docBox->setContent(member, *info);
    QAbstractItemView *pv = m_completer->popup();
    m_docBox->showRightOf(QRect(pv->mapToGlobal(QPoint(0, 0)), pv->size()));
}

// Hover variant: anchored at the mouse and interactive (click to expand). The caller
// has already resolved the hovered token to its MemberInfo (a method/attribute from
// s_members, or a class constructor from s_classInfo), so classes get the same hover
// box that functions do.
void PythonCodeEditor::showHoverDoc(const QString &name, const MemberInfo &info,
                                    const QString &errorMsg, const QPoint &globalPos) {
    if (!m_docBox) return;
    m_docBox->setContent(name, info, errorMsg);
    m_docBox->showNear(globalPos);
}

void PythonCodeEditor::hideCompletionDoc() {
    if (m_docHideTimer) m_docHideTimer->stop();
    m_docBoxHovered = false;
    m_hoverDocBlock = m_hoverDocStart = m_hoverDocEnd = -1;
    m_hoverErrorMsg.clear();
    if (m_docBox) m_docBox->hide();
}

// ---- Parameter-input panel plumbing ----------------------------------------

// Locate the call whose parentheses enclose caretPos. Forward-scans from the document
// start (small scripts), tracking strings (incl. triple-quoted) and "#" comments so
// brackets inside them don't count, and keeping a stack of open brackets. Each "(" notes
// whether it's a *call* (immediately preceded by an identifier, ")" or "]") and where its
// callee chain starts. The innermost still-open call paren at the caret wins.
EnclosingCall PythonCodeEditor::findEnclosingCall(int caretPos) const {
    EnclosingCall none;
    const QString text = toPlainText();
    if (caretPos < 0 || caretPos > text.size()) return none;
    const int n = text.size();

    struct Open { int pos; bool isCall; int calleeStart; };
    QList<Open> stack;
    QChar quote;
    bool triple = false;

    for (int i = 0; i < n && i < caretPos; ++i) {
        const QChar c = text.at(i);
        if (!quote.isNull()) {
            if (triple) {
                if (c == quote && i + 2 < n && text.at(i + 1) == quote && text.at(i + 2) == quote) {
                    i += 2; quote = QChar(); triple = false;
                }
            } else {
                if (c == QLatin1Char('\\')) { ++i; continue; }
                if (c == quote) quote = QChar();
            }
            continue;
        }
        if (c == QLatin1Char('#')) {                       // line comment
            while (i + 1 < n && i + 1 < caretPos && text.at(i + 1) != QLatin1Char('\n')) ++i;
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            if (i + 2 < n && text.at(i + 1) == c && text.at(i + 2) == c) { quote = c; triple = true; i += 2; }
            else quote = c;
            continue;
        }
        if (c == QLatin1Char('(')) {
            int j = i - 1;
            while (j >= 0 && text.at(j).isSpace()) --j;
            Open o; o.pos = i; o.isCall = false; o.calleeStart = i;
            if (j >= 0) {
                const QChar pc = text.at(j);
                if (pc.isLetterOrNumber() || pc == QLatin1Char('_') ||
                    pc == QLatin1Char(')') || pc == QLatin1Char(']')) {
                    o.isCall = true;
                    // Walk the whole receiver chain backwards: identifier/dotted runs, plus any
                    // trailing call "(...)" or subscript "[...]" groups (skipped as balanced
                    // units). This captures receivers like solid_color(255,0,0).apply_filter so
                    // inferType can resolve the constructor's type, not just ".apply_filter".
                    int k = j;
                    for (;;) {
                        while (k >= 0 && (text.at(k).isLetterOrNumber() ||
                                          text.at(k) == QLatin1Char('_') ||
                                          text.at(k) == QLatin1Char('.'))) --k;
                        if (k >= 0 && (text.at(k) == QLatin1Char(')') || text.at(k) == QLatin1Char(']'))) {
                            const QChar close = text.at(k);
                            const QChar open = (close == QLatin1Char(')')) ? QLatin1Char('(') : QLatin1Char('[');
                            int depth = 0;
                            while (k >= 0) {
                                const QChar c2 = text.at(k);
                                if (c2 == close) ++depth;
                                else if (c2 == open && --depth == 0) { --k; break; }
                                --k;
                            }
                            continue;   // keep consuming the part of the chain before the group
                        }
                        break;
                    }
                    o.calleeStart = k + 1;
                }
            }
            stack.append(o);
            continue;
        }
        if (c == QLatin1Char('[') || c == QLatin1Char('{')) {
            Open o; o.pos = i; o.isCall = false; o.calleeStart = i;
            stack.append(o);
            continue;
        }
        if (c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')) {
            if (!stack.isEmpty()) stack.removeLast();
            continue;
        }
    }

    for (int s = stack.size() - 1; s >= 0; --s) {
        const Open &o = stack.at(s);
        if (o.isCall && text.at(o.pos) == QLatin1Char('(')) {
            EnclosingCall ec;
            ec.valid = true;
            ec.openPos = o.pos;
            ec.callee = text.mid(o.calleeStart, o.pos - o.calleeStart).trimmed();
            int closeOut = -1;
            scanArgs(text, o.pos, &closeOut);
            ec.closePos = closeOut;
            const int argEnd = (closeOut >= 0) ? closeOut : n;
            ec.argText = text.mid(o.pos + 1, argEnd - (o.pos + 1));
            return ec;
        }
    }
    return none;
}

// Resolve a callee expression to its documented MemberInfo: a class constructor (s_classInfo)
// or a method on a typed receiver (inferType -> s_members). Returns false when the callee
// isn't documented (e.g. a bare/top-level function, which the API table doesn't carry).
bool PythonCodeEditor::resolveCallee(const QString &callee, QString *nameOut, MemberInfo *infoOut) const {
    if (callee.isEmpty()) return false;
    ensureApiTable();
    const int dot = callee.lastIndexOf(QLatin1Char('.'));
    if (dot < 0) {                                          // bare name -> class constructor
        auto it = s_classInfo.constFind(callee);
        if (it == s_classInfo.constEnd()) return false;
        if (nameOut)  *nameOut = callee;
        if (infoOut)  *infoOut = it.value();
        return true;
    }
    const QString recv = callee.left(dot).trimmed();        // receiver.method
    const QString method = callee.mid(dot + 1).trimmed();
    if (method.isEmpty()) return false;
    const QString cls = inferType(recv, 0);
    if (cls.isEmpty()) return false;
    const QMap<QString, MemberInfo> &mm = s_members.value(cls);
    auto it = mm.constFind(method);
    if (it == mm.constEnd() || !it.value().isMethod) return false;
    if (nameOut)  *nameOut = method;
    if (infoOut)  *infoOut = it.value();
    return true;
}

// Hide the parameter panel and unbind the current call.
void PythonCodeEditor::hideParamPanel() {
    if (m_paramHideTimer) m_paramHideTimer->stop();
    m_callKey.clear();
    m_callParamOrder.clear();
    m_callOpenCursor = QTextCursor();
    m_callCloseCursor = QTextCursor();
    if (m_paramPanel) {
        m_paramPanel->hideNestedPanels();   // close child panels too, so none are orphaned
        m_paramPanel->hide();
    }
}

// Close every pop-up this editor owns: the completion (autofill) popup, the doc/description
// box, and the parameter panel — the panel close cascades into nested field panels (and their
// pop-ups). Used by Esc so a single press clears the whole stack.
void PythonCodeEditor::closeAllPopups() {
    if (m_completer) m_completer->popup()->hide();
    hideCompletionDoc();
    hideParamPanel();
}

// True if w sits inside this editor's parameter-panel subtree. Walks the QObject parent chain
// (m_paramPanel parents its fields, each field parents its own nested panel, and so on) so it
// works ACROSS the separate tool windows — unlike QWidget::isAncestorOf, which only relates
// widgets within a single window and so reports a nested-panel field as "outside".
bool PythonCodeEditor::focusInPanelTree(QWidget *w) const {
    for (QObject *o = w; o; o = o->parent())
        if (o == m_paramPanel) return true;
    return false;
}

// Show / re-sync / hide the parameter panel as the caret moves (connected to
// cursorPositionChanged and re-run after the completion popup closes).
void PythonCodeEditor::updateParamPanel() {
    if (!m_paramPanel || m_paramWriteGuard) return;
    if (isReadOnly()) { hideParamPanel(); return; }   // the panel edits text; pointless read-only
    // While the autofill popup is open, leave the panel exactly as it is. Hiding+rebuilding it
    // here would re-raise the panel's tool window on the next keystroke, which dismisses the
    // popup — so typing arguments would lose autofill. Coexisting (panel to the side, popup at
    // the caret) is fine.
    if (m_completer && m_completer->popup()->isVisible()) return;

    const int caret = textCursor().position();
    const EnclosingCall ec = findEnclosingCall(caret);

    QString dispName;
    MemberInfo info;
    if (!ec.valid || !resolveCallee(ec.callee, &dispName, &info) || info.params.trimmed().isEmpty()) {
        // Not inside a documented call (or it has no fillable params). Grace-hide so the
        // user can travel into the panel; the timer keeps it open if the panel has focus.
        if (m_paramPanel->isVisible() && m_paramHideTimer) m_paramHideTimer->start();
        return;
    }
    if (m_paramHideTimer) m_paramHideTimer->stop();

    // Field values from the live arguments: positional by order, keyword by name. Bound the
    // scan to the call's ")" so a malformed call elsewhere can't make it read far past the end.
    const QList<ParamInfo> params = parseParams(info.params);
    QStringList order;
    for (const ParamInfo &pi : params) order << pi.name;
    const QString text = toPlainText();
    int closeOut = -1;
    const QList<ArgSpan> argspans = scanArgs(text, ec.openPos, &closeOut, ec.closePos);
    QMap<QString, QString> values;
    int positional = 0;
    for (const ArgSpan &a : argspans) {
        if (a.keyword) {
            values.insert(a.name, text.mid(a.valStart, a.valEnd - a.valStart));
        } else {
            if (positional < order.size())
                values.insert(order.at(positional), text.mid(a.valStart, a.valEnd - a.valStart));
            ++positional;
        }
    }

    // Anchor a cursor at the call's ")" so applyParamEdit can bound its scan to the call.
    auto anchorClose = [&]() {
        if (ec.closePos >= 0) {
            m_callCloseCursor = textCursor();
            m_callCloseCursor.setPosition(ec.closePos);
        } else {
            m_callCloseCursor = QTextCursor();
        }
    };

    const QString key = ec.callee + QLatin1Char('@') + QString::number(ec.openPos);
    if (key == m_callKey && m_paramPanel->isVisible()) {
        anchorClose();                                      // the close may have just appeared/moved
        m_paramPanel->setValues(values);                    // same call: refresh, keep focus & place
        return;
    }

    // Parse per-parameter descriptions/tags from the param docstring — for a class that is
    // __init__'s docstring (info.doc is the class docstring, which lacks the Parameters block).
    const QMap<QString, QString> docs =
        parseParamDocs(info.paramDoc.isEmpty() ? info.doc : info.paramDoc);
    if (!m_paramPanel->buildFor(dispName, info, params, docs)) { hideParamPanel(); return; }
    m_paramPanel->setValues(values);
    m_callKey = key;
    m_callParamOrder = order;
    m_callOpenCursor = textCursor();
    m_callOpenCursor.setPosition(ec.openPos);               // anchor at "(" (tracks edits)
    anchorClose();
    const QPoint caretGlobal = viewport()->mapToGlobal(cursorRect().bottomRight());
    m_paramPanel->showAt(caretGlobal);
}

// Rewrite the bound call's entire argument list from the panel's fields. While the panel is
// open it OWNS the parentheses: every non-empty field is emitted as "name=value" in signature
// order, so positional arguments the user typed by hand are converted to keyword form and any
// text that couldn't be mapped to a field is dropped. The single edit replaces only the span
// between the anchored "(" and ")" cursors, so a value containing parentheses or commas (even
// while transiently unbalanced mid-typing) can never corrupt the rest of the document.
//
// The (param, value, cleared) arguments are unused — the panel's full field state is the
// source of truth — but kept so the field's textChanged handler can call this directly.
void PythonCodeEditor::applyParamEdit(const QString &, const QString &, bool) {
    if (!m_paramPanel || m_callOpenCursor.isNull() || m_callCloseCursor.isNull()) return;
    const int openPos = m_callOpenCursor.position();
    const int closePos = m_callCloseCursor.position();
    const QString text = toPlainText();
    if (openPos < 0 || openPos >= text.size() || text.at(openPos) != QLatin1Char('(')) {
        hideParamPanel();                                   // the call's "(" was edited away
        return;
    }
    if (closePos <= openPos || closePos > text.size()) return;   // close anchor went stale

    // Build "p1=v1, p2=v2" from the non-empty fields, in signature order.
    const QMap<QString, QString> vals = m_paramPanel->fieldValues();
    QString args;
    for (const QString &p : m_callParamOrder) {
        const auto it = vals.constFind(p);
        if (it == vals.constEnd()) continue;
        if (!args.isEmpty()) args += QStringLiteral(", ");
        args += p + QLatin1Char('=') + it.value();
    }

    m_paramWriteGuard = true;
    QTextCursor tc = textCursor();
    tc.setPosition(openPos + 1);
    tc.setPosition(closePos, QTextCursor::KeepAnchor);      // the span strictly inside ( )
    tc.insertText(args);
    m_paramWriteGuard = false;
}

// Decide what the hover box shows when the mouse rests in the editor: a Python error pinned
// to the hovered line (on top), the API doc for the token under the pointer (a "receiver.
// member" chain or a bare class name), or both stacked — error over doc. Anything else hides
// the box. Mirrors the completion-context regex, but the hovered word is a full name.
void PythonCodeEditor::maybeShowHoverDoc(const QPoint &viewportPos, const QPoint &globalPos) {
    if (m_completer && m_completer->popup()->isVisible()) return;  // typing wins over hover
    ensureApiTable();

    // (1) Error pinned to the hovered line — but only when the pointer is within that line's
    // vertical band, so the blank area past a short file doesn't trigger the last error.
    QString errorMsg;
    int errorBlock = -1;
    if (!m_errors.isEmpty()) {
        const QTextBlock hb = cursorForPosition(viewportPos).block();
        auto it = m_errors.constFind(hb.blockNumber() + 1);   // map keys are 1-based
        if (it != m_errors.constEnd() && !it.value().message.isEmpty()) {
            const QRectF r = blockBoundingGeometry(hb).translated(contentOffset());
            if (viewportPos.y() >= r.top() && viewportPos.y() <= r.bottom()) {
                errorMsg = it.value().message;
                errorBlock = hb.blockNumber();
            }
        }
    }

    // (2) API doc for the token under the pointer (if any). Two cases give classes the same
    // hover box that methods get: a "receiver.member" chain (infer the receiver's class, then
    // look up the member) or a bare class name (its constructor signature + class docstring).
    QString docName;
    MemberInfo docInfo;
    bool found = false;
    int docBlock = -1, selStart = -1, selEnd = -1;
    {
        QTextCursor wc = cursorForPosition(viewportPos);
        wc.select(QTextCursor::WordUnderCursor);
        const QString word = wc.selectedText();
        static const QRegularExpression identOnly(R"(^[A-Za-z_]\w*$)");
        if (!word.isEmpty() && identOnly.match(word).hasMatch()) {
            const int s = wc.selectionStart(), e = wc.selectionEnd();
            const QTextBlock blk = wc.block();
            // cursorForPosition snaps to the nearest character, so hovering past a line's end
            // still "selects" its last word. Require the pointer to fall within the word's
            // rectangle before treating it as a documented token.
            QTextCursor sc(blk); sc.setPosition(s);
            QTextCursor ec(blk); ec.setPosition(e);
            const QRect r0 = cursorRect(sc), r1 = cursorRect(ec);
            const bool inRect =
                viewportPos.x() >= qMin(r0.left(), r1.left()) - 2 &&
                viewportPos.x() <= qMax(r0.left(), r1.left()) + 2 &&
                viewportPos.y() >= qMin(r0.top(), r1.top()) &&
                viewportPos.y() <= qMax(r0.bottom(), r1.bottom());
            if (inRect) {
                const QString upto = blk.text().left(e - blk.position());
                // <G>: a balanced () / [] group spanning one level of inner nesting, so a member
                // after a tuple/nested-call argument (e.g. "Solid_Color((255,0,0)).set_field")
                // still resolves — see the matching note in completionsForContext().
                static const QString hoverGrp = QStringLiteral(
                    R"((?:\((?:[^()\[\]]|\([^()]*\)|\[[^\[\]]*\])*\)|\[(?:[^()\[\]]|\([^()]*\)|\[[^\[\]]*\])*\]))");
                static const QRegularExpression re(
                    QString(QStringLiteral(R"(((?:[A-Za-z_]\w*)\s*<G>?(?:\s*\.\s*[A-Za-z_]\w*\s*<G>?)*)\s*\.\s*([A-Za-z_]\w*)$)"))
                        .replace(QStringLiteral("<G>"), hoverGrp));
                const QRegularExpressionMatch m = re.match(upto);
                if (m.hasMatch()) {
                    const QString cls = inferType(m.captured(1), 0);
                    const QString member = m.captured(2);
                    if (!cls.isEmpty()) {
                        const QMap<QString, MemberInfo> &mm = s_members.value(cls);
                        auto mit = mm.constFind(member);
                        if (mit != mm.constEnd()) { docName = member; docInfo = mit.value(); found = true; }
                    }
                } else {
                    auto cit = s_classInfo.constFind(word);
                    if (cit != s_classInfo.constEnd()) { docName = word; docInfo = cit.value(); found = true; }
                }
                if (found) { docBlock = blk.blockNumber(); selStart = s; selEnd = e; }
            }
        }
    }

    // (3) Nothing to document and no error here: let the box hide.
    if (!found && errorMsg.isEmpty()) { scheduleHoverDocHide(); return; }

    // While the pointer rests on the same target (same token span, or same error line for an
    // error-only hover) leave the box — and any expand/scroll state — untouched.
    const int idBlock = found ? docBlock : errorBlock;
    if (m_docBox && m_docBox->isVisible() &&
        idBlock == m_hoverDocBlock && selStart == m_hoverDocStart && selEnd == m_hoverDocEnd &&
        errorMsg == m_hoverErrorMsg) {
        if (m_docHideTimer) m_docHideTimer->stop();
        return;
    }

    m_hoverDocBlock = idBlock;
    m_hoverDocStart = selStart;
    m_hoverDocEnd   = selEnd;
    m_hoverErrorMsg = errorMsg;
    if (m_docHideTimer) m_docHideTimer->stop();

    const MemberInfo empty;
    showHoverDoc(found ? docName : QString(), found ? docInfo : empty, errorMsg, globalPos);
}

// The mouse left a documented token (or the box itself): hide after a short grace
// period, but only a hover box — never the popup-anchored one.
void PythonCodeEditor::scheduleHoverDocHide() {
    m_hoverDocBlock = m_hoverDocStart = m_hoverDocEnd = -1;
    m_hoverErrorMsg.clear();
    if (m_docBox && m_docBox->isVisible() && m_docBox->isInteractive() &&
        !m_docBoxHovered && m_docHideTimer)
        m_docHideTimer->start();
}

void PythonCodeEditor::docBoxHoverChanged(bool inside) {
    m_docBoxHovered = inside;
    if (inside) { if (m_docHideTimer) m_docHideTimer->stop(); }
    else        scheduleHoverDocHide();
}

bool PythonCodeEditor::eventFilter(QObject *obj, QEvent *event) {
    if (m_completer) {
        QAbstractItemView *pv = m_completer->popup();

        // The completer hides its popup itself in many cases; mirror that on the doc box.
        // Also re-evaluate the parameter panel once the popup is fully gone: accepting a
        // call completion lands the caret inside the new "()", which should open the panel.
        // Deferred so popup()->isVisible() reads false by the time updateParamPanel() runs.
        if (obj == pv && event->type() == QEvent::Hide) {
            hideCompletionDoc();
            QTimer::singleShot(0, this, [this]() { updateParamPanel(); });
        }

        // Hover-to-highlight: as the pointer moves over the list, make the row under it the
        // current one so the doc box follows the hover (not just clicks). We refresh the box
        // ourselves because QCompleter::highlighted isn't reliably emitted for a programmatic
        // current-index change.
        if (obj == pv->viewport() && event->type() == QEvent::MouseMove) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            const QModelIndex idx = pv->indexAt(me->pos());
            if (idx.isValid() && idx != pv->currentIndex()) {
                pv->setCurrentIndex(idx);
                showCompletionDocFor(idx.data().toString());
            }
        }

        // Wheel over the doc box while the popup owns the mouse grab: forward it to the box so
        // the description scrolls, and consume it so the list doesn't scroll underneath.
        if ((obj == pv || obj == pv->viewport()) && event->type() == QEvent::Wheel &&
            m_docBox && m_docBox->isVisible()) {
            QWheelEvent *we = static_cast<QWheelEvent *>(event);
            if (m_docBox->containsGlobal(we->globalPosition().toPoint())) {
                m_docBox->wheelScroll(we->angleDelta(), we->pixelDelta());
                return true;
            }
        }
    }
    return SearchTextEdit::eventFilter(obj, event);
}

// Draw a bold red squiggle under the offending span of each error line, ON TOP of
// the text. We paint it ourselves rather than using an ExtraSelection WaveUnderline
// because Qt's char format can't control underline thickness, and we specifically do
// NOT want any line/background highlight — just a more noticeable underline. The span
// comes from Python's reported columns (so e.g. a syntax error underlines exactly the
// bad token); when columns are unknown we fall back to the code on the line (leading
// indentation skipped). Positions are recomputed from block geometry every paint, so
// the squiggle stays correct while scrolling and editing.
void PythonCodeEditor::paintEvent(QPaintEvent *event) {
    SearchTextEdit::paintEvent(event);   // text + current-line highlight first
    if (m_errors.isEmpty()) return;

    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(QColor(220, 0, 0));
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);

    const qreal amp = 1.5;       // squiggle amplitude (px)
    const qreal halfWave = 3.0;  // half wavelength (px)

    for (auto it = m_errors.constBegin(); it != m_errors.constEnd(); ++it) {
        const int line = it.key();
        QTextBlock block = document()->findBlockByNumber(line - 1);
        if (!block.isValid() || !block.isVisible()) continue;

        const QString text = block.text();
        const LineError &le = it.value();

        // Determine the [sc, ec) character span to underline. A precise span comes
        // from Python (SyntaxError offset / PEP 657 co_positions).
        int sc, ec;
        if (le.startCol >= 0) {
            sc = qBound(0, le.startCol, text.size());
            // A known end column underlines exactly the span; if only the start is
            // known (some SyntaxErrors), underline from there to the end of the line.
            ec = (le.endCol > le.startCol) ? qBound(0, le.endCol, text.size()) : text.size();
        } else {
            // No column info: underline the whole line (first non-space .. end).
            sc = 0;
            while (sc < text.size() && text.at(sc).isSpace()) ++sc;
            ec = text.size();
        }

        QTextCursor startCur(block);
        startCur.setPosition(block.position() + sc);
        QTextCursor endCur(block);
        endCur.setPosition(block.position() + ec);

        const QRect startRect = cursorRect(startCur);
        const QRect endRect = cursorRect(endCur);

        qreal x1 = startRect.left();
        qreal x2 = endRect.left();
        if (x2 <= x1) x2 = x1 + fontMetrics().horizontalAdvance(QLatin1Char(' '));  // empty/short line
        const qreal baseY = endRect.bottom() - 1.5;   // sit just under the text

        QPolygonF wave;
        bool up = true;
        for (qreal x = x1; x < x2; x += halfWave) {
            wave << QPointF(x, up ? baseY - amp : baseY + amp);
            up = !up;
        }
        wave << QPointF(x2, up ? baseY - amp : baseY + amp);
        painter.drawPolyline(wave);
    }
}

bool PythonCodeEditor::viewportEvent(QEvent *event) {
    // Tooltip events are delivered to the viewport; we repurpose them to show the combined
    // hover box (Python error on top, API signature/doc beneath). Errors no longer use
    // QToolTip — they render inside the box so error and doc share one typeface and size.
    if (event->type() == QEvent::ToolTip) {
        QHelpEvent *he = static_cast<QHelpEvent *>(event);
        QToolTip::hideText();
        maybeShowHoverDoc(he->pos(), he->globalPos());
        return true;
    }

    // Track the pointer so the hover box hides once it leaves its target (with a grace
    // period, so the user can reach the box to click "More ▾" or scroll it).
    if (event->type() == QEvent::MouseMove) {
        if (m_docBox && m_docBox->isVisible() && m_docBox->isInteractive()) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            bool keep = false;
            if (!m_hoverErrorMsg.isEmpty() && m_hoverDocBlock >= 0) {
                // Error (alone or stacked over a doc): keep while the pointer stays anywhere
                // on the error line, so sliding off the token doesn't dismiss the error.
                const QTextBlock block = document()->findBlockByNumber(m_hoverDocBlock);
                if (block.isValid()) {
                    const QRectF r = blockBoundingGeometry(block).translated(contentOffset());
                    keep = me->pos().y() >= r.top() && me->pos().y() <= r.bottom();
                }
            } else if (m_hoverDocStart >= 0) {
                // Doc-only: keep while still over the exact token that was documented.
                QTextCursor wc = cursorForPosition(me->pos());
                wc.select(QTextCursor::WordUnderCursor);
                keep = wc.block().blockNumber() == m_hoverDocBlock &&
                       wc.selectionStart() == m_hoverDocStart &&
                       wc.selectionEnd()   == m_hoverDocEnd;
            }
            if (!keep) scheduleHoverDocHide();
            else if (m_docHideTimer) m_docHideTimer->stop();
        }
    } else if (event->type() == QEvent::Leave) {
        scheduleHoverDocHide();
    }

    return SearchTextEdit::viewportEvent(event);
}

void PythonCodeEditor::keyPressEvent(QKeyEvent *event) {
    QTextCursor cursor = textCursor();

    // While the completion popup is open it owns these keys: Tab/Enter accept the
    // highlighted method, Esc dismisses, and arrows scroll the list. We ignore them so
    // the completer's own handling runs (mouse clicks are wired via activated()). This
    // is Qt's standard custom-completer key pattern.
    if (m_completer && m_completer->popup()->isVisible()) {
        // F1 expands/collapses the (non-interactive) signature box beside the list,
        // since the popup owns the mouse-driven More/Less affordance only on hover.
        if (event->key() == Qt::Key_F1) {
            if (m_docBox && m_docBox->isVisible()) m_docBox->toggleExpanded();
            event->accept();
            return;
        }
        switch (event->key()) {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            // In a value field these ACCEPT the highlighted suggestion, so autofill wins over
            // Tab-moves-fields and Enter-inserts-newline. (The main editor keeps Qt's default
            // completer handling.)
            if (m_valueFieldMode) {
                const QModelIndex idx = m_completer->popup()->currentIndex();
                const QString choice = idx.isValid() ? idx.data().toString()
                                                     : m_completer->currentCompletion();
                if (!choice.isEmpty()) insertCompletion(choice);
                m_completer->popup()->hide();
                event->accept();
                return;
            }
            event->ignore();
            return;
        default:
            break;
        }
    }

    // In a value field, Tab / Shift+Tab move between fields — but only reached when no autofill
    // popup is open (that case accepted the suggestion above), so selection wins over navigation.
    if (m_valueFieldMode && (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab)) {
        if (event->key() == Qt::Key_Backtab || (event->modifiers() & Qt::ShiftModifier))
            focusPreviousChild();
        else
            focusNextChild();
        event->accept();
        return;
    }

    // Handle bulk indent/unindent (a value field lets Tab change focus instead of indenting).
    if (!m_valueFieldMode &&
        (event->key() == Qt::Key_Tab || (event->key() == Qt::Key_Backtab && event->modifiers() & Qt::ShiftModifier))) {
        indentSelection(cursor, event->key() == Qt::Key_Backtab);
        return;
    }

    // Handle comment/uncomment toggle
    if (event->key() == Qt::Key_Slash && event->modifiers() & Qt::ControlModifier) {
        if (m_completer) m_completer->popup()->hide();
        toggleComment(cursor);
        return;
    }

    // Handle backspace for removing paired characters
    if (event->key() == Qt::Key_Backspace) {
        handleBackspace(cursor);
        updateCompletionPopup();   // narrow / dismiss as the typed prefix shrinks
        return;
    }

    // Handle delete for removing paired characters
    if (event->key() == Qt::Key_Delete) {
        handleDelete(cursor);
        updateCompletionPopup();
        return;
    }

    // Handle paired character insertion
    const QMap<int, QString> pairInsertion = {
        {Qt::Key_ParenLeft, "()"},
        {Qt::Key_BraceLeft, "{}"},
        {Qt::Key_BracketLeft, "[]"},
        {Qt::Key_QuoteDbl, "\"\""},
        {Qt::Key_Apostrophe, "''"}
    };

    // Skipped for a plain-text (str) field so typing "(" or a quote doesn't auto-insert pairs.
    if (!m_plainField && pairInsertion.contains(event->key())) {
        if (m_completer) m_completer->popup()->hide();   // e.g. typing "(" to call the chosen method
        QString pair = pairInsertion[event->key()];
        QString open = pair.left(1);
        QString close = pair.right(1);
        handleConditionalPairInsertion(cursor, open, close);
        return;
    }

    // Handle tab key for indentation (skipped in a value field so Tab moves focus).
    if (!m_valueFieldMode && event->key() == Qt::Key_Tab) {
        cursor.insertText("    "); // Insert 4 spaces
        setTextCursor(cursor);
        return;
    }

    // Handle auto-indent. A value field is single-line, so Enter is swallowed (no newline);
    // when the completion popup is up, Enter was already consumed by it above.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_valueFieldMode) { event->accept(); return; }
        handleAutoIndent(cursor);
        return;
    }

    // Handle triple quotes (not in a plain-text field, where quotes are literal characters).
    if (!m_plainField && event->key() == Qt::Key_QuoteDbl && cursor.position() > 1) {
        handleTripleQuotes(cursor);
        return;
    }

    // Handle auto-skip over closing pair
    const QMap<int, QChar> closingCharacters = {
        {Qt::Key_ParenRight, ')'},
        {Qt::Key_BraceRight, '}'},
        {Qt::Key_BracketRight, ']'},
        {Qt::Key_QuoteDbl, '\"'},
        {Qt::Key_Apostrophe, '\''}
    };

    if (closingCharacters.contains(event->key())) {
        QChar closingChar = closingCharacters[event->key()];
        if (cursor.position() < document()->characterCount() &&
            document()->characterAt(cursor.position()) == closingChar) {
            if (m_completer) m_completer->popup()->hide();
            cursor.movePosition(QTextCursor::Right);
            setTextCursor(cursor);
            return;
        }
    }

    SearchTextEdit::keyPressEvent(event); // Default behavior for other keys

    // After ordinary text entry (a letter, or the "." that starts member access)
    // refresh the popup. Skip when a modifier was held so editing shortcuts
    // (Ctrl+V, Ctrl+A, …) don't spuriously trigger it.
    if (!(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        updateCompletionPopup();
}

void PythonCodeEditor::handleConditionalPairInsertion(QTextCursor &cursor, const QString &open, const QString &close) {
    QChar nextChar = cursor.position() < document()->characterCount()
                     ? document()->characterAt(cursor.position())
                     : QChar();

    const QSet<QChar> endingPairCharacters = {'}', ']', ')', '\"', '\''};

    if (nextChar.isNull() || nextChar.isSpace() || nextChar == '\n' || endingPairCharacters.contains(nextChar)) {
        if (cursor.hasSelection()) {
            QString selectedText = cursor.selectedText();
            cursor.insertText(open + selectedText + close);
        } else {
            cursor.insertText(open + close);
            cursor.movePosition(QTextCursor::Left);
        }
    } else {
        cursor.insertText(open);
    }

    setTextCursor(cursor);
}

void PythonCodeEditor::handleTripleQuotes(QTextCursor &cursor) {
    QString previousText = toPlainText().mid(cursor.position() - 2, 2);
    if (previousText == "\"\"") {
        cursor.insertText("\"\"\"");
        cursor.movePosition(QTextCursor::Left, QTextCursor::MoveAnchor, 3);
        setTextCursor(cursor);
    }
}

void PythonCodeEditor::handleBackspace(QTextCursor &cursor) {
    int currentPos = cursor.position();
    QString currentLine = cursor.block().text();
    int columnPos = currentPos - cursor.block().position(); // Column position in the current line

    QString leadingSpaces = currentLine.left(columnPos);

    if (leadingSpaces.trimmed().isEmpty() && columnPos > 0) {
        int spacesToRemove = std::min(4, static_cast<int>(leadingSpaces.length()));
        cursor.movePosition(QTextCursor::Left, QTextCursor::KeepAnchor, spacesToRemove);
        cursor.removeSelectedText();
        setTextCursor(cursor);
        return;
    }

    if (currentPos > 0 && currentPos < document()->characterCount()) {
        QChar previousChar = document()->characterAt(currentPos - 1);
        QChar nextChar = document()->characterAt(currentPos);

        const QMap<QChar, QChar> pairMapping = {
            {'(', ')'},
            {'{', '}'},
            {'[', ']'},
            {'\"', '\"'},
            {'\'', '\''}
        };

        if (pairMapping.contains(previousChar) && pairMapping[previousChar] == nextChar) {
            cursor.deletePreviousChar();
            cursor.deleteChar();
            setTextCursor(cursor);
            return;
        }
    }

    SearchTextEdit::keyPressEvent(new QKeyEvent(QKeyEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier));
}

void PythonCodeEditor::handleDelete(QTextCursor &cursor) {
    if (cursor.position() < document()->characterCount() - 1) {
        QChar currentChar = document()->characterAt(cursor.position());
        QChar nextChar = document()->characterAt(cursor.position() + 1);

        const QMap<QChar, QChar> pairMapping = {
            {'(', ')'},
            {'{', '}'},
            {'[', ']'},
            {'\"', '\"'},
            {'\'', '\''}
        };

        if (pairMapping.contains(currentChar) && pairMapping[currentChar] == nextChar) {
            cursor.deleteChar();
            cursor.deleteChar();
            setTextCursor(cursor);
            return;
        }
    }
    SearchTextEdit::keyPressEvent(new QKeyEvent(QKeyEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier));
}

void PythonCodeEditor::handleAutoIndent(QTextCursor &cursor) {
    QString currentLine = cursor.block().text();
    QString leadingWhitespace;

    for (QChar ch : currentLine) {
        if (ch.isSpace()) {
            leadingWhitespace.append(ch);
        } else {
            break;
        }
    }

    if (currentLine.trimmed().endsWith(':')) {
        leadingWhitespace.append("    ");
    }

    SearchTextEdit::keyPressEvent(new QKeyEvent(QKeyEvent::KeyPress, Qt::Key_Return, Qt::NoModifier));
    cursor.insertText(leadingWhitespace);
    setTextCursor(cursor);
}
