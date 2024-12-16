#include "TabsWidget.h"

TabsWidget::TabsWidget(QWidget *parent) : QWidget(parent), currentFont(QFont())
{
    layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
}

void TabsWidget::addTab(const QString &tabName, bool closeable = true)
{
    TabButton *tab = new TabButton(tabName, closeable, this);
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
        "background-color: none; border: 1px solid lightgray; border-radius: 1px; padding: 5px 5px;"
    );

    // Connect signals for the tab button
    connect(tab, &TabButton::clicked, [this, index]() { handleTabClicked(index); });
    connect(tab, &TabButton::closeClicked, [this, index]() { handleTabCloseClicked(index); });
}

void TabsWidget::removeTab(int index)
{
    if (index < 0 || index >= tabs.size()) {
        return;
    }

    // Remove the tab
    TabButton *tab = tabs.takeAt(index);
    layout->removeWidget(tab);
    tab->deleteLater();

    // Update `selectedTabIndex` if necessary
    if (selectedTabIndex == index) {
        selectedTabIndex = -1; // No tab selected after removal
    } else if (selectedTabIndex > index) {
        selectedTabIndex--; // Adjust the index of the selected tab
    }

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

void TabsWidget::deselectCurrentTab()
{
    if (selectedTabIndex >= 0 && selectedTabIndex < tabs.size()) {
        TabButton *currentTab = tabs[selectedTabIndex];

        // Reset the tab style to default
        currentTab->button()->setStyleSheet(
            "background-color: none; border: 1px solid lightgray; border-radius: 1px; padding: 5px 5px;"
        );

        // Deselect the tab
        currentTab->button()->setChecked(false);
        currentTab->closeButton()->setVisible(false); // Hide the "x" button
    }
}

void TabsWidget::handleTabClicked(int index)
{
    selectTab(index);
}

void TabsWidget::selectTab(int index) {
    // Deselect the currently selected tab
    deselectCurrentTab();

    // Select the new tab
    selectedTabIndex = index;
    TabButton *newTab = tabs[selectedTabIndex];

    // Highlight the tab with light blue
    newTab->button()->setStyleSheet(
        "background-color: rgb(200, 230, 255); border: 1px solid gray; border-radius: 3px; padding: 5px 5px;"
    );

    newTab->button()->setChecked(true);      // Mark the tab as selected
    if (newTab->closeable) {
        newTab->closeButton()->setVisible(true); // Show the "x" button
    }

    emit tabSelected(index);
}

void TabsWidget::handleTabCloseClicked(int index)
{
    removeTab(index); // Remove the tab when its close button is clicked
}
