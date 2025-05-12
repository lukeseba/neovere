#ifndef BUTTONTEXTEDIT_H
#define BUTTONTEXTEDIT_H

#include "SearchTextEdit.h"
#include <QPushButton>
#include <QList>
#include <QTextBlock>
#include <QScrollBar>

class ButtonTextEdit : public SearchTextEdit {
    Q_OBJECT

public:
    enum ButtonAlignment {
        Left,
        Center,
        Right
    };

    struct ButtonInfo {
        int lineNumber;
        QPushButton* button;
        ButtonAlignment alignment;
        int horizontalOffset; // Additional offset from the alignment position
    };

    explicit ButtonTextEdit(QWidget* parent = nullptr) : SearchTextEdit(parent) {
        connect(this, &QPlainTextEdit::textChanged, this, &ButtonTextEdit::updateButtonPositions);
        connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &ButtonTextEdit::updateButtonPositions);
        connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, &ButtonTextEdit::updateButtonPositions);
    }

    ~ButtonTextEdit() {
        for (auto& info : buttons) {
            delete info.button;
        }
    }

    void addButton(int lineNumber, const QString& text, ButtonAlignment alignment = Right, int horizontalOffset = 0) {
        QPushButton* button = new QPushButton(text, this);
        button->setCursor(Qt::PointingHandCursor);
        button->setFont(buttonFont);
        // Adjust button size to fit new font
        button->adjustSize();
        button->setStyleSheet(buttonStyle);

        // Connect button click to emit signal with identifying info
        connect(button, &QPushButton::clicked, this, [=]() {
            emit buttonClicked(lineNumber, text);
        });

        ButtonInfo info{lineNumber, button, alignment, horizontalOffset};
        buttons.append(info);
        updateButtonPositions();
    }

    void setEditorFont(const QFont &font) {
        SearchTextEdit::setEditorFont(font);

        buttonFont = font;

        // Update font for all existing buttons
        for (auto& info : buttons) {
            if (info.button) {
                info.button->setFont(buttonFont);
                // Adjust button size to fit new font
                info.button->adjustSize();
            }
        }

        // Update button positions since font size might have changed
        updateButtonPositions();
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
            "}"
            "QPushButton:hover {"
            "   background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %4, stop:1 %2);"
            "}"
        ).arg(colorName,                      // %1 - base color
              color.lighter(110).name(),       // %2 - 10% lighter
              getContrastColor(color).name(),  // %3 - contrasting text color
              hoverColorName);                // %4 - hover color

        // Apply to all existing buttons
        for (auto& info : buttons) {
            if (info.button) {
                info.button->setStyleSheet(buttonStyle);
            }
        }

        // Store the color for future buttons
        currentButtonColor = color;
        SearchTextEdit::setColor(color);
    }

    void removeButtons() {
        disconnect(this, &ButtonTextEdit::buttonClicked, nullptr, nullptr);
        for (auto& info : buttons) {
            if (info.button) {
                delete info.button;
                info.button = nullptr; // Prevent double deletion
            }
        }
        buttons.clear(); // Clear the container if needed
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        SearchTextEdit::resizeEvent(event);
        updateButtonPositions();
    }

    void scrollContentsBy(int dx, int dy) override {
        SearchTextEdit::scrollContentsBy(dx, dy);
        updateButtonPositions();
    }

private:
    QColor currentButtonColor;

    // Helper function to get contrasting text color
    QColor getContrastColor(const QColor& color) const {
        // Calculate luminance and return black or white for best contrast
        return (color.red() * 0.299 + color.green() * 0.587 + color.blue() * 0.114) > 150
               ? Qt::black : Qt::white;
    }

private slots:
    void updateButtonPositions() {
        QTextDocument* doc = document();
        QFontMetrics fm(font());
        const int lineHeight = fm.height();

        for (auto& info : buttons) {
            QPushButton* button = info.button;
            QTextBlock block = doc->findBlockByLineNumber(info.lineNumber - 1);

            if (block.isValid()) {
                // Calculate vertical position
                QRectF blockRect = blockBoundingGeometry(block).translated(contentOffset());
                int yPos = static_cast<int>(blockRect.y());

                // Get button dimensions
                int buttonWidth = button->sizeHint().width();
                int viewportWidth = viewport()->width();
                int xPos = 0;

                // Calculate horizontal position based on precise alignment
                switch (info.alignment) {
                    case Left:
                        // Left edge of button at left edge + offset
                        xPos = info.horizontalOffset;
                        // Adjust for horizontal scroll (left-aligned buttons move with text)
                        xPos -= horizontalScrollBar()->value();
                        break;
                    case Center:
                        // Center of button at center of viewport + offset
                        xPos = (viewportWidth - buttonWidth) / 2 + info.horizontalOffset;
                        // Adjust for horizontal scroll (center-aligned buttons move with text)
                        xPos -= horizontalScrollBar()->value();
                        break;
                    case Right:
                        // Right edge of button at right edge of viewport - offset
                        xPos = viewportWidth - buttonWidth - info.horizontalOffset;
                        // Don't adjust for horizontal scroll (right-aligned buttons stay fixed)
                        break;
                }

                // Ensure button stays within visible area
                xPos = qMax(0, qMin(xPos, viewportWidth - buttonWidth));

                // Set button position and make it visible
                button->move(xPos, yPos);
                button->show();
            } else {
                button->hide();
            }
        }
    }

private:
    QList<ButtonInfo> buttons;
    QFont buttonFont;
    QString buttonStyle;

signals:
    void buttonClicked(int lineNumber, const QString& text);

};

#endif // BUTTONTEXTEDIT_H