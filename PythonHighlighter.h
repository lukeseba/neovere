#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QRegularExpression>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QKeyEvent>

class PythonHighlighter : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit PythonHighlighter(QTextDocument *parent = nullptr)
        : QSyntaxHighlighter(parent), inMultilineComment(false) {
        HighlightingRule rule;

        // Set default text color to dark grey
        defaultTextFormat.setForeground(QColor(100, 90, 110)); // Dark grey color

        // Keywords
        QTextCharFormat keywordFormat;
        keywordFormat.setForeground(Qt::blue);
        const QStringList keywordPatterns = {
            "\\bclass\\b", "\\bdef\\b", "\\breturn\\b", "\\bif\\b", "\\belse\\b", "\\bimport\\b",
            "\\bwhile\\b", "\\bfor\\b", "\\bin\\b", "\\btry\\b", "\\bglobal\\b", "\\bexcept\\b",
            "\\bfrom\\b"
        };
        for (const QString &pattern : keywordPatterns) {
            rule.pattern = QRegularExpression(pattern);
            rule.format = keywordFormat;
            highlightingRules.append(rule);
        }

        // Operators and comparisons
        QTextCharFormat operatorFormat;
        operatorFormat.setForeground(Qt::darkCyan);
        const QStringList operatorPatterns = {
            "\\+", "-", "\\*", "/", "\\band\\b", "\\bor\\b", "\\bnot\\b", "==", "!=",
            "<", ">", "<=", ">="
        };
        for (const QString &pattern : operatorPatterns) {
            rule.pattern = QRegularExpression(pattern);
            rule.format = operatorFormat;
            highlightingRules.append(rule);
        }

        // Function definitions
        functionDefFormat.setForeground(Qt::darkMagenta);
        functionDefFormat.setFontWeight(QFont::Bold);
        functionDefPattern = QRegularExpression("^\\s*def\\s+(\\w+)\\s*\\(([^)]*)\\)");

        // Class definitions
        classDefFormat.setForeground(Qt::darkRed);
        classDefFormat.setFontWeight(QFont::Bold);
        classDefPattern = QRegularExpression("^\\s*class\\s+(\\w+)\\b");

        // Function calls
        QTextCharFormat functionCallFormat;
        functionCallFormat.setForeground(QColor(185, 80, 135));
        rule.pattern = QRegularExpression("\\b\\w+\\b(?=\\s*\\()");
        rule.format = functionCallFormat;
        highlightingRules.append(rule);

        // Class names
        QTextCharFormat classFormat;
        classFormat.setForeground(Qt::red); // Highlight classes in red
        rule.pattern = QRegularExpression("\\b[A-Z][a-zA-Z0-9_]*\\b(?=\\s*\\()");
        rule.format = classFormat;
        highlightingRules.append(rule);

        // Parentheses
        QTextCharFormat parenthesesFormat;
        parenthesesFormat.setForeground(Qt::darkMagenta);
        rule.pattern = QRegularExpression("[()]");
        rule.format = parenthesesFormat;
        highlightingRules.append(rule);

        // Data types
        QTextCharFormat dataTypeFormat;
        dataTypeFormat.setForeground(QColor(173, 216, 230)); // Light blue
        const QStringList dataTypePatterns = {
            "\\bint\\b", "\\bstr\\b", "\\bbool\\b", "\\bfloat\\b", "\\bdouble\\b",
            "\\blist\\b", "\\bdict\\b", "\\btuple\\b", "\\bset\\b", "\\bNone\\b"
        };
        for (const QString &pattern : dataTypePatterns) {
            rule.pattern = QRegularExpression(pattern);
            rule.format = dataTypeFormat;
            highlightingRules.append(rule);
        }

        // Strings
        QTextCharFormat stringFormat;
        stringFormat.setForeground(Qt::darkGreen);
        rule.pattern = QRegularExpression("\".*\"|'.*'");
        rule.format = stringFormat;
        highlightingRules.append(rule);

        // Comments
        QTextCharFormat commentFormat;
        commentFormat.setForeground(Qt::lightGray);
        rule.pattern = QRegularExpression("#[^\n]*");
        rule.format = commentFormat;
        highlightingRules.append(rule);

        // Multi-line comments
        multiLineCommentFormat.setForeground(Qt::lightGray);
        multiLineCommentStart = QRegularExpression(R"(""")");
        multiLineCommentEnd = QRegularExpression(R"(""")");

        // Enums
        enumFormat.setForeground(QColor(255, 140, 0)); // Custom dark orange
    }

