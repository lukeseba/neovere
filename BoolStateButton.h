//
// Created by lukebalfanz on 11/9/24.
//

#ifndef BOOLSTATEBUTTON_H
#define BOOLSTATEBUTTON_H
#include <QPushButton>


class BoolStateButton : public QPushButton {
    Q_OBJECT

public:
    explicit BoolStateButton(const QString &text1, const QString &text2,
                                std::function<void()> func1,
                                std::function<void()> func2,
                                QWidget *parent = nullptr);

private slots:
    void toggleState();

private:
    QString stateText1;
    QString stateText2;
    std::function<void()> stateFunc1;
    std::function<void()> stateFunc2;
    bool isState1;

};



#endif //BOOLSTATEBUTTON_H
