#ifndef TABSWIDGET_H
#define TABSWIDGET_H

#include <QWidget>
#include <QGridLayout>
#include <QFont>
#include "TabButton.h"

class TabsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TabsWidget(QWidget *parent = nullptr);
    void addTab(const QString &tabName, bool closeable);
    void removeTab(int index);
    void setTabsFont(const QFont &font); // Set font for all existing and future tabs
    void selectTab(int index);

    signals:
        void tabSelected(int index);

private:
    QGridLayout *layout;
    QList<TabButton*> tabs;
    QFont currentFont;  // Store the font for existing and future tabs
    int maxColumns = 5; // Number of tabs per row before stacking vertically
    int selectedTabIndex = -1; // Keeps track of the currently selected tab

    void updateTabGrid();
    void deselectCurrentTab();

    private slots:
        void handleTabClicked(int index);
    void handleTabCloseClicked(int index);
};

#endif // TABSWIDGET_H
