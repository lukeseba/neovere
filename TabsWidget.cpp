//
// Created by lukebalfanz on 12/15/24.
//

#include "TabsWidget.h"

#include <QPushButton>
#include <QVBoxLayout>

TabsWidget::TabsWidget(int columnCount, QWidget *parent) : QWidget(parent) {
    this->layout = new QGridLayout(this);
    this->currentPos = QVector2D(0, 0);
    this->columnCount = columnCount;
    this->tabs = QWidgetList();
    this->tabCount = 0;

    this->setLayout(layout);
    for (int i = 0; i < 20; i++) {
        addTab("Tab #" + QString::number(i + 1));
    }
    removeTab(0);
    removeTab(0);
    removeTab(0);
    removeTab(0);

}

void TabsWidget::addTab(QString name) {
    auto *button = new QPushButton(name, this);
    addWidget(button);
}

void TabsWidget::addWidget(QWidget *widget) {
    if(currentPos.x() >= columnCount) {
        currentPos.setY(currentPos.y() + 1);
        currentPos.setX(0);
    }
    tabs.append(widget);
    layout->addWidget(widget, currentPos.y(), currentPos.x());
    currentPos.setX(currentPos.x() + 1);
    tabCount++;
}

void TabsWidget::removeTab(int index) {
    layout->removeWidget(tabs.at(index));
    delete tabs.at(index);
    tabs.removeAt(index);
    tabCount--;
    redraw();
}

void TabsWidget::redraw() {

}