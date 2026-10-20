#include "project.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
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
QJsonObject versionTwoFixture(QJsonObject project) {
    project["version"] = 2;
    auto tracks = project["tracks"].toArray();
    for (int t = 0; t < tracks.size(); ++t) {
        auto track = tracks[t].toObject();
        auto patterns = track["patterns"].toArray();
        for (int b = 0; b < patterns.size(); ++b) {
            auto pattern = patterns[b].toObject();
            auto steps = pattern["steps"].toArray();
            for (int s = 0; s < steps.size(); ++s) {
                const auto notes = steps[s].toArray();
                steps[s] = notes.empty() ? QJsonObject{{"velocity", 0}, {"note", 87}} : notes[0].toObject();
            }
            pattern["steps"] = steps;
            patterns[b] = pattern;
        }
        track["patterns"] = patterns;
        tracks[t] = track;
    }
    project["tracks"] = tracks;
    return project;
}
QJsonObject withFirstStep(QJsonObject project, const QJsonValue& value) {
    auto tracks = project["tracks"].toArray();
    auto track = tracks[0].toObject();
    auto patterns = track["patterns"].toArray();
    auto pattern = patterns[0].toObject();
    auto steps = pattern["steps"].toArray();
    steps[0] = value;
    pattern["steps"] = steps;
    patterns[0] = pattern;
    track["patterns"] = patterns;
    tracks[0] = track;
    project["tracks"] = tracks;
    return project;
}
bool hasNoOverrides(const fc::Note& note) {
    const auto& o = note.overrides;
    return !o.level && !o.pan && !o.tone && !o.decay && !o.drive;
}
bool hasNoOverrides(const fc::Project& project) {
    for (const auto& track : project.tracks)
        for (const auto& pattern : track.patterns)
            for (const auto& step : pattern.steps)
                for (const auto& note : step.notes)
                    if (!hasNoOverrides(note))
                        return false;
    return true;
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
    check(json["version"].toInt() == 4, "writes note sound override format v4");
    check(fc::projectToJson(out) == json, "roundtrip preserves every field");
    check(hasNoOverrides(out), "default song notes inherit every track sound control");
    auto blank = fc::emptyProject();
    check(blank.bpm == 120 && blank.songBars == 16 && blank.tailSeconds == 2, "neutral default song");
    bool empty = true;
    for (int i = 0; i < fc::TrackCount; ++i)
        empty &= blank.tracks[i].instrument == i && blank.tracks[i].patterns.empty() && blank.tracks[i].clips.empty();
    check(empty, "empty song has six default instruments and no content");
    check(fc::projectFromJson(fc::projectToJson(blank), out, error), "empty arrangement is valid song");
    const auto showcase = fc::showcaseProject();
    const auto showcaseJson = fc::projectToJson(showcase);
    check(showcase.title == "Neon Signal - 2011" && showcase.bpm == 126 && showcase.songBars == 48 &&
          showcase.tailSeconds == 3, "bundled showcase is the complete authored 48-bar Neon Signal song");
    check(showcase.master == .76 && showcase.delay == .17 && showcase.duck == .5 && !showcase.metronome,
          "showcase retains the final authored master mix");
    const double showcaseVolumes[] = {.36, 1.25, .75, 1.4, .6, .5};
    const size_t showcaseEvents[] = {140, 144, 429, 202, 82, 320};
    bool selfContained = true, authoredMix = true, showcaseArrangement = true;
    size_t totalShowcaseEvents = 0;
    for (int t = 0; t < fc::TrackCount; ++t) {
        const auto& track = showcase.tracks[t];
        selfContained &= track.samplePath.isEmpty() && track.instrument == t;
        authoredMix &= track.volume == showcaseVolumes[t] && !track.mute && !track.solo;
        const auto events = fc::scheduledSteps(showcase, t);
        showcaseArrangement &= events.size() == showcaseEvents[t] && !track.patterns.empty() && !track.clips.empty();
        totalShowcaseEvents += events.size();
    }
    check(selfContained, "showcase uses six built-in instruments without external sample dependencies");
    check(authoredMix, "showcase retains all final track levels and audible tracks");
    check(showcaseArrangement && totalShowcaseEvents == 1317 && fc::contentEndBar(showcase) == 48,
          "showcase schedules the complete 1317-note arrangement through its outro");
    const auto& showcaseLead = showcase.tracks[3];
    check(showcaseLead.patterns.size() == 4 && showcaseLead.patterns[0].name == "04 Neon chorus hook" &&
          showcaseLead.patterns[3].name == "04 Final chorus harmony" &&
          showcaseLead.patterns[0].steps[0].notes.size() == 1 &&
          showcaseLead.patterns[3].steps[0].notes.size() == 2,
          "final chorus adds harmony in a separate editable phrase");
    bool firstChorus = false, lastChorus = false;
    for (const auto& clip : showcaseLead.clips) {
        firstChorus |= clip.startBar == 16 && clip.lengthBars == 8 && clip.pattern == 0;
        lastChorus |= clip.startBar == 32 && clip.lengthBars == 12 && clip.pattern == 3;
    }
    check(firstChorus && lastChorus, "original and harmonized hooks play in their respective choruses");
    check(fc::projectFromJson(showcaseJson, out, error) && fc::projectToJson(out) == showcaseJson,
          "complete showcase survives editable project serialization unchanged");
    QFile embeddedShowcase(":/showcase/Neon-Signal-2011.ferro");
    check(embeddedShowcase.open(QIODevice::ReadOnly), "embedded showcase artifact is available");
    const auto embeddedShowcaseBytes = embeddedShowcase.readAll();
    // Lock to the authored, bundled example without requiring source files at runtime.
    const QByteArray authoredShowcaseHash = "2d8b2631d9df1d85acc76162149d775ab13f5e426de0b69af2066f258f9b8242";
    check(QCryptographicHash::hash(embeddedShowcaseBytes, QCryptographicHash::Sha256).toHex() == authoredShowcaseHash &&
          QJsonDocument::fromJson(embeddedShowcaseBytes).object() == showcaseJson,
          "embedded startup project exactly matches the bundled authored JSON");
    auto reject = [&](QJsonObject bad, const char* reason) {
        out = p;
        out.title = "unchanged";
        out.tracks[0].patterns[0].steps[0].notes[0].overrides.pan = -.8;
        const auto original = fc::projectToJson(out);
        check(!fc::projectFromJson(bad, out, error), reason);
        check(fc::projectToJson(out) == original && !error.isEmpty(),
              "failure preserves the entire output project and explains error");
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
    invalidPattern.tracks[0].patterns[0].steps[0].notes[0].note = 128;
    reject(fc::projectToJson(invalidPattern), "out of range MIDI note rejected");
    invalidPattern = p;
    invalidPattern.tracks[0].patterns[0].steps[0].notes[0].velocity = std::numeric_limits<double>::infinity();
    reject(fc::projectToJson(invalidPattern), "nonfinite velocity rejected");
    const QJsonObject c{{"velocity", .7}, {"note", 60}};
    const QJsonObject e{{"velocity", .5}, {"note", 64}};
    const QJsonObject g{{"velocity", .4}, {"note", 67}};
    const auto chordJson = withFirstStep(json, QJsonArray{c, e, g});
    check(fc::projectFromJson(chordJson, out, error), "three simultaneous notes on one step accepted");
    check(fc::projectToJson(out) == chordJson, "chord pitches, order, and independent velocities roundtrip");
    check(fc::projectFromJson(withFirstStep(json, QJsonArray{}), out, error) &&
          out.tracks[0].patterns[0].steps[0].notes.empty(), "empty note array represents a rest");
    reject(withFirstStep(json, c), "v4 requires a note array instead of a legacy step object");
    reject(withFirstStep(json, QJsonValue::Null), "null chord rejected");
    reject(withFirstStep(json, QJsonArray{c, 64}), "nonobject chord member rejected");
    reject(withFirstStep(json, QJsonArray{c, QJsonObject{{"note", 64}}}), "missing chord velocity rejected");
    reject(withFirstStep(json, QJsonArray{c, QJsonObject{{"velocity", .5}, {"note", "64"}}}),
           "string chord pitch rejected");
    reject(withFirstStep(json, QJsonArray{c, QJsonObject{{"velocity", .5}, {"note", 64.5}}}),
           "fractional chord pitch rejected");
    reject(withFirstStep(json, QJsonArray{c, QJsonObject{{"velocity", -.1}, {"note", 64}}}),
           "negative chord velocity rejected");
    reject(withFirstStep(json, QJsonArray{c, QJsonObject{{"velocity", 1.1}, {"note", 64}}}),
           "excessive chord velocity rejected");
    reject(withFirstStep(json, QJsonArray{c, c}), "duplicate chord pitches rejected");
    reject(withFirstStep(json, QJsonArray{c, QJsonObject{{"velocity", 0}, {"note", 60}}}),
           "inactive notes cannot hide duplicate pitches");
    QJsonArray allPitches;
    for (int note = 0; note < 128; ++note)
        allPitches.append(QJsonObject{{"velocity", .25}, {"note", note}});
    check(fc::projectFromJson(withFirstStep(json, allPitches), out, error) &&
          out.tracks[0].patterns[0].steps[0].notes.size() == 128,
          "all 128 MIDI pitches fit in a single chord");
    allPitches.append(c);
    reject(withFirstStep(json, allPitches), "more than 128 chord notes rejected");

    auto overriddenC = c, overriddenE = e;
    overriddenC["overrides"] = QJsonObject{{"level", 0}, {"pan", -1}, {"tone", .7}};
    overriddenE["overrides"] = QJsonObject{{"pan", 1}, {"decay", 1}, {"drive", 0}};
    const auto soundJson = withFirstStep(json, QJsonArray{overriddenC, overriddenE, g});
    check(fc::projectFromJson(soundJson, out, error), "mixed chord with partial note sound overrides loads");
    check(fc::projectToJson(out) == soundJson, "sparse overrides and explicit zero roundtrip exactly");
    const auto& soundNotes = out.tracks[0].patterns[0].steps[0].notes;
    check(soundNotes.size() == 3 && soundNotes[0].overrides.level == 0 &&
          !soundNotes[0].overrides.decay && soundNotes[1].overrides.drive == 0 &&
          hasNoOverrides(soundNotes[2]), "unset controls remain distinguishable from explicit zero");
    auto withOverrides = [&](QJsonValue value) {
        auto note = c;
        note["overrides"] = value;
        return withFirstStep(json, QJsonArray{note, e, g});
    };
    check(fc::projectFromJson(withOverrides(QJsonObject{}), out, error) &&
          fc::projectToJson(out) == chordJson, "empty override objects normalize to inherited settings");
    check(fc::projectFromJson(withOverrides(QJsonObject{{"level", 1.5}, {"pan", 1}, {"tone", 1},
                                                       {"decay", 1}, {"drive", 1}}), out, error),
          "upper bounds of every note sound control accepted");
    auto zeroJson = withOverrides(QJsonObject{{"level", 0}, {"pan", 0}, {"tone", 0},
                                              {"decay", 0}, {"drive", 0}});
    check(fc::projectFromJson(zeroJson, out, error) && fc::projectToJson(out) == zeroJson,
          "all explicit zero overrides remain stored rather than reverting to track values");
    reject(withOverrides(QJsonValue::Null), "null overrides rejected");
    reject(withOverrides(QJsonArray{}), "array overrides rejected");
    reject(withOverrides("inherit"), "string overrides rejected");
    reject(withOverrides(QJsonObject{{"volume", .5}}), "unknown note sound control rejected");
    reject(withOverrides(QJsonObject{{"level", .5}, {"unsupported", 0}}), "unknown sibling control rejected");
    for (const char* key : {"level", "pan", "tone", "decay", "drive"}) {
        const bool panControl = QString::fromLatin1(key) == "pan";
        const bool levelControl = QString::fromLatin1(key) == "level";
        reject(withOverrides(QJsonObject{{key, panControl ? -1.01 : -.01}}), "override below its range rejected");
        reject(withOverrides(QJsonObject{{key, levelControl ? 1.51 : 1.01}}), "override above its range rejected");
        reject(withOverrides(QJsonObject{{key, QJsonValue::Null}}), "null override value rejected");
        reject(withOverrides(QJsonObject{{key, true}}), "boolean override value rejected");
        reject(withOverrides(QJsonObject{{key, "0.5"}}), "string override value rejected");
    }
    invalidPattern = p;
    invalidPattern.tracks[0].patterns[0].steps[0].notes[0].overrides.tone =
        std::numeric_limits<double>::quiet_NaN();
    reject(fc::projectToJson(invalidPattern), "nonfinite note override rejected");

    fc::Track soundTrack;
    soundTrack.volume = .8;
    soundTrack.pan = -.3;
    soundTrack.tone = .4;
    soundTrack.decay = .5;
    soundTrack.drive = .6;
    fc::Note soundNote{.2, 60};
    auto resolved = fc::resolvedNoteSettings(soundTrack, soundNote);
    check(resolved.level == .8 && resolved.pan == -.3 && resolved.tone == .4 &&
          resolved.decay == .5 && resolved.drive == .6, "unset note controls resolve to track values");
    soundNote.overrides = {1.2, 0, 0, 1, .2};
    resolved = fc::resolvedNoteSettings(soundTrack, soundNote);
    check(resolved.level == 1.2 && resolved.pan == 0 && resolved.tone == 0 &&
          resolved.decay == 1 && resolved.drive == .2,
          "overrides replace controls absolutely and resolution does not apply velocity");
    soundNote.overrides = {0, -.8, std::nullopt, std::nullopt, std::nullopt};
    soundTrack.volume = .3;
    soundTrack.pan = .7;
    soundTrack.tone = .9;
    soundTrack.decay = .2;
    soundTrack.drive = .1;
    resolved = fc::resolvedNoteSettings(soundTrack, soundNote);
    check(resolved.level == 0 && resolved.pan == -.8 && resolved.tone == .9 &&
          resolved.decay == .2 && resolved.drive == .1,
          "track edits affect inherited controls while fixed values and zero remain unchanged");
    soundNote.overrides = {};
    resolved = fc::resolvedNoteSettings(soundTrack, soundNote);
    check(resolved.level == .3 && resolved.pan == .7 && resolved.tone == .9 &&
          resolved.decay == .2 && resolved.drive == .1, "clearing overrides restores current track settings");

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
    check(events.size() == 8 && events[0].eventId == 5698292665547922066ULL &&
          events[6].eventId == 5940654743287477170ULL,
          "legacy monophonic clip identities preserve their existing percussion noise seeds");
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

    auto chordSchedule = schedule;
    auto& chordSteps = chordSchedule.tracks[0].patterns[0].steps;
    chordSteps[0].notes = {{.9, 60}, {.7, 64}, {.6, 67}, {0, 90}};
    chordSteps[0].notes[1].overrides = {.4, -.6, .2, .8, 0};
    chordSteps[15].notes = {{.5, 64}, {.4, 69}};
    chordSteps[16].notes = {{.8, 67}, {.6, 71}};
    chordSteps[31].notes = {{.4, 72}, {.3, 76}, {.2, 79}};
    const auto chordEvents = fc::scheduledSteps(chordSchedule, 0);
    const int chordPositions[] = {16, 16, 16, 31, 31, 32, 32, 47, 47, 47,
                                  48, 48, 48, 63, 63, 96, 96, 96, 111, 111};
    const int chordPitches[] = {60, 64, 67, 64, 69, 67, 71, 72, 76, 79,
                                60, 64, 67, 64, 69, 60, 64, 67, 64, 69};
    check(chordEvents.size() == 20, "chords repeat and truncate with clips without emitting silent notes");
    eventIds.clear();
    bool originalIdsPreserved = true;
    for (size_t i = 0; i < chordEvents.size() && i < 20; ++i) {
        const auto& event = chordEvents[i];
        check(event.frame == int(std::llround(chordPositions[i] * 60.0 * fc::SampleRate / (137.0 * 4.0))) &&
              event.step.note == chordPitches[i], "every chord voice shares its step's exact onset");
        eventIds.insert(event.eventId);
        if (i == 0 || chordEvents[i - 1].frame != event.frame)
            for (const auto& original : events)
                if (original.frame == event.frame)
                    originalIdsPreserved &= event.eventId == original.eventId;
    }
    check(originalIdsPreserved, "adding chord voices preserves original onset identities");
    check(eventIds.size() == chordEvents.size(), "chord notes and repeated onsets have distinct event identities");
    auto reorderedChord = chordSchedule;
    std::swap(reorderedChord.tracks[0].patterns[0].steps[0].notes[1],
              reorderedChord.tracks[0].patterns[0].steps[0].notes[2]);
    bool stableChordIds = true;
    for (const auto& event : fc::scheduledSteps(reorderedChord, 0))
        for (const auto& original : chordEvents)
            if (event.frame == original.frame && event.step.note == original.step.note)
                stableChordIds &= event.eventId == original.eventId;
    check(stableChordIds, "additional chord identities follow pitches when their storage order changes");
    int scheduledOverrides = 0;
    for (const auto& event : chordEvents)
        if (event.step.overrides.level) {
            const auto& o = event.step.overrides;
            check(event.step.note == 64 && o.level == .4 && o.pan == -.6 && o.tone == .2 &&
                  o.decay == .8 && o.drive == 0, "scheduler copies every override onto its own chord voice");
            ++scheduledOverrides;
        }
    check(scheduledOverrides == 3, "note overrides survive phrase repetition and clipped source repeats");

    auto versionThree = chordJson;
    versionThree["version"] = 3;
    check(fc::projectFromJson(versionThree, out, error) && hasNoOverrides(out) &&
          fc::projectToJson(out) == chordJson, "v3 chords migrate unchanged with inherited track controls");
    auto oldOverrides = soundJson;
    oldOverrides["version"] = 3;
    reject(oldOverrides, "v3 cannot silently interpret v4 sound overrides");
    oldOverrides = withOverrides(QJsonObject{});
    oldOverrides["version"] = 3;
    reject(oldOverrides, "even empty overrides are rejected in old schemas");

    const auto versionTwo = versionTwoFixture(json);
    fc::Project importedTwo;
    check(fc::projectFromJson(versionTwo, importedTwo, error), "version2 monophonic song migrates");
    check(hasNoOverrides(importedTwo), "every v2 note inherits track controls after migration");
    bool versionTwoNotes = true, versionTwoEvents = true;
    for (int t = 0; t < fc::TrackCount; ++t) {
        const auto oldPatterns = versionTwo["tracks"].toArray()[t].toObject()["patterns"].toArray();
        for (int b = 0; b < oldPatterns.size(); ++b) {
            const auto oldSteps = oldPatterns[b].toObject()["steps"].toArray();
            const auto& newSteps = importedTwo.tracks[t].patterns[b].steps;
            for (int s = 0; s < oldSteps.size(); ++s) {
                const auto oldStep = oldSteps[s].toObject();
                versionTwoNotes &= newSteps[s].notes.size() == 1 &&
                    newSteps[s].notes[0].note == oldStep["note"].toInt() &&
                    newSteps[s].notes[0].velocity == oldStep["velocity"].toDouble();
            }
        }
        const auto before = fc::scheduledSteps(p, t), after = fc::scheduledSteps(importedTwo, t);
        versionTwoEvents &= before.size() == after.size();
        for (size_t i = 0; i < before.size() && i < after.size(); ++i)
            versionTwoEvents &= before[i].frame == after[i].frame && before[i].eventId == after[i].eventId &&
                before[i].step.note == after[i].step.note && before[i].step.velocity == after[i].step.velocity;
    }
    check(versionTwoNotes, "v2 migration preserves active notes and stored pitches of zero-velocity rests");
    check(versionTwoEvents, "v2 migration preserves every audible onset, velocity, and noise identity");
    check(fc::projectFromJson(fc::projectToJson(importedTwo), out, error) &&
          fc::projectToJson(out) == fc::projectToJson(importedTwo), "v2 migration saves and reloads as v4");
    reject(withFirstStep(versionTwo, QJsonArray{c, e, g}), "v2 does not silently accept v3 chords");
    reject(withFirstStep(versionTwo, QJsonObject{{"velocity", .5}, {"note", 60.5}}),
           "v2 legacy notes remain strictly validated");
    reject(withFirstStep(versionTwo, overriddenC), "v2 rejects note sound overrides");

    const auto legacy = legacyFixture();
    fc::Project imported;
    check(fc::projectFromJson(legacy, imported, error), "version1 project migrates");
    check(hasNoOverrides(imported), "every v1 note inherits track controls after migration");
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
                notesPreserved &= newStep.notes.size() == 1 && newStep.notes[0].note == oldStep["note"].toInt() &&
                    newStep.notes[0].velocity == oldStep["velocity"].toDouble();
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
    check(fc::projectFromJson(fc::projectToJson(imported), out, error), "migrated project validates as v4");
    bad = legacy;
    bad["arrangement"] = QJsonArray{1.5};
    reject(bad, "legacy fractional pattern rejected");
    bad = legacy;
    bad["bpm"] = 79;
    reject(bad, "legacy tempo still validated against original contract");
    bad = legacy;
    auto legacyTracks = bad["tracks"].toArray();
    auto legacyTrack = legacyTracks[0].toObject();
    auto legacyPatterns = legacyTrack["patterns"].toArray();
    auto legacySteps = legacyPatterns[0].toArray();
    legacySteps[0] = overriddenC;
    legacyPatterns[0] = legacySteps;
    legacyTrack["patterns"] = legacyPatterns;
    legacyTracks[0] = legacyTrack;
    bad["tracks"] = legacyTracks;
    reject(bad, "v1 rejects note sound overrides");

    QTemporaryDir dir;
    p.tracks[0].samplePath = dir.filePath("sounds/kick.wav");
    const QString path = dir.filePath("session.ferro");
    p.tracks[2].patterns[0].steps[0].notes = {{.5, 60}, {.4, 64}, {.3, 67}};
    p.tracks[2].patterns[0].steps[0].notes[1].overrides = {0, -.8, .3, 1, 0};
    check(fc::saveProject(path, p, error), "atomic save");
    check(fc::loadProject(path, out, error), "read saved file");
    check(fc::projectToJson(out) == fc::projectToJson(p), "saved song restores chords and note sound overrides");
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
    invalidProject = p;
    invalidProject.tracks[2].patterns[0].steps[0].notes[1].overrides.pan = 1.01;
    check(!fc::saveProject(path, invalidProject, error), "invalid note sound override cannot be saved");
    check(fc::loadProject(path, out, error) && fc::projectToJson(out) == fc::projectToJson(p),
          "rejected override save preserves the previously saved song");
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
