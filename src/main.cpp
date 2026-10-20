#include "dsp.h"
#include "project.h"
#include "window.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <iostream>
#include <stdexcept>

int projectTests();
int dspTests();
int audioTests();
int supervisorTests();
int main(int argc, char** argv) {
    bool graphical = true;
    for (int i = 1; i < argc; ++i)
        if (QString(argv[i]) == "--worker" || QString(argv[i]) == "--self-test" ||
            QString(argv[i]) == "--render" || QString(argv[i]) == "--help" ||
            QString(argv[i]) == "--write-demo" || QString(argv[i]) == "--probe-audio")
            graphical = false;
    std::unique_ptr<QCoreApplication> app;
    if (graphical)
        app = std::make_unique<QApplication>(argc, argv);
    else
        app = std::make_unique<QCoreApplication>(argc, argv);
    app->setApplicationName("ferro");
    app->setOrganizationName("Ferro");
    app->setApplicationVersion("2.2.1");
    QStringList args = app->arguments();
    try {
        if (args.contains("--worker"))
            return fc::workerMain();
        if (args.contains("--probe-audio")) {
            fc::AudioEngine engine;
            QString error;
            engine.setGain(0);
            auto silent = fc::renderProject(fc::demoProject());
            std::fill(silent.begin(), silent.end(), 0.0f);
            engine.submit(silent, fc::demoProject().bpm);
            if (!engine.open(error))
                throw std::runtime_error(error.toStdString());
            engine.setPlaying(true);
            QElapsedTimer time;
            time.start();
            double maxCpu = 0;
            while (time.elapsed() < 5000) {
                QThread::msleep(20);
                engine.collect();
                maxCpu = std::max(maxCpu, engine.cpuLoad());
            }
            QJsonObject result{{"device", engine.deviceName()},
                               {"seconds", time.elapsed() / 1000.},
                               {"position_frames", double(engine.position())},
                               {"xruns", double(engine.xruns())},
                               {"max_callback_cpu", maxCpu},
                               {"muted", true}};
            bool ok = engine.position() > fc::SampleRate;
            engine.close();
            std::cout << QJsonDocument(result).toJson().constData();
            return ok ? 0 : 1;
        }
        if (args.contains("--self-test")) {
            int failures = projectTests() + dspTests() + audioTests() + supervisorTests();
            std::cout << "FERRO tests: " << failures << " failures\n";
            return failures ? 1 : 0;
        }
        if (args.contains("--help")) {
            std::cout << "FERRO · Linux song studio\n\nferro                         Open "
                         "workstation\nferro --render demo --output FILE.wav\nferro --render "
                         "FILE.ferro --output FILE.wav\nferro --write-demo FILE.ferro\nferro "
                         "--self-test\nferro --probe-audio            Silent 5-second hardware "
                         "check\nQT_QPA_PLATFORM=offscreen ferro --smoke-gui [--screenshot FILE.png]\n";
            return 0;
        }
        if (args.contains("--write-demo")) {
            int i = args.indexOf("--write-demo");
            if (i + 1 >= args.size())
                throw std::runtime_error("--write-demo requires a destination.");
            QString error;
            if (!fc::saveProject(args[i + 1], fc::demoProject(), error))
                throw std::runtime_error(error.toStdString());
            return 0;
        }
        if (args.contains("--render")) {
            int index = args.indexOf("--render"), out = args.indexOf("--output"),
                rep = args.indexOf("--repeat");
            if (index + 1 >= args.size() || out < 0 || out + 1 >= args.size())
                throw std::runtime_error("Usage: --render demo|PROJECT --output FILE.wav");
            fc::Project project = fc::demoProject();
            QString error;
            if (args[index + 1] != "demo" && !fc::loadProject(args[index + 1], project, error))
                throw std::runtime_error(error.toStdString());
            if (rep >= 0 && rep + 1 >= args.size())
                throw std::runtime_error("--repeat requires a count.");
            if (rep >= 0)
                throw std::runtime_error(
                    "Song export writes the timeline once. Extend clips on the timeline to repeat music.");
            auto samples = fc::renderProject(project);
            if (!fc::writeWave(args[out + 1], samples, error))
                throw std::runtime_error(error.toStdString());
            std::cout << "Exported " << args[out + 1].toStdString() << " · 48000 Hz stereo / 24-bit PCM\n";
            return 0;
        }
        bool smoke = args.contains("--smoke-gui");
        std::unique_ptr<QLockFile> instanceLock;
        if (!smoke) {
            QString appDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
            QDir().mkpath(appDir);
            instanceLock = std::make_unique<QLockFile>(appDir + "/session.lock");
            instanceLock->setStaleLockTime(0);
            if (!instanceLock->tryLock(0)) {
                QMessageBox::information(
                    nullptr, "FERRO", "FERRO is already running. Use the existing window to open a session.");
                return 1;
            }
        }
        fc::Window window(smoke);
        window.show();
        if (smoke) {
            int i = args.indexOf("--screenshot");
            QString path = i >= 0 && i + 1 < args.size() ? args[i + 1] : QString();
            return window.smokeTest(path);
        }
        return app->exec();
    } catch (const std::exception& e) {
        std::cerr << "FERRO: " << e.what() << '\n';
        return 1;
    }
}
