#include "agent.h"
#include "agent_ipc.h"
#include "dsp.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace fc {
namespace {
constexpr qint64 MaxInputBytes = 16 * 1024 * 1024;
constexpr int MaxOperations = 4096;
struct InputError : std::runtime_error {
    QString code, text;
    InputError(QString c, QString t) : std::runtime_error(t.toStdString()), code(c), text(t) {}
};
[[noreturn]] void invalid(QString text, QString code = "INVALID_ARGUMENT") {
    throw InputError(code, text);
}
void keys(const QJsonObject& o, const QStringList& allowed) {
    for (auto it = o.begin(); it != o.end(); ++it)
        if (!allowed.contains(it.key()))
            invalid("Unknown field '" + it.key() + "'. Run ferro-agent schema for supported fields.",
                    "UNKNOWN_FIELD");
}
double number(const QJsonObject& o, const QString& key, double low, double high) {
    auto v = o.value(key);
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < low || v.toDouble() > high)
        invalid(QString("'%1' must be a number in [%2, %3].").arg(key).arg(low).arg(high));
    return v.toDouble();
}
int integer(const QJsonObject& o, const QString& key, int low, int high) {
    double n = number(o, key, low, high);
    if (std::floor(n) != n)
        invalid("'" + key + "' must be an integer (coordinates start at 1).");
    return int(n);
}
QString string(const QJsonObject& o, const QString& key, int max = 80) {
    if (!o.value(key).isString() || o.value(key).toString().trimmed().isEmpty() ||
        o.value(key).toString().size() > max)
        invalid(QString("'%1' must be a nonempty string of at most %2 characters.").arg(key).arg(max));
    return o.value(key).toString();
}
bool boolean(const QJsonObject& o, const QString& key) {
    if (!o.value(key).isBool())
        invalid("'" + key + "' must be true or false.");
    return o.value(key).toBool();
}
int trackIndex(const QJsonObject& o) {
    return integer(o, "track", 1, TrackCount) - 1;
}
int phraseIndex(const Track& t, const QJsonObject& o) {
    if (o.value("phrase").isDouble())
        return integer(o, "phrase", 1, int(t.patterns.size())) - 1;
    QString name = string(o, "phrase");
    int found = -1;
    for (size_t i = 0; i < t.patterns.size(); ++i)
        if (t.patterns[i].name == name) {
            if (found >= 0)
                invalid("Phrase name '" + name + "' is ambiguous. Use the numeric phrase ID from inspect.",
                        "AMBIGUOUS_PHRASE");
            found = int(i);
        }
    if (found < 0)
        invalid("Phrase '" + name + "' does not exist on this track. Inspect the project or create it first.",
                "PHRASE_NOT_FOUND");
    return found;
}
void uniqueName(const Track& t, const QString& name) {
    if (std::any_of(t.patterns.begin(), t.patterns.end(), [&](const Pattern& p) { return p.name == name; }))
        invalid("Phrase '" + name + "' already exists. Use notes.set to edit it or choose another name.",
                "PHRASE_EXISTS");
    if (t.patterns.size() >= 128)
        invalid("Each track supports at most 128 phrases.");
}
uint64_t clipId(const QJsonObject& o) {
    double id = number(o, "clip", 1, 9007199254740991.0);
    if (std::floor(id) != id)
        invalid("'clip' must be an integer ID returned by inspect or clip.create.");
    return uint64_t(id);
}
int pitch(const QJsonValue& v) {
    if (v.isDouble()) {
        double n = v.toDouble();
        if (std::isfinite(n) && n >= 0 && n <= 127 && std::floor(n) == n)
            return int(n);
    } else if (v.isString()) {
        static const QRegularExpression re("^([A-Ga-g])([#b]?)(-?[0-9]{1,2})$");
        auto m = re.match(v.toString());
        if (m.hasMatch()) {
            const QString names = "C D EF G A B";
            int semitone = names.indexOf(m.captured(1).toUpper());
            int accidental = m.captured(2) == "#" ? 1 : m.captured(2) == "b" ? -1 : 0;
            int n = (m.captured(3).toInt() + 1) * 12 + semitone + accidental;
            if (n >= 0 && n <= 127)
                return n;
        }
    }
    invalid("Pitch must be an integer MIDI note 0–127 or a note name such as C4, F#3 or Bb2 (C4 = 60).");
}
QJsonObject soundJson(const NoteOverrides& o) {
    QJsonObject s;
    auto add = [&](QString key, const std::optional<double>& value) {
        if (value)
            s.insert(key, *value * 100);
    };
    add("level", o.level);
    add("pan", o.pan);
    add("tone", o.tone);
    add("decay", o.decay);
    add("drive", o.drive);
    return s;
}
void patchSound(NoteOverrides& out, const QJsonObject& sound) {
    keys(sound, {"level", "pan", "tone", "decay", "drive"});
    for (auto it = sound.begin(); it != sound.end(); ++it) {
        auto* field = it.key() == "level"   ? &out.level
                      : it.key() == "pan"   ? &out.pan
                      : it.key() == "tone"  ? &out.tone
                      : it.key() == "decay" ? &out.decay
                                            : &out.drive;
        if (it.value().isNull())
            field->reset();
        else
            *field =
                number(sound, it.key(), it.key() == "pan" ? -100 : 0, it.key() == "level" ? 150 : 100) / 100.;
    }
}
QJsonArray usages(const Track& t, int pattern) {
    QJsonArray a;
    for (const auto& c : t.clips)
        if (c.pattern == pattern)
            a.append(double(c.id));
    return a;
}
QJsonObject execute(Project& p, const QJsonObject& o) {
    QString op = string(o, "op");
    QJsonObject result{{"op", op}};
    if (op == "song.set") {
        keys(o, {"op", "title", "tempo", "bars", "tail", "output", "delay", "bass_duck", "click"});
        if (o.contains("title"))
            p.title = string(o, "title", 256);
        if (o.contains("tempo"))
            p.bpm = number(o, "tempo", 40, 300);
        if (o.contains("bars"))
            p.songBars = integer(o, "bars", 1, MaxSongBars);
        if (o.contains("tail"))
            p.tailSeconds = number(o, "tail", 0, 8);
        if (o.contains("output"))
            p.master = number(o, "output", 0, 100) / 100.;
        if (o.contains("delay"))
            p.delay = number(o, "delay", 0, 100) / 100.;
        if (o.contains("bass_duck"))
            p.duck = number(o, "bass_duck", 0, 100) / 100.;
        if (o.contains("click"))
            p.metronome = boolean(o, "click");
    } else if (op == "song.fit") {
        keys(o, {"op"});
        p.songBars = std::max(1, contentEndBar(p));
    } else if (op == "track.set") {
        keys(o,
             {"op", "track", "name", "instrument", "level", "pan", "tone", "decay", "drive", "mute", "solo"});
        auto& t = p.tracks[trackIndex(o)];
        if (o.contains("name"))
            t.name = string(o, "name");
        if (o.contains("instrument")) {
            auto name = string(o, "instrument");
            int type = -1;
            for (int i = 0; i < TrackCount; ++i)
                if (instrumentName(i).compare(name, Qt::CaseInsensitive) == 0)
                    type = i;
            if (type < 0)
                invalid("Instrument must be Kick, Bass, Saw, Lead, Clap or Hats.");
            t.instrument = type;
            // Choosing an instrument explicitly restores it, as Use instrument does in the GUI.
            t.samplePath.clear();
        }
        if (o.contains("level"))
            t.volume = number(o, "level", 0, 150) / 100.;
        if (o.contains("pan"))
            t.pan = number(o, "pan", -100, 100) / 100.;
        if (o.contains("tone"))
            t.tone = number(o, "tone", 0, 100) / 100.;
        if (o.contains("decay"))
            t.decay = number(o, "decay", 0, 100) / 100.;
        if (o.contains("drive"))
            t.drive = number(o, "drive", 0, 100) / 100.;
        if (o.contains("mute"))
            t.mute = boolean(o, "mute");
        if (o.contains("solo"))
            t.solo = boolean(o, "solo");
    } else if (op == "phrase.create" || op == "phrase.clone") {
        keys(o, op == "phrase.create" ? QStringList{"op", "track", "name", "bars"}
                                      : QStringList{"op", "track", "phrase", "name"});
        auto& t = p.tracks[trackIndex(o)];
        auto name = string(o, "name");
        uniqueName(t, name);
        Pattern pattern;
        if (op == "phrase.clone")
            pattern = t.patterns[phraseIndex(t, o)];
        else {
            pattern.bars = integer(o, "bars", 1, 16);
            pattern.steps.resize(pattern.bars * Steps);
        }
        pattern.name = name;
        t.patterns.push_back(std::move(pattern));
        result["phrase"] = name;
        result["phrase_id"] = int(t.patterns.size());
    } else if (op == "phrase.resize" || op == "phrase.clear") {
        keys(o, op == "phrase.clear" ? QStringList{"op", "track", "phrase"}
                                     : QStringList{"op", "track", "phrase", "bars", "truncate"});
        auto& t = p.tracks[trackIndex(o)];
        int index = phraseIndex(t, o);
        auto& phrase = t.patterns[index];
        if (op == "phrase.clear")
            phrase.steps.assign(phrase.bars * Steps, {});
        else {
            int bars = integer(o, "bars", 1, 16);
            bool truncate = o.contains("truncate") && boolean(o, "truncate");
            if (bars < phrase.bars && !truncate &&
                std::any_of(
                    phrase.steps.begin() + bars * Steps, phrase.steps.end(),
                    [](const Step& s) { return !s.notes.empty(); }))
                invalid("Shrinking this phrase would delete notes. Set 'truncate': true explicitly.",
                        "WOULD_DELETE_NOTES");
            phrase.steps.resize(bars * Steps);
            phrase.bars = bars;
        }
        result["affected_clips"] = usages(t, index);
    } else if (op == "notes.set" || op == "notes.remove") {
        keys(o, {"op", "track", "phrase", "notes"});
        auto& t = p.tracks[trackIndex(o)];
        int index = phraseIndex(t, o);
        auto& phrase = t.patterns[index];
        if (!o.value("notes").isArray() || o.value("notes").toArray().size() > 8192)
            invalid("'notes' must be an array of at most 8192 note groups.");
        std::set<std::pair<int, int>> touched;
        int changed = 0;
        for (auto value : o.value("notes").toArray()) {
            if (!value.isObject())
                invalid("Each note group must be an object.");
            auto n = value.toObject();
            keys(n, op == "notes.set" ? QStringList{"bar", "step", "pitches", "velocity", "sound"}
                                      : QStringList{"bar", "step", "pitches"});
            int step = (integer(n, "bar", 1, phrase.bars) - 1) * Steps + integer(n, "step", 1, Steps) - 1;
            if (!n.value("pitches").isArray() || n.value("pitches").toArray().empty() ||
                n.value("pitches").toArray().size() > MaxNotesPerStep)
                invalid("'pitches' must contain 1–128 MIDI numbers or note names.");
            int velocity = n.contains("velocity") ? integer(n, "velocity", 0, 127) : 100;
            if (n.contains("sound") && !n.value("sound").isObject())
                invalid("'sound' must be an object.");
            for (auto pv : n.value("pitches").toArray()) {
                int midi = pitch(pv);
                if (!touched.emplace(step, midi).second)
                    invalid("Duplicate pitch at the same bar/step in this notes operation.",
                            "DUPLICATE_NOTE");
                auto& notes = phrase.steps[step].notes;
                auto existing = std::find_if(notes.begin(), notes.end(),
                                             [midi](const Note& note) { return note.note == midi; });
                if (op == "notes.remove") {
                    if (existing == notes.end())
                        invalid("Cannot remove a note that does not exist.", "NOTE_NOT_FOUND");
                    notes.erase(existing);
                } else {
                    if (existing == notes.end()) {
                        notes.push_back(Note{velocity / 127., midi});
                        existing = std::prev(notes.end());
                    } else if (n.contains("velocity"))
                        existing->velocity = velocity / 127.;
                    if (n.contains("sound"))
                        patchSound(existing->overrides, n.value("sound").toObject());
                }
                ++changed;
            }
        }
        result["notes"] = changed;
        result["affected_clips"] = usages(t, index);
    } else if (op == "clip.create" || op == "clip.set" || op == "clip.remove") {
        keys(o, op == "clip.create" ? QStringList{"op", "track", "phrase", "bar", "bars"}
                : op == "clip.set"  ? QStringList{"op", "track", "clip", "phrase", "bar", "bars"}
                                    : QStringList{"op", "track", "clip"});
        auto& t = p.tracks[trackIndex(o)];
        if (op == "clip.create") {
            if (t.clips.size() >= 256)
                invalid("Each track supports at most 256 clips.");
            auto id = nextClipId(p);
            if (id == 0)
                invalid("No unused clip IDs remain.");
            t.clips.push_back({id, phraseIndex(t, o), integer(o, "bar", 1, MaxSongBars) - 1,
                               integer(o, "bars", 1, MaxSongBars)});
            result["clip"] = double(id);
        } else {
            auto id = clipId(o);
            auto c = std::find_if(t.clips.begin(), t.clips.end(),
                                  [id](const Clip& clip) { return clip.id == id; });
            if (c == t.clips.end())
                invalid("Clip ID is not on this track. Inspect current clip IDs.", "CLIP_NOT_FOUND");
            if (op == "clip.remove")
                t.clips.erase(c);
            else {
                if (o.contains("phrase"))
                    c->pattern = phraseIndex(t, o);
                if (o.contains("bar"))
                    c->startBar = integer(o, "bar", 1, MaxSongBars) - 1;
                if (o.contains("bars"))
                    c->lengthBars = integer(o, "bars", 1, MaxSongBars);
            }
            result["clip"] = double(id);
        }
    } else
        invalid("Unknown operation '" + op + "'. Run ferro-agent schema.", "UNKNOWN_OPERATION");
    return result;
}
QJsonObject property(QString type, double low, double high) {
    return {{"type", type}, {"minimum", low}, {"maximum", high}};
}
QJsonObject objectSchema(const QJsonObject& properties, const QJsonArray& required) {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", properties},
            {"required", required}};
}
QJsonObject stringSchema(int max = 80) {
    return {{"type", "string"}, {"minLength", 1}, {"maxLength", max}};
}
QJsonObject boolSchema() {
    return {{"type", "boolean"}};
}
} // namespace

