#include <iostream>
#include <QApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QFrame>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QProcess>
#include <QPlainTextEdit>

#include <QFontDatabase>
#include <QFont>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QSlider>
#include <QTimer>

#include <QUrl>

#include "BoolStateButton.h"
#include "MaintainFrame.h"
#include "MediaFrame.h"
#include "VideoSlider.h"

// Function to load a custom font and return a QFont object
QFont setFont(const QString &fontPath, int fontSize = 12) {
    int fontId = QFontDatabase::addApplicationFont(fontPath);
    if (fontId == -1) {
        qDebug() << "Failed to load custom font from:" << fontPath;
        return QFont(); // Return a default QFont if loading fails
    }

    QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
    if (!fontFamilies.isEmpty()) {
        return QFont(fontFamilies.at(0), fontSize); // Return the custom QFont
    } else {
        qDebug() << "No font families found for the loaded font.";
        return QFont(); // Return a default QFont if no families are found
    }
}

void listResourceFiles(const QString &path = ":/") {
    QDirIterator it(path, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        qDebug() << it.next();
    }
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // create font
    QFont dotrice = setFont(":/resources/fonts/dotrice.otf");
    QFont november = setFont(":/resources/fonts/november.ttf");
    QFont apestron = setFont(":/resources/fonts/apestron.ttf");
    QFont sofachrome = setFont(":/resources/fonts/sofachrome.otf");


    // Main window widget
    QWidget window;
    window.setWindowTitle("NEOVERE");
    window.resize(1200, 560);

    // Create a layout for the window
    QHBoxLayout *mainLayout = new QHBoxLayout(&window);
    QVBoxLayout *rightLayout = new QVBoxLayout();
    QVBoxLayout *leftLayout = new QVBoxLayout;
    QHBoxLayout *buttonLayout = new QHBoxLayout();

    // Create the button layout at the bottom the screen
    QPushButton *runButton = new QPushButton("▶️");
    QPushButton *uploadButton = new QPushButton("IMPORT");
    uploadButton->setFont(sofachrome);
    runButton->setFont(sofachrome);

    buttonLayout->addWidget(runButton);
    buttonLayout->addWidget(uploadButton);
    QWidget *buttonWidget = new QWidget();
    buttonWidget->setLayout(buttonLayout);

    // Create the left panel
    QTextEdit *codePanel = new QTextEdit;
    codePanel->setPlaceholderText("INPUT"); // Set placeholder text
    codePanel->setLineWrapMode(QTextEdit::NoWrap);
    codePanel->setFrameStyle(QFrame::Box | QFrame::Sunken);

    leftLayout->addWidget(codePanel);
    leftLayout->addWidget(buttonWidget);

    // Create the right panel
    MediaFrame *mediaPanel = new MediaFrame;
    mediaPanel->setFrameStyle(QFrame::Box | QFrame::Raised);
    mediaPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); // Expands width, fixed height

    QMediaPlayer *player = mediaPanel->getPlayer();

    // Create code output panel
    QPlainTextEdit *outputDisplay = new QPlainTextEdit;
    outputDisplay->setReadOnly(true);
    outputDisplay->setPlainText("OUTPUT");
    outputDisplay->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); // Set policy to expand vertically

    // Create media controls
    QHBoxLayout *mediaControlsLayout = new QHBoxLayout();
    QWidget *mediaControlsWidget = new QWidget();

    // Pause/play button
    BoolStateButton *pauseButton = new BoolStateButton(
       "⏸", "⏵",
       [mediaPanel]() { mediaPanel->playVideo(); },
       [mediaPanel]() { mediaPanel->pauseVideo(); }
   );

    // create video slider
    const int sliderSize = 1000;
    VideoSlider *videoSlider = new VideoSlider(player, sliderSize);

    mediaControlsLayout->addWidget(pauseButton);
    mediaControlsLayout->addWidget(videoSlider);

    mediaControlsWidget->setLayout(mediaControlsLayout);

    // create right panel
    rightLayout->addWidget(mediaPanel);
    rightLayout->addWidget(mediaControlsWidget);
    rightLayout->addWidget(outputDisplay);

    QWidget *leftWidget = new QWidget;
    QWidget *rightWidget = new QWidget;
    leftWidget->setLayout(leftLayout);
    rightWidget->setLayout(rightLayout);

    // Add the panels to the layout
    mainLayout->addWidget(leftWidget, 10);
    mainLayout->addWidget(rightWidget, 18);

    // set font
    codePanel->setFont(dotrice);
    outputDisplay->setFont(dotrice);

    // --------------- CONNECTIONS ---------------------

    // Make run button run python code
    QObject::connect(runButton, &QPushButton::clicked, [=]() {
        QString code = codePanel->toPlainText();

        // use QProcess to run python code
        QProcess process;
        process.start("python3", QStringList() << "-c" << code);
        process.waitForFinished();

        // Capture and display output
        QString output = process.readAllStandardOutput();
        QString error = process.readAllStandardError();
        outputDisplay->setPlainText(output + error);
    });

    // Make the import button import a media file
    QObject::connect(uploadButton, &QPushButton::clicked, [&window, outputDisplay, mediaPanel]() {
        QString fileName = QFileDialog::getOpenFileName(&window, "Open File", "", "Video Files (*.mp4);;All Files (*)");
        if (!fileName.isEmpty()) {
            QFile file(fileName);
            outputDisplay->setPlainText("imported '"+fileName+"'");
            mediaPanel->setVideo(fileName);
            mediaPanel->playVideo();
        }
    });

    // ---------- FINAL SETUP ---------------

    // Set the layout for the window
    window.setLayout(mainLayout);

    // Show the window
    window.show();

    return app.exec();
}


