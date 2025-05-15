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
#include <QInputDialog>

#include <QUrl>
#include <opencv2/opencv.hpp>
#include <vector>

#include "BoolStateButton.h"
#include "MaintainFrame.h"
#include "MediaFrame.h"
#include "VideoSlider.h"
#include <QTemporaryFile>

#include "ButtonTextEdit.h"
#include "PythonHighlighter.h"
#include "PythonCodeEditor.h"
#include "SearchTextEdit.h"
#include "AiTextBoxWrapper.h"

#include "TabsWidget.h"

QProcess* process = nullptr;

// convert a plural string to a singular string
QString pluralToSingular(const QString& pluralWord) {
    if (pluralWord.isEmpty()) {
        return pluralWord;
    }

    // Check for "es" ending first (longer suffix takes precedence)
    if (pluralWord.endsWith("es") && pluralWord.length() > 2) {
        // Special case for words ending with "ies" -> "y"
        if (pluralWord.endsWith("ies") && pluralWord.length() > 3) {
            QString singular = pluralWord.left(pluralWord.length() - 3);
            return singular + "y";
        }
        // Remove "es" for other cases
        return pluralWord.left(pluralWord.length() - 2);
    }
    // Check for simple "s" ending
    else if (pluralWord.endsWith("s") && pluralWord.length() > 1) {
        return pluralWord.left(pluralWord.length() - 1);
    }

    // Return original if no plural suffix found
    return pluralWord;
}

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

QString openProjectFromFile(QStringList* programs, QStringList* mediaPaths,  QPlainTextEdit *outputText) {
    QString fileName = QFileDialog::getOpenFileName(nullptr, "Open File", "", "NEOVERE Files (*.nv);;All Files (*)");
    if (!fileName.isEmpty()) {
        QFile file(fileName);
        outputText->appendPlainText("opened '"+fileName+"'");

        // Open the file for reading
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            outputText->setPlainText("Failed to open the file.");
            return "";
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
        return "";
    }
    return fileName;
}

QString saveProjectToFile(QString programs[], QStringList mediaPaths, QPlainTextEdit *outputText, QString saveName = "") {
    // Get the file name and location from the user
    QString fileName = saveName;
    if (fileName.isEmpty()) {
        fileName = QFileDialog::getSaveFileName(
        nullptr, "Save File", "nullnomen.nv", "NEOVERE Files (*.nv);;All Files (*)");

        if (fileName.isEmpty()) {
            return ""; // User canceled the dialog
        }
    }

    // Open the file for writing
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        outputText->setPlainText("Error. Cannot save file: " + file.errorString());
        return "";
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

    outputText->appendPlainText("File "+ fileName +" saved successfully");

    return fileName;
}

QString combinePythonFiles(const QStringList &fileNames) {
    QString combinedCode;
    QDir resourcesDir(":/resources/code/");
    QDir classesDir("classes/");

    for (const QString &fileName : fileNames) {
        // First add the main file
        QString filePath = ":/resources/code/" + fileName + ".py";
        QFile file(filePath);

        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            combinedCode += in.readAll() + "\n\n";
            file.close();
        } else {
            qDebug() << "Failed to open file:" << filePath;
        }
        // Check if there's a matching directory in classes/
        QString dirPath = "classes/" + fileName;
        if (classesDir.exists(fileName)) {
            QDir subDir(dirPath);
            QStringList pyFiles = subDir.entryList(QStringList() << "*.py", QDir::Files);

            for (const QString &pyFile : pyFiles) {
                QString subFilePath = dirPath + "/" + pyFile;
                QFile subFile(subFilePath);

                if (subFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    QTextStream subIn(&subFile);
                    combinedCode += subIn.readAll() + "\n\n";
                    subFile.close();
                } else {
                    qDebug() << "Failed to open sub-file:" << subFilePath;
                }
            }
        }
    }

    return combinedCode;
}

QString readFromFile(const QString& fileName) {
    QFile file(fileName);
    if (!file.exists()) {
        return "";
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return "";
    }

    QString contents = file.readAll();
    file.close();
    return contents;
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

    QString openaiKey = readFromFile("openai_key.txt");

    fileString.replace("%$#path#$%", allPaths);
    fileString.replace("[%$#arial#$%]", exportFontResourceToFile(":/resources/fonts/arial-bold.ttf"));
    fileString.replace("api_key = \"\" #[%$# #$%]", "api_key = \"" + openaiKey +"\"");
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
    codePanel->setPlainText(readFromFile(":/resources/code/default_project.py"));
    removeImportedTabs(mediaHeader);
    outputText->appendPlainText("New file created");
}