QString agentRevision(const Project& p) {
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(projectToJson(p)).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
}
QJsonObject agentError(QString code, QString message, int operation) {
    QJsonObject error{{"code", code}, {"message", message}};
    if (operation >= 0)
        error["operation"] = operation;
    return {{"ok", false}, {"error", error}};
}
QJsonObject agentInspect(const Project& p) {
    QJsonArray tracks;
    for (int i = 0; i < TrackCount; ++i) {
        const auto& t = p.tracks[i];
        QJsonArray phrases, clips;
        for (size_t j = 0; j < t.patterns.size(); ++j) {
            const auto& phrase = t.patterns[j];
            QJsonArray notes;
            for (size_t s = 0; s < phrase.steps.size(); ++s)
                for (const auto& n : phrase.steps[s].notes) {
                    QJsonObject note{{"bar", int(s) / Steps + 1},
                                     {"step", int(s) % Steps + 1},
                                     {"pitch", noteName(n.note)},
                                     {"midi", n.note},
                                     {"velocity", int(std::round(n.velocity * 127))}};
                    auto sound = soundJson(n.overrides);
                    if (!sound.empty())
                        note["sound"] = sound;
                    notes.append(note);
                }
            phrases.append(QJsonObject{{"id", int(j) + 1},
                                       {"name", phrase.name},
                                       {"bars", phrase.bars},
                                       {"notes", notes},
                                       {"used_by", usages(t, int(j))}});
        }
        for (const auto& c : t.clips)
            clips.append(QJsonObject{{"id", double(c.id)},
                                     {"phrase", t.patterns[c.pattern].name},
                                     {"phrase_id", c.pattern + 1},
                                     {"bar", c.startBar + 1},
                                     {"bars", c.lengthBars}});
        tracks.append(QJsonObject{{"track", i + 1},
                                  {"name", t.name},
                                  {"instrument", instrumentName(t.instrument)},
                                  {"level", t.volume * 100},
                                  {"pan", t.pan * 100},
                                  {"tone", t.tone * 100},
                                  {"decay", t.decay * 100},
                                  {"drive", t.drive * 100},
                                  {"mute", t.mute},
                                  {"solo", t.solo},
                                  {"sample", t.samplePath},
                                  {"phrases", phrases},
                                  {"clips", clips}});
    }
    return {{"title", p.title},
            {"tempo", p.bpm},
            {"bars", p.songBars},
            {"tail", p.tailSeconds},
            {"duration_seconds", p.songBars * 240. / p.bpm + p.tailSeconds},
            {"output", p.master * 100},
            {"delay", p.delay * 100},
            {"bass_duck", p.duck * 100},
            {"click", p.metronome},
            {"tracks", tracks}};
}
bool agentApply(const Project& base, const QJsonObject& plan, Project& out, QJsonObject& response) {
    int index = -1;
    try {
        keys(plan, {"version", "operations"});
        integer(plan, "version", 1, 1);
        if (!plan.value("operations").isArray() || plan.value("operations").toArray().empty() ||
            plan.value("operations").toArray().size() > MaxOperations)
            invalid("'operations' must contain 1–4096 operations.");
        Project candidate = base;
        QJsonArray results;
        for (auto value : plan.value("operations").toArray()) {
            ++index;
            if (!value.isObject())
                invalid("Each operation must be an object.");
            results.append(execute(candidate, value.toObject()));
        }
        // Cross-operation constraints describe the final transaction, not
        // necessarily its last operation, so do not blame an unrelated index.
        index = -1;
        Project validated;
        QString error;
        if (!projectFromJson(projectToJson(candidate), validated, error))
            invalid(error, "INVALID_PROJECT");
        if (QJsonDocument(projectToJson(candidate)).toJson(QJsonDocument::Compact).size() > MaxInputBytes)
            invalid("Resulting project exceeds 16 MiB.", "PROJECT_TOO_LARGE");
        response = {{"ok", true},
                    {"revision", agentRevision(candidate)},
                    {"changed", projectToJson(base) != projectToJson(candidate)},
                    {"operations", results}};
        out = std::move(candidate);
        return true;
    } catch (const InputError& e) {
        response = agentError(e.code, e.text, index < 0 ? -1 : index + 1);
        return false;
    }
}

