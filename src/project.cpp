#include "project.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace fc {
namespace {
constexpr uint64_t MaxClipId = 9007199254740991ULL; // JSON's largest exact integer.
constexpr qint64 MaxProjectBytes = 16 * 1024 * 1024;
int frameAtStep(const Project& p, int step) {
    return int(std::llround(double(step) * 60.0 * SampleRate / (p.bpm * 4.0)));
}
bool number(const QJsonObject& o, const char* key, double low, double high, double& dest) {
    auto v = o.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < low || v.toDouble() > high)
        return false;
    dest = v.toDouble();
    return true;
}
bool integer(const QJsonObject& o, const char* key, int low, int high, int& dest) {
    double value;
    if (!number(o, key, low, high, value) || std::floor(value) != value)
        return false;
    dest = int(value);
    return true;
}
bool readSteps(const QJsonArray& source, std::vector<Step>& steps, int count, bool chords = false) {
    if (source.size() != count)
        return false;
    steps.assign(count, {});
    for (int s = 0; s < count; ++s) {
        if (chords ? !source[s].isArray() : !source[s].isObject())
            return false;
        const auto notes = chords ? source[s].toArray() : QJsonArray{source[s]};
        if (notes.size() > MaxNotesPerStep)
            return false;
        std::set<int> pitches;
        for (const auto& value : notes) {
            if (!value.isObject())
                return false;
            const auto obj = value.toObject();
            Note note;
            if (!number(obj, "velocity", 0, 1, note.velocity) ||
                !integer(obj, "note", 0, 127, note.note) || !pitches.insert(note.note).second)
                return false;
            steps[s].notes.push_back(note);
        }
    }
    return true;
}
uint64_t eventIdentity(int track, uint64_t clip, int step, int voice = 0) {
    // SplitMix64 keeps noise seeds stable when unrelated clips are edited or inserted.
    uint64_t value = clip ^ (uint64_t(track + 1) << 56) ^ (uint64_t(step + 1) * 0x9e3779b97f4a7c15ULL);
    // Keep legacy single-note noise seeds; extra pitches get distinct, stable seeds.
    value ^= uint64_t(voice) * 0xd6e8feb86659fd93ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
} // namespace

