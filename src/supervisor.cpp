#include "supervisor.h"
#include "dsp.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <optional>

namespace fc {
struct Supervisor::Impl {
    struct Batch {
        Project project;
        quint64 serial = 0;
        int frames = 0;
        std::array<int, TrackCount> state{}; // queued, running, complete
        std::array<int, TrackCount> attempts{};
        std::array<std::vector<float>, TrackCount> stems;
        bool failed = false;
    };
    struct Job {
        quint64 id, serial;
        int track;
        QString path;
    };
    struct Worker {
        QProcess* process = nullptr;
        QTimer* timeout = nullptr;
        int epoch = 0, completed = 0;
        bool draining = false, faulted = false;
        QByteArray output, diagnostics;
        std::optional<Job> job;
    };
    Supervisor* q;
    const int target, recycleAfter, renderTimeoutMs;
    QTemporaryDir directory;
    std::list<std::unique_ptr<Worker>> workers;
    std::shared_ptr<Batch> latest, mixing;
    QFutureWatcher<std::vector<float>>* mixer;
    quint64 serial = 0, nextJob = 0;
    int epoch = 1, restarts = 0, failures = 0;
    bool stopping = false, disabled = false, pendingReload = false;
    QString lastError;

    Impl(Supervisor* owner, int count, int recycle, int timeoutMs)
        : q(owner), target(std::clamp(count, 1, 8)), recycleAfter(std::max(1, recycle)),
          renderTimeoutMs(std::max(250, timeoutMs)), mixer(new QFutureWatcher<std::vector<float>>(owner)) {
        QObject::connect(mixer, &QFutureWatcher<std::vector<float>>::finished, q, [this] {
            const auto finished = mixing;
            mixing.reset();
            try {
                auto samples = mixer->future().takeResult();
                if (!stopping && latest == finished && !finished->failed)
                    emit q->mixReady(samples, finished->project);
            } catch (const std::exception& ex) {
                if (!stopping)
                    report(QStringLiteral("Mix failed: %1").arg(QString::fromUtf8(ex.what())));
            } catch (...) {
                if (!stopping)
                    report(QStringLiteral("Mix failed with an unknown error."));
            }
            if (!stopping) {
                startMix();
                emit q->statusChanged();
            }
        });
        if (!directory.isValid()) {
            disabled = true;
            lastError = QStringLiteral("Cannot create the private render directory.");
        } else
            fillPool();
    }

    void report(const QString& message) {
        lastError = message;
        emit q->errorOccurred(message);
        emit q->statusChanged();
    }

    int liveCurrent() const {
        int n = 0;
        for (const auto& w : workers)
            if (w->epoch == epoch && !w->draining)
                ++n;
        return n;
    }
    bool oldBusy() const {
        for (const auto& w : workers)
            if (w->epoch < epoch)
                return true;
        return false;
    }
    void fillPool() {
        if (stopping || disabled)
            return;
        while (!disabled && liveCurrent() < target)
            spawn();
    }
    void spawn() {
        auto owned = std::make_unique<Worker>();
        auto* w = owned.get();
        w->epoch = epoch;
        w->process = new QProcess(q);
        w->timeout = new QTimer(w->process);
        w->timeout->setSingleShot(true);
        // Each subprocess has at most one job. All later edits replace the single pending batch.
        w->process->setProcessChannelMode(QProcess::SeparateChannels);
        workers.push_back(std::move(owned));
        QObject::connect(w->process, &QProcess::started, q, [this, w] {
            if (w->draining)
                w->process->closeWriteChannel();
            else
                dispatch();
            emit q->statusChanged();
        });
        QObject::connect(w->process, &QProcess::readyReadStandardOutput, q, [this, w] { read(w); });
        QObject::connect(w->process, &QProcess::readyReadStandardError, q, [w] {
            w->diagnostics += w->process->readAllStandardError();
            if (w->diagnostics.size() > 1024)
                w->diagnostics = w->diagnostics.right(1024);
        });
        QObject::connect(w->process, &QProcess::errorOccurred, q, [this, w](QProcess::ProcessError error) {
            if (stopping || w->faulted)
                return;
            if (error == QProcess::FailedToStart) {
                fault(w, QStringLiteral("Worker could not start: %1").arg(w->process->errorString()));
                // FailedToStart does not emit finished(). Defer removal until signal delivery returns.
                QTimer::singleShot(0, q, [this, w] { exited(w); });
            }
        });
        QObject::connect(w->process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), q,
                         [this, w](int, QProcess::ExitStatus) { exited(w); });
        QObject::connect(w->timeout, &QTimer::timeout, q, [this, w] {
            fault(w, w->job ? QStringLiteral("Worker render timed out after %1 seconds.")
                                  .arg(renderTimeoutMs / 1000.0, 0, 'g', 4)
                            : QStringLiteral("Worker did not drain within 2 seconds."));
        });
        w->process->start(QCoreApplication::applicationFilePath(), {QStringLiteral("--worker")});
    }

