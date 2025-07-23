#include <QWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QEvent>
#include <QStyle>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QFile>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>


class AiTextBoxWrapper : public QWidget {
    Q_OBJECT

public:
    public:
    AiTextBoxWrapper(QPlainTextEdit* plainTextEdit, const QString& openAiKey, QWidget* parent = nullptr)
        : QWidget(parent), originalTextEdit(plainTextEdit), isSecondaryVisible(false), openAiKey(openAiKey) {

        networkManager = new QNetworkAccessManager(this);

        // Set up the original text edit
        originalTextEdit->setParent(this);

        // Create the overlay button
        neopalButton = new QPushButton("NEOPAL", originalTextEdit);
        neopalButton->setFixedSize(80, 25);

        connect(neopalButton, &QPushButton::clicked, this, &AiTextBoxWrapper::toggleSecondaryTextEdit);

        // Create the secondary text edit
        secondaryTextEdit = new QPlainTextEdit(this);
        secondaryTextEdit->setVisible(false);
        secondaryTextEdit->setStyleSheet(
            "QPlainTextEdit {"
            "    background-color: #f8f8f8;"
            "    border: 1px solid #ddd;"
            "    border-radius: 3px;"
            "    padding: 5px;"
            "}"
        );
        secondaryTextEdit->setPlaceholderText("Enter your prompt for AI...");

        // Create the GENERATE button (parent is secondaryTextEdit)
        generateButton = new QPushButton("GENERATE", secondaryTextEdit);
        generateButton->setFixedSize(100, 30);
        generateButton->setVisible(false);

        // Connect generate button signal
        connect(generateButton, &QPushButton::clicked,
                this, &AiTextBoxWrapper::generateAiResponse);

        // Main layout
        mainLayout = new QVBoxLayout(this);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(5);
        mainLayout->addWidget(originalTextEdit);

        // Initial button positions
        updateButtonPositions();

        // Connect scroll events
        connect(originalTextEdit->verticalScrollBar(), &QScrollBar::valueChanged,
                this, &AiTextBoxWrapper::updateButtonPositions);
        connect(originalTextEdit->horizontalScrollBar(), &QScrollBar::valueChanged,
                this, &AiTextBoxWrapper::updateButtonPositions);
        connect(secondaryTextEdit->verticalScrollBar(), &QScrollBar::valueChanged,
                this, &AiTextBoxWrapper::updateButtonPositions);
        connect(secondaryTextEdit->horizontalScrollBar(), &QScrollBar::valueChanged,
                this, &AiTextBoxWrapper::updateButtonPositions);
    }


    void setColor(const QColor& color) {
        // Convert QColor to RGB string for stylesheet
        QString colorName = color.name(QColor::HexRgb);
        QString hoverColorName = color.lighter(120).name(QColor::HexRgb); // 20% lighter for hover

        buttonStyle = QString(
            "QPushButton {"
            "   border: 1px solid gray;"
            "   background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %2, stop:1 %1);"
            "   color: %3;"
            "   border-radius: 3px;"
            "}"
            "QPushButton:hover {"
            "   background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %4, stop:1 %2);"
            "}"
        ).arg(colorName,                      // %1 - base color
              color.lighter(110).name(),       // %2 - 10% lighter
              getContrastColor(color).name(),   // %3 - contrasting text color
              hoverColorName);                 // %4 - hover color

        // Apply to all buttons
        neopalButton->setStyleSheet(buttonStyle);
        generateButton->setStyleSheet(buttonStyle);

        // Store the color for future buttons
        currentButtonColor = color;
        m_focusBorderColor = color;

        QString editorStyle = QString(
            "QPlainTextEdit {"
            "    border: 2px solid %1;"
            "    border-radius: 4px;"
            "    padding: 2px;"
            "}"
            "QPlainTextEdit:focus {"
            "    border: 2px solid %2;"
            "}"
        ).arg(palette().color(QPalette::Mid).name(),
          m_focusBorderColor.name());

        secondaryTextEdit->setStyleSheet(editorStyle);
    }

    void setFont(const QFont &font) {
        buttonFont = font;
        neopalButton->setFont(buttonFont);
        generateButton->setFont(buttonFont);
        secondaryTextEdit->setFont(font);
    }

signals:
    void generateRequested(); // Signal emitted when GENERATE button is clicked

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        QTimer::singleShot(0, this, SLOT(updateButtonPositions()));
    }

