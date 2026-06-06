#ifndef PYTHONCODEEDITOR_H
#define PYTHONCODEEDITOR_H

#include <QPlainTextEdit>
#include <QWidget>
#include <QList>
#include <QMap>
#include <QHash>
#include <QString>
#include <QStringList>

#include "SearchTextEdit.h"

class LineNumberArea;
class QCompleter;
class QStandardItemModel;
class CompletionDocBox;   // signature + description popup, defined in the .cpp
class QTimer;

// One reported Python error pinned to a 1-based source line. startCol/endCol are
// 0-based character offsets bounding the offending span on that line; -1 means
// "unknown", in which case the whole line (first non-space .. end) is underlined.
struct LineError {
    QString message;
    int startCol = -1;
    int endCol = -1;
};

// One member of an engine class, parsed from the Python sources. Drives the
// autocomplete popup: isMethod decides whether we append "()", takesArgs decides
// whether the caret lands inside those parens, and returnType/attrType (a class name
// when known) let us infer the type of chained expressions like video.get_frame(f)
// or video.audio so the next dot offers the right class's members.
struct MemberInfo {
    bool isMethod = true;
    bool takesArgs = false;   // method has a parameter beyond self/cls
    QString returnType;       // method: text after "->" (e.g. "Frame", "Optional[Frame]")
    QString attrType;         // attribute: class from "self.x = ClassName(...)" if any
    QString params;           // method: cleaned parameter list (self/cls dropped) for the signature box
    QString doc;              // docstring body (dedented), shown in the documentation box
};

// Category of a completion entry, mirroring the syntax highlighter's colours so the
// popup is colour-coded: Variable (plain identifiers / attributes, dark grey),
// Class (engine or user classes, red) and Callable (methods / functions, pink). The
// kind drives both the row colour and how insertCompletion() finishes the token:
// callables get "()" (caret left inside when they take arguments) while classes and
// variables are inserted bare.
enum class CompletionKind { Variable, Class, Callable };

struct CompletionItem {
    CompletionKind kind = CompletionKind::Variable;
    bool takesArgs = false;   // callables only: leave the caret inside the "()"
};

class PythonCodeEditor : public SearchTextEdit {
    Q_OBJECT

public:
    explicit PythonCodeEditor(QWidget *parent = nullptr);

    void setEditorFont(const QFont &font);
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

    // Red wavy underlines for lines that produced a Python error in the last
    // run/preview, keyed by 1-based line number (matching the editor gutter and
    // the "<your script>" tracebacks). Each entry carries the offending column
    // span (so only the bad part of the line is underlined) and the error message
    // shown on hover. Setting/clearing repaints immediately.
    void setErrors(const QMap<int, LineError> &errors);
    void clearErrors();

    // Member autocomplete: the class->methods lookup table is shared by all editors
    // and built lazily from the engine sources. Call this after the user adds, edits,
    // or removes a custom field/filter/class so the popup reflects the change on next
    // use (the table rebuilds on the next dot).
    static void invalidateApiTable();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;   // draws the red error underlines on top
    bool viewportEvent(QEvent *event) override;     // shows the error message on hover (tooltip)
    bool eventFilter(QObject *obj, QEvent *event) override;  // hides the doc box when the popup hides

private:
    QWidget *lineNumberArea;
    QMap<int, LineError> m_errors;   // 1-based line -> error span+message (empty = none)

    // Helper methods for paired characters, indentation, etc.
    void handleConditionalPairInsertion(QTextCursor &cursor, const QString &open, const QString &close);
    void handleTripleQuotes(QTextCursor &cursor);
    void handleBackspace(QTextCursor &cursor);
    void handleDelete(QTextCursor &cursor);
    void handleAutoIndent(QTextCursor &cursor);
    void toggleComment(QTextCursor &cursor);
    void indentSelection(QTextCursor &cursor, bool unindent);

    // ---- Member autocomplete (pops up on "." after an object) ----
    QCompleter *m_completer = nullptr;
    QStandardItemModel *m_completionModel = nullptr;
    int m_completionStart = -1;   // document position where the member prefix begins

    // Recompute the context at the caret (either "receiver.partial" or a bare partial
    // identifier) and show / narrow / hide the popup accordingly.
    void updateCompletionPopup();
    // Candidate names for the current caret context (prefix returned via out param),
    // sorted case-insensitively, with each name's category recorded in m_completionMeta.
    // In a "receiver." context these are the receiver class's members (we deliberately do
    // NOT fall back to "every member of every class"); otherwise they are the in-scope
    // identifiers (classes, functions, variables). Empty list => the popup hides.
    QStringList completionsForContext(QString *prefixOut);
    // Populate m_completionMeta with the identifiers in scope for the non-member context:
    // engine classes/globals plus names parsed from the current document (assignments,
    // def/class names, function params, and for/with/except/import targets).
    void collectIdentifierCompletions();

