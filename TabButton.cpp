#include "TabButton.h"

TabButton::TabButton(const QString &text, const QString data, bool closeable = true, QWidget *parent)
    : QWidget(parent), mainButton(new QPushButton(text, this)), closeButtonWidget(new QPushButton("x", this))
{
    layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    this->data = data;
    this->closeable = closeable;

    // Configure the main button
    mainButton->setFlat(true); // Optional: make it flat for a tab-like look
    mainButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(mainButton);

    QPalette buttonPalette = mainButton->palette();
    buttonPalette.setColor(QPalette::ButtonText, Qt::black);  // Set text color to black
    mainButton->setPalette(buttonPalette);


    // Configure the close button
    closeButtonWidget->setFlat(true); // Optional: make it flat for a minimalist look
    //closeButtonWidget->setFixedSize(16, 16); // Small square size
    closeButtonWidget->setFocusPolicy(Qt::NoFocus); // Prevent it from stealing focus
    closeButtonWidget->setVisible(false); // Initially hidden

    // Add buttons to the layout
    layout->addWidget(closeButtonWidget);

    // Connect signals
    connect(mainButton, &QPushButton::clicked, this, &TabButton::handleMainButtonClicked);
    connect(closeButtonWidget, &QPushButton::clicked, this, &TabButton::handleCloseButtonClicked);
}

void TabButton::setText(const QString &text)
{
    mainButton->setText(text);
}

QString TabButton::text() const
{
    return mainButton->text();
}
QString TabButton::getData() const {
    return this->data;
}


QPushButton *TabButton::button()
{
    return mainButton;
}

QPushButton *TabButton::closeButton()
{
    return closeButtonWidget;
}

void TabButton::handleMainButtonClicked()
{
    emit clicked();
}

void TabButton::handleCloseButtonClicked()
{
    emit closeClicked();
}