private slots:
    void toggleSecondaryTextEdit() {
        isSecondaryVisible = !isSecondaryVisible;

        // Clear the layout
        QLayoutItem* item;
        while ((item = mainLayout->takeAt(0)) != nullptr) {
            if (item->widget()) {
                item->widget()->setParent(nullptr);
            }
            delete item;
        }

        if (isSecondaryVisible) {
            // Create split container
            QWidget* splitContainer = new QWidget(this);
            QVBoxLayout* splitLayout = new QVBoxLayout(splitContainer);
            splitLayout->setContentsMargins(0, 0, 0, 0);
            splitLayout->setSpacing(5);

            // Add text edits with 1:2 ratio
            secondaryTextEdit->setVisible(true);
            generateButton->setVisible(true);
            splitLayout->addWidget(secondaryTextEdit, 1);
            splitLayout->addWidget(originalTextEdit, 3);

            mainLayout->addWidget(splitContainer);
        } else {
            secondaryTextEdit->setVisible(false);
            generateButton->setVisible(false);
            mainLayout->addWidget(originalTextEdit);
        }

        updateButtonPositions();
    }

    void generateAiResponse() {
        if (openAiKey.isEmpty()) {
            originalTextEdit->appendPlainText("\n[Error: OpenAI API key not configured]");
            return;
        }

        QString prompt = secondaryTextEdit->toPlainText();
        if (prompt.isEmpty()) {
            originalTextEdit->appendPlainText("\n[Error: Prompt is empty]");
            return;
        }

        generateButton->setEnabled(false);
        generateButton->setText("processing");

        QNetworkRequest request(QUrl("https://api.openai.com/v1/chat/completions"));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        request.setRawHeader("Authorization", ("Bearer " + openAiKey).toUtf8());

        QString neovereLibrary = readFromFile("neovere.py");
        QString currentScript = originalTextEdit->toPlainText();

        QTextCursor cursor = originalTextEdit->textCursor();
        QString markedScript;
        QString insertionMarker = "[AI CODE INSERTED HERE]";

        if (cursor.hasSelection()) {
            int start = cursor.selectionStart();
            int end = cursor.selectionEnd();

            // Insert boundary markers around the selected code
            markedScript = currentScript;
            markedScript.insert(end, "[END SELECTED CODE]");
            markedScript.insert(start, "[BEGIN SELECTED CODE]");

        } else {
            // No selection: Insert marker at cursor
            int cursorPos = cursor.position();
            markedScript = currentScript;
            markedScript.insert(cursorPos, insertionMarker);
        }

        // Prepare role message
        QString roleTemplate = readFromFile("AI_role.txt")
            .replace("[NEOVERE LIBRARY HERE]", neovereLibrary)
            .replace("[CURRENT SCRIPT HERE]", markedScript);

        QJsonArray messages;

        QJsonObject systemMsg;
        systemMsg["role"] = "system";
        systemMsg["content"] = roleTemplate;
        messages.append(systemMsg);

        QJsonObject userMsg;
        userMsg["role"] = "user";
        userMsg["content"] = prompt;
        messages.append(userMsg);

        QJsonObject json;
        json["model"] = "gpt-4.1-mini";
        json["messages"] = messages;
        json["temperature"] = 1;

        QByteArray payload = QJsonDocument(json).toJson();

        QNetworkReply* reply = networkManager->post(request, payload);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            handleAiResponse(reply);
        });
    }


    void handleAiResponse(QNetworkReply* reply) {
        // Re-enable button
        generateButton->setEnabled(true);
        generateButton->setText("GENERATE");

        if (reply->error() != QNetworkReply::NoError) {
            int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QByteArray errorBody = reply->readAll();
            originalTextEdit->appendPlainText("\n[HTTP " + QString::number(statusCode) + "] " + reply->errorString());
            originalTextEdit->appendPlainText("\n[Response Body]: " + QString::fromUtf8(errorBody));
            reply->deleteLater();
            return;
        }


        QByteArray response = reply->readAll();
        reply->deleteLater();

        QJsonDocument jsonResponse = QJsonDocument::fromJson(response);
        if (jsonResponse.isNull()) {
            originalTextEdit->appendPlainText("\n[Error: Invalid API response]");
            return;
        }

        QJsonObject jsonObject = jsonResponse.object();
        if (!jsonObject.contains("choices") || !jsonObject["choices"].isArray()) {
            originalTextEdit->appendPlainText("\n[Error: Unexpected API response format]");
            return;
        }

        QJsonArray choices = jsonObject["choices"].toArray();
        if (choices.isEmpty()) {
            originalTextEdit->appendPlainText("\n[Error: No response from AI]");
            return;
        }

        QJsonObject firstChoice = choices.first().toObject();
        if (!firstChoice.contains("message") || !firstChoice["message"].isObject()) {
            originalTextEdit->appendPlainText("\n[Error: Invalid message format]");
            return;
        }

        QJsonObject message = firstChoice["message"].toObject();
        QString content = message["content"].toString();

        // Insert the response at the current cursor position
        QTextCursor cursor = originalTextEdit->textCursor();
        cursor.insertText(content);
    }



    void generateButtonClicked() {
        emit generateRequested();
    }

    void updateButtonPositions() {
        // Position NEOPAL button in original text edit (top right)
        QRect originalViewportRect = originalTextEdit->viewport()->rect();
        QPoint neopalTopRight = originalTextEdit->viewport()->mapToParent(QPoint(
            originalViewportRect.right() - neopalButton->width() - 5,
            originalViewportRect.top() + 5
        ));
        neopalButton->move(neopalTopRight);
        neopalButton->raise();

        // Position GENERATE button in secondary text edit (bottom right)
        if (secondaryTextEdit->isVisible()) {
            QRect secondaryViewportRect = secondaryTextEdit->viewport()->rect();
            QPoint generateBottomRight = secondaryTextEdit->viewport()->mapToParent(QPoint(
                secondaryViewportRect.right() - generateButton->width() - 5,
                secondaryViewportRect.bottom() - generateButton->height() - 5
            ));
            generateButton->move(generateBottomRight);
            generateButton->raise();
        }
    }

private:
    QPlainTextEdit* originalTextEdit;
    QPlainTextEdit* secondaryTextEdit;
    QPushButton* neopalButton;
    QPushButton* generateButton;
    QVBoxLayout* mainLayout;
    bool isSecondaryVisible;
    QString openAiKey;
    QNetworkAccessManager* networkManager;

    QColor m_focusBorderColor;
    QFont buttonFont;
    QString buttonStyle;
    QColor currentButtonColor;

    QString readFromFile(const QString& fileName) {
        QFile file(fileName);
        if (!file.exists()) {
            return "";
        }

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return "";
        }

        QString contents = file.readAll();
        file.close();
        return contents;
    }

    // Helper function to get contrasting text color
    QColor getContrastColor(const QColor& color) const {
        // Calculate luminance and return black or white for best contrast
        return (color.red() * 0.299 + color.green() * 0.587 + color.blue() * 0.114) > 150
               ? Qt::black : Qt::white;
    }
};