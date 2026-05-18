#include <iostream>
#include <fstream>
#include <regex>
#include <functional>
#include <signal.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

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
#include <QComboBox>
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
#include <QLabel>
#include <QTimer>
#include <QMessageBox>
#include <QInputDialog>
#include <QCheckBox>

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
#include "FrameBufferReader.h"
#include "PreviewWidget.h"
#include "PlaybackController.h"

QProcess* process = nullptr;
QProcess* pythonWorker = nullptr;
QByteArray workerOutBuffer;
std::function<void(bool)> workerOnFinished;
FrameBufferReader* g_frameBuffer = nullptr;       // shared-memory frame buffer reader (frame-buffer mode)
TabsWidget* g_mediaHeader = nullptr;              // for updating the preview tab's "[updating]" indicator

// Updates the title label below the media tabs to show "[updating]" while a preview
// render is in flight, but only when the user is actually viewing the preview tab.
static void updatePreviewTabLabel(bool updating) {
    if (!g_mediaHeader) return;
    TabButton* sel = g_mediaHeader->selectedTab();
    if (!sel) return;
    QString base = sel->text();
    // Strip any trailing " [updating]" so we don't accumulate.
    if (base.endsWith(" [updating]")) base.chop(QString(" [updating]").size());
    if (updating && sel->getData() == "preview.mp4") {
        g_mediaHeader->setLabelText(base + " [updating]");
    } else {
        g_mediaHeader->setLabelText(base);
    }
}
bool isPreviewRender = false;            // current render is an auto-preview (suppress output, write preview.mp4)
bool previewInFlight = false;            // an auto-preview is currently being rendered by the worker
bool previewPending = false;             // user typed during a preview render → re-render after current finishes
bool runInFlight = false;                // worker is currently running a Run-button script (output should show)
bool runQueued = false;                  // user clicked Run; will dispatch once any current preview finishes
QTimer* previewDebounceTimer = nullptr;
static const char* WORKER_DONE_SENTINEL = "<<<NEO_DONE>>>";

// Cross-platform interrupt: ask the Python worker to abort the current render with
// KeyboardInterrupt so the next command (Run, latest preview, export) can take over.
// POSIX uses SIGINT. Windows uses CTRL+BREAK delivered via the child's console group;
// the worker is started with CREATE_NEW_PROCESS_GROUP, and WORKER_SCRIPT installs a
// SIGBREAK handler that converts the event into KeyboardInterrupt.
static void sendInterruptToWorker(qint64 pid) {
    if (pid <= 0) return;
#ifdef _WIN32
    DWORD targetPid = static_cast<DWORD>(pid);
    if (AttachConsole(targetPid)) {
        SetConsoleCtrlHandler(nullptr, TRUE);
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, 0);
        FreeConsole();
        SetConsoleCtrlHandler(nullptr, FALSE);
    }
#else
    ::kill(static_cast<pid_t>(pid), SIGINT);
#endif
}

static const char* WORKER_SCRIPT = R"PYTHON(
import sys, importlib, os, signal
# On Windows, CTRL+BREAK's default handler terminates the process. Convert it to
# KeyboardInterrupt so the worker can abort the current render and continue serving.
if hasattr(signal, 'SIGBREAK'):
    try:
        signal.signal(signal.SIGBREAK, signal.default_int_handler)
    except (ValueError, OSError):
        pass
END = "<<<NEO_DONE>>>"
last_mtime = None
while True:
    header = sys.stdin.readline()
    if not header:
        break
    if not header.startswith("LEN:"):
        continue
    try:
        n = int(header[4:].strip())
    except ValueError:
        continue
    script = sys.stdin.read(n)

    # Reload neovere only when its file has actually changed since last render.
    # Otherwise reuse the imported module (preserving in-memory state like Video frame caches).
    if 'neovere' in sys.modules and last_mtime is not None:
        try:
            current_mtime = os.path.getmtime('neovere.py')
            if current_mtime > last_mtime:
                importlib.reload(sys.modules['neovere'])
        except Exception as e:
            print(f"[worker] reload check failed: {e}")

    # Reset renderer state BEFORE compiling/executing user code, so that even if the user
    # script fails with a SyntaxError (which prevents the wrapper from running), the next
    # successful render starts from a clean slate.
    if 'neovere' in sys.modules:
        try:
            sys.modules['neovere'].renderer.reset()
        except Exception:
            pass

    try:
        exec(script, {'__name__': '__main__'})
    except KeyboardInterrupt:
        # Aborted by SIGINT (e.g. Run button interrupting an auto-preview). Just continue.
        pass
    except Exception:
        import traceback
        traceback.print_exc()
    try:
        last_mtime = os.path.getmtime('neovere.py')
    except Exception:
        pass
    sys.stdout.flush()
    sys.stderr.flush()
    print(END, flush=True)
)PYTHON";

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


// Optional dx/dt overrides let callers (Run / Export Re-render) regenerate neovere.py with
// render-quality scales WITHOUT touching settings.txt. Empty strings mean "use settings.txt
// preview values".
void remakeNeoverePy(QStringList &mediaPath, const QString& dxOverride = "", const QString& dtOverride = "") {
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

    QString openaiKey = "";
    QString gpuEnabled = "False";
    QString dxValue = "1.0";
    QString dtValue = "1.0";

    QStringList settings = readFromFile("settings.txt").split("\n");

    if (settings.length() > 0) {
        openaiKey = settings.at(0);
    }
    if (settings.length() > 1) {
        gpuEnabled = settings.at(1) == "1" ? "True" : "False";
    }
    if (settings.length() > 3 && !settings.at(3).trimmed().isEmpty()) {
        dxValue = settings.at(3).trimmed();
    }
    if (settings.length() > 4 && !settings.at(4).trimmed().isEmpty()) {
        dtValue = settings.at(4).trimmed();
    }
    // Apply explicit overrides if provided.
    if (!dxOverride.isEmpty()) dxValue = dxOverride;
    if (!dtOverride.isEmpty()) dtValue = dtOverride;

        fileString.replace("%$#path#$%", allPaths);
        fileString.replace("[%$#arial#$%]", exportFontResourceToFile(":/resources/fonts/arial-bold.ttf"));
        fileString.replace("api_key = \"\" #[%$# #$%]", "api_key = \"" + openaiKey +"\"");
        fileString.replace("gpu_enabled = False #[%%# #$%]", "gpu_enabled = "+gpuEnabled);
        fileString.replace("dx = 1.0 #[%$#dx#$%]", "dx = " + dxValue);
        fileString.replace("dt = 1.0 #[%$#dt#$%]", "dt = " + dtValue);
        outFile << fileString.toStdString();
        outFile.close();
}

