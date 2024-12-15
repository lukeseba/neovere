#include <QWidget>
#include <QPlainTextEdit>

class LineNumberArea : public QWidget {
public:
    explicit LineNumberArea(QPlainTextEdit *editor) : QWidget(editor), codeEditor(editor) {}

    QSize sizeHint() const override {
        return QSize(codeEditor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        codeEditor->lineNumberAreaPaintEvent(event);
    }

private:
    QPlainTextEdit *codeEditor;
};