QJsonObject agentSchema() {
    const QJsonObject phraseRef{{"oneOf", QJsonArray{stringSchema(), property("integer", 1, 128)}}};
    const auto track = property("integer", 1, 6), bar = property("integer", 1, 256),
               bars = property("integer", 1, 16);
    const auto level = property("number", 0, 150), percent = property("number", 0, 100),
               pan = property("number", -100, 100);
    auto soundProperties = QJsonObject{
        {"level", level}, {"pan", pan}, {"tone", percent}, {"decay", percent}, {"drive", percent}};
    for (auto it = soundProperties.begin(); it != soundProperties.end(); ++it) {
        auto prop = it.value().toObject();
        prop["type"] = QJsonArray{"number", "null"};
        it.value() = prop;
    }
    const QJsonObject pitchSchema{
        {"oneOf", QJsonArray{property("integer", 0, 127),
                             QJsonObject{{"type", "string"}, {"pattern", "^[A-Ga-g][#b]?-?[0-9]{1,2}$"}}}}};
    const QJsonObject pitches{{"type", "array"}, {"minItems", 1}, {"maxItems", 128}, {"items", pitchSchema}};
    auto noteProps = QJsonObject{{"bar", bars},
                                 {"step", property("integer", 1, 16)},
                                 {"pitches", pitches},
                                 {"velocity", property("integer", 0, 127)},
                                 {"sound", objectSchema(soundProperties, {})}};
    QJsonArray operations;
    auto add = [&](QString op, QJsonObject props, QJsonArray required) {
        props["op"] = QJsonObject{{"const", op}};
        required.prepend("op");
        operations.append(objectSchema(props, required));
    };
    add("song.set",
        {{"title", stringSchema(256)},
         {"tempo", property("number", 40, 300)},
         {"bars", bar},
         {"tail", property("number", 0, 8)},
         {"output", percent},
         {"delay", percent},
         {"bass_duck", percent},
         {"click", boolSchema()}},
        {});
    add("song.fit", {}, {});
    add("track.set",
        {{"track", track},
         {"name", stringSchema()},
         {"instrument", QJsonObject{{"enum", QJsonArray{"Kick", "Bass", "Saw", "Lead", "Clap", "Hats"}}}},
         {"level", level},
         {"pan", pan},
         {"tone", percent},
         {"decay", percent},
         {"drive", percent},
         {"mute", boolSchema()},
         {"solo", boolSchema()}},
        {"track"});
    add("phrase.create", {{"track", track}, {"name", stringSchema()}, {"bars", bars}},
        {"track", "name", "bars"});
    add("phrase.clone", {{"track", track}, {"phrase", phraseRef}, {"name", stringSchema()}},
        {"track", "phrase", "name"});
    add("phrase.resize",
        {{"track", track}, {"phrase", phraseRef}, {"bars", bars}, {"truncate", boolSchema()}},
        {"track", "phrase", "bars"});
    add("phrase.clear", {{"track", track}, {"phrase", phraseRef}}, {"track", "phrase"});
    auto notes = QJsonObject{{"type", "array"},
                             {"maxItems", 8192},
                             {"items", objectSchema(noteProps, {"bar", "step", "pitches"})}};
    add("notes.set", {{"track", track}, {"phrase", phraseRef}, {"notes", notes}},
        {"track", "phrase", "notes"});
    noteProps.remove("velocity");
    noteProps.remove("sound");
    notes["items"] = objectSchema(noteProps, {"bar", "step", "pitches"});
    add("notes.remove", {{"track", track}, {"phrase", phraseRef}, {"notes", notes}},
        {"track", "phrase", "notes"});
    const auto id = property("integer", 1, 9007199254740991.0);
    add("clip.create", {{"track", track}, {"phrase", phraseRef}, {"bar", bar}, {"bars", bar}},
        {"track", "phrase", "bar", "bars"});
    add("clip.set", {{"track", track}, {"clip", id}, {"phrase", phraseRef}, {"bar", bar}, {"bars", bar}},
        {"track", "clip"});
    add("clip.remove", {{"track", track}, {"clip", id}}, {"track", "clip"});
    auto schema = objectSchema({{"version", QJsonObject{{"const", 1}}},
                                {"operations", QJsonObject{{"type", "array"},
                                                           {"minItems", 1},
                                                           {"maxItems", MaxOperations},
                                                           {"items", QJsonObject{{"oneOf", operations}}}}}},
                               {"version", "operations"});
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "Ferro agent plan v1";
    schema["description"] =
        "Atomic operations. Track/bar/step are 1-based, 4/4, 16 steps per bar. C4=60. Controls use GUI "
        "units. No note duration: use decay. Phrase references are unique names or numeric IDs from inspect; "
        "shared edits affect all "
        "using clips. Final state must fit 256 bars and 600 seconds including tail. notes.set upserts; "
        "omitted velocity/sound preserve existing values (new notes default velocity100). sound:null values "
        "restore inheritance. No unknown fields.";
    return schema;
}