// Returns the path to the Python interpreter shipped inside the packaged
// build (.app on macOS, install dir on Windows), or an empty string if this
// is a dev build with no bundled python.
//
// Layouts:
//   macOS:   Neovere.app/Contents/MacOS/Neovere       (applicationDirPath)
//            Neovere.app/Contents/Resources/python/bin/python3
//   Windows: <install>/Neovere.exe                    (applicationDirPath)
//            <install>/python/python.exe
QString resolveBundledPython() {
    QString appDir = QCoreApplication::applicationDirPath();
    // macOS: codesign refuses to seal arbitrary directory trees under
    // Frameworks/, so the python tree lives under Resources/ instead.
    QString macPath = QDir::cleanPath(appDir + "/../Resources/python/bin/python3");
    if (QFile::exists(macPath)) return macPath;
    // Windows: bundled python sits next to the .exe.
    QString winPath = QDir::cleanPath(appDir + "/python/python.exe");
    if (QFile::exists(winPath)) return winPath;
    return QString();
}

// True iff we're running from a packaged build (.app / Windows install dir)
// rather than a dev build invoked from a source checkout. Detected via the
// presence of the bundled python tree.
bool isPackagedBuild() {
    return !resolveBundledPython().isEmpty();
}

// Returns the process environment used for every QProcess we spawn:
// system env + the directory containing the main exe (which holds the bundled
// ffmpeg/ffprobe) prepended to PATH. Without this, subprocesses launched from
// a Finder-opened .app or Explorer-opened .exe can't find ffmpeg.
QProcessEnvironment neovereProcessEnvironment() {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString bundledBin = QCoreApplication::applicationDirPath();
    QString existing = env.value("PATH");
    // Path separator differs by platform: ':' on Unix, ';' on Windows.
#ifdef Q_OS_WIN
    const QChar sep = ';';
#else
    const QChar sep = ':';
#endif
    if (!existing.contains(bundledBin)) {
        env.insert("PATH", bundledBin + sep + existing);
    }
    return env;
}

QString resolvePythonExecutable() {
    // 1) Explicit override in settings.txt wins
    QStringList settings = readFromFile("settings.txt").split("\n");
    if (settings.length() > 2 && !settings.at(2).trimmed().isEmpty()) {
        return settings.at(2).trimmed();
    }
    // 2) User-level venv created on first launch
#ifdef Q_OS_WIN
    QString autoVenv = QDir::homePath() + "/neovere_venv/Scripts/python.exe";
#else
    QString autoVenv = QDir::homePath() + "/neovere_venv/bin/python3";
#endif
    if (QFile::exists(autoVenv)) return autoVenv;
    // 3) Python bundled inside the .app (only present in packaged builds)
    QString bundled = resolveBundledPython();
    if (!bundled.isEmpty()) return bundled;
    // 4) Last-resort PATH lookup (dev builds with system python)
#ifdef Q_OS_WIN
    return QStringLiteral("python");
#else
    return QStringLiteral("python3");
#endif
}

void startPythonWorker(QPlainTextEdit* outputDisplay, TabsWidget* mediaHeader, MediaFrame* mediaPanel) {
    if (pythonWorker) {
        if (pythonWorker->state() == QProcess::Running) return;
        delete pythonWorker;
        pythonWorker = nullptr;
    }
    workerOutBuffer.clear();

    pythonWorker = new QProcess();
    pythonWorker->setProcessEnvironment(neovereProcessEnvironment());
#ifdef _WIN32
    // Put the worker in its own process group so we can target it with
    // GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, ...) without affecting the GUI.
    pythonWorker->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) {
            args->flags |= CREATE_NEW_PROCESS_GROUP;
        });
