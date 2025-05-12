#ifndef PYTHONCODEEDITOR_H
#define PYTHONCODEEDITOR_H

#include <QPlainTextEdit>
#include <QWidget>

#include "SearchTextEdit.h"

class LineNumberArea;

class PythonCodeEditor : public SearchTextEdit {
    Q_OBJECT

public:
    explicit PythonCodeEditor(QWidget *parent = nullptr);

    void setEditorFont(const QFont &font);
    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QWidget *lineNumberArea;

    // Helper methods for paired characters, indentation, etc.
    void handleConditionalPairInsertion(QTextCursor &cursor, const QString &open, const QString &close);
    void handleTripleQuotes(QTextCursor &cursor);
    void handleBackspace(QTextCursor &cursor);
    void handleDelete(QTextCursor &cursor);
    void handleAutoIndent(QTextCursor &cursor);
    void toggleComment(QTextCursor &cursor);
    void indentSelection(QTextCursor &cursor, bool unindent);

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void highlightCurrentLine();

    friend class LineNumberArea;
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
