#ifndef TABSWIDGET_H
#define TABSWIDGET_H

#include <QWidget>
#include <QGridLayout>
#include <QFont>
#include <QLineEdit>
#include "TabButton.h"

class TabsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TabsWidget(QWidget *parent = nullptr);
    void addTab(const QString &tabName, QString data, bool closeable);
    void removeTab(int index);
    void setTabsFont(const QFont &font); // Set font for all existing and future tabs
    void setLabelFont(const QFont &font);
    void selectTab(int index);
    void resizeEvent(QResizeEvent *event) override;
    int tabCount();
    TabButton * selectedTab();
    TabButton * getTab(int index);

    signals:
        void tabSelected(int index);
        void tabRemoved(int index);

private:
    QGridLayout *layout;
    QVBoxLayout *mainLayout;
    QWidget * mainWidget;
    QLineEdit * nameLabel;
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