    void retire(Worker* w) {
        w->draining = true;
        if (!w->job) {
            w->process->closeWriteChannel();
            w->timeout->start(2000);
        }
    }
    void doReload() {
        ++epoch;
        ++restarts;
        for (auto& w : workers)
            retire(w.get());
        fillPool();
        dispatch();
        emit q->statusChanged();
    }
    void reload() {
        if (stopping || !directory.isValid())
            return;
        // Retrying uses a fresh render serial so outstanding jobs from a failed
        // batch cannot complete a replacement batch with stale stems.
        const std::optional<Project> retry =
            latest && latest->failed ? std::optional<Project>(latest->project) : std::nullopt;
        disabled = false;
        failures = 0;
        lastError.clear();
        // Repeated reloads coalesce while a previous generation still has work to drain.
        if (oldBusy()) {
            pendingReload = true;
            emit q->statusChanged();
        } else
            doReload();
        if (retry)
            render(*retry);
    }
    void failJob(Worker* w, const QString& message) {
        if (!w->job)
            return;
        const auto job = *w->job;
        QFile::remove(job.path);
        w->job.reset();
        if (latest && latest->serial == job.serial && !latest->failed) {
            if (++latest->attempts[job.track] <= 2)
                latest->state[job.track] = 0;
            else {
                latest->failed = true;
                report(
                    QStringLiteral("Track %1 failed after 3 attempts. %2").arg(job.track + 1).arg(message));
            }
        }
    }
    void fault(Worker* w, const QString& message) {
        if (stopping || w->faulted)
            return;
        w->faulted = true;
        w->timeout->stop();
        failJob(w, message);
        w->draining = true;
        ++restarts;
        if (++failures >= 6) {
            disabled = true;
            if (latest)
                latest->failed = true;
            report(QStringLiteral("Worker pool stopped after repeated failures. Reload workers to retry. %1")
                       .arg(message));
        } else {
            lastError = message;
            emit q->statusChanged();
        }
        if (w->process->state() != QProcess::NotRunning)
            w->process->kill();
    }
    void exited(Worker* w) {
        const auto found =
            std::find_if(workers.begin(), workers.end(), [w](const auto& item) { return item.get() == w; });
        if (found == workers.end())
            return;
        if (!w->faulted && (!w->draining || w->job)) {
            QString diagnostic = QString::fromUtf8(w->diagnostics).trimmed();
            fault(w, QStringLiteral("Worker exited unexpectedly%1.")
                         .arg(diagnostic.isEmpty() ? QString() : QStringLiteral(": ") + diagnostic));
        }
        w->timeout->stop();
        w->process->disconnect(q);
        w->timeout->disconnect(q);
        w->process->deleteLater();
        workers.erase(found);
        if (stopping)
            return;
        if (pendingReload && !oldBusy()) {
            pendingReload = false;
            doReload();
        } else {
            fillPool();
            dispatch();
            emit q->statusChanged();
        }
    }

    void dispatch() {
        if (stopping || disabled || !latest || latest->failed)
            return;
        for (const auto& owned : workers) {
            auto* w = owned.get();
            if (w->epoch != epoch || w->draining || w->job || w->process->state() != QProcess::Running)
                continue;
            const auto it = std::find(latest->state.begin(), latest->state.end(), 0);
            if (it == latest->state.end())
                break;
            const int track = int(it - latest->state.begin());
            Job job{++nextJob, latest->serial, track,
                    directory.filePath(QStringLiteral("%1-%2.f32").arg(latest->serial).arg(nextJob))};
            latest->state[track] = 1;
            w->job = job;
            QJsonObject request{{QStringLiteral("id"), double(job.id)},
                                {QStringLiteral("project"), projectToJson(latest->project)},
                                {QStringLiteral("track"), track},
                                {QStringLiteral("path"), job.path}};
            auto line = QJsonDocument(request).toJson(QJsonDocument::Compact);
            line.append('\n');
            if (w->process->write(line) != line.size()) {
                fault(w, QStringLiteral("Cannot write to worker IPC."));
                continue;
            }
            w->timeout->start(renderTimeoutMs);
        }
        startMix();
    }

