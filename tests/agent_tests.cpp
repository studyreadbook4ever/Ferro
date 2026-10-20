#include "agent.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <set>

namespace {
QJsonObject plan(const QJsonArray& operations) {
    return {{"version", 1}, {"operations", operations}};
}
QJsonObject notes(const QString& phrase, const QJsonArray& groups, int track = 3) {
    return {{"op", "notes.set"}, {"track", track}, {"phrase", phrase}, {"notes", groups}};
}
QJsonObject group(int bar, int step, const QJsonArray& pitches) {
    return {{"bar", bar}, {"step", step}, {"pitches", pitches}};
}
const fc::Note* findNote(const fc::Project& p, int track, int phrase, int step, int pitch) {
    if (track < 0 || track >= fc::TrackCount || phrase < 0 || phrase >= int(p.tracks[track].patterns.size()))
        return nullptr;
    const auto& steps = p.tracks[track].patterns[phrase].steps;
    if (step < 0 || step >= int(steps.size()))
        return nullptr;
    const auto found = std::find_if(steps[step].notes.begin(), steps[step].notes.end(),
                                    [pitch](const fc::Note& n) { return n.note == pitch; });
    return found == steps[step].notes.end() ? nullptr : &*found;
}
} // namespace

int agentTests() {
    int failures = 0;
    const auto check = [&](bool ok, const char* description) {
        if (!ok) {
            std::cerr << "AGENT FAIL: " << description << '\n';
            ++failures;
        }
    };
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-12; };
    const fc::Project blank = fc::emptyProject();
    fc::Project p;
    QJsonObject response;
    auto opening = group(1, 1, {"C4", "E4", "G4"});
    opening["velocity"] = 73;
    opening["sound"] = QJsonObject{{"level", 150}, {"pan", -100}, {"tone", 0}, {"decay", 100}, {"drive", 35}};
    const QJsonArray composition{
        QJsonObject{
            {"op", "song.set"}, {"title", "Agent contract"}, {"tempo", 126}, {"bars", 4}, {"tail", 2}},
        QJsonObject{{"op", "track.set"}, {"track", 3}, {"instrument", "Lead"}, {"level", 125}, {"pan", -15}},
        QJsonObject{{"op", "phrase.create"}, {"track", 3}, {"name", "Chords"}, {"bars", 4}},
        notes("Chords",
              {opening, group(4, 16, {"F#3", "Bb2", "C-1", "G9", 60}), group(4, 15, {"B#3", "Cb4"})}),
        QJsonObject{{"op", "phrase.create"}, {"track", 1}, {"name", "Kick"}, {"bars", 1}},
        notes("Kick", {group(1, 1, {"C2"})}, 1),
        QJsonObject{{"op", "phrase.create"}, {"track", 6}, {"name", "Hats"}, {"bars", 1}},
        notes("Hats", {group(1, 3, {"F#2"})}, 6),
        QJsonObject{{"op", "clip.create"}, {"track", 1}, {"phrase", "Kick"}, {"bar", 1}, {"bars", 4}},
        QJsonObject{{"op", "clip.create"}, {"track", 3}, {"phrase", "Chords"}, {"bar", 1}, {"bars", 4}},
        QJsonObject{{"op", "clip.create"}, {"track", 6}, {"phrase", "Hats"}, {"bar", 1}, {"bars", 4}}};
    const auto blankBefore = fc::projectToJson(blank);
    const bool composed = fc::agentApply(blank, plan(composition), p, response);
    check(composed && response.value("ok").toBool(), "one plan composes a valid multi-track song");
    check(fc::projectToJson(blank) == blankBefore, "successful apply leaves its base project unchanged");
    if (!composed) {
        std::cout << "Agent tests: " << failures << " failures\n";
        return failures;
    }
    check(p.title == "Agent contract" && p.bpm == 126 && p.songBars == 4 && p.tailSeconds == 2,
          "song controls use the documented musical units");
    check(p.tracks[2].instrument == 3 && p.tracks[2].volume == 1.25 && p.tracks[2].pan == -.15 &&
              p.tracks[1].patterns.empty() && p.tracks[4].patterns.empty(),
          "track coordinates select 1 through 6 without shifting adjacent tracks");
    check(findNote(p, 0, 0, 0, 36) && findNote(p, 5, 0, 2, 42),
          "first and last track note coordinates are 1-based");
    check(findNote(p, 2, 0, 63, 54) && findNote(p, 2, 0, 63, 46) && findNote(p, 2, 0, 63, 0) &&
              findNote(p, 2, 0, 63, 127) && findNote(p, 2, 0, 63, 60) && findNote(p, 2, 0, 62, 60) &&
              findNote(p, 2, 0, 62, 59),
          "bar 4 step 16, MIDI, sharps, flats, enharmonic octaves and boundary pitches resolve correctly");
    const auto* c = findNote(p, 2, 0, 0, 60);
    check(c && close(c->velocity, 73 / 127.) && c->overrides.level == 1.5 && c->overrides.pan == -1 &&
              c->overrides.tone == 0 && c->overrides.decay == 1 && c->overrides.drive == .35,
          "note velocity and all five sound controls translate from GUI units");
    std::set<uint64_t> ids;
    for (const auto& track : p.tracks)
        for (const auto& clip : track.clips)
            ids.insert(clip.id);
    check(ids.size() == 3 && *ids.begin() > 0 &&
              response["operations"].toArray()[9].toObject()["clip"].toDouble() ==
                  double(p.tracks[2].clips[0].id),
          "clip IDs are globally unique and returned for later edits");
    const auto inspection = fc::agentInspect(p);
    const auto inspected =
        inspection["tracks"].toArray()[2].toObject()["phrases"].toArray()[0].toObject()["notes"].toArray();
    bool boundaryInspected = false;
    for (const auto& value : inspected) {
        const auto note = value.toObject();
        boundaryInspected |= note["bar"].toInt() == 4 && note["step"].toInt() == 16 &&
                             note["midi"].toInt() == 127 && note["pitch"].toString() == "G9";
    }
    check(boundaryInspected && inspection["tempo"].toDouble() == 126,
          "inspect returns the same 1-based coordinates and standard note names");

    const auto reject = [&](const fc::Project& base, const QJsonObject& badPlan, const char* code,
                            const char* description) {
        const auto originalBase = fc::projectToJson(base);
        auto sentinel = p;
        sentinel.title = "Sentinel output must survive";
        sentinel.tracks[2].patterns[0].steps[0].notes[0].overrides.pan = .82;
        const auto originalOutput = fc::projectToJson(sentinel);
        QJsonObject error;
        check(!fc::agentApply(base, badPlan, sentinel, error), description);
        check(fc::projectToJson(base) == originalBase && fc::projectToJson(sentinel) == originalOutput,
              "rejected plan preserves the entire base and preexisting output");
        check(!error.value("ok").toBool() && error["error"].toObject()["code"].toString() == code &&
                  !error["error"].toObject()["message"].toString().isEmpty(),
              "rejected plan returns a useful structured error");
        return error;
    };
    const auto badBatch =
        reject(p,
               plan({QJsonObject{{"op", "song.set"}, {"title", "Must not leak"}},
                     notes("Chords", {group(1, 0, {"C4"})})}),
               "INVALID_ARGUMENT", "invalid second operation rolls back earlier valid operations");
    check(badBatch["error"].toObject()["operation"].toInt() == 2,
          "error operation index identifies the failing operation using 1-based indexing");
    auto extraPlanKey = plan({QJsonObject{{"op", "song.fit"}}});
    extraPlanKey["surprise"] = true;
    reject(p, extraPlanKey, "UNKNOWN_FIELD", "unknown top-level fields are rejected");
    reject(p, plan({QJsonObject{{"op", "song.set"}, {"duration", 3}}}), "UNKNOWN_FIELD",
           "unknown operation fields are rejected");
    auto duration = group(1, 1, {"C4"});
    duration["duration"] = 4;
    reject(p, plan({notes("Chords", {duration})}), "UNKNOWN_FIELD",
           "unsupported note durations are rejected");
    auto fractional = group(1, 1, {"C4"});
    fractional["bar"] = 1.5;
    reject(p, plan({notes("Chords", {fractional})}), "INVALID_ARGUMENT",
           "fractional note coordinates are rejected");
    reject(p, plan({notes("Chords", {group(0, 1, {"C4"})})}), "INVALID_ARGUMENT", "bar zero is rejected");
    reject(p, plan({notes("Chords", {group(1, 1, {"C4"})}, 0)}), "INVALID_ARGUMENT",
           "track zero is rejected");
    reject(p, plan({notes("Chords", {group(1, 1, {60.5})})}), "INVALID_ARGUMENT",
           "fractional MIDI pitch is rejected");
    reject(p, plan({notes("Chords", {group(1, 1, {"C99"})})}), "INVALID_ARGUMENT",
           "out-of-range named pitch is rejected");
    reject(p, plan({notes("Chords", {group(1, 1, {"C4", 60})})}), "DUPLICATE_NOTE",
           "named and numeric duplicates are rejected");
    reject(p, plan({notes("Chords", {group(1, 1, {"B#3", "C4"})})}), "DUPLICATE_NOTE",
           "enharmonic duplicates are rejected");
    reject(p, plan({notes("Chords", {group(1, 1, {"C4"}), group(1, 1, {60})})}), "DUPLICATE_NOTE",
           "duplicate groups in one note operation are rejected");
    reject(p, {{"version", 1}, {"operations", QJsonArray{}}}, "INVALID_ARGUMENT",
           "empty operation plans are rejected");
    reject(p, {{"version", 1.5}, {"operations", composition}}, "INVALID_ARGUMENT",
           "fractional protocol versions are rejected");

    fc::Project upserted;
    auto partial = group(1, 1, {"C4"});
    partial["sound"] = QJsonObject{{"tone", 80}};
    const bool upsertOk =
        fc::agentApply(p, plan({notes("Chords", {partial, group(1, 1, {"D4"})})}), upserted, response);
    check(upsertOk, "notes.set can patch an existing note and add another chord voice");
    if (!upsertOk)
        return failures;
    c = findNote(upserted, 2, 0, 0, 60);
    const auto* d = findNote(upserted, 2, 0, 0, 62);
    check(c && close(c->velocity, 73 / 127.) && c->overrides.tone == .8 && c->overrides.pan == -1 &&
              c->overrides.level == 1.5 && c->overrides.decay == 1 && c->overrides.drive == .35 &&
              findNote(upserted, 2, 0, 0, 64) && findNote(upserted, 2, 0, 63, 127),
          "upsert preserves omitted velocity, other overrides, sibling chord notes and other steps");
    check(d && close(d->velocity, 100 / 127.) && !d->overrides.level && !d->overrides.pan,
          "new notes default to velocity 100 and inherit their track sound");
    auto inherited = group(1, 1, {"C4"});
    inherited["velocity"] = 0;
    inherited["sound"] = QJsonObject{{"tone", QJsonValue::Null}, {"pan", 0}};
    fc::Project silent;
    check(fc::agentApply(upserted, plan({notes("Chords", {inherited})}), silent, response),
          "explicit inheritance and velocity zero are valid edits");
    c = findNote(silent, 2, 0, 0, 60);
    check(c && c->velocity == 0 && !c->overrides.tone && c->overrides.pan == 0 && c->overrides.level == 1.5,
          "velocity zero remains stored, null clears one override and numeric zero stays explicit");
    check(fc::scheduledSteps(silent, 2).size() + 1 == fc::scheduledSteps(upserted, 2).size(),
          "stored zero-velocity note does not schedule audible playback");
    const QJsonObject removeD{{"op", "notes.remove"},
                              {"track", 3},
                              {"phrase", "Chords"},
                              {"notes", QJsonArray{group(1, 1, {"D4"})}}};
    fc::Project removed;
    check(fc::agentApply(upserted, plan({removeD}), removed, response) && !findNote(removed, 2, 0, 0, 62) &&
              findNote(removed, 2, 0, 0, 60) && findNote(removed, 2, 0, 63, 127),
          "notes.remove deletes only the requested chord voice");
    reject(p, plan({removeD}), "NOTE_NOT_FOUND",
           "removing an absent note reports an error instead of guessing");

    fc::Project shared;
    const bool sharedOk = fc::agentApply(
        p,
        plan({QJsonObject{{"op", "song.set"}, {"bars", 8}},
              QJsonObject{
                  {"op", "clip.create"}, {"track", 3}, {"phrase", "Chords"}, {"bar", 5}, {"bars", 4}}}),
        shared, response);
    check(sharedOk, "a phrase can be reused in another non-overlapping clip");
    if (!sharedOk)
        return failures;
    const uint64_t secondClip = shared.tracks[2].clips.size() > 1 ? shared.tracks[2].clips[1].id : 0;
    fc::Project sharedEdit;
    const bool sharedEditOk =
        fc::agentApply(shared, plan({notes("Chords", {partial})}), sharedEdit, response);
    check(sharedEditOk, "shared phrase can be edited once for all its clips");
    if (!sharedEditOk)
        return failures;
    const auto affected = response["operations"].toArray()[0].toObject()["affected_clips"].toArray();
    check(affected.size() == 2 && affected.contains(double(shared.tracks[2].clips[0].id)) &&
              affected.contains(double(secondClip)),
          "note edit explicitly reports both affected shared clip IDs");
    fc::Project cleared;
    check(fc::agentApply(shared,
                         plan({QJsonObject{{"op", "phrase.clear"}, {"track", 3}, {"phrase", "Chords"}}}),
                         cleared, response) &&
              fc::scheduledSteps(cleared, 2).empty() && cleared.tracks[2].patterns[0].bars == 4 &&
              cleared.tracks[2].clips.size() == 2 &&
              fc::scheduledSteps(cleared, 0).size() == fc::scheduledSteps(shared, 0).size(),
          "clearing a shared phrase silences all its clips while preserving their arrangement and other "
          "tracks");
    fc::Project fitted;
    check(fc::agentApply(shared,
                         plan({QJsonObject{{"op", "clip.remove"}, {"track", 3}, {"clip", double(secondClip)}},
                               QJsonObject{{"op", "song.fit"}}}),
                         fitted, response) &&
              fitted.songBars == 4 && fitted.tracks[2].clips.size() == 1,
          "removing the final clip then fitting trims the song to its remaining content");
    auto changedClone = group(1, 1, {"C4"});
    changedClone["velocity"] = 20;
    fc::Project cloned;
    check(
        fc::agentApply(
            sharedEdit,
            plan(
                {QJsonObject{
                     {"op", "phrase.clone"}, {"track", 3}, {"phrase", "Chords"}, {"name", "Variation"}},
                 QJsonObject{
                     {"op", "clip.set"}, {"track", 3}, {"clip", double(secondClip)}, {"phrase", "Variation"}},
                 notes("Variation", {changedClone})}),
            cloned, response),
        "clone, redirect one clip and edit its independent variation in one plan");
    const auto* original = findNote(cloned, 2, 0, 0, 60);
    const auto* variation = findNote(cloned, 2, 1, 0, 60);
    check(original && variation && close(original->velocity, 73 / 127.) &&
              close(variation->velocity, 20 / 127.) && original->overrides.tone == .8 &&
              variation->overrides.tone == .8 && cloned.tracks[2].clips[0].pattern == 0 &&
              cloned.tracks[2].clips[1].pattern == 1,
          "clone copies sound settings but subsequent changes affect only the redirected clip");
    const QJsonObject shrink{{"op", "phrase.resize"}, {"track", 3}, {"phrase", "Chords"}, {"bars", 1}};
    reject(p, plan({shrink}), "WOULD_DELETE_NOTES", "shrinking notes requires explicit truncation");
    auto truncate = shrink;
    truncate["truncate"] = true;
    fc::Project shortened;
    check(fc::agentApply(p, plan({truncate}), shortened, response) &&
              shortened.tracks[2].patterns[0].bars == 1 &&
              shortened.tracks[2].patterns[0].steps.size() == 16 && findNote(shortened, 2, 0, 0, 60) &&
              !findNote(shortened, 2, 0, 63, 127),
          "explicit truncation retains earlier notes and drops only the removed bars");
    reject(p,
           plan({QJsonObject{
               {"op", "clip.create"}, {"track", 3}, {"phrase", "Chords"}, {"bar", 4}, {"bars", 1}}}),
           "INVALID_PROJECT", "final overlapping clips are rejected");
    reject(p,
           plan({QJsonObject{
               {"op", "clip.create"}, {"track", 3}, {"phrase", "Chords"}, {"bar", 5}, {"bars", 1}}}),
           "INVALID_PROJECT", "clips beyond the final song length are rejected");
    reject(
        p,
        plan({QJsonObject{{"op", "clip.remove"}, {"track", 1}, {"clip", double(p.tracks[2].clips[0].id)}}}),
        "CLIP_NOT_FOUND", "clip edits cannot accidentally address another track's ID");
    reject(p, plan({QJsonObject{{"op", "song.set"}, {"bars", 100}, {"tempo", 40}, {"tail", 1}}}),
           "INVALID_PROJECT", "final song duration includes the effect tail in its ten-minute limit");
    fc::Project durationBoundary;
    check(fc::agentApply(p,
                         plan({QJsonObject{{"op", "song.set"}, {"bars", 100}, {"tempo", 40}, {"tail", 0}}}),
                         durationBoundary, response),
          "exact ten-minute song without a tail is valid");

    const auto revision = fc::agentRevision(p);
    check(revision.size() == 64 && fc::agentRevision(p) == fc::agentRevision(fc::Project(p)),
          "revision is a deterministic complete project fingerprint");
    using Change = std::function<void(fc::Project&)>;
    const std::vector<Change> changes{
        [](auto& v) { v.title += " changed"; },
        [](auto& v) { v.bpm += 1; },
        [](auto& v) { v.songBars += 1; },
        [](auto& v) { v.master += .01; },
        [](auto& v) { v.delay += .01; },
        [](auto& v) { v.duck += .01; },
        [](auto& v) { v.tailSeconds += .1; },
        [](auto& v) { v.metronome = !v.metronome; },
        [](auto& v) { v.tracks[2].name += " changed"; },
        [](auto& v) { v.tracks[2].instrument = 2; },
        [](auto& v) { v.tracks[2].volume -= .01; },
        [](auto& v) { v.tracks[2].pan += .01; },
        [](auto& v) { v.tracks[2].tone += .01; },
        [](auto& v) { v.tracks[2].decay += .01; },
        [](auto& v) { v.tracks[2].drive += .01; },
        [](auto& v) { v.tracks[2].mute = true; },
        [](auto& v) { v.tracks[2].solo = true; },
        [](auto& v) { v.tracks[2].samplePath = "/sample.wav"; },
        [](auto& v) { v.tracks[2].patterns[0].name += " changed"; },
        [](auto& v) {
            v.tracks[2].patterns[0].bars = 3;
            v.tracks[2].patterns[0].steps.resize(48);
        },
        [](auto& v) { v.tracks[2].patterns[0].steps[0].notes[0].velocity += .01; },
        [](auto& v) { v.tracks[2].patterns[0].steps[0].notes[0].note = 61; },
        [](auto& v) { v.tracks[2].patterns[0].steps[0].notes[0].overrides.level = .9; },
        [](auto& v) { v.tracks[2].patterns[0].steps[0].notes[0].overrides.pan = .1; },
        [](auto& v) { v.tracks[2].patterns[0].steps[0].notes[0].overrides.tone = .1; },
        [](auto& v) { v.tracks[2].patterns[0].steps[0].notes[0].overrides.decay = .1; },
        [](auto& v) { v.tracks[2].patterns[0].steps[0].notes[0].overrides.drive = .1; },
        [](auto& v) { v.tracks[2].clips[0].id += 100; },
        [](auto& v) {
            auto copy = v.tracks[2].patterns[0];
            v.tracks[2].patterns.push_back(copy);
            v.tracks[2].clips[0].pattern = 1;
        },
        [](auto& v) {
            v.tracks[2].clips[0].startBar = 1;
            v.tracks[2].clips[0].lengthBars = 3;
        },
        [](auto& v) { v.tracks[2].clips[0].lengthBars = 3; }};
    bool everyFieldTracked = true;
    for (const auto& change : changes) {
        auto changed = p;
        change(changed);
        everyFieldTracked &= fc::agentRevision(changed) != revision;
    }
    check(everyFieldTracked, "revision changes for every song, track, phrase, note sound and clip setting");
    auto duplicated = p;
    duplicated.tracks[2].patterns.push_back(duplicated.tracks[2].patterns.front());
    reject(duplicated, plan({notes("Chords", {group(1, 1, {"C4"})})}), "AMBIGUOUS_PHRASE",
           "duplicate human-created phrase names never silently select the wrong phrase");
    auto byId = notes("Chords", {group(1, 1, {"C4"})});
    byId["phrase"] = 2;
    byId["notes"] =
        QJsonArray{QJsonObject{{"bar", 1}, {"step", 1}, {"pitches", QJsonArray{60}}, {"velocity", 19}}};
    fc::Project editedDuplicate;
    check(fc::agentApply(duplicated, plan({byId}), editedDuplicate, response) &&
              close(editedDuplicate.tracks[2].patterns[0].steps[0].notes[0].velocity, 73 / 127.) &&
              close(editedDuplicate.tracks[2].patterns[1].steps[0].notes[0].velocity, 19 / 127.),
          "numeric phrase IDs edit exactly the intended phrase even when GUI names duplicate");
    const auto duplicatePhrases =
        fc::agentInspect(duplicated)["tracks"].toArray()[2].toObject()["phrases"].toArray();
    check(duplicatePhrases[0].toObject()["id"].toInt() == 1 &&
              duplicatePhrases[1].toObject()["id"].toInt() == 2,
          "inspect exposes unambiguous 1-based phrase IDs for imported projects");
    const auto schema = fc::agentSchema();
    check(schema["additionalProperties"] == false && schema["properties"]
                                                             .toObject()["operations"]
                                                             .toObject()["items"]
                                                             .toObject()["oneOf"]
                                                             .toArray()
                                                             .size() == 12,
          "machine-readable schema enumerates the supported operations with closed objects");
    std::cout << "Agent tests: " << failures << " failures\n";
    return failures;
}
