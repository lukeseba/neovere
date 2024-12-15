#include <iostream>
#include <fstream>

#include <QApplication>
#include <QWidget>
#include <QHBoxLayout>
#include <QFrame>
#include <QTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QProcess>
#include <QPlainTextEdit>
#include <QLineEdit>

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
#include <opencv2/opencv.hpp>
#include <vector>

#include "BoolStateButton.h"
#include "MaintainFrame.h"
#include "MediaFrame.h"
#include "VideoSlider.h"
#include <QTemporaryFile>

#include "PythonHighlighter.h"
#include "PythonCodeEditor.h"

QProcess* process = nullptr;


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

QString exportFontResourceToFile(const QString& resourcePath) {
    QFile fontFile(resourcePath);  // Path to the resource
    if (!fontFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to load font resource.";
        return QString();
    }

    // Specify a path for the exported font file
    QString tempFilePath = QDir::tempPath() + "/arial-bold.ttf";

    QFile outputFile(tempFilePath);
    if (!outputFile.open(QIODevice::WriteOnly)) {
        qWarning() << "Failed to create font file at temporary location.";
        return QString();
    }

    // Write the contents of the resource file to the temporary file
    outputFile.write(fontFile.readAll());
    fontFile.close();
    outputFile.close();

    return tempFilePath;  // Return the path to the temporary file
}



void listResourceFiles(const QString &path = ":/") {
    QDirIterator it(path, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        qDebug() << it.next();
    }
}

bool openProjectFromFile(QStringList* programs, QStringList* videoPaths,  QPlainTextEdit *outputText) {
    QString fileName = QFileDialog::getOpenFileName(nullptr, "Open File", "", "NEOVERE Files (*.nv);;All Files (*)");
    if (!fileName.isEmpty()) {
        QFile file(fileName);
        outputText->appendPlainText("opened '"+fileName+"'");

        // Open the file for reading
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            outputText->setPlainText("Failed to open the file.");
            return false;
        }
        // read file contents
        QTextStream in(&file);
        QString content = in.readAll();
        QStringList lines = content.split('\n'); // Preserve empty lines

        file.close();
        for (int i = 1; i < lines.size(); i++) {
            if (lines.at(i) == "<>") {
                const int limit = i+3+lines.at(i+2).toInt();
                videoPaths->append(lines.at(i+1));
                QString program = "";
                for (int j = i+3; j < limit; j++) {
                    program += lines.at(j) + (j < limit-1 ? "\n":"");
                }
                programs->append(program);
                i=limit;
            }
        }
    } else {
        return false;
    }
    return true;
}

void saveProjectToFile(QString programs[], QString videoPaths[],  QPlainTextEdit *outputText) {
    // Get the file name and location from the user
    QString fileName = QFileDialog::getSaveFileName(
        nullptr, "Save File", "nullnomen.nv", "NEOVERE Files (*.nv);;All Files (*)");

    if (fileName.isEmpty()) {
        return; // User canceled the dialog
    }

    // Open the file for writing
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        outputText->setPlainText("Error. Cannot save file: " + file.errorString());
        return;
    }

    // Write the text from the QTextEdit to the file
    QTextStream out(&file);
    const int progSize = sizeof(*programs) / sizeof(programs[0]);
    out << "NV/vA_0::1\n";
    for (int i = 0; i < progSize; i++) {
        out << "<>\n";
        if (videoPaths[i].isEmpty()) {
            out << "//";
        } else {
            out << videoPaths[i];
        }
        out << "\n" << programs[i].count("\n") + 1 << "\n";
        out << programs[i];
    }
    file.close();

    outputText->setPlainText("File "+ fileName +" saved successfully");
}

QString combinePythonFiles(const QStringList &fileNames) {
    QString combinedCode;

    for (const QString &fileName : fileNames) {
        QString filePath = ":/resources/code/" + fileName + ".py";

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qDebug() << "Failed to open file:" << filePath;
            continue; // Skip this file, but try others
        }

        QTextStream in(&file);
        combinedCode += in.readAll() + "\n"; // Add file content to combined code
        file.close();
    }

    return combinedCode;
}

void remakeNeoverePy(QString &videoPath) {
    QStringList pythonFiles = {
        "header",
        "classes",
        "functions",
        "setVideo",
        "fields",
        "filters",
        "footer"};
    std::ofstream outFile("neovere.py");
    QString fileString = combinePythonFiles(pythonFiles);
    fileString.replace("[%$#path#$%]", videoPath);
    fileString.replace("[%$#arial#$%]", exportFontResourceToFile(":/resources/fonts/arial-bold.ttf"));
    outFile << fileString.toStdString();
    outFile.close();
}

