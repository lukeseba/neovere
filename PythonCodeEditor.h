#ifndef PYTHONCODEEDITOR_H
#define PYTHONCODEEDITOR_H

#include <QPlainTextEdit>
#include <QWidget>
#include <QList>
#include <QMap>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QTextCursor>

#include "SearchTextEdit.h"

class LineNumberArea;
class QCompleter;
class QStandardItemModel;
class CompletionDocBox;   // signature + description popup, defined in the .cpp
class ParamPanel;         // interactive parameter-input panel, defined in the .cpp
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
    QString paramDoc;         // docstring to parse for per-parameter descriptions/tags. For a
                              // class constructor this is __init__'s docstring (not the class
                              // docstring, which is what doc holds); for a method it equals doc.
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

// The call whose parentheses enclose the caret, located by findEnclosingCall().
// valid is false when the caret isn't inside any call's "(...)". openPos is the
// document position of the "(" itself; closePos is its matching ")" (or -1 when the
// call isn't closed yet). callee is the (possibly dotted) name in front of the "(",
// e.g. "FEllipse" or "text.set_position"; argText is the raw text between "(" and the
// matching ")" (or the caret, when the call isn't closed yet).
struct EnclosingCall {
    bool valid = false;
    int openPos = -1;
    int closePos = -1;
    QString callee;
    QString argText;
};

class PythonCodeEditor : public SearchTextEdit {
    Q_OBJECT

public:
    explicit PythonCodeEditor(QWidget *parent = nullptr);

    void setEditorFont(const QFont &font);
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

    // Configure this editor as a single-line value field for the parameter panel: no gutter,
    // no wrapping, one line tall, Enter is swallowed. contextEditor is the script this value
    // belongs to, consulted so the field's autocomplete sees that script's variables. When
    // plainText is true the field is a literal-text input (e.g. a str argument): no syntax
    // highlighting and no member autocomplete, so what you type is taken verbatim.
    void enableValueFieldMode(PythonCodeEditor *contextEditor, bool plainText = false);

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
    // flicker while the pointer rests on the same word. For an error-only hover (no API
    // token) the span is (-1,-1) and m_hoverDocBlock is the error line's block number.
    int m_hoverDocBlock = -1;
    int m_hoverDocStart = -1;
    int m_hoverDocEnd   = -1;
    // The error message currently shown in the hover box (empty = none). Part of the
    // rebuild key so the box stays put while the pointer rests on the same error line.
    QString m_hoverErrorMsg;

    // Show the doc box for a resolved entry: showCompletionDocFor() looks up <completion
    // class, member> (or a class constructor) and anchors the box beside the completion
    // popup (non-interactive); showHoverDoc() takes an already-resolved name+MemberInfo
    // (a method/attribute or a class constructor) plus an optional error message, and anchors
    // it at the mouse, letting the user click to expand. When name is empty the box shows the
    // error alone; when errorMsg is empty it shows the doc alone; both shows error-over-doc.
    // hideCompletionDoc() hides + resets state.
    void showCompletionDocFor(const QString &member);
    void showHoverDoc(const QString &name, const MemberInfo &info,
                      const QString &errorMsg, const QPoint &globalPos);
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

    // ---- Interactive parameter-input panel ---------------------------------------
    // A focusable, frameless panel floated to the right of the caret whenever the caret
    // sits inside the parentheses of a documented call. It lists every documented
    // parameter with an input box + description; editing a field rewrites the whole argument
    // list as "name=value" in signature order (the panel owns the parens while open, so any
    // initial positional args are converted to keyword form). Defined in the .cpp (no Q_OBJECT).
    ParamPanel *m_paramPanel = nullptr;
    // Anchored at the bound call's "(" so its position stays valid as the document is
    // edited; an invalid (isNull) cursor means no call is currently bound.
    QTextCursor m_callOpenCursor;
    // Anchored at the bound call's ")" (isNull when the call isn't closed). Bounds the
    // argument scan in applyParamEdit so a value with an unbalanced "(" can never make the
    // scan — and the resulting rewrite — run past the call into the rest of the document.
    QTextCursor m_callCloseCursor;
    // Identifies the bound call ("<callee>@<openParenPos>") so a caret move within the
    // same call re-syncs the fields instead of rebuilding (which would drop field focus).
    QString m_callKey;
    // Documented parameter names in signature order, for mapping positional arguments.
    QStringList m_callParamOrder;
    // Set while the panel writes into the document, so the resulting cursor move / edit
    // doesn't recursively rebuild the panel.
    bool m_paramWriteGuard = false;
    QTimer *m_paramHideTimer = nullptr;   // grace period before hiding (lets the pointer/focus travel to the panel)

    // Value-field mode: set when this editor IS one of the parameter panel's input boxes
    // (see enableValueFieldMode). m_contextEditor is the script the value belongs to, used so
    // the field's autocomplete/type-inference resolves the script's variables, not its own
    // (single-expression) text.
    bool m_valueFieldMode = false;
    bool m_plainField = false;          // value field with no highlighting/autocomplete (e.g. a str arg)
    PythonCodeEditor *m_contextEditor = nullptr;

    // Locate the call whose parentheses enclose caretPos (pure text scan; tracks strings
    // and comments so brackets inside them don't count). See EnclosingCall.
    EnclosingCall findEnclosingCall(int caretPos) const;
    // Resolve a callee expression to its documented MemberInfo: a class constructor
    // (s_classInfo) or a method on a typed receiver (inferType -> s_members). Returns
    // false when the callee isn't documented; on success fills the display name
    // (e.g. "FEllipse") and a copy of the MemberInfo.
    bool resolveCallee(const QString &callee, QString *nameOut, MemberInfo *infoOut) const;
    // Rewrite the bound call's entire argument list from the panel's current field values,
    // as uniform "name=value" in signature order (the panel owns the parens while open, so
    // positional args become keyword args and unmappable text is dropped). Replaces only the
    // span between the anchored "(" and ")" cursors. The parameters are unused (the panel's
    // field state is the source of truth) but kept so the field's textChanged can call it.
    void applyParamEdit(const QString &param, const QString &value, bool cleared);
    // Hide the parameter panel and clear its tracking state (unbind the current call).
    void hideParamPanel();
    // Close every pop-up this editor owns — the completion popup, the doc/description box, and
    // the parameter panel (cascading into nested field panels). Used by Esc.
    void closeAllPopups();
    // True if w is inside this editor's parameter-panel subtree. Walks the QObject parent
    // chain (which spans our separate tool windows) rather than QWidget::isAncestorOf, which
    // is limited to a single window and so wrongly excludes nested-panel fields.
    bool focusInPanelTree(QWidget *w) const;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();
    void updateParamPanel();   // show / re-sync / hide the parameter-input panel as the caret moves
    void insertCompletion(const QString &completion);   // fill the chosen method at the caret

    friend class LineNumberArea;
    friend class CompletionDocBox;
    friend class ParamPanel;
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