    void finishJob(Worker* w) {
        if (w->job)
            QFile::remove(w->job->path);
        w->timeout->stop();
        w->job.reset();
        ++w->completed;
        failures = 0;
        if (w->draining)
            retire(w);
        else if (w->completed >= recycleAfter) {
            ++restarts;
            retire(w);
            fillPool();
        }
        dispatch();
        emit q->statusChanged();
    }

    void read(Worker* w) {
        w->output += w->process->readAllStandardOutput();
        if (w->output.size() > 1024 * 1024) {
            fault(w, QStringLiteral("Oversized worker response."));
            return;
        }
        while (!w->faulted) {
            const auto newline = w->output.indexOf('\n');
            if (newline < 0)
                break;
            auto line = w->output.left(newline);
            w->output.remove(0, newline + 1);
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject() || !w->job) {
                fault(w, QStringLiteral("Malformed or unsolicited worker response."));
                return;
            }
            const auto response = document.object();
            const auto job = *w->job;
            if (response.value(QStringLiteral("id")).toDouble(-1) != double(job.id) ||
                !response.value(QStringLiteral("ok")).isBool()) {
                fault(w, QStringLiteral("Worker response ID or status is invalid."));
                return;
            }
            const bool wanted = latest && latest->serial == job.serial && !latest->failed;
            if (!response.value(QStringLiteral("ok")).toBool()) {
                // A valid failure response means the worker is healthy. Missing
                // samples and other deterministic input errors need a user fix,
                // not process retries or a depleted crash budget.
                if (wanted) {
                    latest->failed = true;
                    QString reason = response.value(QStringLiteral("error")).toString();
                    if (reason.isEmpty())
                        reason = QStringLiteral("The worker could not render this track.");
                    report(QStringLiteral("Track %1 could not render: %2").arg(job.track + 1).arg(reason));
                }
                finishJob(w);
                continue;
            }
            if (wanted) {
                const qint64 expectedBytes = qint64(latest->frames) * 2 * qint64(sizeof(float));
                QFile file(job.path);
                if (response.value(QStringLiteral("path")).toString() != job.path ||
                    response.value(QStringLiteral("frames")).toDouble(-1) != latest->frames ||
                    !file.open(QIODevice::ReadOnly) || file.size() != expectedBytes) {
                    fault(w, QStringLiteral("Worker stem metadata or byte count is invalid."));
                    return;
                }
                const auto bytes = file.readAll();
                file.close();
                if (bytes.size() != expectedBytes) {
                    fault(w, QStringLiteral("Cannot read complete worker stem."));
                    return;
                }
                std::vector<float> samples(size_t(expectedBytes / qint64(sizeof(float))));
                for (size_t i = 0; i < samples.size(); ++i) {
                    const quint32 bits = qFromLittleEndian<quint32>(bytes.constData() + i * sizeof(float));
                    std::memcpy(&samples[i], &bits, sizeof(float));
                    if (!std::isfinite(samples[i])) {
                        fault(w, QStringLiteral("Worker stem contains a non-finite sample."));
                        return;
                    }
                }
                latest->stems[job.track] = std::move(samples);
                latest->state[job.track] = 2;
            }
            finishJob(w);
        }
    }

    void startMix() {
        if (stopping || mixing || !latest || latest->failed ||
            !std::all_of(latest->state.begin(), latest->state.end(), [](int state) { return state == 2; }))
            return;
        // A batch is mixed at most once, including while a stale mix finishes in the background.
        if (latest->state[0] == 3)
            return;
        auto batch = latest;
        latest->state.fill(3);
        mixing = batch;
        mixer->setFuture(QtConcurrent::run([batch] {
            auto result = mixTracks(batch->project, batch->stems);
            // Every track is complete and no worker can write this batch now.
            // Playback needs only the mix, not six full-song stem allocations.
            for (auto& stem : batch->stems)
                std::vector<float>().swap(stem);
            return result;
        }));
    }
    void render(const Project& project) {
        if (stopping)
            return;
        Project validated;
        QString error;
        if (!projectFromJson(projectToJson(project), validated, error)) {
            report(QStringLiteral("Cannot render project: %1").arg(error));
            return;
        }
        if (!directory.isValid()) {
            report(lastError);
            return;
        }
        auto batch = std::make_shared<Batch>();
        batch->project = std::move(validated);
        batch->frames = renderFrames(batch->project);
        batch->serial = ++serial;
        latest = std::move(batch);
        dispatch();
        emit q->statusChanged();
    }

    void stop() {
        if (stopping)
            return;
        stopping = true;
        pendingReload = false;
        // Send EOF to every worker first, then share one grace period across the whole pool.
        for (auto& w : workers) {
            w->timeout->stop();
            w->timeout->disconnect(q);
            w->process->disconnect(q);
            w->process->closeWriteChannel();
        }
        QElapsedTimer grace;
        grace.start();
        for (auto& w : workers)
            if (w->process->state() != QProcess::NotRunning)
                w->process->waitForFinished(int(std::max<qint64>(1, 900 - grace.elapsed())));
        for (auto& w : workers)
            if (w->process->state() != QProcess::NotRunning)
                w->process->terminate();
        grace.restart();
        for (auto& w : workers)
            if (w->process->state() != QProcess::NotRunning)
                w->process->waitForFinished(int(std::max<qint64>(1, 400 - grace.elapsed())));
        for (auto& w : workers)
            if (w->process->state() != QProcess::NotRunning)
                w->process->kill();
        for (auto& w : workers) {
            if (w->process->state() != QProcess::NotRunning)
                w->process->waitForFinished(1000);
            delete w->process;
        }
        workers.clear();
        mixer->disconnect(q);
        mixer->future().waitForFinished();
        mixing.reset();
        latest.reset();
        emit q->statusChanged();
    }
};

