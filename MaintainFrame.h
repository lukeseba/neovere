#ifndef MAINTAINFRAME_H
#define MAINTAINFRAME_H

#include <QFrame>
#include <QResizeEvent>

class MaintainFrame final : public QFrame {
    Q_OBJECT // Needed for any QObject subclass

public:
    explicit MaintainFrame(QWidget *parent = nullptr); // Constructor declaration

protected:
    void resizeEvent(QResizeEvent *event) override; // Event handler declaration
};

#endif // MAINTAINFRAME_H
