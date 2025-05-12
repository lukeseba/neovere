#include "PythonCodeEditor.h"
#include <QPainter>
#include <QTextBlock>

PythonCodeEditor::PythonCodeEditor(QWidget *parent)
    : SearchTextEdit(parent), lineNumberArea(new LineNumberArea(this)) {
    // Connect signals to update line number area and highlight the current line
    connect(this, &SearchTextEdit::blockCountChanged, this, &PythonCodeEditor::updateLineNumberAreaWidth);
    connect(this, &SearchTextEdit::updateRequest, this, &PythonCodeEditor::updateLineNumberArea);
    connect(this, &SearchTextEdit::cursorPositionChanged, this, &PythonCodeEditor::highlightCurrentLine);

    updateLineNumberAreaWidth(0);
    highlightCurrentLine();
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
            QString number = QString::number(blockNumber + 1)   ;
            painter.setPen(QColor(25, 25, 40));
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

void PythonCodeEditor::keyPressEvent(QKeyEvent *event) {
    QTextCursor cursor = textCursor();

    // Handle bulk indent/unindent
    if (event->key() == Qt::Key_Tab || (event->key() == Qt::Key_Backtab && event->modifiers() & Qt::ShiftModifier)) {
        indentSelection(cursor, event->key() == Qt::Key_Backtab);
        return;
    }

    // Handle comment/uncomment toggle
    if (event->key() == Qt::Key_Slash && event->modifiers() & Qt::ControlModifier) {
        toggleComment(cursor);
        return;
    }

    // Handle backspace for removing paired characters
    if (event->key() == Qt::Key_Backspace) {
        handleBackspace(cursor);
        return;
    }

    // Handle delete for removing paired characters
    if (event->key() == Qt::Key_Delete) {
        handleDelete(cursor);
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
            cursor.movePosition(QTextCursor::Right);
            setTextCursor(cursor);
            return;
        }
    }

    SearchTextEdit::keyPressEvent(event); // Default behavior for other keys
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
