#ifndef TABBUTTON_H
#define TABBUTTON_H

#include <QWidget>
#include <QPushButton>
#include <QHBoxLayout>

class TabButton : public QWidget
{
    Q_OBJECT

public:
    explicit TabButton(const QString &text, bool closeable, QWidget *parent = nullptr);

    void setText(const QString &text);
    QString text() const;

    QPushButton *button();       // Access to the main button
    QPushButton *closeButton();  // Access to the close button
    bool closeable;

    signals:
        void clicked();       // Emitted when the main button is clicked
    void closeClicked();  // Emitted when the close button is clicked



private:
    QPushButton *mainButton;
    QPushButton *closeButtonWidget;
    QHBoxLayout *layout;

    private slots:
        void handleMainButtonClicked();
    void handleCloseButtonClicked();
};

#endif // TABBUTTON_H
