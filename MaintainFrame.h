#ifndef MAINTAINFRAME_H
#define MAINTAINFRAME_H

#include <QFrame>
#include <QResizeEvent>

class MaintainFrame : public QFrame {
    Q_OBJECT // Needed for any QObject subclass

public:
    explicit MaintainFrame(int w, int h, QWidget *parent = nullptr); // Constructor declaration

protected:
    void resizeEvent(QResizeEvent *event) override; // Event handler declaration
    int aspectWidth;
    int aspectHeight;
};

#endif // MAINTAINFRAME_H
