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