// Example function to process individual Python files
void processPythonFile(const QString &filePath) {
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&file);
        QString content = in.readAll();
        file.close();

        // Do something with the file content
        // For example, extract class names, analyze code, etc.
    } else {
        qWarning() << "Could not open file:" << filePath;
    }
}

void processPythonFilesFlat(QString path) {
    QDir classesDir(path);
    classesDir.setNameFilters(QStringList() << "*.py");
    classesDir.setFilter(QDir::Files);

    QStringList pythonFiles = classesDir.entryList();

    for (const QString &fileName : pythonFiles) {
        QString filePath = classesDir.absoluteFilePath(fileName);
        qDebug() << "Found Python file:" << filePath;
        processPythonFile(filePath);
    }
}

QString documentPython(QString docName) {
    QStringList paths = QStringList();
    paths.append(":/resources/code/" + docName + ".py");
    // add file paths for custom classes
    QDir classesDir("classes/"+docName);
    classesDir.setNameFilters(QStringList() << "*.py");
    classesDir.setFilter(QDir::Files);

    QStringList pythonFiles = classesDir.entryList();

    for (const QString &fileName : pythonFiles) {
        QString filePath = classesDir.absoluteFilePath(fileName);
        paths.append(filePath);
    }

    QString totalDocument = "";

    for (int i = 0; i < paths.length(); i++) {
        QFile file(paths.at(i));

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return "Error: Could not open file at " + paths.at(i);
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
                    documented += QString(0, ' ');
                    if (i == 0) {
                        documented += "* ";
                    }
                    documented += "class " + className + baseClasses + ":\n";
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
        totalDocument += documented;
    }

    return totalDocument;
}

bool saveClass(const QString &category, const QString &content, QWidget *parent = nullptr) {
    // Extract class name from content
    QRegularExpression classRegex("class\\s+(\\w+)");
    QRegularExpressionMatch match = classRegex.match(content);

    if (!match.hasMatch()) {
        QMessageBox::warning(parent,
                           "Class Not Found",
                           "No class definition found in the provided content.\n"
                           "Please ensure your content contains a Python class definition.");
        return false;
    }

    QString className = match.captured(1);

    // Create directory structure if it doesn't exist
    QDir dir;
    QString path = QString("classes/%1").arg(category);
    if (!dir.exists(path)) {
        if (!dir.mkpath(path)) {
            QMessageBox::critical(parent,
                                "Directory Creation Failed",
                                QString("Failed to create directory: %1").arg(path));
            return false;
        }
    }

    // Create and write to file
    QString filePath = QString("%1/%2.py").arg(path).arg(className);
    QFile file(filePath);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(parent,
                            "File Error",
                            QString("Failed to open file for writing:\n%1\nError: %2")
                            .arg(filePath)
                            .arg(file.errorString()));
        return false;
    }

    QTextStream out(&file);
    out << content;
    file.close();

    QMessageBox::information(parent,
                           "Save Successful",
                           QString("Successfully saved class %1 to:\n%2")
                           .arg(className)
                           .arg(filePath));
    return true;
}

