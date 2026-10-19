#include "project.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>

namespace {
QJsonObject legacyFixture() {
    QJsonArray tracks;
    for (int t = 0; t < fc::TrackCount; ++t) {
        QJsonArray patterns;
        for (int b = 0; b < 4; ++b) {
            QJsonArray steps;
            for (int s = 0; s < fc::Steps; ++s)
                steps.append(QJsonObject{{"velocity", s % 4 == 0 ? .5 + t * .03 : 0.0},
                                         {"note", 29 + t * 5 + b + s}});
            patterns.append(steps);
        }
        tracks.append(QJsonObject{{"name", QString("Legacy %1").arg(t)}, {"volume", .8},
                                  {"pan", -.15}, {"tone", .4}, {"decay", .7}, {"drive", .6},
                                  {"mute", t == 1}, {"solo", t == 2}, {"sample", "sounds/test.wav"},
                                  {"patterns", patterns}});
    }
    return {{"format", "ferro-project"}, {"version", 1}, {"title", "Original title"},
            {"bpm", 193.5}, {"master", .7}, {"delay", .2}, {"duck", .8}, {"metronome", true},
            {"arrangement", QJsonArray{0, 0, 1, 3, 2, 2}}, {"tracks", tracks}};
}
}
int projectTests() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) {
            std::cerr << "PROJECT FAIL: " << message << '\n';
            ++failures;
        }
    };
    fc::Project p = fc::demoProject(), out;
    QString error;
    auto json = fc::projectToJson(p);
    check(fc::projectFromJson(json, out, error), "valid demo roundtrip");
    check(json["version"].toInt() == 2, "writes song format v2");
    check(fc::projectToJson(out) == json, "roundtrip preserves every field");
    auto blank = fc::emptyProject();
    check(blank.bpm == 120 && blank.songBars == 16 && blank.tailSeconds == 2, "neutral default song");
    bool empty = true;
    for (int i = 0; i < fc::TrackCount; ++i)
        empty &= blank.tracks[i].instrument == i && blank.tracks[i].patterns.empty() && blank.tracks[i].clips.empty();
    check(empty, "empty song has six default instruments and no content");
    check(fc::projectFromJson(fc::projectToJson(blank), out, error), "empty arrangement is valid song");
    auto reject = [&](QJsonObject bad, const char* reason) {
        out.title = "unchanged";
        check(!fc::projectFromJson(bad, out, error), reason);
        check(out.title == "unchanged" && !error.isEmpty(), "failure preserves output and explains error");
    };
    auto bad = json;
    bad["bpm"] = 0;
    reject(bad, "zero tempo rejected");
    bad = json;
    bad["bpm"] = 39;
    reject(bad, "tempo below40 rejected");
    bad = json;
    bad["songBars"] = 1.5;
    reject(bad, "fractional song length rejected");
    bad = json;
    bad["songBars"] = 0;
    reject(bad, "zero song length rejected");
    bad = json;
    bad["songBars"] = 257;
    reject(bad, "excessive song length rejected");
    bad = json;
    bad["songBars"] = 101;
    bad["bpm"] = 40;
    reject(bad, "ten minute render duration cap enforced");
    bad = json;
    bad["tailSeconds"] = 8.01;
    reject(bad, "excessive tail rejected");
    bad = json;
    bad["version"] = 2.5;
    reject(bad, "fractional version rejected");
    bad = json;
    bad["version"] = 99;
    reject(bad, "unknown version rejected");
    auto trackMutation = [&](const char* key, QJsonValue value, const char* reason) {
        auto modified = json;
        auto tracks = modified["tracks"].toArray();
        auto track = tracks[0].toObject();
        track[key] = value;
        tracks[0] = track;
        modified["tracks"] = tracks;
        reject(modified, reason);
    };
    trackMutation("drive", -1, "negative drive rejected");
    trackMutation("instrument", 6, "unknown instrument rejected");
    trackMutation("instrument", 1.2, "fractional instrument rejected");
    trackMutation("solo", 1, "nonboolean solo rejected");
    trackMutation("clips", QJsonArray{QJsonObject{{"id", 1}, {"pattern", 0}, {"startBar", 0}, {"lengthBars", 4}},
                                          QJsonObject{{"id", 99}, {"pattern", 0}, {"startBar", 3}, {"lengthBars", 2}}},
                  "overlapping clips rejected");
    trackMutation("clips", QJsonArray{QJsonObject{{"id", 2}, {"pattern", 0}, {"startBar", 0}, {"lengthBars", 1}}},
                  "global duplicate clip identity rejected");
    trackMutation("clips", QJsonArray{QJsonObject{{"id", 99}, {"pattern", 5}, {"startBar", 0}, {"lengthBars", 1}}},
                  "missing source phrase rejected");
    trackMutation("clips", QJsonArray{QJsonObject{{"id", 99}, {"pattern", 0}, {"startBar", 15}, {"lengthBars", 2}}},
                  "clips beyond song end rejected");
    trackMutation("clips", QJsonArray{QJsonObject{{"id", 0}, {"pattern", 0}, {"startBar", 0}, {"lengthBars", 1}}},
                  "zero clip identity rejected");
    auto invalidPattern = p;
    invalidPattern.tracks[0].patterns[0].steps.pop_back();
    reject(fc::projectToJson(invalidPattern), "incorrect phrase step count rejected");
    invalidPattern = p;
    invalidPattern.tracks[0].patterns[0].steps[0].note = 128;
    reject(fc::projectToJson(invalidPattern), "out of range MIDI note rejected");
    invalidPattern = p;
    invalidPattern.tracks[0].patterns[0].steps[0].velocity = std::numeric_limits<double>::infinity();
    reject(fc::projectToJson(invalidPattern), "nonfinite velocity rejected");

    auto boundary = blank;
    boundary.bpm = 40;
    boundary.songBars = 100;
    boundary.tailSeconds = 0;
    check(fc::projectFromJson(fc::projectToJson(boundary), out, error), "forty BPM and exactly ten minutes accepted");
    boundary.tailSeconds = .01;
    reject(fc::projectToJson(boundary), "effect tail counts toward ten minute limit");
    boundary = blank;
    boundary.tracks[0].patterns.assign(129, fc::Pattern{});
    reject(fc::projectToJson(boundary), "phrase count limit enforced");
    boundary = p;
    boundary.tracks[0].clips.assign(257, fc::Clip{1, 0, 0, 1});
    reject(fc::projectToJson(boundary), "clip count limit enforced");

    // A two-bar source, a three-bar repeat, a gap, and a clipped final repeat.
    auto schedule = fc::emptyProject();
    schedule.bpm = 137;
    schedule.songBars = 9;
    schedule.tailSeconds = 1.25;
    fc::Pattern phrase;
    phrase.name = "Two bars";
    phrase.bars = 2;
    phrase.steps.assign(32, {});
    phrase.steps[0] = {.9, 60};
    phrase.steps[15] = {.5, 64};
    phrase.steps[16] = {.8, 67};
    phrase.steps[31] = {.4, 72};
    schedule.tracks[0].patterns.push_back(phrase);
    // Intentionally reverse storage order: scheduling must be chronological.
    schedule.tracks[0].clips = {{22, 0, 6, 1}, {21, 0, 1, 3}};
    check(fc::projectFromJson(fc::projectToJson(schedule), out, error), "nonoverlapping clips with gap valid");
    const auto events = fc::scheduledSteps(schedule, 0);
    const int positions[] = {16, 31, 32, 47, 48, 63, 96, 111};
    check(events.size() == 8, "repeat exact event count and truncated source");
    std::set<uint64_t> eventIds;
    for (size_t i = 0; i < events.size() && i < 8; ++i) {
        const int expected = int(std::llround(positions[i] * 60.0 * fc::SampleRate / (137.0 * 4.0)));
        check(events[i].frame == expected, "absolute onset avoids per-bar rounding drift");
        eventIds.insert(events[i].eventId);
    }
    check(eventIds.size() == events.size(), "repeated onsets have distinct event identities");
    check(fc::scheduledSteps(schedule, 1).empty(), "empty lane is silent");
    check(fc::scheduledSteps(schedule, -1).empty(), "invalid lane returns no events");
    auto changedElsewhere = schedule;
    changedElsewhere.tracks[1].patterns.push_back(phrase);
    changedElsewhere.tracks[1].clips.push_back({100, 0, 0, 1});
    const auto stable = fc::scheduledSteps(changedElsewhere, 0);
    check(!stable.empty() && stable[0].eventId == events[0].eventId, "other lane edits preserve event identities");
    check(fc::contentEndBar(schedule) == 7, "content end respects clips and gap");
    check(fc::contentEndBar(blank) == 1, "empty content end stays at one bar");
    check(fc::nextClipId(schedule) == 23, "next clip identity follows global maximum");
    check(fc::songFrames(schedule) == int(std::llround(9.0 * 240 * fc::SampleRate / 137)), "song duration in frames");
    check(fc::renderFrames(schedule) == fc::songFrames(schedule) + 60000, "render includes explicit effect tail");
    check(fc::loopFrames(schedule) == fc::renderFrames(schedule), "legacy duration alias includes tail");
    check(fc::songFrames(p) == 1536000, "sixteen bars at120 BPM duration");

    const auto legacy = legacyFixture();
    fc::Project imported;
    check(fc::projectFromJson(legacy, imported, error), "version1 project migrates");
    check(imported.title == "Original title" && imported.songBars == 6 && imported.tailSeconds == 2 &&
          imported.bpm == 193.5 && imported.master == .7 && imported.delay == .2 &&
          imported.duck == .8 && imported.metronome, "legacy song metadata preserved");
    bool notesPreserved = true;
    bool controlsPreserved = true;
    std::set<uint64_t> importedIds;
    const auto arrangement = legacy["arrangement"].toArray();
    for (int t = 0; t < fc::TrackCount; ++t) {
        const auto& track = imported.tracks[t];
        const auto oldTrack = legacy["tracks"].toArray()[t].toObject();
        controlsPreserved &= track.instrument == t && track.name == oldTrack["name"].toString() &&
            track.samplePath == "sounds/test.wav" && track.volume == .8 && track.pan == -.15 &&
            track.tone == .4 && track.decay == .7 && track.drive == .6 && track.mute == (t == 1) && track.solo == (t == 2);
        for (const auto& clip : track.clips)
            importedIds.insert(clip.id);
        for (int b = 0; b < 4; ++b)
            for (int s = 0; s < fc::Steps; ++s) {
                const auto oldStep = oldTrack["patterns"].toArray()[b].toArray()[s].toObject();
                const auto& newStep = track.patterns[b].steps[s];
                notesPreserved &= newStep.note == oldStep["note"].toInt() && newStep.velocity == oldStep["velocity"].toDouble();
            }
        auto scheduled = fc::scheduledSteps(imported, t);
        notesPreserved &= scheduled.size() == 24;
        for (size_t e = 0; e < scheduled.size(); ++e) {
            const int bar = int(e) / 4, s = (int(e) % 4) * 4;
            const int expectedNote = 29 + t * 5 + arrangement[bar].toInt() + s;
            const int expectedFrame = int(std::llround((bar * 16 + s) * 60.0 * fc::SampleRate / (193.5 * 4)));
            notesPreserved &= scheduled[e].step.note == expectedNote && scheduled[e].frame == expectedFrame;
        }
    }
    check(notesPreserved, "migration preserves every stored note and every arranged onset");
    check(controlsPreserved, "migration preserves all track controls and source paths");
    check(importedIds.size() == 24, "migrated clips use globally unique identities");
    check(fc::projectFromJson(fc::projectToJson(imported), out, error), "migrated project validates as v2");
    bad = legacy;
    bad["arrangement"] = QJsonArray{1.5};
    reject(bad, "legacy fractional pattern rejected");
    bad = legacy;
    bad["bpm"] = 79;
    reject(bad, "legacy tempo still validated against original contract");

    QTemporaryDir dir;
    p.tracks[0].samplePath = dir.filePath("sounds/kick.wav");
    const QString path = dir.filePath("session.ferro");
    check(fc::saveProject(path, p, error), "atomic save");
    check(fc::loadProject(path, out, error), "read saved file");
    check(out.tracks[0].samplePath == p.tracks[0].samplePath, "sample relative paths resolve correctly");
    QFile saved(path);
    check(saved.open(QIODevice::ReadOnly), "read saved JSON");
    const auto savedJson = QJsonDocument::fromJson(saved.readAll()).object();
    check(savedJson["tracks"].toArray()[0].toObject()["sample"].toString() == "sounds/kick.wav", "portable relative sample path stored");
    saved.close();
    auto invalidProject = p;
    invalidProject.title = QString(257, 'x');
    check(!fc::saveProject(path, invalidProject, error), "invalid project save rejected");
    check(fc::loadProject(path, out, error) && out.title == p.title, "rejected save preserves prior file");
    QFile invalid(dir.filePath("invalid.ferro"));
    check(invalid.open(QIODevice::WriteOnly), "open corrupt fixture");
    invalid.write("{broken");
    invalid.close();
    out.title = "unchanged";
    check(!fc::loadProject(invalid.fileName(), out, error) && out.title == "unchanged", "corrupt JSON rejected without replacing song");
    QFile oversized(dir.filePath("oversized.ferro"));
    check(oversized.open(QIODevice::WriteOnly) && oversized.resize(16 * 1024 * 1024 + 1), "create oversized fixture");
    oversized.close();
    check(!fc::loadProject(oversized.fileName(), out, error) && out.title == "unchanged", "oversized document rejected without replacing song");
    QFile oldFile(dir.filePath("legacy.ferro"));
    check(oldFile.open(QIODevice::WriteOnly), "open legacy fixture");
    oldFile.write(QJsonDocument(legacy).toJson());
    oldFile.close();
    check(fc::loadProject(oldFile.fileName(), out, error) && out.songBars == 6, "legacy file load migrates");
    check(out.tracks[0].samplePath == dir.filePath("sounds/test.wav"), "legacy sample paths resolve relative to file");
    check(fc::noteName(60) == "C4", "MIDI note name uses standard C4 at60");
    std::cout << "Project tests: " << failures << " failures\n";
    return failures;
}
