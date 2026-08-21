#include "SearchTextEdit.h"
#include <QPainter>
#include <QRegularExpression>

// Add this new method implementation
void SearchTextEdit::setEditorFont(const QFont &font) {
    currentFont = font;
    QPlainTextEdit::setFont(font);  // Set font for main text box

    // Set font for search bar components
    if (searchEdit) {
        searchEdit->setFont(font);
    }
    if (matchLabel) {
        matchLabel->setFont(font);
    }
    if (prevButton) {
        prevButton->setFont(font);
    }
    if (nextButton) {
        nextButton->setFont(font);
    }
    if (closeButton) {
        closeButton->setFont(font);
    }
}

SearchTextEdit::SearchTextEdit(QWidget *parent)
    : QPlainTextEdit(parent), currentMatchIndex(0),
      m_focusBorderColor(Qt::blue), currentFont(font())
{
    mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 0, 5, 0); // Initial padding
    mainLayout->setSpacing(0);

    // Create search bar (will be first in the layout)
    createSearchBar();
    mainLayout->addWidget(searchBar);

    // Add stretch to push editor to take remaining space
    mainLayout->addStretch(1);

    // Set initial state
    hideSearchBar();
    setColor(m_focusBorderColor);
}

void SearchTextEdit::updateLayout() {
    if (searchBar->isVisible()) {
        searchBar->setFixedHeight(36);
        mainLayout->setStretch(1, 1); // Editor takes remaining space
    } else {
        searchBar->setFixedHeight(0); // Effectively hide but maintain layout structure
    }
}

void SearchTextEdit::showSearchBar() {
    if (!searchBar->isVisible()) {
        searchBar->show();
        updateLayout();
        searchEdit->setFocus();
    }
}

void SearchTextEdit::setSearchBarPadding(int left, int right) {
    if (searchBar) {
        // Update the main layout's spacing
        if (mainLayout) {
            mainLayout->setContentsMargins(left, 0, right, 0);
        }
    }
}

void SearchTextEdit::hideSearchBar() {
    searchBar->hide();
    updateLayout();
    // Clear search highlights when hiding
    QList<QTextEdit::ExtraSelection> extraSelections;
    setExtraSelections(extraSelections);
    setFocus();
}

void SearchTextEdit::resizeEvent(QResizeEvent *event) {
    QPlainTextEdit::resizeEvent(event);
    updateLayout();
}

void SearchTextEdit::createSearchBar() {
    searchBar = new QWidget(this);
    int defaultPadding = 5;

    // Set initial layout margins
    QHBoxLayout *layout = new QHBoxLayout(searchBar);
    layout->setContentsMargins(defaultPadding, 0, defaultPadding, 0);
    layout->setSpacing(5);

    // Simplified style - let the layout handle the spacing
    searchBar->setStyleSheet(
        "QWidget {"
        "   background-color: white;"
        "   border: 1px solid lightgray;"
        "   border-bottom: none;"
        "   border-radius: 1px;"
        "}"
    );
    searchBar->setFixedHeight(36);
    searchBar->hide();

    // Style the search input
    searchEdit = new QLineEdit(searchBar);
    searchEdit->setFont(currentFont);  // Set initial font
    searchEdit->setPlaceholderText("Search...");
    searchEdit->setStyleSheet(
        "QLineEdit {"
        "   background-color: white;"
        "   border: 1px solid lightgray;"
        "   border-radius: 1px;"
        "   padding: 3px 5px;"
        "}"
        "QLineEdit:focus {"
        "   border: 1px solid rgb(150, 100, 215);"
        "}"
    );
    searchEdit->setMinimumWidth(50);

    // Style the match label
    matchLabel = new QLabel("0/0", searchBar);
    matchLabel->setFont(currentFont);  // Set initial font
    matchLabel->setStyleSheet(
        "QLabel {"
        "   color: #666;"
        "   padding: 0 5px;"
        "   background-color: none;"
        "   border: none;"
        "}"
    );
    matchLabel->setAlignment(Qt::AlignCenter);
    matchLabel->setFixedWidth(60);

    // Style the navigation buttons
    QString buttonStyle =
        "QPushButton {"
        "   background-color: white;"
        "   border: 1px solid lightgray;"
        "   border-radius: 1px;"
        "   padding: 2px 5px;"
        "   min-width: 20px;"
        "   font-family: " + currentFont.family() + ";"  // Ensure font family matches
        "   font-size: " + QString::number(currentFont.pointSize()) + "pt;"
        "}"
        "QPushButton:hover {"
        "   background-color: #f0f0f0;"  // Light gray on hover
        "}"
        "QPushButton:pressed {"
        "   background-color: #e0e0e0;"  // Slightly darker when pressed
        "}";

    prevButton = new QPushButton("◀", searchBar);
    prevButton->setFont(currentFont);
    prevButton->setStyleSheet(buttonStyle);
    prevButton->setFixedWidth(30);

    nextButton = new QPushButton("▶", searchBar);
    nextButton->setFont(currentFont);
    nextButton->setStyleSheet(buttonStyle);
    nextButton->setFixedWidth(30);

    closeButton = new QPushButton("×", searchBar);
    closeButton->setFont(currentFont);
    closeButton->setStyleSheet(buttonStyle);
    closeButton->setFixedWidth(30);

    layout->addWidget(searchEdit);
    layout->addWidget(matchLabel);
    layout->addWidget(prevButton);
    layout->addWidget(nextButton);
    layout->addWidget(closeButton);

    connect(searchEdit, &QLineEdit::textChanged, this, &SearchTextEdit::searchTextChanged);
    connect(nextButton, &QPushButton::clicked, this, &SearchTextEdit::findNext);
    connect(prevButton, &QPushButton::clicked, this, &SearchTextEdit::findPrevious);
    connect(closeButton, &QPushButton::clicked, this, &SearchTextEdit::closeSearchBar);

    // Connect returnPressed signal to handle Enter key
    connect(searchEdit, &QLineEdit::returnPressed, this, &SearchTextEdit::handleSearchEnterPressed);

    // Add shortcut to close the search bar with Escape
    QShortcut *shortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(shortcut, &QShortcut::activated, this, &SearchTextEdit::closeSearchBar);
}