int songFrames(const Project& p) { return frameAtStep(p, p.songBars * Steps); }
int renderFrames(const Project& p) { return songFrames(p) + int(std::llround(p.tailSeconds * SampleRate)); }
int loopFrames(const Project& p) { return renderFrames(p); }
int contentEndBar(const Project& p) {
    int end = 1;
    for (const auto& track : p.tracks)
        for (const auto& clip : track.clips)
            end = std::max(end, clip.startBar + clip.lengthBars);
    return end;
}
uint64_t nextClipId(const Project& p) {
    uint64_t id = 1;
    for (const auto& track : p.tracks)
        for (const auto& clip : track.clips)
            id = std::max(id, clip.id + 1);
    if (id <= MaxClipId)
        return id;
    for (id = 1; id <= MaxClipId; ++id) {
        bool used = false;
        for (const auto& track : p.tracks)
            for (const auto& clip : track.clips)
                used = used || clip.id == id;
        if (!used)
            return id;
    }
    return 0;
}
QString noteName(int midi) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    midi = std::clamp(midi, 0, 127);
    return QString("%1%2").arg(names[midi % 12]).arg(midi / 12 - 1);
}
QString instrumentName(int instrument) {
    static const char* names[] = {"Kick", "Bass", "Saw", "Lead", "Clap", "Hats"};
    return names[std::clamp(instrument, 0, TrackCount - 1)];
}
std::vector<ScheduledStep> scheduledSteps(const Project& p, int trackIndex) {
    std::vector<ScheduledStep> events;
    if (trackIndex < 0 || trackIndex >= TrackCount)
        return events;
    const auto& track = p.tracks[trackIndex];
    for (const auto& clip : track.clips) {
        if (clip.pattern < 0 || clip.pattern >= int(track.patterns.size()))
            continue;
        const auto& pattern = track.patterns[clip.pattern];
        if (pattern.steps.empty())
            continue;
        const int endBar = std::min(p.songBars, clip.startBar + clip.lengthBars);
        for (int local = 0; local < (endBar - clip.startBar) * Steps; ++local) {
            const int absoluteStep = clip.startBar * Steps + local;
            if (absoluteStep < 0)
                continue;
            const auto& step = pattern.steps[size_t(local) % pattern.steps.size()];
            for (size_t n = 0; n < step.notes.size(); ++n) {
                const auto& note = step.notes[n];
                if (note.velocity <= 0)
                    continue;
                events.push_back({frameAtStep(p, absoluteStep), note,
                                  eventIdentity(trackIndex, clip.id, local, n == 0 ? 0 : note.note + 1)});
            }
        }
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.frame < b.frame; });
    return events;
}
Project emptyProject() {
    Project p;
    p.title = "Untitled song";
    p.bpm = 120;
    p.songBars = 16;
    p.duck = .35;
    const double volumes[] = {.8, .55, .24, .24, .42, .26};
    for (int t = 0; t < TrackCount; ++t) {
        auto& track = p.tracks[t];
        track.name = instrumentName(t);
        track.instrument = t;
        track.volume = volumes[t];
        track.drive = .2;
    }
    p.tracks[0].decay = .35;
    p.tracks[4].decay = .25;
    p.tracks[5].decay = .14;
    p.tracks[5].pan = .15;
    return p;
}
Project demoProject() {
    Project p = emptyProject();
    p.title = "First light";
    p.delay = .12;
    uint64_t id = 1;
    auto phrase = [](const QString& name, int bars) {
        Pattern pat;
        pat.name = name;
        pat.bars = bars;
        pat.steps.assign(bars * Steps, {});
        return pat;
    };
    auto& kick = p.tracks[0];
    kick.patterns.push_back(phrase("Pulse", 1));
    for (int s : {0, 8})
        kick.patterns[0].steps[s] = {.85, 36};
    kick.clips.push_back({id++, 0, 0, 14});
    auto& bass = p.tracks[1];
    bass.patterns.push_back(phrase("Changes", 4));
    const int roots[] = {36, 33, 41, 43}; // C, A, F, G.
    for (int bar = 0; bar < 4; ++bar)
        for (int s : {0, 6, 10, 14})
            bass.patterns[0].steps[bar * Steps + s] = {.66, roots[bar]};
    bass.clips.push_back({id++, 0, 4, 10});
    auto& synth = p.tracks[2];
    synth.name = "Synth";
    synth.tone = .33;
    synth.pan = -.18;
    synth.patterns.push_back(phrase("Arpeggio", 4));
    const int chords[4][4] = {{60, 64, 67, 64}, {57, 60, 64, 60}, {53, 57, 60, 57}, {55, 59, 62, 59}};
    for (int bar = 0; bar < 4; ++bar)
        for (int s = 0; s < 8; ++s)
            synth.patterns[0].steps[bar * Steps + s * 2] = {s % 2 ? .38 : .50, chords[bar][s % 4]};
    synth.clips.push_back({id++, 0, 4, 10});
    auto& lead = p.tracks[3];
    lead.pan = .15;
    lead.patterns.push_back(phrase("Melody", 4));
    const int melody[4][4] = {{72, 76, 79, 76}, {76, 72, 69, 72}, {69, 72, 77, 76}, {74, 71, 67, 72}};
    for (int bar = 0; bar < 4; ++bar)
        for (int s = 0; s < 4; ++s)
            lead.patterns[0].steps[bar * Steps + s * 4] = {.48, melody[bar][s]};
    lead.clips.push_back({id++, 0, 8, 8});
    auto& clap = p.tracks[4];
    clap.patterns.push_back(phrase("Backbeat", 1));
    for (int s : {4, 12})
        clap.patterns[0].steps[s] = {.7, 60};
    clap.clips.push_back({id++, 0, 4, 10});
    auto& hats = p.tracks[5];
    hats.patterns.push_back(phrase("Eighths", 1));
    for (int s = 0; s < Steps; s += 2)
        hats.patterns[0].steps[s] = {s % 4 ? .48 : .3, 72};
    hats.clips.push_back({id++, 0, 0, 14});
    return p;
}
QJsonObject projectToJson(const Project& p) {
    QJsonArray tracks;
    for (const auto& t : p.tracks) {
        QJsonArray patterns, clips;
        for (const auto& pattern : t.patterns) {
            QJsonArray steps;
            for (const auto& s : pattern.steps) {
                QJsonArray notes;
                for (const auto& note : s.notes)
                    notes.append(QJsonObject{{"velocity", note.velocity}, {"note", note.note}});
                steps.append(notes);
            }
            patterns.append(QJsonObject{{"name", pattern.name}, {"bars", pattern.bars}, {"steps", steps}});
        }
        for (const auto& clip : t.clips)
            clips.append(QJsonObject{{"id", double(clip.id)}, {"pattern", clip.pattern},
                                     {"startBar", clip.startBar}, {"lengthBars", clip.lengthBars}});
        tracks.append(QJsonObject{{"name", t.name}, {"instrument", t.instrument}, {"volume", t.volume},
                                  {"pan", t.pan}, {"tone", t.tone}, {"decay", t.decay}, {"drive", t.drive},
                                  {"mute", t.mute}, {"solo", t.solo}, {"sample", t.samplePath},
                                  {"patterns", patterns}, {"clips", clips}});
    }
    return {{"format", "ferro-project"}, {"version", 3}, {"title", p.title}, {"bpm", p.bpm},
            {"master", p.master}, {"delay", p.delay}, {"duck", p.duck}, {"metronome", p.metronome},
            {"songBars", p.songBars}, {"tailSeconds", p.tailSeconds}, {"tracks", tracks}};
}
bool projectFromJson(const QJsonObject& o, Project& out, QString& error) {
    auto fail = [&](const QString& message) { error = message; return false; };
    int version = 0;
    if (o.value("format") != "ferro-project" || !integer(o, "version", 1, 3, version))
        return fail("Unsupported project format/version.");
    Project p;
    if (!o.value("title").isString() || o.value("title").toString().size() > 256)
        return fail("Invalid song title.");
    p.title = o.value("title").toString();
    if (!number(o, "bpm", version == 1 ? 80 : 40, 300, p.bpm) ||
        !number(o, "master", 0, 1, p.master) || !number(o, "delay", 0, 1, p.delay) ||
        !number(o, "duck", 0, 1, p.duck))
        return fail("Tempo or master controls are out of range (tempo: 40–300 BPM).");
    if (!o.value("metronome").isBool())
        return fail("Invalid metronome setting.");
    p.metronome = o.value("metronome").toBool();
    QJsonArray arrangement;
    if (version == 1) {
        if (!o.value("arrangement").isArray())
            return fail("Invalid legacy arrangement.");
        arrangement = o.value("arrangement").toArray();
        if (arrangement.isEmpty() || arrangement.size() > 32)
            return fail("Legacy arrangement must contain 1–32 bars.");
        for (auto bar : arrangement)
            if (!bar.isDouble() || bar.toDouble() != bar.toInt(-1) || bar.toInt(-1) < 0 || bar.toInt(-1) > 3)
                return fail("Invalid legacy arrangement pattern.");
        p.songBars = arrangement.size();
        p.tailSeconds = 2;
    } else if (!integer(o, "songBars", 1, MaxSongBars, p.songBars) ||
               !number(o, "tailSeconds", 0, 8, p.tailSeconds)) {
        return fail("Song length must be 1–256 bars and the tail 0–8 seconds.");
    }
    if (p.songBars * 240.0 / p.bpm + p.tailSeconds > MaxSongSeconds)
        return fail("Song and effect tail must fit within 10 minutes. Increase tempo or shorten the song.");
    if (!o.value("tracks").isArray() || o.value("tracks").toArray().size() != TrackCount)
        return fail("A song needs six tracks.");
    auto tracks = o.value("tracks").toArray();
    std::set<uint64_t> ids;
    uint64_t migrationId = 1;
    for (int i = 0; i < TrackCount; ++i) {
        if (!tracks[i].isObject())
            return fail("Invalid track.");
        auto obj = tracks[i].toObject();
        auto& t = p.tracks[i];
        if (!obj.value("name").isString() || obj.value("name").toString().size() > 80 ||
            !obj.value("sample").isString() || obj.value("sample").toString().size() > 4096)
            return fail("Invalid track name or sample path.");
        t.name = obj.value("name").toString();
        t.samplePath = obj.value("sample").toString();
        t.instrument = i;
        if (version >= 2 && !integer(obj, "instrument", 0, TrackCount - 1, t.instrument))
            return fail("Invalid instrument.");
        if (!number(obj, "volume", 0, 1.5, t.volume) || !number(obj, "pan", -1, 1, t.pan) ||
            !number(obj, "tone", 0, 1, t.tone) || !number(obj, "decay", 0, 1, t.decay) ||
            !number(obj, "drive", 0, 1, t.drive))
            return fail("Track controls are out of range.");
        if (!obj.value("mute").isBool() || !obj.value("solo").isBool())
            return fail("Invalid track mute/solo.");
        t.mute = obj.value("mute").toBool();
        t.solo = obj.value("solo").toBool();
        if (!obj.value("patterns").isArray())
            return fail("Invalid phrases.");
        auto patterns = obj.value("patterns").toArray();
        if ((version == 1 && patterns.size() != 4) || patterns.size() > 128)
            return fail("Each track supports up to 128 phrases.");
        t.patterns.clear();
        for (int b = 0; b < patterns.size(); ++b) {
            Pattern pattern;
            if (version == 1) {
                pattern.name = QString("Imported %1").arg(QChar('A' + b));
                if (!patterns[b].isArray() || !readSteps(patterns[b].toArray(), pattern.steps, Steps))
                    return fail("Invalid legacy phrase steps.");
            } else {
                if (!patterns[b].isObject())
                    return fail("Invalid phrase.");
                auto source = patterns[b].toObject();
                if (!source.value("name").isString() || source.value("name").toString().size() > 80 ||
                    !integer(source, "bars", 1, 16, pattern.bars))
                    return fail("Phrase names must fit 80 characters and phrase lengths must be 1–16 bars.");
                pattern.name = source.value("name").toString();
                if (!source.value("steps").isArray() ||
                    !readSteps(source.value("steps").toArray(), pattern.steps, pattern.bars * Steps, version >= 3))
                    return fail("Invalid phrase steps: expected unique MIDI notes (0–127) with velocity 0–1.");
            }
            t.patterns.push_back(std::move(pattern));
        }
        if (version == 1) {
            // Preserve every legacy pattern and group consecutive uses into stretched clips.
            for (int bar = 0; bar < p.songBars;) {
                int end = bar + 1;
                int pattern = arrangement[bar].toInt();
                while (end < p.songBars && arrangement[end].toInt() == pattern)
                    ++end;
                t.clips.push_back({migrationId++, pattern, bar, end - bar});
                bar = end;
            }
        } else {
            if (!obj.value("clips").isArray() || obj.value("clips").toArray().size() > 256)
                return fail("Each track supports up to 256 clips.");
            for (auto source : obj.value("clips").toArray()) {
                if (!source.isObject())
                    return fail("Invalid clip.");
                auto c = source.toObject();
                Clip clip;
                double id;
                if (!number(c, "id", 1, double(MaxClipId), id) || std::floor(id) != id ||
                    !integer(c, "pattern", 0, int(t.patterns.size()) - 1, clip.pattern) ||
                    !integer(c, "startBar", 0, p.songBars - 1, clip.startBar) ||
                    !integer(c, "lengthBars", 1, p.songBars, clip.lengthBars) ||
                    clip.startBar + clip.lengthBars > p.songBars)
                    return fail("Clip identity, phrase or range is invalid.");
                clip.id = uint64_t(id);
                if (!ids.insert(clip.id).second)
                    return fail("Clip identifiers must be unique throughout the song.");
                t.clips.push_back(clip);
            }
            auto ordered = t.clips;
            std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.startBar < b.startBar; });
            for (size_t c = 1; c < ordered.size(); ++c)
                if (ordered[c].startBar < ordered[c - 1].startBar + ordered[c - 1].lengthBars)
                    return fail("Clips on the same track cannot overlap.");
        }
    }
    out = std::move(p);
    error.clear();
    return true;
}
bool saveProject(const QString& path, const Project& p, QString& error) {
    Project validated;
    if (!projectFromJson(projectToJson(p), validated, error))
        return false;
    QJsonObject obj = projectToJson(p);
    QJsonArray tracks = obj["tracks"].toArray();
    QDir dir = QFileInfo(path).absoluteDir();
    for (int i = 0; i < TrackCount; ++i) {
        if (!p.tracks[i].samplePath.isEmpty()) {
            auto t = tracks[i].toObject();
            t["sample"] = dir.relativeFilePath(p.tracks[i].samplePath);
            tracks[i] = t;
        }
    }
    obj["tracks"] = tracks;
    QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    if (data.size() > MaxProjectBytes) {
        error = "Project exceeds 16 MiB.";
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    error.clear();
    return true;
}
bool loadProject(const QString& path, Project& p, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = file.errorString();
        return false;
    }
    if (file.size() > MaxProjectBytes) {
        error = "Project exceeds 16 MiB.";
        return false;
    }
    QJsonParseError parse;
    auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        error = "Invalid JSON: " + parse.errorString();
        return false;
    }
    Project candidate;
    if (!projectFromJson(doc.object(), candidate, error))
        return false;
    QDir dir = QFileInfo(path).absoluteDir();
    for (auto& t : candidate.tracks)
        if (!t.samplePath.isEmpty())
            t.samplePath = QFileInfo(dir.absoluteFilePath(t.samplePath)).absoluteFilePath();
    p = std::move(candidate);
    error.clear();
    return true;
}
} // namespace fc
