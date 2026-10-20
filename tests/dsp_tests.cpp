#include "dsp.h"

#include <QFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <sndfile.h>
#include <stdexcept>

namespace {
double energy(const std::vector<float>& audio, size_t begin = 0, size_t end = 0) {
    if (end == 0)
        end = audio.size();
    double sum = 0;
    for (size_t i = begin; i < std::min(audio.size(), end); ++i)
        sum += audio[i] * audio[i];
    return sum;
}
bool finite(const std::vector<float>& audio) {
    return std::all_of(audio.begin(), audio.end(), [](float f) { return std::isfinite(f); });
}
fc::Project blank() {
    auto p = fc::emptyProject();
    p.bpm = 120;
    p.songBars = 1;
    p.tailSeconds = 1;
    p.delay = 0;
    for (int i = 0; i < fc::TrackCount; ++i) {
        auto& track = p.tracks[i];
        track.instrument = i;
        track.patterns = {fc::Pattern{}};
        track.clips = {{static_cast<uint64_t>(i + 1), 0, 0, 1}};
    }
    return p;
}
fc::Step& step(fc::Project& project, int track, int position) {
    return project.tracks[track].patterns[0].steps[position];
}
} // namespace

int dspTests() {
    int failed = 0;
    const auto check = [&](bool passed, const char* label) {
        if (!passed) {
            std::cerr << "DSP: " << label << '\n';
            ++failed;
        }
    };
    const auto throws = [&](auto callback, const char* label) {
        bool caught = false;
        try {
            callback();
        } catch (const std::exception&) {
            caught = true;
        }
        check(caught, label);
    };
    auto p = blank();
    check(energy(fc::renderProject(p)) == 0, "an empty song must be silent");
    constexpr int notes[fc::TrackCount] = {36, 36, 48, 60, 39, 42};
    for (int track = 0; track < fc::TrackCount; ++track) {
        p = blank();
        step(p, track, 0) = {.9, notes[track]};
        const auto first = fc::renderTrack(p, track), second = fc::renderTrack(p, track);
        check(first.size() == static_cast<size_t>(fc::renderFrames(p)) * 2, "stereo song plus tail frame count");
        check(finite(first) && energy(first) > 0.01, "every native voice produces finite, audible sound");
        check(first == second, "native rendering and percussion noise must be deterministic");
    }
    p = blank();
    p.tracks[0].decay = 1;
    step(p, 0, 15) = {1, 36};
    const auto lateKick = fc::renderTrack(p, 0);
    check(energy(lateKick, 0, 2000) == 0, "a final-step kick cannot wrap into the song introduction");
    check(energy(lateKick, static_cast<size_t>(fc::songFrames(p)) * 2) > .01,
          "a final-step kick continues into the finite export tail");
    p.tailSeconds = 0;
    check(fc::renderTrack(p, 0).size() == static_cast<size_t>(fc::songFrames(p)) * 2,
          "a zero-second tail renders exactly the song length");

    p = blank();
    p.tracks[0].volume = 1;
    step(p, 0, 0) = {1, 36};
    step(p, 1, 0) = {1, 36};
    p.duck = 0;
    const auto unducked = fc::renderTrack(p, 1);
    p.duck = 1;
    const auto ducked = fc::renderTrack(p, 1);
    check(energy(ducked, 0, 4800) < energy(unducked, 0, 4800) * .65, "kick sidechain must duck the bass");
    p.tracks[0].mute = true;
    check(fc::renderTrack(p, 1) == unducked, "muting the kick releases its sidechain");
    p.tracks[0].mute = false;
    p.tracks[0].clips.clear();
    check(fc::renderTrack(p, 1) == unducked, "unscheduled kicks must not create hidden duck events");
    p.tracks[4] = p.tracks[0];
    p.tracks[4].clips = {{42, 0, 0, 1}};
    check(energy(fc::renderTrack(p, 1), 0, 4800) < energy(unducked, 0, 4800) * .65,
          "a kick instrument on another track still drives sidechain");
    p.tracks[4].clips[0].startBar = 1;
    p.songBars = 2;
    check(energy(fc::renderTrack(p, 1), 0, 4800) == energy(unducked, 0, 4800),
          "a later kick cannot duck bass at the beginning");

    p = blank();
    step(p, 0, 0) = {1, 36};
    const auto kick = fc::renderTrack(p, 0);
    step(p, 0, 0).notes[0].note = 43;
    check(fc::renderTrack(p, 0) != kick, "kick tuning must change its waveform");
    p.tracks[0].instrument = 3;
    check(fc::renderTrack(p, 0) != kick, "instrument selection is independent of track index");
    p.tracks[0].clips.clear();
    check(energy(fc::renderTrack(p, 0)) == 0, "unplaced phrases must not sound");
    p.tracks[0].clips = {{1, 0, 0, 1}};
    p.tracks[0].pan = -1;
    const auto panned = fc::renderTrack(p, 0);
    double right = 0;
    for (size_t i = 1; i < panned.size(); i += 2)
        right += std::abs(panned[i]);
    check(right == 0, "hard left pan must silence the right channel");

    p = blank();
    p.tracks[3].volume = .35;
    step(p, 3, 0).notes = {{.7, 60}, {.5, 64}, {.4, 67}};
    step(p, 3, 8).notes = {{.6, 57}, {.4, 60}, {.3, 64}};
    const auto chord = fc::renderTrack(p, 3);
    std::vector<float> separateVoices(chord.size(), 0);
    for (int voice = 0; voice < 3; ++voice) {
        auto single = p;
        for (int position : {0, 8})
            step(single, 3, position).notes = {step(p, 3, position).notes[voice]};
        const auto rendered = fc::renderTrack(single, 3);
        for (size_t i = 0; i < rendered.size(); ++i)
            separateVoices[i] += rendered[i];
    }
    double chordDifference = 0;
    for (size_t i = 0; i < chord.size(); ++i)
        chordDifference = std::max(chordDifference, std::abs(double(chord[i] - separateVoices[i])));
    check(finite(chord) && energy(chord) > .01 && chordDifference < 1e-6,
          "a same-track chord sums all pitched voices with independent velocities");
    check(fc::renderTrack(p, 3) == chord, "polyphonic rendering is deterministic");
    step(p, 3, 0).notes.push_back({0, 72});
    check(fc::renderTrack(p, 3) == chord, "a zero-velocity chord member contributes no sound");

    p = blank();
    p.songBars = 4;
    p.tracks[0].clips = {{1, 0, 2, 1}};
    step(p, 0, 0) = {1, 36};
    const auto sparse = fc::renderProject(p);
    const int barFrames = fc::songFrames(p) / 4;
    check(energy(sparse, 0, static_cast<size_t>(barFrames) * 4) == 0,
          "an empty timeline introduction remains exactly silent through mastering");
    check(energy(sparse, static_cast<size_t>(barFrames) * 4,
                 static_cast<size_t>(barFrames) * 6) > .01,
          "a clip plays at its absolute timeline position");
    check(energy(sparse, static_cast<size_t>(barFrames) * 7) < 1e-12,
          "an empty timeline ending receives no hidden phrase repetition");

    p = blank();
    p.songBars = 3;
    p.tracks[5].patterns[0].bars = 2;
    p.tracks[5].patterns[0].steps.resize(fc::Steps * 2);
    step(p, 5, 0) = {.8, 42};
    step(p, 5, 20) = {.8, 42};
    p.tracks[5].clips = {{6, 0, 0, 3}};
    const auto repeated = fc::renderTrack(p, 5);
    const int twoBarFrames = fc::songFrames(p) * 2 / 3;
    check(energy(repeated, static_cast<size_t>(twoBarFrames) * 2,
                 static_cast<size_t>(twoBarFrames) * 2 + 4000) > .01,
          "drag-extended clips repeat multi-bar phrases at their source length");
    check(energy(repeated, static_cast<size_t>(barFrames) * 2,
                 static_cast<size_t>(barFrames) * 2 + 4000) == 0,
          "a two-bar phrase is not accidentally repeated every bar");

    p = blank();
    p.songBars = 3;
    step(p, 0, 0) = {1, 36};
    step(p, 5, 4) = {1, 42};
    p.tracks[0].clips[0].lengthBars = 3;
    p.tracks[5].clips[0].startBar = 1;
    const auto independentlyPlaced = fc::renderTrack(p, 5);
    check(energy(independentlyPlaced, 0, static_cast<size_t>(barFrames) * 2) == 0 &&
              energy(independentlyPlaced, static_cast<size_t>(barFrames) * 2,
                     static_cast<size_t>(barFrames) * 4) > .01 &&
              energy(independentlyPlaced, static_cast<size_t>(barFrames) * 4) == 0,
          "each track uses independent clips rather than a shared song pattern");

    p = blank();
    p.tailSeconds = 2;
    p.delay = 1;
    step(p, 2, 15) = {1, 48};
    const auto delayed = fc::renderProject(p);
    check(energy(delayed, 0, 2000) == 0, "delay and DC blocking must not create pre-echo");
    check(energy(delayed, static_cast<size_t>(fc::songFrames(p) + fc::SampleRate / 2) * 2) > 1e-5,
          "late delay taps continue into the export tail");
    p = blank();
    p.metronome = true;
    const auto metronome = fc::renderProject(p);
    check(energy(metronome) > .01 &&
              energy(metronome, static_cast<size_t>(fc::songFrames(p) + fc::SampleRate / 2) * 2) < 1e-12,
          "metronome stops at song end instead of ticking through the tail");

    p = fc::demoProject();
    p.songBars = 2;
    auto mix = fc::renderProject(p);
    const auto maximum =
        std::max_element(mix.begin(), mix.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
    check(finite(mix) && maximum != mix.end() && std::abs(*maximum) <= .981,
          "master output must remain finite and below full scale");
    p.master = 0;
    check(energy(fc::renderProject(p)) == 0, "zero master gain must produce silence");
    p = blank();
    throws([&] { fc::renderTrack(p, -1); }, "reject invalid track indices");
    p.bpm = std::numeric_limits<double>::quiet_NaN();
    throws([&] { fc::renderTrack(p, 0); }, "reject non-finite tempo");
    p = blank();
    p.bpm = 40;
    p.tailSeconds = 0;
    check(fc::renderTrack(p, 0).size() == size_t(fc::SampleRate) * 6 * 2,
          "40 BPM is supported for slow full-song composition");
    p.songBars = fc::MaxSongBars;
    throws([&] { fc::renderTrack(p, 0); }, "reject arrangements exceeding ten minutes");
    p = blank();
    p.tracks[0].samplePath = "/there/is/no/sample.wav";
    throws([&] { fc::renderTrack(p, 0); }, "missing samples must report an error");
    p = blank();
    std::array<std::vector<float>, fc::TrackCount> invalidStems;
    throws([&] { fc::mixTracks(p, invalidStems); }, "reject incomplete stem buffers");

    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary export directory");
    std::vector<float> source(2400 * 2);
    for (size_t i = 0; i < source.size() / 2; ++i) {
        source[i * 2] = static_cast<float>(.25 * std::sin(6.283185307179586 * 440 * i / fc::SampleRate));
        source[i * 2 + 1] = static_cast<float>(.2 * std::sin(6.283185307179586 * 880 * i / fc::SampleRate));
    }
    QString error;
    const QString wavePath = temporary.filePath("roundtrip.wav");
    check(fc::writeWave(wavePath, source, error), "atomic WAV export");
    SF_INFO info{};
    SNDFILE* wave = sf_open(QFile::encodeName(wavePath).constData(), SFM_READ, &info);
    check(wave && info.channels == 2 && info.samplerate == fc::SampleRate && info.frames == 2400 &&
              (info.format & SF_FORMAT_SUBMASK) == SF_FORMAT_PCM_24,
          "WAV defaults to one complete stereo 48 kHz PCM24 song");
    if (wave) {
        std::vector<float> roundtrip(source.size());
        check(sf_readf_float(wave, roundtrip.data(), 2400) == 2400, "read back complete WAV export");
        double errorMax = 0;
        for (size_t i = 0; i < roundtrip.size(); ++i)
            errorMax = std::max(errorMax, std::abs(static_cast<double>(roundtrip[i] - source[i])));
        check(errorMax < 2e-6, "24-bit export preserves the complete song without gaps");
        sf_close(wave);
    }
    p = blank();
    p.tracks[0].samplePath = wavePath;
    step(p, 0, 0) = {1, 60};
    const auto sampled = fc::renderTrack(p, 0);
    check(finite(sampled) && energy(sampled) > .01, "imported stereo WAV can be resampled and triggered");
    step(p, 0, 0).notes[0].note = 72;
    const auto transposed = fc::renderTrack(p, 0);
    check(finite(transposed) && transposed != sampled && energy(transposed, 6000) == 0,
          "octave sample transposition shortens its duration");
    step(p, 0, 0).notes = {{1, 60}, {1, 72}};
    const auto sampledChord = fc::renderTrack(p, 0);
    double sampleChordDifference = 0;
    for (size_t i = 0; i < sampledChord.size(); ++i)
        sampleChordDifference = std::max(sampleChordDifference,
                                        std::abs(double(sampledChord[i] - (sampled[i] + transposed[i]))));
    check(finite(sampledChord) && sampleChordDifference < 1e-6,
          "sample chords mix independently resampled pitches at one onset");
    step(p, 0, 0).notes.clear();
    step(p, 0, 15) = {1, 48};
    const auto lateSample = fc::renderTrack(p, 0);
    check(energy(lateSample, 0, 2000) == 0,
          "a final-step sample cannot wrap into the song introduction");

    const QString heldPath = temporary.filePath("held-sample.wav");
    check(fc::writeWave(heldPath, source, error, 64), "create sample held beyond the song end");
    p = blank();
    p.tailSeconds = 0;
    p.tracks[0].samplePath = heldPath;
    step(p, 0, 0) = {1, 60};
    const auto held = fc::renderProject(p);
    check(energy(held, held.size() - 2048, held.size() - 256) > 1e-5 &&
              held[held.size() - 2] == 0 && held.back() == 0,
          "a held sample with no ending tail fades to exact stereo silence at the WAV boundary");

    const QString longPath = temporary.filePath("four-seconds.wav");
    check(fc::writeWave(longPath, std::vector<float>(fc::SampleRate * 4 * 2, .1f), error),
          "create bounded long sample fixture");
    p = blank();
    p.tracks[0].samplePath = longPath;
    p.songBars = 24;
    p.tracks[0].clips[0].lengthBars = 24;
    for (auto& event : p.tracks[0].patterns[0].steps)
        event = {1, 48};
    bool mixingBudget = false;
    try {
        fc::renderTrack(p, 0);
    } catch (const std::exception& e) {
        mixingBudget = QString::fromUtf8(e.what()).contains("mixing budget");
    }
    check(mixingBudget, "dense long sample tracks fail preflight with an actionable mixing budget error");
    p.songBars = 1;
    p.tracks[0].clips[0].lengthBars = 1;
    for (int i = 0; i < fc::Steps; ++i)
        step(p, 0, i) = {1, 48 + i};
    bool resamplingBudget = false;
    try {
        fc::renderTrack(p, 0);
    } catch (const std::exception& e) {
        resamplingBudget = QString::fromUtf8(e.what()).contains("resampling budget");
    }
    check(resamplingBudget, "many sample pitches fail preflight before excessive sinc conversion");
    for (auto& event : p.tracks[0].patterns[0].steps)
        event.notes.clear();
    for (int pitch = 48; pitch < 64; ++pitch)
        step(p, 0, 0).notes.push_back({1, pitch});
    resamplingBudget = false;
    try {
        fc::renderTrack(p, 0);
    } catch (const std::exception& e) {
        resamplingBudget = QString::fromUtf8(e.what()).contains("resampling budget");
    }
    check(resamplingBudget, "simultaneous sample pitches also count toward the resampling work budget");
    check(!fc::writeWave(temporary.filePath("bad.wav"), {0, std::numeric_limits<float>::infinity()}, error),
          "reject non-finite WAV export");
    check(!fc::writeWave(temporary.filePath("odd.wav"), {.1f}, error), "reject incomplete stereo frames");
    return failed;
}