protected:
    void highlightBlock(const QString &text) override {
        // Apply default format to the entire block
        setFormat(0, text.length(), defaultTextFormat);

        // Highlight multi-line comments
        setCurrentBlockState(0);
        if (inMultilineComment) {
            int endIndex = text.indexOf(multiLineCommentEnd);
            if (endIndex == -1) {
                setFormat(0, text.length(), multiLineCommentFormat);
                inMultilineComment = true;
                return;
            } else {
                setFormat(0, endIndex + 3, multiLineCommentFormat);
                inMultilineComment = false;
            }
        }
        int startIndex = text.indexOf(multiLineCommentStart);
        while (startIndex >= 0) {
            int endIndex = text.indexOf(multiLineCommentEnd, startIndex + 3);
            int commentLength = (endIndex == -1) ? text.length() - startIndex
                                                 : endIndex - startIndex + 3;
            setFormat(startIndex, commentLength, multiLineCommentFormat);
            startIndex = (endIndex == -1) ? -1 : text.indexOf(multiLineCommentStart, endIndex + 3);
        }

        // Highlight all other rules
        for (const HighlightingRule &rule : highlightingRules) {
            QRegularExpressionMatchIterator matchIterator = rule.pattern.globalMatch(text);
            while (matchIterator.hasNext()) {
                QRegularExpressionMatch match = matchIterator.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }

        // Highlight function definitions
        QRegularExpressionMatch funcMatch = functionDefPattern.match(text);
        if (funcMatch.hasMatch()) {
            int nameStart = funcMatch.capturedStart(1);
            int nameLength = funcMatch.capturedLength(1);
            setFormat(nameStart, nameLength, functionDefFormat);
        }

        // Highlight class definitions
        QRegularExpressionMatch classMatch = classDefPattern.match(text);
        if (classMatch.hasMatch()) {
            int nameStart = classMatch.capturedStart(1);
            int nameLength = classMatch.capturedLength(1);
            setFormat(nameStart, nameLength, classDefFormat);
        }

        // Highlight enums
        QRegularExpression enumRegex("\\b\\w+\\.([a-zA-Z_][a-zA-Z_0-9]*)\\b(?!\\s*\\()"); // Avoid parentheses
        QRegularExpressionMatchIterator enumMatchIterator = enumRegex.globalMatch(text);
        while (enumMatchIterator.hasNext()) {
            QRegularExpressionMatch enumMatch = enumMatchIterator.next();
            int enumStart = enumMatch.capturedStart(1); // Position after the period
            int enumLength = enumMatch.capturedLength(1); // Length of the part after the period
            setFormat(enumStart, enumLength, enumFormat);
        }
    }

private:
    struct HighlightingRule {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    QVector<HighlightingRule> highlightingRules;

    QTextCharFormat multiLineCommentFormat;
    QRegularExpression multiLineCommentStart;
    QRegularExpression multiLineCommentEnd;
    bool inMultilineComment;

    QTextCharFormat functionDefFormat;
    QRegularExpression functionDefPattern;

    QTextCharFormat classDefFormat;
    QRegularExpression classDefPattern;

    QTextCharFormat functionCallFormat;

    QTextCharFormat enumFormat; // For enums

    QTextCharFormat defaultTextFormat;
};