Supervisor::Supervisor(QObject* parent, int workers, int recycleAfter, int renderTimeoutMs)
    : QObject(parent), d(std::make_unique<Impl>(this, workers, recycleAfter, renderTimeoutMs)) {}
Supervisor::~Supervisor() {
    stop();
}
void Supervisor::render(const Project& project) {
    d->render(project);
}
void Supervisor::reload() {
    d->reload();
}
void Supervisor::stop() {
    d->stop();
}
int Supervisor::generation() const {
    return d->epoch;
}
int Supervisor::restartCount() const {
    return d->restarts;
}
QList<qint64> Supervisor::workerPids() const {
    QList<qint64> pids;
    for (const auto& w : d->workers)
        if (w->process->processId() > 0)
            pids.append(w->process->processId());
    return pids;
}
QStringList Supervisor::statusLines() const {
    QStringList lines;
    const QString state = d->stopping   ? QStringLiteral("Stopped")
                          : d->disabled ? QStringLiteral("Pool disabled")
                                        : QStringLiteral("Online");
    lines << QStringLiteral("%1 · generation %2 · %3 restarts · 48 kHz stereo float")
                 .arg(state)
                 .arg(d->epoch)
                 .arg(d->restarts);
    if (d->latest) {
        int ready = 0;
        for (int step : d->latest->state)
            if (step >= 2)
                ++ready;
        lines << QStringLiteral("Render %1 · %2/%3 stems · %4")
                     .arg(d->latest->serial)
                     .arg(ready)
                     .arg(TrackCount)
                     .arg(d->latest->failed        ? QStringLiteral("failed; previous audio retained")
                          : d->mixing == d->latest ? QStringLiteral("mixing")
                          : ready == TrackCount    ? QStringLiteral("ready")
                                                   : QStringLiteral("rendering"));
    }
    for (const auto& w : d->workers) {
        const QString activity = w->faulted ? QStringLiteral("recovering")
                                 : w->job   ? QStringLiteral("track %1").arg(w->job->track + 1)
                                 : w->process->state() == QProcess::Starting ? QStringLiteral("starting")
                                                                             : QStringLiteral("idle");
        lines << QStringLiteral("PID %1 · gen %2 · %3%4 · %5/%6 jobs")
                     .arg(w->process->processId())
                     .arg(w->epoch)
                     .arg(activity)
                     .arg(w->draining ? QStringLiteral(" · draining") : QString())
                     .arg(w->completed)
                     .arg(d->recycleAfter);
    }
    if (d->pendingReload)
        lines << QStringLiteral("Reload queued while previous generation drains.");
    if (!d->lastError.isEmpty())
        lines << d->lastError;
    return lines;
}
} // namespace fc
