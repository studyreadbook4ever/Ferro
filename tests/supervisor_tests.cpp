#include "dsp.h"
#include "supervisor.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <functional>
#include <iostream>
#include <unistd.h>

namespace {
bool until(const std::function<bool()>& predicate, int milliseconds = 15000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}
bool require(bool value, const char* message) {
    if (!value)
        std::cerr << "Supervisor test failed: " << message << '\n';
    return value;
}
fc::Project smallProject() {
    auto project = fc::demoProject();
    project.title = QStringLiteral("Supervisor IPC test");
    project.bpm = 300;
    project.songBars = 1;
    project.tailSeconds = .5;
    for (auto& track : project.tracks) {
        track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(),
                                        [](const fc::Clip& clip) { return clip.startBar > 0; }),
                          track.clips.end());
        for (auto& clip : track.clips)
            clip.lengthBars = 1;
    }
    auto& lead = project.tracks[3];
    lead.patterns = {fc::Pattern{}};
    lead.patterns[0].steps[0].notes = {{.6, 60}, {.5, 64}, {.4, 67}};
    lead.patterns[0].steps[0].notes[0].overrides = {.2, -.8, .3, .1, 0};
    lead.patterns[0].steps[0].notes[1].overrides = {.4, .7, .8, .6, .5};
    lead.clips = {{fc::nextClipId(project), 0, 0, 1}};
    return project;
}
void extendSong(fc::Project& project, int bars) {
    project.songBars = bars;
    for (auto& track : project.tracks)
        for (auto& clip : track.clips)
            clip.lengthBars = bars;
}
} // namespace

