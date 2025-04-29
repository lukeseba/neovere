#include <iostream>
#include <fstream>
#include <regex>

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
#include <QStackedWidget>
#include <QRegularExpression>

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
#include <QMessageBox>

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

#include "TabsWidget.h"

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

bool openProjectFromFile(QStringList* programs, QStringList* mediaPaths,  QPlainTextEdit *outputText) {
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
            if (lines.at(i)     == "<>") {
                const int limit = i+2+lines.at(i+1).toInt();
                QString program = "";
                for (int j = i+2; j < limit; j++) {
                    program += lines.at(j) + (j < limit-1 ? "\n":"");
                }
                programs->append(program);
                i=limit;
            } else if (lines.at(i) == "|>") {
                mediaPaths->append(lines.at(i+1));
                i++;
            }
        }
    } else {
        return false;
    }
    return true;
}

void saveProjectToFile(QString programs[], QStringList mediaPaths,  QPlainTextEdit *outputText) {
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
        out << programs[i].count("\n") + 1 << "\n";
        out << programs[i] << "\n";
    }
    for (int i = 0; i < mediaPaths.size(); i++) {
        out << "|>\n";
        out << mediaPaths.at(i) << "\n";
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

void remakeNeoverePy(QStringList &mediaPath) {
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
    // add all file paths
    QString allPaths = "";
    for (int i = 0; i < mediaPath.length(); i++) {
        allPaths += "\"" + mediaPath.at(i) + "\"";
        if (i < mediaPath.length() - 1) {
            allPaths += ", ";
        }
    }
    fileString.replace("%$#path#$%", allPaths);
    fileString.replace("[%$#arial#$%]", exportFontResourceToFile(":/resources/fonts/arial-bold.ttf"));
    outFile << fileString.toStdString();
    outFile.close();
}

void compileCode(QString code, QPlainTextEdit* outputDisplay, TabsWidget * mediaHeader, MediaFrame * mediaPanel) {
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
    QString pythonExecutable = "python"; // Adjust for Windows if needed

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

    // when process is finished show rendered tab
    QObject::connect(process, &QProcess::finished, [mediaHeader, mediaPanel]() {
        mediaHeader->selectTab(0);
        mediaPanel->reloadVideo();
    });

    // Run the Python code
    process->start(pythonExecutable, QStringList() << "-c" << fullCode);

    // Check if the process starts successfully
    if (!process->waitForStarted()) {
        outputDisplay->appendPlainText("Failed to start process. Check your command and environment.");
    }
}

void importMedia(QString fileName, QStringList &mediaPath, QPlainTextEdit *outputDisplay, TabsWidget *header) {
    // Extract the base name of the new video (e.g., "render.mp4")
    QString newBaseName = QFileInfo(fileName).fileName();

    // Check if any video in mediaPath has the same base name
    bool duplicateFound = false;
    for (const QString &path : mediaPath) {
        if (QFileInfo(path).fileName() == newBaseName) {
            duplicateFound = true;
            break;
        }
    }

    // Handle duplicate videos
    if (duplicateFound) {
        outputDisplay->appendPlainText("'" + fileName + "' cannot be imported because a video with the same name already exists.");
        return;
    }

    // Import the video if no duplicate found
    mediaPath.append(fileName);
    QFile file(fileName);
    outputDisplay->appendPlainText("Imported '" + fileName + "'");

    QString tabName = QFileInfo(fileName).baseName();
    header->addTab(tabName, fileName, true);

    remakeNeoverePy(mediaPath);
}

void removeVideo(QString fileName, QStringList &mediaPaths) {
    mediaPaths.removeAll(fileName);
    remakeNeoverePy(mediaPaths);
}

void removeImportedTabs(TabsWidget *mediaHeader) {
    for (int i = mediaHeader->tabCount() - 1; i >= 0; i--) {
        if (mediaHeader->getTab(i)->closeable) {
            mediaHeader->removeTab(i);
        }
    }
}

void createNewFile(QPlainTextEdit* codePanel, QPlainTextEdit *outputText, TabsWidget * mediaHeader) {
    QFile defaultFile(":/resources/code/default_project.py");
    defaultFile.open(QIODevice::ReadOnly);
    codePanel->setPlainText(defaultFile.readAll());
    defaultFile.close();
    removeImportedTabs(mediaHeader);
    outputText->appendPlainText("New file created");
}


QString documentPython(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return "Error: Could not open file at " + path;
    }

    QTextStream in(&file);
    QString documented;
    bool insideDefinition = false;
    bool expectDocstring = false;
    int headerIndent = 0;
    bool insideParametersOrReturns = false;

    while (!in.atEnd()) {
        QString line = in.readLine();
        QString trimmed = line.trimmed();

        // Detect class
        QRegularExpression classRegex("^\\s*class\\s+(\\w+)\\s*(\\([^)]*\\))?");
        QRegularExpressionMatch classMatch = classRegex.match(line);

        // Detect method with parameters and optional return type
        QRegularExpression methodRegex("^\\s*def\\s+(\\w+)\\s*(\\(.*\\))\\s*(->\\s*[^:]*)?:");
        QRegularExpressionMatch methodMatch = methodRegex.match(line);

        if (classMatch.hasMatch()) {
            QString className = classMatch.captured(1);
            QString baseClasses = classMatch.captured(2);
            if (baseClasses.isNull()) {
                baseClasses = "";
            }

            // Only document public classes (those that don't start with '_')
            if (!className.startsWith("_")) {
                if (insideDefinition) {
                    documented += "\n";
                }

                int indent = line.indexOf(QRegularExpression("\\S")); // Find the first non-whitespace character
                documented += QString(0, ' ') + "class " + className + baseClasses + ":\n";
                insideDefinition = true;
                expectDocstring = true;
                headerIndent = indent;
            }
            continue;
        }

        if (methodMatch.hasMatch()) {
            QString methodName = methodMatch.captured(1);
            // Skip private methods or methods starting with a single '_', but not '__init__'
            if (methodName.startsWith("_") && !methodName.startsWith("__init__")) {
                continue;  // Skip this method entirely
            }

            QString parameters = methodMatch.captured(2);
            QString returnType = methodMatch.captured(3);

            // Always indent function signatures by 4 spaces
            int indent = 4;

            // Clean parameters: remove "self" if present
            parameters = parameters.trimmed();
            if (parameters.startsWith("(") && parameters.endsWith(")")) {
                QString innerParams = parameters.mid(1, parameters.length() - 2).trimmed();
                QStringList paramList = innerParams.split(",", Qt::SkipEmptyParts);

                // Remove 'self' from param list
                QStringList cleanedParams;
                for (QString param : paramList) {
                    param = param.trimmed();
                    if (param != "self") {
                        cleanedParams.append(param);
                    }
                }

                parameters = "(" + cleanedParams.join(", ") + ")";
            }

            QString signature = methodName + parameters;
            if (!returnType.isEmpty()) {
                signature += " " + returnType.trimmed();
            }
            signature += ":";

            if (insideDefinition) {
                documented += "\n";
            }

            // Add 4 spaces of indentation for method definitions
            documented += QString(indent, ' ') + signature + "\n";

            insideDefinition = true;
            expectDocstring = true;
            headerIndent = indent;
            continue;
        }

        if (expectDocstring && trimmed.startsWith("\"\"\"")) {
            QStringList docLines;

            QString temp = trimmed.mid(3);

            if (temp.endsWith("\"\"\"")) {
                temp.chop(3);
                docLines << temp.trimmed();
            } else {
                docLines << temp.trimmed();
                while (!in.atEnd()) {
                    QString nextLine = in.readLine();
                    QString nextTrimmed = nextLine.trimmed();
                    if (nextTrimmed.endsWith("\"\"\"")) {
                        docLines << nextTrimmed.left(nextTrimmed.length() - 3).trimmed();
                        break;
                    } else {
                        docLines << nextTrimmed;
                    }
                }
            }

            // Process docstring
            for (const QString& docLine : docLines) {
                if (docLine.isEmpty()) {
                    documented += "#\n"; // Empty line
                    insideParametersOrReturns = false;
                    continue;
                }

                QString lowered = docLine.toLower();
                if (lowered.startsWith("parameters:") ||
                    lowered.startsWith("returns:") ||
                    lowered.startsWith("raises:")) {
                    documented += "#" + QString(headerIndent, ' ') + docLine + "\n";
                    insideParametersOrReturns = true;
                    continue;
                }

                if (insideParametersOrReturns) {
                    documented += "#" + QString(headerIndent + 4, ' ') + docLine + "\n";
                } else {
                    documented += "#" + QString(headerIndent, ' ') + docLine + "\n";
                }
            }

            expectDocstring = false;
            continue;
        }

        if (insideDefinition) {
            if (trimmed.isEmpty()) {
                documented += "\n";
                insideDefinition = false;
            }
            continue;
        }
    }

    return documented;
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
    QStringList mediaPath;
    QString currentVideo = "";

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
    // create help / media tab header
    TabsWidget *rightSideTabs  = new TabsWidget(false);
    rightSideTabs->setTabsFont(sftel_bold);
    rightSideTabs->addTab("M E D I A", "", false);
    rightSideTabs->addTab("C O M M A N D S", "", false);
    rightSideTabs->selectTab(0);


    // create video headers
    TabsWidget *mediaHeader = new TabsWidget();
    mediaHeader->setTabsFont(dotim5);
    mediaHeader->setLabelFont(dotim7);
    importMedia("render.mp4", mediaPath, codePanel, mediaHeader);
    mediaHeader->getTab(0)->closeable = false;


    // create media panel
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
       "⏵", "⏸",
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

    // create media container
    QWidget *mediaContainer = new QWidget();
    QVBoxLayout *mediaContainerLayout = new QVBoxLayout(mediaContainer);
    mediaContainerLayout->addWidget(outputDisplay);
    mediaContainerLayout->addWidget(mediaHeader);
    mediaContainerLayout->addWidget(mediaPanel);
    mediaContainerLayout->addWidget(mediaControlsWidget);
    mediaContainerLayout->addWidget(outputDisplay);

    // create command tabs header
    TabsWidget *commandsHeader = new TabsWidget();
    commandsHeader->setTabsFont(dotim5);
    commandsHeader->setLabelFont(dotim7);
    commandsHeader->addTab("Filters", "Filters", false);
    commandsHeader->addTab("Fields", "Fields", false);
    commandsHeader->addTab("Other", "Other", false);
    commandsHeader->selectTab(0);

    // craate commands help container
    QWidget *commandsContainer = new QWidget();
    QVBoxLayout *commandsContainerLayout = new QVBoxLayout(commandsContainer);
    commandsContainerLayout->addWidget(commandsHeader);
    QPlainTextEdit *commandsText = new QPlainTextEdit();
    new PythonHighlighter(commandsText->document());
    commandsText->setFont(dotim5);
    commandsText->setReadOnly(true);
    commandsText->setLineWrapMode(QPlainTextEdit::NoWrap);

    bool docOverride = true;

    QStringList docSources = {
        "filters",
        "fields",
        "classes"
    };
    QString docsText[3];

    for (int i = 0; i < docSources.size(); i++) {
        QString docName = docSources.at(i);
        QString docPath = "documentation/" + docName + ".txt";

        QFile docFile(docPath);

        if (docFile.exists() && docOverride == false) {
            // Read from the existing documentation .txt file
            if (docFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&docFile);
                docsText[i] = in.readAll();
                docFile.close();
            } else {
                // Fall back if reading fails
                docsText[i] = documentPython(":/resources/code/" + docName + ".py");
            }
        } else {
            // Generate documentation
            docsText[i] = documentPython(":/resources/code/" + docName + ".py");

            // Save the generated documentation
            if (docFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&docFile);
                out << docsText[i];
                docFile.close();
            }
        }
    }

    commandsText->setPlainText(docsText[0]);
    commandsContainerLayout->addWidget(commandsText);

    // create tabbed right side widgets
    QStackedWidget *rightStack = new QStackedWidget();
    rightStack->addWidget(mediaContainer);
    rightStack->addWidget(commandsContainer);

    // put right panel together
    rightLayout->addWidget(rightSideTabs);
    rightLayout->addWidget(rightStack);

    QWidget *leftWidget = new QWidget;
    QWidget *rightWidget = new QWidget;
    leftWidget->setLayout(leftLayout);
    rightWidget->setLayout(rightLayout);

    // Add the panels to the layout
    mainLayout->addWidget(leftWidget, 1);
    mainLayout->addWidget(rightWidget, 1);

    // set font
    codePanel->setFont(dotim5);
    outputDisplay->setFont(dotrice);

    // generote neovere.py file
    remakeNeoverePy(mediaPath);

    // open default file
    createNewFile(codePanel, outputDisplay, mediaHeader);

    // --------------- CONNECTIONS ---------------------

    // Make run button run python code
    QObject::connect(runButton, &QPushButton::clicked, [mediaHeader, outputDisplay, codePanel, mediaPanel]() {
        QString code = codePanel->toPlainText();
        compileCode(code, outputDisplay, mediaHeader, mediaPanel);
    });

    // Make the import button import a media file
    QObject::connect(uploadButton, &QPushButton::clicked, [&window, &mediaPath, outputDisplay, mediaPanel, mediaHeader]() {
        QString fileName = QFileDialog::getOpenFileName(&window, "Open File", "", "Media Files (*.mp4 *.mp3);;All Files (*)");
        if (!fileName.isEmpty()) {
            importMedia(fileName, mediaPath, outputDisplay, mediaHeader);
        }
    });

    // Make the open button open a nv file
    QObject::connect(openButton, &QPushButton::clicked, [codePanel, outputDisplay, &mediaPath, mediaHeader]() {
        QStringList programs;
        QStringList videos;

        if (openProjectFromFile(&programs, &videos, outputDisplay)) {
            if (!programs.isEmpty()) {
                codePanel->setPlainText(programs.at(0));
            }
            removeImportedTabs(mediaHeader);
            for (const QString &video : videos) {
                importMedia(video, mediaPath, outputDisplay, mediaHeader);
            }
        }
    });


    QObject::connect(mediaHeader, &TabsWidget::tabRemoved, [mediaHeader, &mediaPath, mediaPanel, videoSlider](int index) {
        removeVideo(mediaHeader->getTab(index)->getData(), mediaPath);
        mediaPanel->setVideo("");
        videoSlider->updateTimeStamp(0,0);
    });

    QObject::connect(mediaHeader, &TabsWidget::tabSelected, [mediaHeader, &currentVideo, mediaPanel, videoSlider, pauseButton](int index) {
        currentVideo = mediaHeader->getTab(index)->getData();
        mediaPanel->setVideo(currentVideo);
        pauseButton->setState(true);
    });


    // make the new button open a new default file
    QObject::connect(newButton, &QPushButton::clicked, [codePanel, outputDisplay, mediaHeader]() {
        QMessageBox confirmationDialog;
        confirmationDialog.setWindowTitle("Confirm New Program");
        confirmationDialog.setText("Are you sure you want to create a new program? Unsaved changes will be lost.");
        confirmationDialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        confirmationDialog.setDefaultButton(QMessageBox::No);

        if (confirmationDialog.exec() == QMessageBox::Yes) {
            createNewFile(codePanel, outputDisplay, mediaHeader);
        }
    });


    // Make save button download file
    QObject::connect(saveButton, &QPushButton::clicked, [codePanel, outputDisplay, &mediaPath]() {
        QString programs[] = {codePanel->toPlainText()};

        saveProjectToFile(programs, mediaPath, outputDisplay);
    });

    // change from media to command tabs
    QObject::connect(rightSideTabs, &TabsWidget::tabSelected, [rightStack](int index) {
        rightStack->setCurrentIndex(index);
    });

    // change documentation based on selected tab
    QObject::connect(commandsHeader, &TabsWidget::tabSelected, [docsText, commandsText](int index) {
        commandsText->setPlainText(docsText[index]);
    });

    // ---------- FINAL SETUP ---------------

    // Set the layout for the window
    window.setLayout(mainLayout);

    // Show the window
    window.show();

    return app.exec();
}