#endif

    QObject::connect(pythonWorker, &QProcess::readyReadStandardOutput, [outputDisplay, mediaHeader, mediaPanel]() {
        workerOutBuffer.append(pythonWorker->readAllStandardOutput());
        int idx;
        while ((idx = workerOutBuffer.indexOf('\n')) != -1) {
            QByteArray line = workerOutBuffer.left(idx);
            workerOutBuffer.remove(0, idx + 1);
            // Trim trailing \r if present
            if (!line.isEmpty() && line.endsWith('\r')) line.chop(1);

            if (line == "<<<NEO_BEGIN_PREVIEW>>>") {
                isPreviewRender = true;
            } else if (line == "<<<NEO_BEGIN_RUN>>>") {
                isPreviewRender = false;
            } else if (line == "<<<NEO_FRAMES_READY>>>") {
                // Auto-preview wrote frames into shared memory. Switch the preview tab
                // to FrameBuffer mode and refresh.
                if (!g_frameBuffer) {
                    // Must match neovere.py's __fb_path: platform temp dir + filename.
                    g_frameBuffer = new FrameBufferReader(QDir::tempPath() + "/neovere_frames.bin");
                }
                if (!g_frameBuffer->isOpen()) {
                    g_frameBuffer->open();
                }
                // Global flag so we don't asynchronously crash the position on every keystroke
                    static bool fbAudioSet = false;

                    if (g_frameBuffer->isOpen()) {
                        g_frameBuffer->refreshHeader();
                        mediaPanel->setFrameBuffer(g_frameBuffer);

                        // Let <<<NEO_AUDIO_READY>>> handle the active reload.
                        // Only seed this on the very first playback to prevent early resets.
                        if (!fbAudioSet && QFile::exists("cached_preview_audio.aac")) {
                            mediaPanel->setFrameBufferAudio(QFileInfo("cached_preview_audio.aac").absoluteFilePath());
                            fbAudioSet = true;
                        }
                        if (mediaPanel->fbController()) {
                            mediaPanel->fbController()->refresh();
                        }
                    mediaPanel->switchToFrameBuffer();
                }
            } else if (line == "<<<NEO_AUDIO_READY>>>") {
                // Phase 2 just rewrote cached_preview_audio.aac. Force-reload it so the
                // user hears THIS render's audio, not the previous one Qt has cached.
                if (mediaPanel->currentMode() == MediaFrame::Mode::FrameBuffer) {
                    mediaPanel->reloadFrameBufferAudio();
                }
            } else if (line == "<<<NEO_VIDEO_READY>>>") {
                // First-pass preview.mp4 reload (legacy path). Skip when the panel is
                // already in FrameBuffer mode — the buffer is the live source there and
                // reloading the video file would steal control + pause the controller.
                // Also skip on Windows: reloading mid-render would re-lock preview.mp4
                // and Phase 2 (audio attach) couldn't overwrite it. NEO_DONE handles
                // the final reload.
#ifdef Q_OS_WIN
                const bool canReloadPhase1 = false;
#else
                const bool canReloadPhase1 = previewInFlight && mediaPanel->currentMode() != MediaFrame::Mode::FrameBuffer;
#endif
                if (canReloadPhase1) {
                    TabButton* sel = mediaHeader->selectedTab();
                    if (sel && sel->getData() == "preview.mp4") {
                        qint64 savedPos = mediaPanel->getPlayer()->position();
                        bool wasPlaying = mediaPanel->getPlayer()->playbackState() == QMediaPlayer::PlayingState;
                        for (int i = 0; i < mediaHeader->tabCount(); ++i) {
                            if (mediaHeader->getTab(i)->getData() == "preview.mp4") {
                                mediaHeader->selectTab(i);
                                break;
                            }
                        }
                        mediaPanel->reloadVideo(savedPos, wasPlaying);
                    }
                }
            } else if (line == WORKER_DONE_SENTINEL) {
                // Use previewInFlight (set at dispatch time) as the source of truth.
                // isPreviewRender depends on the BEGIN marker, which doesn't print when the
                // user's script fails with a SyntaxError (compile-time error).
                bool wasPreview = previewInFlight;
                if (wasPreview) {
                    // Run renders write preview.mp4 and need the video-file player to display it.
                    // Auto-previews wrote into the shared frame buffer (FRAMES_READY already
                    // switched the panel to FrameBuffer mode) so we leave the panel alone.
                    if (runInFlight) {
                        mediaPanel->switchToVideoFile();
                        TabButton* sel = mediaHeader->selectedTab();
                        if (sel && sel->getData() == "preview.mp4") {
                            qint64 savedPos = mediaPanel->getPlayer()->position();
                            bool wasPlaying = mediaPanel->getPlayer()->playbackState() == QMediaPlayer::PlayingState;
                            for (int i = 0; i < mediaHeader->tabCount(); ++i) {
                                if (mediaHeader->getTab(i)->getData() == "preview.mp4") {
                                    mediaHeader->selectTab(i);
                                    break;
                                }
                            }
                            mediaPanel->reloadVideo(savedPos, wasPlaying);
                        }
                    }
                    previewInFlight = false;
                    // Only clear the [updating] label if there's no follow-up queued.
                    if (!previewPending) {
                        updatePreviewTabLabel(false);
                    }
                    if (previewPending) {
                        previewPending = false;
                        if (previewDebounceTimer) previewDebounceTimer->start(0);
                    }
                } else {
                    // Explicit render via Run button: jump to render tab and reload.
                    mediaHeader->selectTab(0);
                    mediaPanel->reloadVideo();
                }
                isPreviewRender = false;
                if (workerOnFinished) {
                    auto cb = workerOnFinished;
                    workerOnFinished = nullptr;
                    cb(true);
                }
            } else if (runInFlight || (!isPreviewRender && !previewInFlight)) {
                outputDisplay->appendPlainText(QString::fromUtf8(line));
            }
        }
    });

    QObject::connect(pythonWorker, &QProcess::readyReadStandardError, [outputDisplay]() {
        QString err = QString::fromUtf8(pythonWorker->readAllStandardError());
        if (!err.isEmpty() && (runInFlight || !previewInFlight)) outputDisplay->appendPlainText(err.trimmed());
    });

    QObject::connect(pythonWorker, &QProcess::errorOccurred, [outputDisplay](QProcess::ProcessError error) {
        outputDisplay->appendPlainText(QString("Worker process error: %1").arg(error));
    });

    QObject::connect(pythonWorker, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        [outputDisplay, mediaHeader, mediaPanel](int exitCode, QProcess::ExitStatus) {
            outputDisplay->appendPlainText(QString("Worker exited (code %1). Respawning...").arg(exitCode));

            // Clear flags
            previewInFlight = false;
            isPreviewRender = false;
            updatePreviewTabLabel(false);

            // 1. Respawn FIRST so the worker is running for any subsequent callbacks
            startPythonWorker(outputDisplay, mediaHeader, mediaPanel);

            // 2. Fire any pending explicit commands (Run/Export)
            if (workerOnFinished) {
                auto cb = workerOnFinished;
                workerOnFinished = nullptr;
                cb(false);
            }

            // 3. Fire any pending debounced previews
            if (previewPending) {
                previewPending = false;
                if (previewDebounceTimer) previewDebounceTimer->start(0);
            }
        });

    pythonWorker->start(resolvePythonExecutable(), QStringList() << "-u" << "-c" << WORKER_SCRIPT);
    if (!pythonWorker->waitForStarted(5000)) {
        outputDisplay->appendPlainText("Failed to start Python worker. Check interpreter path in Settings.");
    }
}

void compileCode(QString code, QPlainTextEdit* outputDisplay, TabsWidget * mediaHeader, MediaFrame * mediaPanel) {
    if (!pythonWorker || pythonWorker->state() != QProcess::Running) {
        startPythonWorker(outputDisplay, mediaHeader, mediaPanel);
    }
    if (!pythonWorker || pythonWorker->state() != QProcess::Running) {
        outputDisplay->appendPlainText("Cannot compile: worker not running.");
        if (workerOnFinished) {
            auto cb = workerOnFinished;
            workerOnFinished = nullptr;
            cb(false);
        }
        return;
    }

    outputDisplay->appendPlainText("Compiling video ...");

    // Use .format() to avoid illegal backslash escapes inside f-strings
    QString wrapped =
        "print('<<<NEO_BEGIN_RUN>>>', flush=True)\n"
        "import neovere as _neovere_mod\n"
        "print('[info] Engine Compute: {}'.format('GPU Accelerated (CuPy)' if _neovere_mod.np.__name__ == 'cupy' else 'CPU Default (NumPy)'), flush=True)\n"
        "_neovere_mod.renderer.set_preview_mode(False)\n"
        "_neovere_mod.renderer.set_show_previews(True)\n"
        + code;

    QByteArray scriptBytes = wrapped.toUtf8();
    QByteArray header = QString("LEN:%1\n").arg(scriptBytes.size()).toUtf8();
    pythonWorker->write(header);
    pythonWorker->write(scriptBytes);
}