int supervisorTests() {
    auto project = smallProject();
    const auto reference = fc::renderProject(project);
    int delivered = 0;
    std::vector<float> received;
    QString receivedTitle;
    QStringList errors;
    fc::Supervisor pool(nullptr, 2, 32);
    QObject::connect(&pool, &fc::Supervisor::mixReady, &pool,
                     [&](const std::vector<float>& mix, const fc::Project& result) {
                         ++delivered;
                         received = mix;
                         receivedTitle = result.title;
                     });
    QObject::connect(&pool, &fc::Supervisor::errorOccurred, &pool,
                     [&](const QString& error) { errors << error; });
    if (!require(until([&] { return pool.workerPids().size() == 2; }), "workers start"))
        return 1;
    pool.render(project);
    if (!require(until([&] { return delivered == 1; }), "persistent IPC produces mix"))
        return 1;
    if (!require(received == reference, "IPC preserves chord velocities and sound overrides and matches direct DSP"))
        return 1;
    // Crash an idle persistent subprocess, then require a replacement and another complete render.
    const auto killed = pool.workerPids().first();
    if (!require(::kill(pid_t(killed), SIGKILL) == 0, "test can kill worker"))
        return 1;
    if (!require(until([&] {
                     return pool.restartCount() > 0 && pool.workerPids().size() == 2 &&
                            !pool.workerPids().contains(killed);
                 }),
                 "crashed worker is reaped and replaced"))
        return 1;
    pool.render(project);
    if (!require(until([&] { return delivered == 2; }), "pool renders after crash"))
        return 1;
    // Kill a worker while an assigned request is in progress; that track must be retried.
    extendSong(project, 8);
    pool.render(project);
    const int beforeCrash = pool.restartCount();
    if (!require(::kill(pid_t(pool.workerPids().first()), SIGKILL) == 0, "kill an in-flight worker"))
        return 1;
    if (!require(until([&] { return delivered == 3; }), "in-flight crash retries render"))
        return 1;
    if (!require(pool.restartCount() > beforeCrash, "in-flight crash recorded"))
        return 1;
    // Several edits and reloads arrive before the event loop can deliver any old result.
    const auto oldPids = pool.workerPids();
    const int oldGeneration = pool.generation();
    for (int edit = 0; edit < 15; ++edit) {
        project.title = QStringLiteral("Latest edit %1").arg(edit);
        project.bpm = 190 + edit;
        pool.render(project);
        pool.reload();
    }
    if (!require(
            until([&] { return delivered >= 4 && receivedTitle == QStringLiteral("Latest edit 14"); }, 30000),
            "coalesced edits and graceful reload publish latest render"))
        return 1;
    if (!require(delivered == 4, "stale render results are discarded"))
        return 1;
    if (!require(pool.generation() > oldGeneration, "reload advances worker generation"))
        return 1;
    if (!require(until([&] {
                     const auto pids = pool.workerPids();
                     return pids.size() == 2 && std::none_of(oldPids.begin(), oldPids.end(),
                                                             [&](qint64 pid) { return pids.contains(pid); });
                 }),
                 "old generation fully drains"))
        return 1;
    const auto remaining = pool.workerPids();
    pool.stop();
    if (!require(pool.workerPids().isEmpty(), "stop clears pool"))
        return 1;
    for (auto pid : remaining)
        if (!require(::kill(pid_t(pid), 0) != 0, "stop reaps child process"))
            return 1;
    if (!require(errors.isEmpty(), "ordinary crash recovery does not surface a terminal render error"))
        return 1;

    // A stopped subprocess stays alive but cannot answer IPC. The render
    // watchdog must kill/reap it, replace it, and retry its assigned track.
    fc::Supervisor watchdog(nullptr, 1, 32, 1000);
    int watchdogMixes = 0;
    QStringList watchdogErrors;
    QObject::connect(&watchdog, &fc::Supervisor::mixReady, &watchdog,
                     [&](const std::vector<float>&, const fc::Project&) { ++watchdogMixes; });
    QObject::connect(&watchdog, &fc::Supervisor::errorOccurred, &watchdog,
                     [&](const QString& message) { watchdogErrors << message; });
    if (!require(until([&] {
                     return watchdog.workerPids().size() == 1 &&
                            watchdog.statusLines().join(' ').contains(QStringLiteral(" · idle · "));
                 }),
                 "watchdog worker starts and becomes idle"))
        return 1;
    auto longProject = smallProject();
    extendSong(longProject, 8);
    watchdog.render(longProject);
    const auto stoppedPid = watchdog.workerPids().first();
    if (!require(watchdog.statusLines().join(' ').contains("track 1"), "watchdog request is assigned"))
        return 1;
    if (!require(::kill(pid_t(stoppedPid), SIGSTOP) == 0, "suspend assigned worker"))
        return 1;
    if (!require(until([&] { return watchdogMixes == 1; }),
                 "watchdog replaces hung worker and retries render"))
        return 1;
    if (!require(watchdog.restartCount() > 0 && !watchdog.workerPids().contains(stoppedPid),
                 "watchdog records replacement of stopped process"))
        return 1;
    if (!require(::kill(pid_t(stoppedPid), 0) != 0, "watchdog reaps stopped child"))
        return 1;
    if (!require(watchdogErrors.isEmpty(), "watchdog recovery produces no terminal render error"))
        return 1;
    watchdog.stop();

    // A low job lifetime exercises replacement repeatedly without a crash.
    fc::Supervisor recycled(nullptr, 2, 1);
    int recycledMixes = 0;
    QObject::connect(&recycled, &fc::Supervisor::mixReady, &recycled,
                     [&](const std::vector<float>&, const fc::Project&) { ++recycledMixes; });
    recycled.render(smallProject());
    if (!require(until([&] { return recycledMixes == 1; }), "recycled workers finish all stems"))
        return 1;
    if (!require(recycled.restartCount() >= fc::TrackCount, "job lifetime triggers recycling"))
        return 1;
    if (!require(until([&] { return recycled.workerPids().size() == 2; }), "replacement pool settles"))
        return 1;
    recycled.render(smallProject());
    if (!require(until([&] { return recycledMixes == 2; }), "replacement workers remain usable"))
        return 1;
    recycled.stop();

    // A permanent render error must become retryable through the advertised
    // reload action after its cause is repaired, without another render request.
    QTemporaryDir samples;
    if (!require(samples.isValid(), "sample recovery temporary directory"))
        return 1;
    fc::Supervisor recoverable(nullptr, 2, 32);
    auto broken = smallProject();
    broken.tracks[0].samplePath = samples.filePath(QStringLiteral("repaired.wav"));
    int recoveryMixes = 0;
    QStringList recoveryErrors;
    QObject::connect(&recoverable, &fc::Supervisor::mixReady, &recoverable,
                     [&](const std::vector<float>& mix, const fc::Project& result) {
                         if (mix.size() == size_t(fc::renderFrames(result)) * 2)
                             ++recoveryMixes;
                     });
    QObject::connect(&recoverable, &fc::Supervisor::errorOccurred, &recoverable,
                     [&](const QString& message) { recoveryErrors << message; });
    recoverable.render(broken);
    if (!require(until([&] { return !recoveryErrors.isEmpty(); }),
                 "missing sample reports an actionable render error"))
        return 1;
    if (!require(recoveryMixes == 0, "failed sample cannot publish a mix"))
        return 1;
    if (!require(recoveryErrors.size() == 1, "user render error is reported once"))
        return 1;
    if (!require(recoverable.restartCount() == 0 && recoverable.workerPids().size() == 2,
                 "invalid sample retains healthy worker pool without restarts"))
        return 1;
    QString writeError;
    if (!require(fc::writeWave(broken.tracks[0].samplePath, reference, writeError),
                 "repair missing sample at same path"))
        return 1;
    recoverable.reload();
    if (!require(until([&] { return recoveryMixes == 1; }), "reload retries failed render without an edit"))
        return 1;
    recoverable.stop();
    std::cout << "Supervisor tests passed (IPC, coalescing, crash retry, watchdog, reload, recycling, "
                 "reaping, failure "
                 "recovery).\n";
    return 0;
}