void updateDocumentationTextBox(const QString &classCatagory,
                                ButtonTextEdit *docsTextBoxes[], const QStringList &docSources,
                                QStackedWidget *rightStack, QLineEdit *editClassLine, PythonCodeEditor *editClassEditor, QStringList &mediaPath) {
    for (int i = 0; i < docSources.length(); i++) {
        if (docSources.at(i) == classCatagory) {
            QString documentedCode = documentPython(classCatagory);
            docsTextBoxes[i]->setPlainText(documentedCode);
            docsTextBoxes[i]->removeButtons();
            QStringList docsTextLines = documentedCode.split("\n");
            for (int j = 0; j < docsTextLines.length(); j++) {
                const QString& line = docsTextLines.at(j);
                if (line.startsWith("class ")) {
                    docsTextBoxes[i]->addButton(j+1, "[EDIT]", ButtonTextEdit::Right, 75);
                    docsTextBoxes[i]->addButton(j+1, "[DEL]", ButtonTextEdit::Right, 10);
                }
            }
            QString singularClassName = pluralToSingular(docSources[i]).toUpper();
            QObject::connect(docsTextBoxes[i], &ButtonTextEdit::buttonClicked,
            [rightStack, editClassLine, singularClassName, editClassEditor, classCatagory,
             docsTextBox = docsTextBoxes[i], docsTextBoxes, docSources, &mediaPath](int lineNumber, const QString& text) {

                QStringList lines = docsTextBox->toPlainText().split('\n');
                QString className;

                if (lineNumber >= 1 && lineNumber <= lines.size()) {
                    QString line = lines[lineNumber - 1].trimmed();
                    QRegularExpression classRegex("^class\\s+(\\w+)");
                    QRegularExpressionMatch match = classRegex.match(line);
                    if (match.hasMatch()) {
                        className = match.captured(1);
                    }
                }

                if (className.isEmpty())
                    return;

                QString filePath = "classes/" + classCatagory + "/" + className + ".py";
                QString diskPath = QDir::currentPath() + "/" + filePath;

                if (text == "[EDIT]") {
                    rightStack->setCurrentIndex(2);
                    editClassLine->setText(singularClassName);
                    editClassEditor->setPlainText(readFromFile(filePath));
                } else if (text == "[DEL]") {
                    QMessageBox::StandardButton reply;
                    reply = QMessageBox::question(nullptr, "Delete Class",
                                                  QString("Are you sure you want to delete class '%1'?").arg(className),
                                                  QMessageBox::Yes | QMessageBox::No);
                    if (reply == QMessageBox::Yes) {
                        if (QFile::exists(diskPath)) {
                            if (!QFile::remove(diskPath)) {
                                QMessageBox::warning(nullptr, "Delete Failed", "Could not delete the class file.");
                                qWarning() << "Failed to delete:" << diskPath;
                            } else {
                                updateDocumentationTextBox(classCatagory, docsTextBoxes,
                                    docSources, rightStack, editClassLine, editClassEditor, mediaPath);
                                remakeNeoverePy(mediaPath);
                            }
                        } else {
                            QMessageBox::warning(nullptr, "File Not Found", "The class file does not exist.");
                            qWarning() << "File does not exist:" << diskPath;
                        }
                    }
                }
            });
        }
    }
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
    QString currentFile = "";
    QString openaiKey;
    QFile keyFile("openai_key.txt");
    if (keyFile.exists() && keyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&keyFile);
        openaiKey = in.readLine().trimmed(); // Read and trim the key
        keyFile.close();
    } else {
        openaiKey = "";
    }


    // Main window widget
    QWidget window;
    window.setWindowTitle("NEOVERE");
    window.resize(1200, 560);

    // Color Palette
    QPalette palette = window.palette();
    QColor nvWhite = QColor(245, 245, 255);
    QColor nvMid = QColor(225, 225, 240);
    QColor nvPurple = QColor(195, 175, 215);
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
    QPushButton *aiButton = new QPushButton("A I");
    QPushButton *exportButton = new QPushButton("E X P O R T");
    openButton->setFont(sftel_bold);
    newButton->setFont(sftel_bold);
    saveButton->setFont(sftel_bold);
    aiButton->setFont(sftel_bold);
    exportButton->setFont(sftel_bold);

    QHBoxLayout *topButtonLayout = new QHBoxLayout();
    topButtonLayout->addWidget(openButton);
    topButtonLayout->addWidget(newButton);
    topButtonLayout->addWidget(saveButton);
    topButtonLayout->addWidget(aiButton);
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
    PythonCodeEditor *codePanel = new PythonCodeEditor;
    codePanel->setSearchBarPadding(150, 75);
    codePanel->setColor(nvPurple);
    codePanel->setEditorFont(dotim5);
    codePanel->setPlaceholderText("INPUT"); // Set placeholder text
    codePanel->setLineWrapMode(QPlainTextEdit::NoWrap);
    codePanel->setFrameStyle(QFrame::Box | QFrame::Sunken);

    new PythonHighlighter(codePanel->document());

    AiTextBoxWrapper * aiCodePanel = new AiTextBoxWrapper(codePanel, openaiKey);
    aiCodePanel->setColor(nvPurple);
    aiCodePanel->setFont(dotim5);

    // Create the left panel
    leftLayout->addWidget(topButtonWidget);
    leftLayout->addWidget(aiCodePanel);
    leftLayout->addWidget(bottomButtonWidget);

    // Create the right panel
    QStackedWidget *rightStack = new QStackedWidget();
    // create help / media tab header
    TabsWidget *rightSideTabs  = new TabsWidget(false);
    rightSideTabs->setColor(nvMid);
    rightSideTabs->setTabsFont(sftel_bold);
    rightSideTabs->addTab("M E D I A", "", false);
    rightSideTabs->addTab("C O M M A N D S", "", false);
    rightSideTabs->selectTab(0);


    // create video headers
    TabsWidget *mediaHeader = new TabsWidget();
    mediaHeader->setColor(nvMid);
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
    videoSlider->setColor(nvMid);

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

    // create container for editing classes
    QWidget *editClassContainer = new QWidget();
    QVBoxLayout *editClassContainerLayout = new QVBoxLayout(editClassContainer);
    QLineEdit *editClassLine = new QLineEdit();
    editClassLine->setReadOnly(true);
    editClassLine->setStyleSheet("QLineEdit{ background-color: none;  color: black; border: 0px;}");
    editClassLine->setFont(dotim7);
    editClassLine->setAlignment(Qt::AlignCenter);
    editClassContainerLayout->addWidget(editClassLine);
    PythonCodeEditor *editClassEditor = new PythonCodeEditor();
    editClassEditor->setSearchBarPadding(150, 75);
    editClassEditor->setColor(nvPurple);
    editClassEditor->setEditorFont(dotim5);
    editClassEditor->setPlaceholderText("INPUT"); // Set placeholder text
    editClassEditor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editClassEditor->setFrameStyle(QFrame::Box | QFrame::Sunken);
    new PythonHighlighter(editClassEditor->document());

    AiTextBoxWrapper * aiClassEditor = new AiTextBoxWrapper(editClassEditor, openaiKey);
    aiClassEditor->setColor(nvPurple);
    aiClassEditor->setFont(dotim5);
    editClassContainerLayout->addWidget(aiClassEditor);

    // create cancel and done buttons for class editor
    QWidget *editClassButtons = new QWidget();
    QHBoxLayout *editClassButtonsLayout = new QHBoxLayout(editClassButtons);
    QPushButton *cancelEditClassButton = new QPushButton("CANCEL");
    cancelEditClassButton->setFont(sftel_bold);
    QPushButton *doneEditClassButton = new QPushButton("DONE");
    doneEditClassButton->setFont(sftel_bold);
    editClassButtonsLayout->addWidget(cancelEditClassButton);
    editClassButtonsLayout->addWidget(doneEditClassButton);
    editClassContainerLayout->addWidget(editClassButtons);

    // create command tabs header
    TabsWidget *commandsHeader = new TabsWidget();
    commandsHeader->setColor(nvMid);
    commandsHeader->setTabsFont(dotim5);
    commandsHeader->setLabelFont(dotim7);
    commandsHeader->addTab("Filters", "Filters", false);
    commandsHeader->addTab("Fields", "Fields", false);
    commandsHeader->addTab("Other", "Classes", false);
    commandsHeader->selectTab(0);

    // craate commands help container
    QWidget *commandsContainer = new QWidget();
    QVBoxLayout *commandsContainerLayout = new QVBoxLayout(commandsContainer);
    commandsContainerLayout->addWidget(commandsHeader);

    bool docOverride = true;

    QStackedWidget *docStack = new QStackedWidget();

    QStringList docSources = {
        "filters",
        "fields",
        "classes"
    };
    QString docsText[docSources.size()];
    ButtonTextEdit * docsTextBoxes[docSources.size()];

    for (int i = 0; i < docSources.size(); i++) {
        QString docName = docSources.at(i);
        QString docPath = "documentation/" + docName + ".txt";

        QFile docFile(docPath);

        QWidget *docsPage = new QWidget();
        QVBoxLayout *docsPageLayout = new QVBoxLayout(docsPage);

        if (docFile.exists() && docOverride == false) {
            // Read from the existing documentation .txt file
            if (docFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&docFile);
                docsText[i] = in.readAll();
                docFile.close();
            } else {
                // Fall back if reading fails
                docsText[i] = documentPython(docName);
            }
        } else {
            // Generate documentation
            docsText[i] = documentPython(docName);

            // Save the generated documentation
            if (docFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&docFile);
                out << docsText[i];
                docFile.close();
            }
        }
        docsTextBoxes[i] = new ButtonTextEdit();
        docsTextBoxes[i]->setPlainText(docsText[i]);
        docsTextBoxes[i]->setSearchBarPadding(150, 75);
        updateDocumentationTextBox(docSources.at(i), docsTextBoxes,
            docSources, rightStack, editClassLine, editClassEditor, mediaPath);
        docsTextBoxes[i]->setColor(nvPurple);
        new PythonHighlighter(docsTextBoxes[i]->document());
        docsTextBoxes[i]->setEditorFont(dotim5);
        docsTextBoxes[i]->setReadOnly(true);
        docsTextBoxes[i]->setLineWrapMode(QPlainTextEdit::NoWrap);

        docsPageLayout->addWidget(docsTextBoxes[i]);

        QString singularClassName = pluralToSingular(docSources[i]).toUpper();

        QPushButton *newClassButton = new QPushButton("NEW "+singularClassName);
        newClassButton->setFont(sftel_bold);
        docsPageLayout->addWidget(newClassButton);

        docStack->addWidget(docsPage);

        // make the new class button make a new class
        QObject::connect(newClassButton, &QPushButton::clicked,
            [rightStack, editClassLine, singularClassName, editClassEditor]() {
            rightStack->setCurrentIndex(2);
            editClassLine->setText(singularClassName);
            editClassEditor->setPlainText(readFromFile(":/resources/code/default_"+singularClassName.toLower()+".py"));
        });
    }

    commandsContainerLayout->addWidget(docStack);

    // create tabbed right side widgets
    rightStack->addWidget(mediaContainer);
    rightStack->addWidget(commandsContainer);
    rightStack->addWidget(editClassContainer);

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

    // create shortcut for saving0
    QShortcut *saveShortcut = new QShortcut(QKeySequence::Save, &window);


    // --------------- CONNECTIONS ---------------------

    // map save shortcut
    QObject::connect(saveShortcut, &QShortcut::activated, [codePanel, outputDisplay, &mediaPath, &currentFile]() {
        QString programs[] = {codePanel->toPlainText()};

        if (!currentFile.isEmpty()) {
            // Save to current file if one exists
            saveProjectToFile(programs, mediaPath, outputDisplay, currentFile);
        } else {
            // Otherwise prompt for new file name
            currentFile = saveProjectToFile(programs, mediaPath, outputDisplay);
        }
    });

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
    QObject::connect(openButton, &QPushButton::clicked, [codePanel, outputDisplay, &mediaPath, mediaHeader, &currentFile]() {
        QStringList programs;
        QStringList videos;

        currentFile = openProjectFromFile(&programs, &videos, outputDisplay);
        if (!currentFile.isEmpty()) {
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
    QObject::connect(saveButton, &QPushButton::clicked, [codePanel, outputDisplay, &mediaPath, &currentFile]() {
        QString programs[] = {codePanel->toPlainText()};

        currentFile = saveProjectToFile(programs, mediaPath, outputDisplay);
    });

    QObject::connect(aiButton, &QPushButton::clicked, [&openaiKey]() {
        // Load current key from file if it exists
        QString currentKey;
        QFile file("openai_key.txt");
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&file);
            currentKey = in.readLine().trimmed();
            file.close();
        }

        bool ok;
        QString key = QInputDialog::getText(nullptr,
                                            "Set OpenAI API Key",
                                            "Enter your OpenAI API key:",
                                            QLineEdit::Normal,
                                            currentKey,  // pre-fill with current key
                                            &ok);

        if (ok && !key.isEmpty()) {
            openaiKey = key;

            QFile outFile("openai_key.txt");
            if (outFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&outFile);
                out << openaiKey;
                outFile.close();
                QMessageBox::information(nullptr, "Success", "API key saved successfully.");
            } else {
                QMessageBox::critical(nullptr, "Error", "Failed to save API key.");
            }
        }
    });

    // change from media to command tabs
    QObject::connect(rightSideTabs, &TabsWidget::tabSelected, [rightStack](int index) {
        rightStack->setCurrentIndex(index);
    });

    // change documentation based on selected tab
    QObject::connect(commandsHeader, &TabsWidget::tabSelected, [docStack](int index) {
        docStack->setCurrentIndex(index);
    });

    // connect cancel button on class editor to go back to documentation
    QObject::connect(cancelEditClassButton, &QPushButton::clicked, [rightStack]() {
        rightStack->setCurrentIndex(1);
    });

    // connect done button on class editor to save
    QObject::connect(doneEditClassButton, &QPushButton::clicked,
        [commandsHeader, editClassEditor, rightStack,
            docSources, &docsTextBoxes, editClassLine, &mediaPath]() {
        QString classCatagory = commandsHeader->selectedTab()->getData().toLower();
        saveClass(classCatagory,
            editClassEditor->toPlainText());
        updateDocumentationTextBox(classCatagory, docsTextBoxes, docSources,
            rightStack, editClassLine, editClassEditor, mediaPath);

        rightStack->setCurrentIndex(1);
        remakeNeoverePy(mediaPath);
    });


    // ---------- FINAL SETUP ---------------

    // Set the layout for the window
    window.setLayout(mainLayout);

    // Show the window
    window.show();

    return app.exec();
}