void dispatchPreview(QString code, QPlainTextEdit* outputDisplay, TabsWidget* mediaHeader, MediaFrame* mediaPanel) {
    if (!pythonWorker || pythonWorker->state() != QProcess::Running) {
        return;
    }
    if (previewInFlight) {
        if (!runInFlight && pythonWorker->state() == QProcess::Running) {
            pythonWorker->kill();
        }
        previewPending = true;
        return;
    }
    previewInFlight = true;
    updatePreviewTabLabel(true);

    QString useFrameBuffer = runInFlight ? "False" : "True";

    // Use .format() to avoid illegal backslash escapes inside f-strings
    QString computeLog = runInFlight ? "print('[info] Engine Compute: {}'.format('GPU Accelerated (CuPy)' if _neovere_mod.np.__name__ == 'cupy' else 'CPU Default (NumPy)'), flush=True)\n" : "";

    QString wrapped =
        "print('<<<NEO_BEGIN_PREVIEW>>>', flush=True)\n"
        "import neovere as _neovere_mod\n"
        + computeLog +
        "_neovere_mod.renderer.set_preview_mode(True)\n"
        "_neovere_mod.renderer.set_show_previews(" + QString(runInFlight ? "True" : "False") + ")\n"
        "_neovere_mod.renderer.set_use_frame_buffer(" + useFrameBuffer + ")\n"
        + code;

#ifdef Q_OS_WIN
    if (runInFlight && mediaPanel) mediaPanel->releaseFile();
#endif
    QByteArray scriptBytes = wrapped.toUtf8();
    QByteArray header = QString("LEN:%1\n").arg(scriptBytes.size()).toUtf8();
    pythonWorker->write(header);
    pythonWorker->write(scriptBytes);
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

    // When running as a packaged build (Finder-opened .app, or a Windows
    // install dir double-clicked from Explorer), CWD is "/" / "C:\Windows\System32"
    // — neither writable nor sensible for the renderer's relative paths.
    // Use ~/Documents/Neovere as the workspace. Dev builds leave CWD alone.
    if (isPackagedBuild()) {
        QString workspace = QDir::homePath() + "/Documents/Neovere";
        QDir().mkpath(workspace);
        QDir::setCurrent(workspace);
    }

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
    if (!QFile::exists("settings.txt")) {
        QFile defaultSettings("settings.txt");
        if (defaultSettings.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&defaultSettings);
            // lines: openai_key, gpu_enabled, python_path, preview_dx, preview_dt, render_dx, render_dt
            out << "\n0\n\n0.25\n0.25\n1.0\n1.0\n";
            defaultSettings.close();
        }
    }
    QStringList settings = readFromFile("settings.txt").split("\n");
    if (settings.length() > 0) {
        openaiKey = settings.at(0);
    }



    // Main window widget
    QWidget window;
    window.setWindowTitle("NEOVERE");

    auto updateTitle = [&window, &currentFile]() {
        if (currentFile.isEmpty()) {
            window.setWindowTitle("NEOVERE");
        } else {
            window.setWindowTitle("NEOVERE - " + QFileInfo(currentFile).baseName());
        }
    };
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
    QPushButton *settingsButton = new QPushButton("S E T T I N G S");
    QPushButton *openButton = new QPushButton("O P E N");
    QPushButton *newButton = new QPushButton("N E W");
    QPushButton *saveButton = new QPushButton("S A V E");
    QPushButton *exportButton = new QPushButton("E X P O R T");
    openButton->setFont(sftel_bold);
    newButton->setFont(sftel_bold);
    saveButton->setFont(sftel_bold);
    settingsButton->setFont(sftel_bold);
    exportButton->setFont(sftel_bold);

    QHBoxLayout *topButtonLayout = new QHBoxLayout();
    topButtonLayout->addWidget(settingsButton);
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
    g_mediaHeader = mediaHeader;
    mediaHeader->setColor(nvMid);
    mediaHeader->setTabsFont(dotim5);
    mediaHeader->setLabelFont(dotim7);
    // render.mp4 stays in mediaPath so user scripts can reference media["render"],
    // but we don't show a separate tab for it — the unified "preview" tab below
    // shows whatever was rendered last (auto-preview OR explicit Run).
    mediaPath.append("render.mp4");
    remakeNeoverePy(mediaPath);

    // Seed preview.mp4 from render.mp4 on first launch so the preview tab has something
    // to show before the first auto-preview render completes.
    if (!QFile::exists("preview.mp4") && QFile::exists("render.mp4")) {
        QFile::copy("render.mp4", "preview.mp4");
    }
    mediaHeader->addTab("preview", "preview.mp4", false);
    mediaHeader->getTab(0)->closeable = false;
    mediaHeader->selectTab(0);


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
    VideoSlider *videoSlider = new VideoSlider(mediaPanel, sliderSize);
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
    QObject::connect(saveShortcut, &QShortcut::activated, [codePanel, outputDisplay, &mediaPath, &currentFile, &updateTitle]() {
        QString programs[] = {codePanel->toPlainText()};

        if (!currentFile.isEmpty()) {
            // Save to current file if one exists
            saveProjectToFile(programs, mediaPath, outputDisplay, currentFile);
        } else {
            // Otherwise prompt for new file name
            currentFile = saveProjectToFile(programs, mediaPath, outputDisplay);
            updateTitle();
        }
    });

    // Make run button do a high-quality preview render: output to preview.mp4 (so the
    // unified preview tab shows it), then copy preview.mp4 to render.mp4 for export.
    auto runHighQualityPreview = [&window, codePanel, outputDisplay, mediaHeader, mediaPanel, &mediaPath]() {
        runInFlight = true;
        runQueued = false;
        outputDisplay->appendPlainText("Compiling video ...");

        // Read render dx/dt from settings.txt (lines 5-6), but DON'T modify settings.txt.
        // Pass them as overrides to remakeNeoverePy so neovere.py uses them for this render.
        QStringList settings = readFromFile("settings.txt").split("\n");
        QString renderDx = settings.size() > 5 && !settings.at(5).trimmed().isEmpty() ? settings.at(5).trimmed() : "1.0";
        QString renderDt = settings.size() > 6 && !settings.at(6).trimmed().isEmpty() ? settings.at(6).trimmed() : "1.0";
        remakeNeoverePy(mediaPath, renderDx, renderDt);

        // After render: regenerate neovere.py using settings.txt's preview values (no overrides),
        // then copy preview.mp4 → render.mp4 for export.
        workerOnFinished = [&mediaPath, outputDisplay](bool success) {
            remakeNeoverePy(mediaPath);  // back to preview dx/dt from settings.txt
            if (success && QFile::exists("preview.mp4")) {
                if (QFile::exists("render.mp4")) QFile::remove("render.mp4");
                QFile::copy("preview.mp4", "render.mp4");
                outputDisplay->appendPlainText("Render complete (saved as render.mp4)");
            }
            runInFlight = false;
        };

        QString code = codePanel->toPlainText();
        dispatchPreview(code, outputDisplay, mediaHeader, mediaPanel);
    };

    QObject::connect(runButton, &QPushButton::clicked, [runHighQualityPreview, outputDisplay]() {
        if (runQueued) return;  // Prevent spamming the button while waiting for a kill

        previewPending = false;
        if (previewDebounceTimer) previewDebounceTimer->stop();

        if (runInFlight || previewInFlight) {
            outputDisplay->appendPlainText("Aborting current render to start a new one...");
            runQueued = true;

            // Queue the new render to happen exactly when the worker finishes dying and respawns
            workerOnFinished = [runHighQualityPreview](bool) {
                runInFlight = false; // Reset the flag from the aborted run
                runHighQualityPreview();
            };

            // Force-kill the active worker. The 'finished' signal will catch this,
            // respawn the Python engine, and trigger the workerOnFinished lambda above.
            if (pythonWorker && pythonWorker->state() == QProcess::Running) {
                pythonWorker->kill();
            }
        } else {
            runHighQualityPreview();
        }
    });

    // Auto-preview: debounce keystrokes by a short window so rapid edits coalesce into one
    // render. previewPending also coalesces beyond that window — if a render is already
    // in flight when more changes come, only the LATEST state will be re-rendered next.
    auto firePreview = [codePanel, outputDisplay, mediaHeader, mediaPanel]() {
        QString code = codePanel->toPlainText();
        if (code.trimmed().isEmpty()) return;
        dispatchPreview(code, outputDisplay, mediaHeader, mediaPanel);
    };
    previewDebounceTimer = new QTimer(&window);
    previewDebounceTimer->setSingleShot(true);
    previewDebounceTimer->setInterval(150);
    QObject::connect(previewDebounceTimer, &QTimer::timeout, firePreview);
    QObject::connect(codePanel, &QPlainTextEdit::textChanged, [&]() {
        if (previewDebounceTimer) previewDebounceTimer->start();
    });

    // Make the import button import a media file
    QObject::connect(uploadButton, &QPushButton::clicked, [&window, &mediaPath, outputDisplay, mediaPanel, mediaHeader]() {
        QString fileName = QFileDialog::getOpenFileName(&window, "Open File", "", "Media Files (*.mp4 *.mp3 *.jpg *.jpeg *.png);;All Files (*)");
        if (!fileName.isEmpty()) {
            importMedia(fileName, mediaPath, outputDisplay, mediaHeader);
        }
    });

    // Make the open button open a nv file
    QObject::connect(openButton, &QPushButton::clicked, [codePanel, outputDisplay, &mediaPath, mediaHeader, &currentFile, &updateTitle]() {
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
        updateTitle();
    });


    QObject::connect(mediaHeader, &TabsWidget::tabRemoved, [mediaHeader, &mediaPath, mediaPanel, videoSlider](int index) {
        removeVideo(mediaHeader->getTab(index)->getData(), mediaPath);
        mediaPanel->setVideo("");
        videoSlider->updateTimeStamp(0,0);
    });

    QObject::connect(mediaHeader, &TabsWidget::tabSelected, [mediaHeader, &currentVideo, mediaPanel, videoSlider, pauseButton](int index) {
        currentVideo = mediaHeader->getTab(index)->getData();
        // Selecting any tab forces video-file mode so the user actually sees the chosen
        // video. The preview tab will switch back to FrameBuffer mode automatically when
        // the next auto-preview completes (FRAMES_READY marker).
        mediaPanel->switchToVideoFile();
        mediaPanel->setVideo(currentVideo);
        pauseButton->setState(true);
        // Re-apply the [updating] suffix if a preview is currently rendering.
        updatePreviewTabLabel(previewInFlight);
    });


    // make the new button open a new default file
    QObject::connect(newButton, &QPushButton::clicked, [codePanel, outputDisplay, mediaHeader, &currentFile, &updateTitle]() {
        QMessageBox confirmationDialog;
        confirmationDialog.setWindowTitle("Confirm New Program");
        confirmationDialog.setText("Are you sure you want to create a new program? Unsaved changes will be lost.");
        confirmationDialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        confirmationDialog.setDefaultButton(QMessageBox::No);

        if (confirmationDialog.exec() == QMessageBox::Yes) {
            createNewFile(codePanel, outputDisplay, mediaHeader);
            currentFile = "";
            updateTitle();
        }
    });


    // Make save button download file
    QObject::connect(saveButton, &QPushButton::clicked, [codePanel, outputDisplay, &mediaPath, &currentFile, &updateTitle]() {
        QString programs[] = {codePanel->toPlainText()};

        currentFile = saveProjectToFile(programs, mediaPath, outputDisplay);
        updateTitle();
    });

    QObject::connect(exportButton, &QPushButton::clicked, [&window, codePanel, outputDisplay, mediaHeader, mediaPanel, &mediaPath]() {
        QString destPath = QFileDialog::getSaveFileName(&window, "Export Render", "render.mp4", "MP4 Video (*.mp4)");
        if (destPath.isEmpty()) return;

        if (!QFile::exists("render.mp4")) {
            QMessageBox::warning(&window, "Export", "No render.mp4 found yet. Run a render first.");
            return;
        }

        QMessageBox choice(&window);
        choice.setWindowTitle("Export");
        choice.setText("Export the last render, or re-render first?");
        QPushButton* rerenderBtn = choice.addButton("Re-render", QMessageBox::AcceptRole);
        QPushButton* useLastBtn = choice.addButton("Use Last Render", QMessageBox::ActionRole);
        choice.addButton(QMessageBox::Cancel);
        choice.setDefaultButton(rerenderBtn);
        choice.exec();

        auto copyOver = [destPath, outputDisplay, &window]() {
            if (QFile::exists(destPath)) QFile::remove(destPath);
            if (QFile::copy("render.mp4", destPath)) {
                outputDisplay->appendPlainText("Exported to: " + destPath);
            } else {
                QMessageBox::critical(&window, "Export", "Failed to copy render.mp4 to " + destPath);
            }
        };

        if (choice.clickedButton() == useLastBtn) {
            copyOver();
        } else if (choice.clickedButton() == rerenderBtn) {
            auto startExportRender = [copyOver, outputDisplay, mediaHeader, mediaPanel, codePanel, &mediaPath]() {
                // Force full quality (dx=dt=1.0) for export via override
                remakeNeoverePy(mediaPath, "1.0", "1.0");

                workerOnFinished = [copyOver, outputDisplay, &mediaPath](bool success) {
                    remakeNeoverePy(mediaPath);
                    if (success) {
                        copyOver();
                    } else {
                        outputDisplay->appendPlainText("Re-render failed; export aborted.");
                    }
                };

                QString code = codePanel->toPlainText();
                compileCode(code, outputDisplay, mediaHeader, mediaPanel);
            };

            if (previewInFlight) {
                // Force-kill the active preview worker.
                if (!runInFlight && pythonWorker && pythonWorker->state() == QProcess::Running) {
                    pythonWorker->kill();
                }
                workerOnFinished = [startExportRender](bool) { startExportRender(); };
            } else {
                startExportRender();
            }
        }
    });

    QObject::connect(settingsButton, &QPushButton::clicked, [&openaiKey, &mediaPath]() {
        // Load current key from file if it exists
        QString currentKey;
        bool currentGpuEnabled = false;
        QString currentPythonPath;
        QString currentDx = "0.25";
        QString currentDt = "0.25";
        QString currentRenderDx = "1.0";
        QString currentRenderDt = "1.0";
        QFile settingsFile("settings.txt");
        if (settingsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&settingsFile);
            currentKey = in.readLine().trimmed();
            currentGpuEnabled = in.readLine().trimmed().toInt();
            currentPythonPath = in.readLine().trimmed();
            QString dxLine = in.readLine().trimmed();
            QString dtLine = in.readLine().trimmed();
            QString rdxLine = in.readLine().trimmed();
            QString rdtLine = in.readLine().trimmed();
            if (!dxLine.isEmpty()) currentDx = dxLine;
            if (!dtLine.isEmpty()) currentDt = dtLine;
            if (!rdxLine.isEmpty()) currentRenderDx = rdxLine;
            if (!rdtLine.isEmpty()) currentRenderDt = rdtLine;
            settingsFile.close();
        }

        // Create a dialog manually to allow more complex input
        QDialog dialog;
        dialog.setWindowTitle("Settings");

        QVBoxLayout* layout = new QVBoxLayout(&dialog);

        QLineEdit* keyInput = new QLineEdit(currentKey);
        keyInput->setEchoMode(QLineEdit::Normal);
        layout->addWidget(new QLabel("Enter your OpenAI API key:"));
        layout->addWidget(keyInput);

        QCheckBox* gpuCheckbox = new QCheckBox("Enable GPU acceleration");
        layout->addWidget(gpuCheckbox);
        gpuCheckbox->setChecked(currentGpuEnabled);

        QLineEdit* pythonPathInput = new QLineEdit(currentPythonPath);
        pythonPathInput->setPlaceholderText("python3");
        layout->addWidget(new QLabel("Python interpreter path (leave blank for auto-detect):"));
        layout->addWidget(pythonPathInput);

        layout->addWidget(new QLabel("Preview resolution scale (dx):"));
        QComboBox* dxCombo = new QComboBox();
        dxCombo->addItem("Full (1.0)", "1.0");
        dxCombo->addItem("Half (0.5)", "0.5");
        dxCombo->addItem("Quarter (0.25)", "0.25");
        dxCombo->addItem("Eighth (0.125)", "0.125");
        for (int i = 0; i < dxCombo->count(); ++i) {
            if (dxCombo->itemData(i).toString() == currentDx) { dxCombo->setCurrentIndex(i); break; }
        }
        layout->addWidget(dxCombo);

        layout->addWidget(new QLabel("Preview fps scale (dt):"));
        QComboBox* dtCombo = new QComboBox();
        dtCombo->addItem("Full (1.0)", "1.0");
        dtCombo->addItem("Half (0.5)", "0.5");
        dtCombo->addItem("Quarter (0.25)", "0.25");
        dtCombo->addItem("Eighth (0.125)", "0.125");
        for (int i = 0; i < dtCombo->count(); ++i) {
            if (dtCombo->itemData(i).toString() == currentDt) { dtCombo->setCurrentIndex(i); break; }
        }
        layout->addWidget(dtCombo);

        layout->addWidget(new QLabel("Render resolution scale (dx):"));
        QComboBox* renderDxCombo = new QComboBox();
        renderDxCombo->addItem("Full (1.0)", "1.0");
        renderDxCombo->addItem("Half (0.5)", "0.5");
        renderDxCombo->addItem("Quarter (0.25)", "0.25");
        renderDxCombo->addItem("Eighth (0.125)", "0.125");
        for (int i = 0; i < renderDxCombo->count(); ++i) {
            if (renderDxCombo->itemData(i).toString() == currentRenderDx) { renderDxCombo->setCurrentIndex(i); break; }
        }
        layout->addWidget(renderDxCombo);

        layout->addWidget(new QLabel("Render fps scale (dt):"));
        QComboBox* renderDtCombo = new QComboBox();
        renderDtCombo->addItem("Full (1.0)", "1.0");
        renderDtCombo->addItem("Half (0.5)", "0.5");
        renderDtCombo->addItem("Quarter (0.25)", "0.25");
        renderDtCombo->addItem("Eighth (0.125)", "0.125");
        for (int i = 0; i < renderDtCombo->count(); ++i) {
            if (renderDtCombo->itemData(i).toString() == currentRenderDt) { renderDtCombo->setCurrentIndex(i); break; }
        }
        layout->addWidget(renderDtCombo);

        QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        layout->addWidget(buttonBox);

        QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

        if (dialog.exec() == QDialog::Accepted) {
            QString key = keyInput->text().trimmed();
            bool gpuEnabled = gpuCheckbox->isChecked();
            QString pythonPath = pythonPathInput->text().trimmed();
            QString dxValue = dxCombo->currentData().toString();
            QString dtValue = dtCombo->currentData().toString();
            QString renderDxValue = renderDxCombo->currentData().toString();
            QString renderDtValue = renderDtCombo->currentData().toString();

            openaiKey = key;

            QFile outFile("settings.txt");
            if (outFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&outFile);
                out << openaiKey;
                out << "\n";
                out << gpuEnabled;
                out << "\n";
                out << pythonPath;
                out << "\n";
                out << dxValue;
                out << "\n";
                out << dtValue;
                out << "\n";
                out << renderDxValue;
                out << "\n";
                out << renderDtValue;
                outFile.close();
            } else {
                QMessageBox::critical(nullptr, "Error", "Failed to save settings.");
            }

            QFile gpuOutFile("use_gpu.txt");
            if (gpuOutFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&gpuOutFile);
                out << (gpuEnabled ? "1" : "0");
                gpuOutFile.close();
            } else {
                QMessageBox::critical(nullptr, "Error", "Failed to save GPU setting.");
            }

            remakeNeoverePy(mediaPath);

            QMessageBox::information(nullptr, "Success", "Settings saved successfully.");
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

    // Auto-create venv on first run if it doesn't exist.
    // Prefer the Python interpreter bundled inside the packaged build,
    // falling back to /usr/bin/python3 (macOS dev) or python3 from PATH (Windows dev).
    //
    // venv layouts differ between platforms:
    //   POSIX:   ~/neovere_venv/bin/python3
    //   Windows: ~/neovere_venv/Scripts/python.exe