void compileCode(QString code, QPlainTextEdit* outputDisplay, MediaFrame* player, const QString& videoPath) {
    // Clean up any existing process
    if (process != nullptr) {
        if (process->state() == QProcess::Running) {
            process->terminate();
            if (!process->waitForFinished(3000)) {
                process->kill();
            }
        }
        delete process;
        process = nullptr;
    }

    process = new QProcess();

    outputDisplay->appendPlainText("Compiling video ...");

    // Add import for neovere.py
    QString fullCode = /*QString("import neovere\n") +*/ code;

    // Set up environment
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    process->setProcessEnvironment(env);
    QString pythonExecutable = "python3"; // Adjust for Windows if needed

    // Connect process output signals
    QObject::connect(process, &QProcess::readyReadStandardOutput, [outputDisplay]() {
        QString output = process->readAllStandardOutput();
        outputDisplay->appendPlainText(output);
    });

    QObject::connect(process, &QProcess::readyReadStandardError, [outputDisplay]() {
        QString error = process->readAllStandardError();
        outputDisplay->appendPlainText("Error:\n" + error);
    });

    QObject::connect(process, &QProcess::errorOccurred, [outputDisplay](QProcess::ProcessError error) {
        QString errorMsg;
        switch (error) {
            case QProcess::FailedToStart:
                errorMsg = "Failed to start: The executable could not be found or is not executable.";
                break;
            case QProcess::Crashed:
                errorMsg = "Crashed: The process crashed after starting.";
                break;
            case QProcess::Timedout:
                errorMsg = "Timed out: The process timed out.";
                break;
            case QProcess::WriteError:
                errorMsg = "Write error: Unable to write to the process.";
                break;
            case QProcess::ReadError:
                errorMsg = "Read error: Unable to read from the process.";
                break;
            case QProcess::UnknownError:
            default:
                errorMsg = "Unknown error occurred.";
                break;
        }
        outputDisplay->appendPlainText("Process error occurred: " + errorMsg);
    });

    // Run the Python code
    process->start(pythonExecutable, QStringList() << "-c" << fullCode);

    // Check if the process starts successfully
    if (!process->waitForStarted()) {
        outputDisplay->appendPlainText("Failed to start process. Check your command and environment.");
    }
}



