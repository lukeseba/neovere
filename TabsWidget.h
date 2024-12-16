//
// Created by lukebalfanz on 12/15/24.
//

#ifndef TABSWIDGET_H
#define TABSWIDGET_H
#include <qgridlayout.h>
#include <qvectornd.h>
#include <QWidget>
#include <QPushButton>


class TabsWidget: public QWidget {
    Q_OBJECT
public:
    explicit TabsWidget(int columnCount, QWidget *parent = nullptr);
    void addTab(QString name);
    void removeTab(int index);
    int currentTab();
    QString getTabName(int index);

protected:
    void redraw();

    void addWidget(QWidget *button);

    QGridLayout * layout;
    QVector2D currentPos;
    int columnCount;
    QWidgetList tabs;
    int tabCount;
};



#endif //TABSWIDGET_H