    // ---- type inference over the parsed API ----
    // Infer the class of a receiver expression: an identifier, a ClassName(...)
    // constructor, a media[...] subscript, or a chain such as video.get_frame(f) /
    // video.audio. Returns "" when the type can't be determined; depth guards against
    // cyclic "a = b; b = a" assignments.
    QString inferType(const QString &expr, int depth) const;
    // Right-hand side of the nearest "name = ..." assignment above the caret ("" if
    // none), used to type local variables.
    QString nearestAssignmentRhs(const QString &name) const;
    // Reduce a return/attribute type string to a known class name ("" if none), e.g.
    // "Optional[Frame]" -> "Frame", "'Audio'" -> "Audio".
    QString resolveReturnClass(const QString &typeStr) const;

    // Class whose members currently populate the popup (member context only), so the doc
    // box can look up each entry's MemberInfo (signature + docstring).
    QString m_completionClass;
    // Category (+ takes-args) for every name currently offered, keyed by the name. Filled
    // by completionsForContext()/collectIdentifierCompletions(); drives the popup row
    // colour and whether insertCompletion() appends "()". Covers both the member and the
    // identifier context, so insertion no longer depends on m_completionClass.
    QHash<QString, CompletionItem> m_completionMeta;

    // ---- Documentation / signature box (to the right of the popup, and on hover) ----
    // A frameless panel showing "name(params) -> returnType" plus the (collapsible)
    // docstring. It is defined in the .cpp (no Q_OBJECT) and never takes focus, so the
    // editor keeps its caret and keystrokes while the box is visible.
    CompletionDocBox *m_docBox = nullptr;
    QTimer *m_docHideTimer = nullptr;   // debounces hiding the hover box as the mouse moves
    bool m_docBoxHovered = false;       // mouse is currently over the box itself
    // The method token currently described by the *hover* box, so we don't rebuild/
    // flicker while the pointer rests on the same word. (-1 = none.)
    int m_hoverDocBlock = -1;
    int m_hoverDocStart = -1;
    int m_hoverDocEnd   = -1;

    // Show the doc box for a resolved entry: showCompletionDocFor() looks up <completion
    // class, member> (or a class constructor) and anchors the box beside the completion
    // popup (non-interactive); showHoverDoc() takes an already-resolved name+MemberInfo
    // (a method/attribute or a class constructor) and anchors it at the mouse, letting the
    // user click to expand. hideCompletionDoc() hides + resets state.
    void showCompletionDocFor(const QString &member);
    void showHoverDoc(const QString &name, const MemberInfo &info, const QPoint &globalPos);
    void hideCompletionDoc();
    // Hover plumbing: resolve the token under the mouse and (un)schedule hiding.
    void maybeShowHoverDoc(const QPoint &viewportPos, const QPoint &globalPos);
    void scheduleHoverDocHide();
public:
    // Called by CompletionDocBox (friend) when the pointer enters/leaves it, so a hover
    // box the user is reaching toward (to click "More") isn't hidden out from under them.
    void docBoxHoverChanged(bool inside);
private:

    // The API table is global to the engine, so it is shared (static) across editors
    // and built once on demand.
    static void ensureApiTable();
    static void buildApiTable();
    // class -> (member name -> info), inheritance flattened; holds public methods AND
    // public attributes.
    static QMap<QString, QMap<QString, MemberInfo>> s_members;
    static QMap<QString, QString> s_globalTypes;   // global identifier -> class (e.g. renderer)
    // class -> constructor signature + class docstring, so a "ClassName" completion can
    // auto-insert "()" (caret inside when __init__ takes args) and show the same doc box
    // the methods get. params/takesArgs come from __init__ (self dropped); doc is the
    // class docstring (falling back to __init__'s). isMethod is always true (a class is
    // callable); returnType stays empty so the box shows "ClassName(params)" with no "->".
    static QMap<QString, MemberInfo> s_classInfo;
    static bool s_tableBuilt;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();
    void insertCompletion(const QString &completion);   // fill the chosen method at the caret

    friend class LineNumberArea;
    friend class CompletionDocBox;
};

class LineNumberArea : public QWidget {
public:
    LineNumberArea(PythonCodeEditor *editor) : QWidget(editor), codeEditor(editor) {
        setFont(editor->font()); // Initialize with editor's font
    }

    void setFont(const QFont &font) {
        QWidget::setFont(font);
        update();
    }

    QSize sizeHint() const override {
        return QSize(codeEditor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        codeEditor->lineNumberAreaPaintEvent(event);
    }

private:
    PythonCodeEditor *codeEditor;
};

#endif // PYTHONCODEEDITOR_H
