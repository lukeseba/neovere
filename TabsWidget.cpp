#include "TabsWidget.h"

#include <iostream>
#include <ostream>
#include <qstyle.h>
#include <QTextBlock>

TabsWidget::TabsWidget(bool includeLabel, bool buttonStyleTabs, QWidget *parent) : QWidget(parent), currentFont(QFont())
{
    layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    mainWidget = new QWidget(this);
    mainWidget->setLayout(layout);

    hasLabel = includeLabel;
    buttonStyleTabs = buttonStyleTabs;

    if (hasLabel) {
        nameLabel = new QLineEdit(mainWidget);
        nameLabel->setAlignment(Qt::AlignCenter);
        nameLabel->setReadOnly(true);
        nameLabel->setStyleSheet("QLineEdit{ background-color: transparent;  color: black; border: 0px;}"
                                            );
    }

    mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(mainWidget);
    if (hasLabel) {
        mainLayout->addWidget(nameLabel);
    }
    mainWidget->setMinimumWidth(1);
    setLayout(mainLayout);
}

TabButton * TabsWidget::selectedTab() {
    return tabs.at(selectedTabIndex);
}

TabButton * TabsWidget::getTab(int index) {
    return tabs.at(index);
}

void TabsWidget::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
}

void TabsWidget::addTab(const QString &tabName, const QString data, bool closeable)
{
    TabButton *tab = new TabButton(tabName, data, closeable, this);
    tab->button()->setFont(currentFont); // Apply the current font to the tab
    tab->closeButton()->setFont(currentFont);

    int index = tabs.size();
    tabs.append(tab);

    int row = index / maxColumns;
    int col = index % maxColumns;

    layout->addWidget(tab, row, col);

    // Style the close button to make it red and initially hidden
    tab->closeButton()->setStyleSheet(
        "background-color: rgb(235, 100, 175); color: white; border: 1px solid lightgray; border-radius: 1px; padding: 5px 5px;");
    tab->closeButton()->setVisible(false);

    // Set the default style for the tab
    tab->button()->setStyleSheet(
        "background-color: white; border: 1px solid lightgray; border-radius: 1px; padding: 5px 5px;"
    );

    // Connect signals for the tab button
    connect(tab, &TabButton::clicked, [this, index]() { handleTabClicked(index); });
    connect(tab, &TabButton::closeClicked, [this, index]() { handleTabCloseClicked(index); });
}

void TabsWidget::removeTab(int index)
{
    emit tabRemoved(index);

    if (index < 0 || index >= tabs.size()) {
        return;
    }

    // deselect current tab if selected
    if (selectedTabIndex == index) {
        deselectCurrentTab();
    }

    // Remove the tab
    TabButton *tab = tabs.takeAt(index);
    layout->removeWidget(tab);
    tab->deleteLater();

    // Update `selectedTabIndex` if necessary
    // Reorganize the layout
    updateTabGrid();
}

void TabsWidget::setTabsFont(const QFont &font)
{
    currentFont = font; // Update the font for future tabs
    for (auto *tab : tabs) {
        tab->button()->setFont(font); // Update the font for existing tabs
        tab->closeButton()->setFont(font);
    }
}

void TabsWidget::setLabelFont(const QFont &font)
{
    if (hasLabel) {
        nameLabel->setFont(font);
    }
}

void TabsWidget::updateTabGrid()
{
    for (int i = 0; i < tabs.size(); ++i) {
        int row = i / maxColumns;
        int col = i % maxColumns;
        layout->addWidget(tabs[i], row, col);

        // Update connections for each tab's index
        disconnect(tabs[i], &TabButton::clicked, nullptr, nullptr);
        disconnect(tabs[i], &TabButton::closeClicked, nullptr, nullptr);

        connect(tabs[i], &TabButton::clicked, [this, i]() { handleTabClicked(i); });
        connect(tabs[i], &TabButton::closeClicked, [this, i]() { handleTabCloseClicked(i); });
    }
}

int TabsWidget::tabCount() {
    return tabs.size();
}


void TabsWidget::deselectCurrentTab()
{
    if (selectedTabIndex >= 0 && selectedTabIndex < tabs.size()) {
        TabButton *currentTab = tabs[selectedTabIndex];

        // Reset the tab style to default
        currentTab->button()->setStyleSheet(
            "background-color: white; border: 1px solid lightgray; border-radius: 1px; padding: 5px 5px;"
        );

        // Deselect the tab
        currentTab->button()->setChecked(false);
        currentTab->closeButton()->setVisible(false); // Hide the "x" button

        if (hasLabel) {
            nameLabel->setText("");
        }
        selectedTabIndex = -1;
    }
}

void TabsWidget::handleTabClicked(int index)
{
    selectTab(index);
}

// Add this new method implementation
void TabsWidget::setColor(const QColor &color) {
    highlightColor = color;

    // Update the style of the currently selected tab if there is one
    if (selectedTabIndex >= 0 && selectedTabIndex < tabs.size()) {
        updateSelectedTabStyle();
    }
}

// Add this helper method
void TabsWidget::updateSelectedTabStyle() {
    if (selectedTabIndex >= 0 && selectedTabIndex < tabs.size()) {
        TabButton *currentTab = tabs[selectedTabIndex];
        currentTab->button()->setStyleSheet(
            QString("background-color: rgb(%1, %2, %3); border: 1px solid lightgray; border-radius: 3px; padding: 5px 5px;")
                .arg(highlightColor.red())
                .arg(highlightColor.green())
                .arg(highlightColor.blue())
        );
    }
}

// Modify the selectTab method to use the helper
void TabsWidget::selectTab(int index) {
    // Deselect the currently selected tab
    deselectCurrentTab();

    // Select the new tab
    selectedTabIndex = index;
    TabButton *newTab = tabs[selectedTabIndex];

    // Update the style using the current highlight color
    updateSelectedTabStyle();

    newTab->button()->setChecked(true);
    if (newTab->closeable) {
        newTab->closeButton()->setVisible(true);
    }

    if (hasLabel) {
        nameLabel->setText(tabs.at(selectedTabIndex)->text());
    }

    emit tabSelected(index);
}

void TabsWidget::handleTabCloseClicked(int index)
{
    removeTab(index); // Remove the tab when its close button is clicked
    if (selectedTabIndex == index) {
        deselectCurrentTab();
    }
}