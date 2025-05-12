#ifndef TABSWIDGET_H
#define TABSWIDGET_H

#include <QWidget>
#include <QGridLayout>
#include <QFont>
#include <QLineEdit>
#include <QColor>
#include "TabButton.h"

class TabsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit TabsWidget(bool includeLabel = true, bool buttonStyleTabs = false, QWidget *parent = nullptr);
    void addTab(const QString &tabName, QString data, bool closeable);
    void removeTab(int index);
    void setTabsFont(const QFont &font);
    void setLabelFont(const QFont &font);
    void selectTab(int index);
    void resizeEvent(QResizeEvent *event) override;
    int tabCount();
    TabButton * selectedTab();
    TabButton * getTab(int index);
    void setColor(const QColor &color);  // Add this line

    signals:
        void tabSelected(int index);
    void tabRemoved(int index);

private:
    QGridLayout *layout;
    QVBoxLayout *mainLayout;
    QWidget * mainWidget;
    QLineEdit * nameLabel;
    QList<TabButton*> tabs;
    QFont currentFont;
    int maxColumns = 5;
    int selectedTabIndex = -1;
    bool hasLabel;
    QColor highlightColor = QColor(200, 230, 255);  // Add this member variable

    void updateTabGrid();
    void deselectCurrentTab();
    void updateSelectedTabStyle();  // Add this helper method

    private slots:
        void handleTabClicked(int index);
    void handleTabCloseClicked(int index);
};

#endif // TABSWIDGET_H