void SearchTextEdit::setColor(const QColor& color) {
    m_focusBorderColor = color;

    QString editorStyle = QString(
        "QPlainTextEdit {"
        "    border: 2px solid %1;"
        "    border-radius: 4px;"
        "    padding: 2px;"
        "    background-color: white;"
        "}"
        "QPlainTextEdit:focus {"
        "    border: 2px solid %2;"
        "}"
    ).arg(palette().color(QPalette::Mid).name(),
      m_focusBorderColor.name());
    // Slim app-coloured scrollbars for every editor (script panel, AI panel, class
    // editors, docs boxes) without reaching for an application-wide stylesheet.
    editorStyle += neoScrollBarQss();

    QString searchBarStyle = QString(
        "QLineEdit {"
        "    border: 1px solid %1;"
        "    border-radius: 2px;"
        "}"
        "QLineEdit:focus {"
        "    border: 1px solid %2;"
        "}"
    ).arg(palette().color(QPalette::Mid).name(),
      m_focusBorderColor.name());

    setStyleSheet(editorStyle);
    searchEdit->setStyleSheet(searchBarStyle);
}

bool SearchTextEdit::isSearchBarVisible() const {
    return searchBar->isVisible();
}

void SearchTextEdit::updateSearchBarPosition() {
    if (searchBar) {
        searchBar->setGeometry(0, 0, width(), searchBar->height());
    }
}

void SearchTextEdit::keyPressEvent(QKeyEvent *event) {
    // Handle search bar visibility
    if (event->key() == Qt::Key_F && event->modifiers() & Qt::ControlModifier) {
        if (isSearchBarVisible()) {
            hideSearchBar();
        } else {
            showSearchBar();
        }
        return;
    }

    // Close search bar when pressing Escape if it's visible
    if (event->key() == Qt::Key_Escape && isSearchBarVisible()) {
        hideSearchBar();
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

void SearchTextEdit::searchTextChanged(const QString &text) {
    findText(text);
}

void SearchTextEdit::findText(const QString &text) {
    if (text.isEmpty()) {
        // Clear existing highlights
        QList<QTextEdit::ExtraSelection> extraSelections;
        setExtraSelections(extraSelections);
        matchLabel->setText("0/0");
        return;
    }

    searchMatches.clear();
    QTextCursor cursor(document());
    QTextDocument::FindFlags flags;

    while (!cursor.isNull() && !cursor.atEnd()) {
        cursor = document()->find(text, cursor, flags);
        if (!cursor.isNull()) {
            searchMatches.append(cursor);
        }
    }

    highlightMatches();

    if (!searchMatches.isEmpty()) {
        currentMatchIndex = 0;
        moveToMatch(currentMatchIndex);
    } else {
        matchLabel->setText("0/0");
    }
}

void SearchTextEdit::highlightMatches() {
    QList<QTextEdit::ExtraSelection> extraSelections;

    // Highlight all matches
    QTextEdit::ExtraSelection highlight;
    highlight.format.setBackground(QColor(255, 255, 0, 100)); // Semi-transparent yellow
    highlight.format.setProperty(QTextFormat::FullWidthSelection, false);

    for (const QTextCursor &match : searchMatches) {
        highlight.cursor = match;
        extraSelections.append(highlight);
    }

    // Highlight current match with a different color
    if (!searchMatches.isEmpty() && currentMatchIndex >= 0 && currentMatchIndex < searchMatches.count()) {
        QTextEdit::ExtraSelection current;
        current.format.setBackground(QColor(100, 200, 255, 150)); // Semi-transparent blue
        current.format.setProperty(QTextFormat::FullWidthSelection, false);
        current.cursor = searchMatches[currentMatchIndex];
        extraSelections.append(current);
    }

    setExtraSelections(extraSelections);

    // Update match counter
    matchLabel->setText(QString("%1/%2").arg(searchMatches.isEmpty() ? 0 : currentMatchIndex + 1)
                                       .arg(searchMatches.count()));
}

void SearchTextEdit::moveToMatch(int index) {
    if (index >= 0 && index < searchMatches.count()) {
        currentMatchIndex = index;
        setTextCursor(searchMatches[index]);
        highlightMatches();
        centerCursor();
    }
}

void SearchTextEdit::findNext() {
    if (!searchMatches.isEmpty()) {
        currentMatchIndex = (currentMatchIndex + 1) % searchMatches.count();
        moveToMatch(currentMatchIndex);
    }
}

void SearchTextEdit::findPrevious() {
    if (!searchMatches.isEmpty()) {
        currentMatchIndex = (currentMatchIndex - 1 + searchMatches.count()) % searchMatches.count();
        moveToMatch(currentMatchIndex);
    }
}

void SearchTextEdit::closeSearchBar() {
    hideSearchBar();
}

void SearchTextEdit::handleSearchEnterPressed() {
    if (searchMatches.isEmpty()) {
        return;  // No matches to navigate through
    }

    // Check if Shift is pressed for reverse navigation
    bool shiftPressed = QGuiApplication::keyboardModifiers() & Qt::ShiftModifier;

    if (shiftPressed) {
        findPrevious();  // Shift+Enter goes to previous match
    } else {
        findNext();      // Regular Enter goes to next match
    }
}