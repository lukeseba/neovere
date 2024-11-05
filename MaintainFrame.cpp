#include "MaintainFrame.h"
#include <iostream>

MaintainFrame::MaintainFrame(QWidget *parent)
    : QFrame(parent) {
    // Constructor implementation (can be empty if no custom logic is needed)
}

void MaintainFrame::resizeEvent(QResizeEvent *event) {

    // Calculate the new width and height while maintaining a 16:9 aspect ratio
    int newWidth = event->size().width();
    int newHeight = newWidth * 9 / 16; // Maintain 16:9 ratio


    // Ensure the height doesn't exceed the available size
    if (newHeight > this->window()->frameGeometry().height()) {
        newHeight = this->window()->frameGeometry().height();
        newWidth = newHeight * 16 / 9;
    }

    // Resize the widget while maintaining the aspect ratio

    setMinimumHeight(newHeight);
    setMaximumHeight(newHeight);

    QFrame::resizeEvent(event); // Call the base class implementation
}
