#include "PythonCodeEditor.h"
#include "PythonHighlighter.h"   // single source of truth for the completion-popup colours
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
#include <QRegularExpression>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QSet>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>
#include <QTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QMouseEvent>
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

        m_sig = new QLabel(this);
        m_sig->setTextFormat(Qt::RichText);
        m_sig->setWordWrap(true);
        m_sig->setStyleSheet("font-family:Consolas,'Courier New',monospace;");

        m_desc = new QLabel(this);
        m_desc->setTextFormat(Qt::PlainText);      // preserves the docstring's line breaks
        m_desc->setWordWrap(true);
        m_desc->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        m_desc->setStyleSheet("color:#33333F;");

        m_footer = new QLabel(this);
        m_footer->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_footer->setStyleSheet("color:#3A6EA5;");

        lay->addWidget(m_sig);
        lay->addWidget(m_desc);
        lay->addWidget(m_footer);
    }

    // Fill the box for one member (resets to the collapsed state). Methods show
    // "name(params) -> ReturnType"; attributes show "name : Type". The docstring (if any)
    // goes underneath.
    void setContent(const QString &name, const MemberInfo &mi) {
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

        m_fullDoc = mi.doc;
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

    // Recompute sizes: how tall the doc wants to be, whether it overflows the collapsed
    // band, and the resulting box size. Does not move the window.
    void relayout() {
        const int textW = kBoxWidth - 22;     // box width minus margins + 1px border each side
        int fullH = 0;
        if (!m_fullDoc.isEmpty()) {
            fullH = m_desc->heightForWidth(textW);
            if (fullH <= 0) fullH = m_desc->sizeHint().height();
        }
        const int lineH = m_desc->fontMetrics().lineSpacing();
        const int collapsedH = lineH * kCollapsedLines;
        m_truncated = fullH > collapsedH + 2;

        if (m_fullDoc.isEmpty()) {
            m_desc->hide();
            m_footer->hide();
        } else {
            m_desc->show();
            m_desc->setFixedHeight(m_expanded ? fullH : qMin(fullH, collapsedH));
            updateFooter();
        }
        setFixedWidth(kBoxWidth);
        adjustSize();
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

    static constexpr int kBoxWidth = 340;
    static constexpr int kCollapsedLines = 6;
    static constexpr int kGap = 6;

    PythonCodeEditor *m_editor = nullptr;
    QLabel *m_sig = nullptr;
    QLabel *m_desc = nullptr;
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
    m_completer->popup()->installEventFilter(this);

    // Hover doc: a short grace period before hiding so the pointer can travel from the
    // method onto the box (to click "More ▾") without it disappearing.
    m_docHideTimer = new QTimer(this);
    m_docHideTimer->setSingleShot(true);
    m_docHideTimer->setInterval(220);
    connect(m_docHideTimer, &QTimer::timeout, this, [this]() {
        if (!m_docBoxHovered && m_docBox) m_docBox->hide();
    });
    viewport()->setMouseTracking(true);   // so viewportEvent sees MouseMove for hover tracking
}

int PythonCodeEditor::lineNumberAreaWidth() const {
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

    // Same class/def shapes as documentPython()'s regexes, but we also capture
    // indentation (so a method is only attributed to a class when it's a *direct*
    // child), the parameter list + return annotation, and public "self.x = ..." attrs.
    static const QRegularExpression classRe(R"(^(\s*)class\s+(\w+)\s*(?:\(([^)]*)\))?\s*:)");
    static const QRegularExpression defRe(R"(^(\s*)def\s+(\w+)\s*\((.*)$)");
    static const QRegularExpression attrRe(R"(^(\s+)self\.(\w+)\s*=\s*(.*)$)");
    static const QRegularExpression ctorRhsRe(R"(^([A-Za-z_]\w*)\s*\()");
    static const QRegularExpression identRe(R"(^[A-Za-z_]\w*$)");

    for (const QString &path : sources) {
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
                curClass = name;
                classIndent = cm.captured(1).length();
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
                        // Split the text after "(" into the parameter list (up to the
                        // first ")") and the "-> ReturnType" annotation.
                        const QString rest = dm.captured(3);
                        const int close = rest.indexOf(')');
                        const QString params = (close >= 0) ? rest.left(close) : rest;
                        QString ret;
                        if (close >= 0) {
                            const int arrow = rest.indexOf("->", close);
                            if (arrow >= 0) {
                                const int colon = rest.indexOf(':', arrow);
                                ret = (colon >= 0 ? rest.mid(arrow + 2, colon - arrow - 2)
                                                  : rest.mid(arrow + 2)).trimmed();
                            }
                        }
                        int docLast = li;
                        const QString doc = extractDocstring(lines, li, &docLast);
                        if (isCtor) {
                            // Feed the constructor's signature into the class record so
                            // "ClassName(" completes with the right parens + doc box. Keep
                            // the class docstring if we already have one; else use __init__'s.
                            MemberInfo &cinfo = s_classInfo[curClass];
                            cinfo.isMethod  = true;
                            cinfo.params    = cleanParams(params);
                            cinfo.takesArgs = methodTakesArgs(params);
                            if (cinfo.doc.isEmpty()) cinfo.doc = doc;
                        } else {
                            MemberInfo mi;
                            mi.isMethod  = true;
                            mi.takesArgs = methodTakesArgs(params);
                            mi.returnType = ret;
                            mi.params = cleanParams(params);
                            mi.doc = doc;
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
    const int curPos = textCursor().position();
    const QString text = toPlainText().left(curPos);
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

    // ---- head: media[...] | ClassName(...) | identifier ----
    static const QRegularExpression headMedia(R"(^media\s*\[[^\[\]]*\])");
    static const QRegularExpression headCtor(R"(^([A-Za-z_]\w*)\s*\([^()]*\))");
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
        R"(^\s*\.\s*([A-Za-z_]\w*)\s*(\([^()]*\))?\s*(\[[^\[\]]*\])?)");
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
    static const QRegularExpression dotRe(
        R"(((?:[A-Za-z_]\w*)\s*(?:\([^()]*\)|\[[^\[\]]*\])?(?:\s*\.\s*[A-Za-z_]\w*\s*(?:\([^()]*\)|\[[^\[\]]*\])?)*)\s*\.\s*([A-Za-z_]\w*|)$)");
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

    for (QTextBlock b = document()->firstBlock(); b.isValid(); b = b.next()) {
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
                                    const QPoint &globalPos) {
    if (!m_docBox) return;
    m_docBox->setContent(name, info);
    m_docBox->showNear(globalPos);
}

void PythonCodeEditor::hideCompletionDoc() {
    if (m_docHideTimer) m_docHideTimer->stop();
    m_docBoxHovered = false;
    m_hoverDocBlock = m_hoverDocStart = m_hoverDocEnd = -1;
    if (m_docBox) m_docBox->hide();
}

// Resolve the token under the mouse and show its doc box. Mirrors the completion context
// regex, but the hovered word is the full name (not a partial prefix). A "receiver.member"
// token documents the member; a bare class name documents its constructor. Anything else
// (a plain variable, a keyword) has nothing to document, so we let the box hide.
void PythonCodeEditor::maybeShowHoverDoc(const QPoint &viewportPos, const QPoint &globalPos) {
    if (m_completer && m_completer->popup()->isVisible()) return;  // typing wins over hover
    ensureApiTable();

    QTextCursor wc = cursorForPosition(viewportPos);
    wc.select(QTextCursor::WordUnderCursor);
    const QString word = wc.selectedText();
    static const QRegularExpression identOnly(R"(^[A-Za-z_]\w*$)");
    if (word.isEmpty() || !identOnly.match(word).hasMatch()) { scheduleHoverDocHide(); return; }

    const int selStart = wc.selectionStart();
    const int selEnd   = wc.selectionEnd();
    const QTextBlock blk = wc.block();
    // Same token as last time: leave the box (and any expand state) untouched.
    if (m_docBox && m_docBox->isVisible() && blk.blockNumber() == m_hoverDocBlock &&
        selStart == m_hoverDocStart && selEnd == m_hoverDocEnd) {
        if (m_docHideTimer) m_docHideTimer->stop();
        return;
    }

    // cursorForPosition snaps to the nearest character, so hovering the blank area past a
    // line still "selects" its last word. Require the pointer to actually fall within the
    // word's rectangle before treating it as a hover.
    QTextCursor sc(blk); sc.setPosition(selStart);
    QTextCursor ec(blk); ec.setPosition(selEnd);
    const QRect r0 = cursorRect(sc), r1 = cursorRect(ec);
    if (viewportPos.x() < qMin(r0.left(), r1.left()) - 2 ||
        viewportPos.x() > qMax(r0.left(), r1.left()) + 2 ||
        viewportPos.y() < qMin(r0.top(), r1.top())   ||
        viewportPos.y() > qMax(r0.bottom(), r1.bottom())) {
        scheduleHoverDocHide();
        return;
    }

    // Resolve the hovered token to its documentation. Two cases give classes the same
    // hover box that functions/methods get:
    //   1) "receiver.member" — infer the receiver's class, then look up the member.
    //   2) a bare class name — show its constructor signature + class docstring.
    const QString upto = blk.text().left(selEnd - blk.position());
    static const QRegularExpression re(
        R"(((?:[A-Za-z_]\w*)\s*(?:\([^()]*\)|\[[^\[\]]*\])?(?:\s*\.\s*[A-Za-z_]\w*\s*(?:\([^()]*\)|\[[^\[\]]*\])?)*)\s*\.\s*([A-Za-z_]\w*)$)");
    const QRegularExpressionMatch m = re.match(upto);

    QString docName;
    MemberInfo docInfo;
    bool found = false;
    if (m.hasMatch()) {
        const QString cls = inferType(m.captured(1), 0);
        const QString member = m.captured(2);
        if (!cls.isEmpty()) {
            const QMap<QString, MemberInfo> &mm = s_members.value(cls);
            auto it = mm.constFind(member);
            if (it != mm.constEnd()) { docName = member; docInfo = it.value(); found = true; }
        }
    } else {
        // No dot-receiver in front: the word itself may be a class name.
        auto it = s_classInfo.constFind(word);
        if (it != s_classInfo.constEnd()) { docName = word; docInfo = it.value(); found = true; }
    }
    if (!found) { scheduleHoverDocHide(); return; }

    m_hoverDocBlock = blk.blockNumber();
    m_hoverDocStart = selStart;
    m_hoverDocEnd   = selEnd;
    if (m_docHideTimer) m_docHideTimer->stop();
    showHoverDoc(docName, docInfo, globalPos);
}

// The mouse left a documented token (or the box itself): hide after a short grace
// period, but only a hover box — never the popup-anchored one.
void PythonCodeEditor::scheduleHoverDocHide() {
    m_hoverDocBlock = m_hoverDocStart = m_hoverDocEnd = -1;
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
    // The completer hides its popup itself in many cases; mirror that on the doc box.
    if (m_completer && obj == m_completer->popup() && event->type() == QEvent::Hide)
        hideCompletionDoc();
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
    // Tooltip events are delivered to the viewport, so we intercept them here to show
    // either the error message (priority) or the method/attribute signature+doc box.
    if (event->type() == QEvent::ToolTip) {
        QHelpEvent *he = static_cast<QHelpEvent *>(event);
        if (!m_errors.isEmpty()) {
            QTextBlock block = cursorForPosition(he->pos()).block();
            auto it = m_errors.constFind(block.blockNumber() + 1);   // map keys are 1-based
            if (it != m_errors.constEnd() && !it.value().message.isEmpty()) {
                // Only when the pointer is actually within this line's vertical band, so
                // the empty area below a short file doesn't trigger the last line's error.
                const QRectF r = blockBoundingGeometry(block).translated(contentOffset());
                if (he->pos().y() >= r.top() && he->pos().y() <= r.bottom()) {
                    QToolTip::showText(he->globalPos(), it.value().message, viewport());
                    return true;
                }
            }
        }
        // Not on an error: offer the API doc box for a hovered "receiver.method" token.
        QToolTip::hideText();
        maybeShowHoverDoc(he->pos(), he->globalPos());
        return true;
    }

    // Track the pointer so the hover box hides once it leaves the documented word (with
    // a grace period, so the user can reach the box to click "More ▾").
    if (event->type() == QEvent::MouseMove) {
        if (m_docBox && m_docBox->isVisible() && m_docBox->isInteractive()) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            QTextCursor wc = cursorForPosition(me->pos());
            wc.select(QTextCursor::WordUnderCursor);
            const bool sameToken = wc.block().blockNumber() == m_hoverDocBlock &&
                                   wc.selectionStart() == m_hoverDocStart &&
                                   wc.selectionEnd()   == m_hoverDocEnd;
            if (!sameToken) scheduleHoverDocHide();
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
        case Qt::Key_Escape:
            event->ignore();
            return;
        default:
            break;
        }
    }

    // Handle bulk indent/unindent
    if (event->key() == Qt::Key_Tab || (event->key() == Qt::Key_Backtab && event->modifiers() & Qt::ShiftModifier)) {
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

    if (pairInsertion.contains(event->key())) {
        if (m_completer) m_completer->popup()->hide();   // e.g. typing "(" to call the chosen method
        QString pair = pairInsertion[event->key()];
        QString open = pair.left(1);
        QString close = pair.right(1);
        handleConditionalPairInsertion(cursor, open, close);
        return;
    }

    // Handle tab key for indentation
    if (event->key() == Qt::Key_Tab) {
        cursor.insertText("    "); // Insert 4 spaces
        setTextCursor(cursor);
        return;
    }

    // Handle auto-indent
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        handleAutoIndent(cursor);
        return;
    }

    // Handle triple quotes
    if (event->key() == Qt::Key_QuoteDbl && cursor.position() > 1) {
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
