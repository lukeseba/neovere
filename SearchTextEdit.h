#ifndef SEARCHTEXTEDIT_H
#define SEARCHTEXTEDIT_H

#include <QPlainTextEdit>
#include <QWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QShortcut>

// Slim, app-coloured scrollbars, matching the ones the completion doc box and the
// parameter panel already draw. Applied per-widget rather than through an
// application-wide stylesheet: qApp->setStyleSheet() swaps every widget over to
// QStyleSheetStyle and re-polishes the ones that already exist, which on Wayland
// re-creates the parameter panel's surface and loses the position it was moved to
// (the compositor then centres it on the parent window).
inline const char *neoScrollBarQss() {
    return "QScrollBar:vertical { background:transparent; width:8px; margin:0px; }"
           "QScrollBar::handle:vertical { background:#C8C8D0; border-radius:4px; min-height:24px; }"
           "QScrollBar::handle:vertical:hover { background:#B0B0BC; }"
           "QScrollBar:horizontal { background:transparent; height:8px; margin:0px; }"
           "QScrollBar::handle:horizontal { background:#C8C8D0; border-radius:4px; min-width:24px; }"
           "QScrollBar::handle:horizontal:hover { background:#B0B0BC; }"
           "QScrollBar::add-line, QScrollBar::sub-line { width:0px; height:0px; }"
           "QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }"
           "QAbstractScrollArea::corner { background:transparent; }";
}

class SearchTextEdit : public QPlainTextEdit {
    Q_OBJECT

    // In SearchTextEdit.h, add to the public section:
public:
    void setSearchBarPadding(int left, int right);
    explicit SearchTextEdit(QWidget *parent = nullptr);
    void showSearchBar();
    void hideSearchBar();
    bool isSearchBarVisible() const;
    void setColor(const QColor& color);
    void setEditorFont(const QFont &font);

protected:
    void updateLayout();
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    // Search bar widgets
    QWidget *searchBar;
    QLineEdit *searchEdit;
    QLabel *matchLabel;
    QPushButton *closeButton;
    QPushButton *nextButton;
    QPushButton *prevButton;

    QColor m_focusBorderColor;  // Add this member variable
    QFont currentFont;

    QVBoxLayout *mainLayout;  // Add this member
    QWidget *mainWidget;

    // Search functionality
    QList<QTextCursor> searchMatches;
    int currentMatchIndex;

    void createSearchBar();
    void findText(const QString &text);
    void highlightMatches();
    void moveToMatch(int index);
    void updateSearchBarPosition();

    private slots:
        void findNext();
        void findPrevious();
        void searchTextChanged(const QString &text);
        void closeSearchBar();
        void handleSearchEnterPressed();
};

#endif // SEARCHTEXTEDIT_H