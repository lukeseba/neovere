#include "MaintainFrame.h"
#include <iostream>

MaintainFrame::MaintainFrame(int w, int h, QWidget *parent)
    : QFrame(parent) {
    aspectWidth = w;
    aspectHeight = h;
}

void MaintainFrame::resizeEvent(QResizeEvent *event) {
    // Calculate the new width and   height while maintaining a 16:9 aspect ratio
    int newWidth = event->size().width();
    int newHeight = newWidth * aspectHeight / aspectWidth; // Maintain 16:9 ratio


    // Ensure the height doesn't exceed the available size
    if (newHeight > this->window()->frameGeometry().height()/1.2) {
        newHeight = this->window()->frameGeometry().height()/1.2;
        newWidth = newHeight * aspectWidth / aspectHeight;
    }

    // Resize the widget while maintaining the aspect ratio

    resize(newWidth, newHeight);
    setMinimumHeight(newHeight);
    setMaximumHeight(newHeight);

    QFrame::resizeEvent(event);
}
