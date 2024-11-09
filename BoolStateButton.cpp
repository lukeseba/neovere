//
// Created by lukebalfanz on 11/9/24.
//

#include "BoolStateButton.h"

BoolStateButton::BoolStateButton(const QString &text1, const QString &text2,
                                       std::function<void()> func1,
                                       std::function<void()> func2,
                                       QWidget *parent)
    : QPushButton(text1, parent), stateText1(text1), stateText2(text2),
      stateFunc1(func1), stateFunc2(func2), isState1(true) {

    // Connect the button's clicked signal to the toggleState slot
    connect(this, &QPushButton::clicked, this, &BoolStateButton::toggleState);
}

void BoolStateButton::toggleState() {
    if (isState1) {
        setText(stateText2); // Change button text
        stateFunc2();        // Execute second function
    } else {
        setText(stateText1); // Change button text
        stateFunc1();        // Execute first function
    }
    isState1 = !isState1; // Toggle state
}