void importVideo(QString fileName, QString &videoPath, QPlainTextEdit *outputDisplay, MediaFrame *mediaPanel) {
    videoPath = fileName;
    QFile file(fileName);
    outputDisplay->appendPlainText("imported '"+fileName+"'");

    mediaPanel->setVideo(fileName);
    mediaPanel->playVideo();

    remakeNeoverePy(videoPath);
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // create font
    QFont dotrice = setFont(":/resources/fonts/dotrice.otf");
    QFont november = setFont(":/resources/fonts/november.ttf");
    QFont apestron = setFont(":/resources/fonts/apestron.ttf");
    QFont sofachrome = setFont(":/resources/fonts/sofachrome.otf");
    QFont pixcel = setFont(":/resources/fonts/pixcel.ttf");
    QFont twoK = setFont(":/resources/fonts/2k12.ttf");
    QFont dotim3 = setFont(":/resources/fonts/dotim3.ttf");
    QFont dotim5 = setFont(":/resources/fonts/dotim5.ttf");
    QFont dotim7 = setFont(":/resources/fonts/dotim7.ttf");
    QFont sucuba = setFont(":/resources/fonts/sucaba.ttf");
    QFont preforation = setFont(":/resources/fonts/preforation.ttf");
    QFont sftel = setFont(":/resources/fonts/sftel.ttf");
    QFont sftel_bold = setFont(":/resources/fonts/sftel_bold.ttf");
    QFont sftel_light = setFont(":/resources/fonts/sftel_light.ttf");
    QFont sftel_lightbold = setFont(":/resources/fonts/sftel_lightbold.ttf");
    QFont greaseBalls = setFont(":/resources/fonts/GreaseBalls.ttf");
    QFont niocTresni = setFont(":/resources/fonts/NiocTresni.ttf");
    QFont fifteenOkay = setFont(":/resources/fonts/FifteenOkay.ttf");
    QFont synthetic = setFont(":/resources/fonts/synthetic.ttf");
    QFont ledpanel = setFont(":/resources/fonts/ledpanel.ttf");
    QFont arial = setFont(":/resources/fonts/arial-bold.ttf");

    // project data
    QString videoPath;

    // Main window widget
    QWidget window;
    window.setWindowTitle("NEOVERE");
    window.resize(1200, 560);

    // Color Palette
    QPalette palette = window.palette();
    QColor nvWhite = QColor(250, 250, 255);
    palette.setColor(QPalette::Window, nvWhite);
    palette.setColor(QPalette::Button, Qt::white);
    palette.setColor(QPalette::ButtonText, Qt::black);
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::Text, Qt::black);
    window.setPalette(palette);
    window.setAutoFillBackground(true);

    // Create a layout for the window
    QHBoxLayout *mainLayout = new QHBoxLayout(&window);
    QVBoxLayout *rightLayout = new QVBoxLayout();
    QVBoxLayout *leftLayout = new QVBoxLayout;

    // Create top buttons
    QPushButton *openButton = new QPushButton("O P E N");
    QPushButton *newButton = new QPushButton("N E W");
    QPushButton *saveButton = new QPushButton("S A V E");
    QPushButton *exportButton = new QPushButton("E X P O R T");
    openButton->setFont(sftel_bold);
    newButton->setFont(sftel_bold);
    saveButton->setFont(sftel_bold);
    exportButton->setFont(sftel_bold);

    QHBoxLayout *topButtonLayout = new QHBoxLayout();
    topButtonLayout->addWidget(openButton);
    topButtonLayout->addWidget(newButton);
    topButtonLayout->addWidget(saveButton);
    topButtonLayout->addWidget(exportButton);
    QWidget *topButtonWidget = new QWidget();
    topButtonWidget->setLayout(topButtonLayout);

    // Create the button layout at the bottom the screen

    QPushButton *runButton = new QPushButton("▶️");
    QPushButton *uploadButton = new QPushButton("I M P O R T");
    uploadButton->setFont(sftel_bold);
    runButton->setFont(greaseBalls);

    QHBoxLayout *bottomButtonLayout = new QHBoxLayout();
    bottomButtonLayout->addWidget(runButton);
    bottomButtonLayout->addWidget(uploadButton);
    QWidget *bottomButtonWidget = new QWidget();
    bottomButtonWidget->setLayout(bottomButtonLayout);

    // Create the code panel
    QPlainTextEdit *codePanel = new PythonCodeEditor;
    codePanel->setPlaceholderText("INPUT"); // Set placeholder text
    codePanel->setLineWrapMode(QPlainTextEdit::NoWrap);
    codePanel->setFrameStyle(QFrame::Box | QFrame::Sunken);

    new PythonHighlighter(codePanel->document());

    // Create the left panel
    leftLayout->addWidget(topButtonWidget);
    leftLayout->addWidget(codePanel);
    leftLayout->addWidget(bottomButtonWidget);

    // Create the right panel
    MediaFrame *mediaPanel = new MediaFrame;
    mediaPanel->setFrameStyle(QFrame::Box | QFrame::Raised);
    mediaPanel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); // Expands width, fixed height

    QMediaPlayer *player = mediaPanel->getPlayer();

    // Create code output panel
    QPlainTextEdit *outputDisplay = new QPlainTextEdit;
    outputDisplay->setReadOnly(true);
    outputDisplay->setPlaceholderText("OUTPUT");
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

    // create timestamp
    QPushButton *timeStampButton  = new QPushButton();
    timeStampButton->setFont(niocTresni);
    videoSlider->assignButton(timeStampButton);

    mediaControlsLayout->addWidget(pauseButton);
    mediaControlsLayout->addWidget(videoSlider);
    mediaControlsLayout->addWidget(timeStampButton);

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
    mainLayout->addWidget(leftWidget, 1);
    mainLayout->addWidget(rightWidget, 1);

    // set font
    codePanel->setFont(dotrice);
    outputDisplay->setFont(dotrice);

    // generote neovere.py file
    remakeNeoverePy(videoPath);


    // --------------- CONNECTIONS ---------------------

    // Make run button run python code
    QObject::connect(runButton, &QPushButton::clicked, [outputDisplay, codePanel, mediaPanel, &videoPath]() {
        QString code = codePanel->toPlainText();
        compileCode(code, outputDisplay, mediaPanel, videoPath);
    });

    // Make the import button import a media file
    QObject::connect(uploadButton, &QPushButton::clicked, [&window, &videoPath, outputDisplay, mediaPanel]() {
        QString fileName = QFileDialog::getOpenFileName(&window, "Open File", "", "Video Files (*.mp4);;All Files (*)");
        if (!fileName.isEmpty()) {
            importVideo(fileName, videoPath, outputDisplay, mediaPanel);
        }
    });

    // Make the open button open a nv file
    QObject::connect(openButton, &QPushButton::clicked, [codePanel, outputDisplay, &videoPath, mediaPanel]() {
        QStringList programs;
        QStringList videos;

        if(openProjectFromFile(&programs, &videos, outputDisplay)) {
            codePanel->setPlainText(programs.at(0));
            importVideo(videos.at(0), videoPath, outputDisplay, mediaPanel);
        }
    });

    // Make save button download file
    QObject::connect(saveButton, &QPushButton::clicked, [codePanel, outputDisplay, &videoPath]() {
        QString programs[] = {codePanel->toPlainText()};
        QString videos[] = {videoPath};

        saveProjectToFile(programs, videos, outputDisplay);
    });

    // ---------- FINAL SETUP ---------------

    // Set the layout for the window
    window.setLayout(mainLayout);

    // Show the window
    window.show();

    return app.exec();
}