#ifdef Q_OS_WIN
    QString venvPython = QDir::homePath() + "/neovere_venv/Scripts/python.exe";
    QString venvPip    = QDir::homePath() + "/neovere_venv/Scripts/pip.exe";
#else
    QString venvPython = QDir::homePath() + "/neovere_venv/bin/python3";
    QString venvPip    = QDir::homePath() + "/neovere_venv/bin/pip";
#endif
    if (!QFile::exists(venvPython)) {
        outputDisplay->appendPlainText("Setting up Python environment for the first time, please wait...");

        QString bootstrapPython = resolveBundledPython();
        if (bootstrapPython.isEmpty()) {
#ifdef Q_OS_WIN
            bootstrapPython = "python";   // PATH lookup on Windows dev builds
#else
            bootstrapPython = "/usr/bin/python3";
#endif
        }
        outputDisplay->appendPlainText("Using interpreter: " + bootstrapPython);

        QProcess* setupProcess = new QProcess();
        setupProcess->setProcessEnvironment(neovereProcessEnvironment());
        QString venvDir = QDir::homePath() + "/neovere_venv";
#ifndef Q_OS_WIN
        QString setupScript = QString(
            "\"%1\" -m venv \"%2\" && "
            "\"%3\" install --upgrade pip && "
            "\"%3\" install pillow opencv-python scipy librosa soundfile openai pyqt5 numpy psutil"
        ).arg(bootstrapPython, venvDir, venvPip);
#endif

        QObject::connect(setupProcess, &QProcess::readyReadStandardOutput, [setupProcess, outputDisplay]() {
            outputDisplay->appendPlainText(setupProcess->readAllStandardOutput().trimmed());
        });
        QObject::connect(setupProcess, &QProcess::readyReadStandardError, [setupProcess, outputDisplay]() {
            outputDisplay->appendPlainText(setupProcess->readAllStandardError().trimmed());
        });
        QObject::connect(setupProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            [outputDisplay](int exitCode, QProcess::ExitStatus) {
                if (exitCode == 0) {
                    outputDisplay->appendPlainText("Python environment ready.");
                    // The persistent worker was started earlier with the bundled python
                    // (since ~/neovere_venv/bin/python3 didn't exist yet). Now that the
                    // venv is fully installed with all its packages (scipy, numpy, librosa,
                    // ...), kill the worker — its existing auto-respawn handler will
                    // restart it pointing at the venv interpreter.
                    if (pythonWorker && pythonWorker->state() == QProcess::Running) {
                        outputDisplay->appendPlainText("Reloading worker against the new venv...");
                        pythonWorker->kill();
                    }
                } else {
                    outputDisplay->appendPlainText("Environment setup failed. You can set a Python interpreter manually in Settings.");
                }
            });

#ifdef Q_OS_WIN
        // Qt's command-line construction escapes embedded quotes as `\"`, which cmd.exe
        // doesn't interpret — so running the script directly through `cmd.exe /C "..."`
        // ends up with cmd trying to execute `\"python\"` as a literal command. Sidestep
        // the whole quoting problem by writing the script to a temp .bat that cmd reads
        // from disk (where cmd-native `"..."` quoting works normally).
        QString batPath = QDir::tempPath() + "/neovere_venv_setup.bat";
        {
            QFile bat(batPath);
            if (bat.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                // Use `python.exe -m pip` rather than pip.exe — on Windows, pip can't
                // upgrade itself via pip.exe (the running pip.exe is locked and can't be
                // overwritten). pip itself prints this guidance when it detects the case.
                QByteArray script;
                script += "@echo on\r\n";
                script += "\"" + bootstrapPython.toUtf8() + "\" -m venv \""
                          + venvDir.toUtf8() + "\"\r\n";
                script += "if errorlevel 1 exit /b 1\r\n";
                script += "\"" + venvPython.toUtf8() + "\" -m pip install --upgrade pip\r\n";
                script += "if errorlevel 1 exit /b 1\r\n";
                script += "\"" + venvPython.toUtf8() + "\" -m pip install pillow opencv-python "
                          "scipy librosa soundfile openai pyqt5 numpy psutil cupy-cuda12x\r\n";
                bat.write(script);
                bat.close();
            }
        }
        setupProcess->start("cmd.exe", QStringList() << "/C" << batPath);
#else
        setupProcess->start("/bin/bash", QStringList() << "-lc" << setupScript);
#endif
    }

    // Generate placeholder render.mp4 if it doesn't exist
    if (!QFile::exists("render.mp4")) {
#ifdef Q_OS_WIN
        QString pythonExecutable = "python";
#else
        QString pythonExecutable = "python3";
#endif
        QStringList startupSettings = readFromFile("settings.txt").split("\n");
        if (startupSettings.length() > 2 && !startupSettings.at(2).trimmed().isEmpty()) {
            pythonExecutable = startupSettings.at(2).trimmed();
        } else if (QFile::exists(venvPython)) {
            pythonExecutable = venvPython;
        }

        QString arialPath = exportFontResourceToFile(":/resources/fonts/arial-bold.ttf");

        QString placeholderScript = QString(R"SCRIPT(import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONT_PATHS = ["%1"]

W, H = 1920, 1080
img = np.zeros((H, W, 3), dtype=np.uint8)

row1 = [(192,192,192),(192,192,0),(0,192,192),(0,192,0),(192,0,192),(192,0,0),(0,0,192)]
row2 = [(0,0,192),(19,19,19),(192,0,192),(19,19,19),(0,192,192),(19,19,19),(192,192,192)]
row3 = [(0,33,76),(255,255,255),(50,0,106),(19,19,19),(0,0,0),(19,19,19),(38,38,38)]

num_cols = 7
col_w = W // num_cols
row1_h = int(H * 0.67)
row2_h = int(H * 0.08)
rows = [(0, row1_h, row1), (row1_h, row1_h + row2_h, row2), (row1_h + row2_h, H, row3)]

for y1, y2, colors in rows:
    for c in range(num_cols):
        x1 = c * col_w
        x2 = x1 + col_w if c < num_cols - 1 else W
        r, g, b = colors[c]
        img[y1:y2, x1:x2] = (b, g, r)

# Draw text with Arial via PIL
pil_img = Image.fromarray(cv2.cvtColor(img, cv2.COLOR_BGR2RGB))
draw = ImageDraw.Draw(pil_img)
font = None
for p in FONT_PATHS:
    try:
        font = ImageFont.truetype(p, 110)
        break
    except Exception:
        continue
if font is None:
    font = ImageFont.load_default()

draw.text((20, H // 2 - 120), "your next render will", fill=(255,255,255), font=font)
draw.text((20, H // 2),       "be displayed here",     fill=(255,255,255), font=font)

img = cv2.cvtColor(np.array(pil_img), cv2.COLOR_RGB2BGR)

import os, subprocess

fourcc = cv2.VideoWriter_fourcc(*'mp4v')
silent_path = "render_silent.mp4"
writer = cv2.VideoWriter(silent_path, fourcc, 24.0, (W, H))
for _ in range(24 * 5):
    writer.write(img)
writer.release()

# Mux a silent stereo audio track using ffmpeg's lavfi anullsrc
result = subprocess.run([
    "ffmpeg", "-y",
    "-i", silent_path,
    "-f", "lavfi", "-i", "anullsrc=r=44100:cl=stereo",
    "-shortest",
    "-c:v", "copy",
    "-c:a", "aac",
    "render.mp4"
], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

if result.returncode != 0:
    # fall back to no-audio render if ffmpeg unavailable
    os.replace(silent_path, "render.mp4")
    print("ffmpeg unavailable; placeholder has no audio track")
else:
    if os.path.exists(silent_path):
        os.remove(silent_path)
    print("placeholder render.mp4 written with silent audio track")
)SCRIPT").arg(arialPath);

        outputDisplay->appendPlainText("Generating placeholder render.mp4 with: " + pythonExecutable);
        QProcess* placeholderProcess = new QProcess();
        placeholderProcess->setProcessEnvironment(neovereProcessEnvironment());
        QObject::connect(placeholderProcess, &QProcess::readyReadStandardOutput, [placeholderProcess, outputDisplay]() {
            outputDisplay->appendPlainText("[placeholder] " + QString::fromUtf8(placeholderProcess->readAllStandardOutput()).trimmed());
        });
        QObject::connect(placeholderProcess, &QProcess::readyReadStandardError, [placeholderProcess, outputDisplay]() {
            outputDisplay->appendPlainText("[placeholder err] " + QString::fromUtf8(placeholderProcess->readAllStandardError()).trimmed());
        });
        QObject::connect(placeholderProcess, &QProcess::errorOccurred, [outputDisplay](QProcess::ProcessError e) {
            outputDisplay->appendPlainText(QString("[placeholder] process error: %1").arg(e));
        });
        QObject::connect(placeholderProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            [mediaPanel, outputDisplay](int exitCode, QProcess::ExitStatus) {
                outputDisplay->appendPlainText(QString("[placeholder] finished exit=%1").arg(exitCode));
                if (exitCode == 0) {
                    if (!QFile::exists("preview.mp4") && QFile::exists("render.mp4")) {
                        QFile::copy("render.mp4", "preview.mp4");
                    }
                    mediaPanel->reloadVideo();
                }
            });
        placeholderProcess->start(pythonExecutable, QStringList() << "-c" << placeholderScript);
    }

    // Pre-generate a silent AAC audio file once. The preview render mux can then
    // reuse it via -c:a copy instead of regenerating audio per render.
    if (!QFile::exists("silent_audio.aac")) {
        QProcess silentAudioProc;
        silentAudioProc.setProcessEnvironment(neovereProcessEnvironment());
        silentAudioProc.start("ffmpeg", QStringList()
            << "-y"
            << "-f" << "lavfi"
            << "-i" << "anullsrc=r=44100:cl=stereo"
            << "-t" << "3600"
            << "-c:a" << "aac"
            << "silent_audio.aac");
        silentAudioProc.waitForFinished(15000);
    }

    // Spawn the persistent Python worker eagerly so passive previews work
    // before the user clicks Run for the first time.
    startPythonWorker(outputDisplay, mediaHeader, mediaPanel);

    // Show the window
    window.show();

    return app.exec();
}