#pragma once
#include <QJsonObject>
#include <QString>
#include <array>
#include <cstdint>
#include <vector>

namespace fc {
constexpr int SampleRate = 48000;
constexpr int TrackCount = 6;
constexpr int Steps = 16;
constexpr int MaxNotesPerStep = 128;
constexpr int MaxSongBars = 256;
constexpr double MaxSongSeconds = 600.0;
struct Note {
    double velocity = 0.0;
    int note = 60;
};
struct Step {
    std::vector<Note> notes;
    Step() = default;
    Step(double velocity, int note) : notes{{velocity, note}} {}
};
struct Pattern {
    QString name = "Phrase";
    int bars = 1;
    std::vector<Step> steps = std::vector<Step>(Steps);
};
struct Clip {
    uint64_t id = 0;
    int pattern = 0;
    int startBar = 0;
    int lengthBars = 1;
};
struct Track {
    QString name;
    int instrument = 0;
    double volume = 0.75, pan = 0.0, tone = 0.5, decay = 0.5, drive = 0.3;
    bool mute = false, solo = false;
    QString samplePath;
    std::vector<Pattern> patterns;
    std::vector<Clip> clips;
};
struct Project {
    QString title = "Untitled song";
    double bpm = 120.0, master = 0.7, delay = 0.15, duck = 0.35;
    double tailSeconds = 2.0;
    bool metronome = false;
    int songBars = 16;
    std::array<Track, TrackCount> tracks;
};
Project demoProject();
Project emptyProject();
QJsonObject projectToJson(const Project& project);
bool projectFromJson(const QJsonObject& json, Project& out, QString& error);
bool saveProject(const QString& path, const Project& project, QString& error);
bool loadProject(const QString& path, Project& project, QString& error);
int loopFrames(const Project& project);
int songFrames(const Project& project);
int renderFrames(const Project& project);
int contentEndBar(const Project& project);
uint64_t nextClipId(const Project& project);
QString noteName(int midi);
QString instrumentName(int instrument);
struct ScheduledStep { int frame; Note step; uint64_t eventId; };
std::vector<ScheduledStep> scheduledSteps(const Project& project, int track);
} // namespace fc