namespace {
QJsonObject help() {
    QString guide = QCoreApplication::applicationDirPath() + "/../share/ferro/docs/AGENT_CLI.md";
    if (!QFileInfo::exists(guide))
        guide = QCoreApplication::applicationDirPath() + "/../docs/AGENT_CLI.md";
    guide = QFileInfo::exists(guide) ? QFileInfo(guide).canonicalFilePath() : "docs/AGENT_CLI.md";
    return {
        {"ok", true},
        {"protocol", "ferro-agent-v1"},
        {"entrypoint", "ferro-agent COMMAND (equivalent: ferro agent COMMAND)"},
        {"commands",
         QJsonArray{"help | schema | version", "new --project FILE.ferro [--demo] [--force]",
                    "inspect --project FILE.ferro | --live",
                    "apply --project FILE.ferro --file PLAN.json [--expect REVISION] [--dry-run]",
                    "apply --live --expect REVISION --file PLAN.json [--dry-run]",
                    "apply --project FILE.ferro | --live --expect REVISION --stdin",
                    "validate --project FILE.ferro [--file PLAN.json | --stdin]",
                    "render --project FILE.ferro | --live --output FILE.wav [--force]", "status --live",
                    "open --live --project FILE.ferro [--force] [--expect REVISION]",
                    "new --live [--demo] [--force] [--expect REVISION]",
                    "save --live [--output FILE.ferro] [--force]", "play --live | stop --live"}},
        {"rules",
         QJsonArray{
             "All replies are a single JSON object on stdout; ok=false also exits nonzero. Diagnostics may "
             "appear on stderr.",
             "File mode needs no display or audio device. --live explicitly targets the running GUI in this "
             "desktop session.",
             "Inspect before edits. Copy root revision to --expect; live apply requires it. A conflict means "
             "inspect and rebuild the plan.",
             "Track, bar and step start at 1. There are six tracks, 16 steps/bar, fixed 4/4; C4 is MIDI60.",
             "Plans have version:1 and operations:[{op:...}]. Run schema for the complete strict JSON "
             "Schema.",
             "Controls use GUI units: level0..150, pan-100..100, tone/decay/drive0..100, velocity0..127.",
             "Apply is all-or-nothing. Dry-run and validate never save. One live apply is one GUI Undo step; "
             "it does not start playback.",
             "Phrases use a unique name or numeric ID from inspect within their track. Shared phrase edits "
             "affect every "
             "clip using it; clone for variation.",
             "No duration/gate fields. Notes trigger fixed instrument envelopes; control their tails using "
             "decay.",
             "Use --file or --stdin for plans; do not hand-edit native .ferro JSON. Force is only for "
             "intentional overwrites/discards.",
             "The GUI opens the bundled Neon Signal showcase. Agent new starts empty; new --demo loads the "
             "showcase."}},
        {"guide", guide}};
}
QString absolutePath(const QString& value) {
    return QFileInfo(value).absoluteFilePath();
}
QString comparablePath(const QString& value) {
    auto f = QFileInfo(value);
    return f.exists() ? f.canonicalFilePath() : f.absoluteFilePath();
}
QJsonObject readJson(const QString& path, bool stdinInput) {
    QFile file;
    if (stdinInput) {
        if (!file.open(stdin, QIODevice::ReadOnly))
            invalid("Cannot read standard input.", "READ_FAILED");
    } else {
        file.setFileName(path);
        if (!file.open(QIODevice::ReadOnly))
            invalid(file.errorString(), "READ_FAILED");
    }
    auto bytes = file.read(MaxInputBytes + 1);
    if (bytes.size() > MaxInputBytes)
        invalid("Plan input exceeds 16 MiB.", "INPUT_TOO_LARGE");
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject())
        invalid(QString("Invalid plan JSON at byte %1: %2").arg(parse.offset).arg(parse.errorString()),
                "INVALID_JSON");
    return doc.object();
}
void preventOpenFileWrite(const QString& path) {
    const auto status = sendAgentRequest({{"command", "status"}}, 1500);
    if (!status.value("ok").toBool() &&
        status.value("error").toObject().value("code").toString() != "not_running")
        invalid("Could not establish whether this file is open in Ferro. Retry after the GUI responds, or "
                "use explicit --live commands.",
                "GUI_STATE_UNAVAILABLE");
    const auto openPath = status.value("path").toString();
    if (status.value("ok").toBool() && !openPath.isEmpty() &&
        comparablePath(path) == comparablePath(openPath))
        invalid("This file is open in Ferro. Use inspect --live and apply --live --expect REVISION so "
                "unsaved GUI work is preserved.",
                "PROJECT_OPEN_USE_LIVE");
}
int emitReply(const QJsonObject& reply) {
    std::cout << QJsonDocument(reply).toJson(QJsonDocument::Compact).constData() << '\n';
    return reply.value("ok").toBool() ? 0 : 1;
}
} // namespace

int agentMain(const QStringList& args) {
    try {
        QString command = args.isEmpty() ? "help" : args.front();
        if (command == "--help" || command == "-h")
            command = "help";
        if (command == "--version")
            command = "version";
        const QSet<QString> commands{"help",   "schema", "version", "new",  "inspect", "apply", "validate",
                                     "render", "status", "open",    "save", "play",    "stop"};
        if (!commands.contains(command))
            invalid("Unknown command '" + command + "'. Run ferro-agent help.", "UNKNOWN_COMMAND");
        QMap<QString, QString> options;
        QSet<QString> flags;
        const QSet<QString> booleans{"--live", "--demo", "--force", "--dry-run", "--stdin"};
        const QSet<QString> values{"--project", "--file", "--output", "--expect"};
        for (int i = 1; i < args.size(); ++i) {
            const auto key = args[i];
            if (options.contains(key) || flags.contains(key))
                invalid("Duplicate option: " + key);
            if (booleans.contains(key))
                flags.insert(key);
            else if (values.contains(key)) {
                if (i + 1 >= args.size() || args[i + 1].startsWith("--") || args[i + 1].isEmpty())
                    invalid("Missing value for " + key);
                options.insert(key, args[++i]);
            } else
                invalid("Unknown option '" + key + "'. Run ferro-agent help.", "UNKNOWN_OPTION");
        }
        QSet<QString> allowed;
        if (command == "new")
            allowed = {"--live", "--project", "--demo", "--force", "--expect"};
        if (command == "inspect")
            allowed = {"--live", "--project"};
        if (command == "apply")
            allowed = {"--live", "--project", "--file", "--stdin", "--expect", "--dry-run"};
        if (command == "validate")
            allowed = {"--project", "--file", "--stdin", "--expect"};
        if (command == "render")
            allowed = {"--live", "--project", "--output", "--force"};
        if (command == "open")
            allowed = {"--live", "--project", "--force", "--expect"};
        if (command == "save")
            allowed = {"--live", "--output", "--force"};
        if (command == "status" || command == "play" || command == "stop")
            allowed = {"--live"};
        for (auto it = options.begin(); it != options.end(); ++it)
            if (!allowed.contains(it.key()))
                invalid("Option " + it.key() + " is not supported by " + command);
        for (const auto& flag : flags)
            if (!allowed.contains(flag))
                invalid("Option " + flag + " is not supported by " + command);
        if (command == "help")
            return emitReply(help());
        if (command == "schema")
            return emitReply({{"ok", true}, {"schema", agentSchema()}});
        if (command == "version")
            return emitReply(
                {{"ok", true}, {"protocol", 1}, {"version", QCoreApplication::applicationVersion()}});
        const bool live = flags.contains("--live");
        const bool force = flags.contains("--force");
        const bool isApply =
            command == "apply" ||
            (command == "validate" && (options.contains("--file") || flags.contains("--stdin")));
        if (live && options.contains("--project") && command != "open")
            invalid("Choose either --live or --project, not both.");
        if ((command == "open" || command == "save" || command == "status" || command == "play" ||
             command == "stop") &&
            !live)
            invalid(command + " requires --live; it controls the running GUI explicitly.");
        if (options.contains("--file") && flags.contains("--stdin"))
            invalid("Choose --file or --stdin, not both.");
        if (isApply && !options.contains("--file") && !flags.contains("--stdin"))
            invalid("apply requires --file PLAN.json or --stdin.");
        if (options.contains("--expect") &&
            !QRegularExpression("^[0-9a-f]{64}$").match(options["--expect"]).hasMatch())
            invalid("--expect must be the 64-character revision returned by inspect.");
        if (live && command == "apply" && !options.contains("--expect"))
            invalid("Live apply requires --expect REVISION from inspect --live.", "REVISION_REQUIRED");
        QJsonObject plan;
        if (isApply)
            plan = readJson(options.value("--file"), flags.contains("--stdin"));
        if (live) {
            QJsonObject request{{"command", command}};
            if (options.contains("--expect"))
                request["expected_revision"] = options["--expect"];
            if (command == "apply") {
                request["plan"] = plan;
                request["dry_run"] = flags.contains("--dry-run");
            }
            if (command == "new") {
                request["demo"] = flags.contains("--demo");
                request["force"] = force;
            }
            if (command == "open") {
                if (!options.contains("--project"))
                    invalid("open requires --project FILE.ferro.");
                request["path"] = absolutePath(options["--project"]);
                request["force"] = force;
            }
            if (command == "save") {
                if (options.contains("--output"))
                    request["path"] = absolutePath(options["--output"]);
                request["force"] = force;
            }
            if (command != "render")
                return emitReply(sendAgentRequest(request));
        }
        if (!live && !options.contains("--project"))
            invalid(command + " requires --project FILE.ferro.");
        const QString path = live ? QString() : absolutePath(options["--project"]);
        if (command == "new" && !path.endsWith(".ferro", Qt::CaseInsensitive))
            invalid("New project paths must end in .ferro.");
        const bool mutation = command == "new" || (command == "apply" && !flags.contains("--dry-run"));
        std::unique_ptr<QLockFile> lock;
        if (mutation) {
            preventOpenFileWrite(path);
            lock = std::make_unique<QLockFile>(path + ".agent.lock");
            if (!lock->tryLock(0))
                invalid("Another agent is editing this file, or its directory is not writable.",
                        "PROJECT_BUSY");
        }
        Project p;
        QString renderSourcePath = path;
        QString error;
        if (live) {
            auto snapshot = sendAgentRequest({{"command", "inspect"}});
            if (!snapshot.value("ok").toBool())
                return emitReply(snapshot);
            if (!snapshot.value("project").isObject() ||
                !projectFromJson(snapshot.value("project").toObject(), p, error))
                invalid("Invalid live snapshot: " + error, "INVALID_RESPONSE");
            renderSourcePath = snapshot.value("path").toString();
        } else if (command == "new") {
            if (QFileInfo::exists(path) && !force)
                invalid("Project already exists. Use --force only to replace it intentionally.",
                        "FILE_EXISTS");
            if (options.contains("--expect")) {
                Project old;
                if (!loadProject(path, old, error))
                    invalid(error, "LOAD_FAILED");
                if (agentRevision(old) != options["--expect"])
                    invalid("Project changed. Inspect and retry with the new revision.", "REVISION_CONFLICT");
            }
            p = flags.contains("--demo") ? showcaseProject() : emptyProject();
        } else if (!loadProject(path, p, error))
            invalid(error, "LOAD_FAILED");
        if (!live && command != "new" && options.contains("--expect") &&
            options["--expect"] != agentRevision(p))
            invalid("Project changed. Inspect and retry with the new revision.", "REVISION_CONFLICT");
        if (command == "new") {
            if (!saveProject(path, p, error))
                invalid(error, "SAVE_FAILED");
            return emitReply(
                {{"ok", true}, {"path", path}, {"revision", agentRevision(p)}, {"data", agentInspect(p)}});
        }
        if (command == "inspect")
            return emitReply(
                {{"ok", true}, {"path", path}, {"revision", agentRevision(p)}, {"data", agentInspect(p)}});
        if (isApply) {
            Project changed;
            QJsonObject response;
            if (!agentApply(p, plan, changed, response))
                return emitReply(response);
            const bool dryRun = flags.contains("--dry-run") || command == "validate";
            if (!dryRun) {
                // The lock protects other CLI writers. Re-read also detects external writes
                // made while a plan was being parsed/validated, instead of losing them.
                Project current;
                if (!loadProject(path, current, error))
                    invalid(error, "LOAD_FAILED");
                if (agentRevision(current) != agentRevision(p))
                    invalid("Project changed during apply. Inspect and retry.", "REVISION_CONFLICT");
                if (!saveProject(path, changed, error))
                    invalid(error, "SAVE_FAILED");
            }
            response["dry_run"] = dryRun;
            response["path"] = path;
            response["base_revision"] = agentRevision(p);
            return emitReply(response);
        }
        if (command == "validate")
            return emitReply({{"ok", true}, {"valid", true}, {"path", path}, {"revision", agentRevision(p)}});
        if (command == "render") {
            if (!options.contains("--output"))
                invalid("render requires --output FILE.wav.");
            auto output = absolutePath(options["--output"]);
            if (!output.endsWith(".wav", Qt::CaseInsensitive))
                invalid("Render output paths must end in .wav.");
            if (!renderSourcePath.isEmpty() && comparablePath(output) == comparablePath(renderSourcePath))
                invalid("Render output must differ from the source project.", "OUTPUT_COLLISION");
            QLockFile outputLock(output + ".agent.lock");
            if (!outputLock.tryLock(0))
                invalid("Another agent is writing this output, or its directory is not writable.",
                        "OUTPUT_BUSY");
            if (QFileInfo::exists(output) && !force)
                invalid("Output exists. Use --force only to replace it intentionally.", "FILE_EXISTS");
            auto rendered = renderProject(p);
            if (!writeWave(output, rendered, error))
                invalid(error, "RENDER_FAILED");
            return emitReply({{"ok", true},
                              {"path", output},
                              {"revision", agentRevision(p)},
                              {"sample_rate", SampleRate},
                              {"channels", 2},
                              {"bits", 24},
                              {"duration_seconds", rendered.size() / double(SampleRate * 2)}});
        }
        invalid("Unhandled command.", "INTERNAL_ERROR");
    } catch (const InputError& e) {
        return emitReply(agentError(e.code, e.text));
    } catch (const std::exception& e) {
        return emitReply(agentError("INTERNAL_ERROR", QString::fromUtf8(e.what())));
    }
}
} // namespace fc
