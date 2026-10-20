#include "dsp.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QtEndian>
#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>

namespace fc {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double Tau = 2.0 * Pi;
constexpr int MaxSampleSeconds = 16;

double bounded(double x, double lo, double hi) {
    if (!std::isfinite(x))
        throw std::runtime_error("A sound parameter is not finite.");
    return std::clamp(x, lo, hi);
}

int checkedFrames(const Project& p) {
    if (!std::isfinite(p.bpm) || p.bpm < 40 || p.bpm > 300 || p.songBars < 1 ||
        p.songBars > MaxSongBars)
        throw std::runtime_error("Tempo must be 40–300 BPM and the song must have 1–256 bars.");
    if (!std::isfinite(p.tailSeconds) || p.tailSeconds < 0 || p.tailSeconds > 8)
        throw std::runtime_error("The effect tail must be between 0 and 8 seconds.");
    const double seconds = p.songBars * 240.0 / p.bpm;
    if (seconds + p.tailSeconds > MaxSongSeconds)
        throw std::runtime_error("The song exceeds the ten-minute duration limit.");
    const int n = renderFrames(p);
    if (n <= 0 || n > SampleRate * MaxSongSeconds)
        throw std::runtime_error("Invalid song duration.");
    return n;
}

double frequency(int note) {
    return 440.0 * std::exp2((std::clamp(note, 0, 127) - 69) / 12.0);
}
double fadeEnd(double t, double duration, double fade = 0.012) {
    return std::clamp((duration - t) / fade, 0.0, 1.0);
}
double polyBlep(double phase, double increment) {
    if (phase < increment) {
        const double x = phase / increment;
        return x + x - x * x - 1.0;
    }
    if (phase > 1.0 - increment) {
        const double x = (phase - 1.0) / increment;
        return x * x + x + x + 1.0;
    }
    return 0.0;
}
double saw(double phase, double increment) {
    return 2.0 * phase - 1.0 - polyBlep(phase, increment);
}
double square(double phase, double increment) {
    return (phase < 0.5 ? 1.0 : -1.0) + polyBlep(phase, increment) -
           polyBlep(std::fmod(phase + 0.5, 1.0), increment);
}
void advance(double& phase, double increment) {
    phase += increment;
    phase -= std::floor(phase);
}

struct Noise {
    uint32_t state;
    explicit Noise(uint32_t seed) : state(seed ? seed : 1) {}
    double next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (static_cast<double>(state) / 2147483648.0) - 1.0;
    }
};

// Transposed direct form II. These filters also remove ultrasonic products
// before the nonlinear voices are decimated back to the device sample rate.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void lowpass(double cutoff, double rate, double q = 0.7071067811865475) {
        const double w = Tau * std::clamp(cutoff, 10.0, rate * 0.45) / rate;
        const double c = std::cos(w), alpha = std::sin(w) / (2.0 * q), inv = 1.0 / (1.0 + alpha);
        b0 = (1.0 - c) * 0.5 * inv;
        b1 = (1.0 - c) * inv;
        b2 = b0;
        a1 = -2.0 * c * inv;
        a2 = (1.0 - alpha) * inv;
    }
    void highpass(double cutoff, double rate) {
        const double w = Tau * std::clamp(cutoff, 10.0, rate * 0.45) / rate;
        const double c = std::cos(w), alpha = std::sin(w) / std::sqrt(2.0), inv = 1.0 / (1.0 + alpha);
        b0 = (1.0 + c) * 0.5 * inv;
        b1 = -(1.0 + c) * inv;
        b2 = b0;
        a1 = -2.0 * c * inv;
        a2 = (1.0 - alpha) * inv;
    }
    double next(double in) {
        const double out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        return out;
    }
};

// Topology-preserving state variable filter stays stable while its cutoff
// sweeps quickly. Resonance is deliberately bounded below self oscillation.
struct ResonantFilter {
    double s1 = 0, s2 = 0;
    double next(double in, double cutoff, double resonance, double rate) {
        const double g = std::tan(Pi * std::clamp(cutoff, 25.0, rate * 0.20) / rate);
        const double k = 2.0 - 1.86 * std::clamp(resonance, 0.0, 1.0);
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = in - s2, v1 = a1 * s1 + a2 * v3, v2 = s2 + a2 * s1 + a3 * v3;
        s1 = 2.0 * v1 - s1;
        s2 = 2.0 * v2 - s2;
        return v2;
    }
};

struct Voice {
    double tone, decay, drive, hz, duration;
    double phase = 0, phase2 = 0.23, phase3 = 0.57;
    std::array<double, 6> metalPhase{};
    Noise noise;
    Biquad anti1, anti2, hp, lp, body;
    ResonantFilter filter;
    int type;
    Voice(int track, const Track& t, int note, uint32_t seed)
        : tone(bounded(t.tone, 0, 1)), decay(bounded(t.decay, 0, 1)), drive(bounded(t.drive, 0, 1)),
          hz(frequency(note)), duration(0), noise(seed), type(track) {
        if (type == 0)
            duration = 0.20 + decay * 0.58;
        else if (type == 1)
            duration = 0.10 + decay * 0.38;
        else if (type == 2)
            duration = 0.10 + decay * 0.44;
        else if (type == 3)
            duration = 0.12 + decay * 0.66;
        else if (type == 4)
            duration = 0.10 + decay * 0.30;
        else
            duration = 0.035 + decay * 0.18;
        const int os = type == 0 ? 4 : ((type == 1 || type == 2 || type == 3) ? 2 : 1);
        anti1.lowpass(14500, SampleRate * os, 0.5411961001);
        anti2.lowpass(14500, SampleRate * os, 1.3065629649);
        hp.highpass(type == 4 ? 850 : 4300 - tone * 1300, SampleRate);
        lp.lowpass(type == 4 ? 3500 + tone * 6000 : 6500 + tone * 11000, SampleRate);
        body.lowpass(130 + tone * 620, SampleRate * 4);
        for (size_t i = 0; i < metalPhase.size(); ++i)
            metalPhase[i] = 0.11 * i;
    }
    std::pair<double, double> next(int frame) {
        const double t = frame / static_cast<double>(SampleRate);
        const double end = fadeEnd(t, duration);
        if (type == 0) {
            double y = 0;
            for (int k = 0; k < 4; ++k) {
                const double st = (frame + k / 4.0) / SampleRate;
                const double pitch = hz * (1.0 + 13.0 * std::exp(-st / 0.006) + 1.15 * std::exp(-st / 0.037));
                const double inc = std::min(pitch / (SampleRate * 4), 0.22);
                advance(phase, inc);
                const double fundamental = std::sin(Tau * phase);
                const double growl = saw(phase, inc) * (.10 + tone * .29);
                const double raw = fundamental + growl + .15 * std::sin(2 * Tau * phase + .3);
                const double gain = 1.7 + 11.0 * drive;
                const double shaped = std::tanh(raw * gain) / std::tanh(gain);
                const double low = body.next(shaped);
                const double punch = (1.0 - std::exp(-st / .0005)) * std::exp(-st / (.085 + .23 * decay));
                const double attack = noise.next() * std::exp(-st / .0027) * (.15 + .35 * tone);
                const double signal =
                    (shaped * (.36 + .32 * tone) + low * .46 + fundamental * .14) * punch + attack;
                y = anti2.next(anti1.next(signal));
            }
            y *= end * .92;
            return {y, y};
        }
        if (type <= 3) {
            double left = 0, right = 0;
            for (int k = 0; k < 2; ++k) {
                const double st = (frame + k / 2.0) / SampleRate;
                const double inc = std::min(hz / (SampleRate * 2), 0.20);
                advance(phase, inc);
                advance(phase2, inc * (type == 3 ? 1.0058 : .999));
                advance(phase3, inc * .9942);
                const double env = (1.0 - std::exp(-st / (type == 3 ? .006 : .0015))) *
                                   std::exp(-st / (.035 + decay * (type == 3 ? .31 : .16)));
                double signal;
                if (type == 1) {
                    const double raw = .70 * std::sin(Tau * phase) + .30 * saw(phase, inc);
                    signal = std::tanh(raw * (1.0 + drive * 3.0)) * .54;
                    signal = filter.next(signal, 100 + tone * tone * 1800 + 140 * std::exp(-st / .025), .15,
                                         SampleRate * 2);
                    left = right = anti2.next(anti1.next(signal)) * env * end;
                } else if (type == 2) {
                    const double raw = .79 * saw(phase, inc) + .21 * square(phase, inc);
                    const double cutoff =
                        110 + tone * tone * 3200 + (700 + tone * 6500) * std::exp(-st / (.022 + decay * .09));
                    signal = filter.next(std::tanh(raw * (1 + drive * 2)), cutoff, .59 + tone * .23,
                                         SampleRate * 2);
                    left = right = anti2.next(anti1.next(signal)) * .58 * env * end;
                } else {
                    const double a = saw(phase, inc), b = saw(phase2, inc * 1.0058),
                                 c = saw(phase3, inc * .9942);
                    signal = (a * .48 + b * .26 + c * .26);
                    signal = filter.next(signal, 700 + tone * tone * 9500, .13, SampleRate * 2);
                    const double core = anti2.next(anti1.next(std::tanh(signal * (1 + drive * 1.8))));
                    left = (core * .48 + (b - c) * .07) * env * end;
                    right = (core * .48 + (c - b) * .07) * env * end;
                }
            }
            return {left, right};
        }
        if (type == 4) {
            const double a = std::exp(-t / .007);
            const double b = t > .011 ? .8 * std::exp(-(t - .011) / .007) : 0;
            const double c = t > .023 ? .72 * std::exp(-(t - .023) / (.020 + decay * .10)) : 0;
            const double signal = lp.next(hp.next(noise.next())) * (a + b + c);
            const double shaped = std::tanh(signal * (1 + drive * 2.2)) * .57 * end;
            return {shaped, shaped};
        }
        double metallic = 0;
        constexpr double freqs[6] = {411, 578, 807, 1093, 1583, 2111};
        for (int i = 0; i < 6; ++i) {
            const double inc = freqs[i] * (.8 + tone * .65) / SampleRate;
            advance(metalPhase[i], inc);
            metallic += square(metalPhase[i], inc);
        }
        const double envelope = std::exp(-t / (.008 + decay * .055)) * end;
        const double filtered = lp.next(hp.next(noise.next() * .68 + metallic * .053));
        const double y = std::tanh(filtered * (1 + drive * 1.7)) * .42 * envelope;
        return {y, y};
    }
};

struct Sample {
    int rate = SampleRate;
    std::vector<float> stereo;
};
Sample loadSample(const QString& path) {
    SF_INFO info{};
    using Handle = std::unique_ptr<SNDFILE, decltype(&sf_close)>;
    Handle file(sf_open(QFile::encodeName(path).constData(), SFM_READ, &info), sf_close);
    if (!file)
        throw std::runtime_error(QString("Cannot open sample ‘%1’: %2")
                                     .arg(path, QString::fromUtf8(sf_strerror(nullptr)))
                                     .toStdString());
    if (info.channels < 1 || info.channels > 8 || info.samplerate < 8000 || info.samplerate > 384000 ||
        info.frames < 1 || info.frames > static_cast<sf_count_t>(info.samplerate) * MaxSampleSeconds)
        throw std::runtime_error("Samples must be 1–8 channels, 8–384 kHz, and at most 16 seconds long.");
    // Read in fixed blocks: even a high-rate eight-channel file cannot force a
    // second allocation proportional to its channel count.
    Sample result;
    result.rate = info.samplerate;
    result.stereo.resize(static_cast<size_t>(info.frames) * 2);
    std::vector<float> block(static_cast<size_t>(4096) * info.channels);
    sf_count_t offset = 0;
    while (offset < info.frames) {
        const sf_count_t wanted = std::min<sf_count_t>(4096, info.frames - offset);
        const sf_count_t got = sf_readf_float(file.get(), block.data(), wanted);
        if (got != wanted)
            throw std::runtime_error("The sample is truncated or could not be decoded.");
        for (sf_count_t i = 0; i < got; ++i) {
            const double left = block[static_cast<size_t>(i) * info.channels];
            const double right =
                info.channels == 1 ? left : block[static_cast<size_t>(i) * info.channels + 1];
            if (!std::isfinite(left) || !std::isfinite(right))
                throw std::runtime_error("The sample contains non-finite audio.");
            result.stereo[static_cast<size_t>(offset + i) * 2] =
                static_cast<float>(std::clamp(left, -4.0, 4.0));
            result.stereo[static_cast<size_t>(offset + i) * 2 + 1] =
                static_cast<float>(std::clamp(right, -4.0, 4.0));
        }
        offset += got;
    }
    return result;
}

size_t sampleVoiceFrames(const Sample& source, int note, int rootNote) {
    const double ratio = source.rate / static_cast<double>(SampleRate) * std::exp2((note - rootNote) / 12.0);
    const size_t sourceFrames = source.stereo.size() / 2;
    const double length = std::ceil(sourceFrames / ratio);
    if (!std::isfinite(length) || length < 1 || length > SampleRate * 32.0)
        throw std::runtime_error("The sample at this pitch exceeds the 32-second voice limit.");
    return static_cast<size_t>(length);
}

std::vector<float> resample(const Sample& source, int note, int rootNote) {
    const double ratio = source.rate / static_cast<double>(SampleRate) * std::exp2((note - rootNote) / 12.0);
    const size_t sourceFrames = source.stereo.size() / 2;
    const size_t frames = sampleVoiceFrames(source, note, rootNote);
    std::vector<float> out(frames * 2);
    // Windowed sinc with a ratio-dependent cutoff avoids folding high-frequency
    // sample content into the audible band when playing it faster.
    const double cutoff = std::min(1.0, 1.0 / ratio) * .94;
    constexpr int radius = 16;
    for (size_t i = 0; i < frames; ++i) {
        const double position = i * ratio;
        const int64_t center = static_cast<int64_t>(std::floor(position));
        double l = 0, r = 0, norm = 0;
        for (int tap = -radius + 1; tap <= radius; ++tap) {
            const int64_t index = center + tap;
            const double x = index - position, window = .5 + .5 * std::cos(Pi * x / radius);
            const double sx = Pi * x * cutoff;
            const double weight = (std::abs(sx) < 1e-9 ? cutoff : cutoff * std::sin(sx) / sx) * window;
            norm += weight;
            if (index >= 0 && static_cast<size_t>(index) < sourceFrames) {
                l += source.stereo[static_cast<size_t>(index) * 2] * weight;
                r += source.stereo[static_cast<size_t>(index) * 2 + 1] * weight;
            }
        }
        const double t = i / static_cast<double>(SampleRate),
                     remaining = (frames - i) / static_cast<double>(SampleRate);
        const double fade = std::min(1.0, t / .001) * std::min(1.0, remaining / .004);
        out[i * 2] = static_cast<float>(l / norm * fade);
        out[i * 2 + 1] = static_cast<float>(r / norm * fade);
    }
    return out;
}

bool audible(const Project& p, int track) {
    const bool anySolo = std::any_of(p.tracks.begin(), p.tracks.end(), [](const Track& t) { return t.solo; });
    return !p.tracks[track].mute && (!anySolo || p.tracks[track].solo);
}

template <class Callback> void events(const Project& p, int track, Callback callback) {
    for (const auto& scheduled : scheduledSteps(p, track)) {
        Note event = scheduled.step;
        event.velocity = bounded(event.velocity, 0, 1);
        if (event.note < 0 || event.note > 127)
            throw std::runtime_error("A note is outside MIDI range 0–127.");
        if (scheduled.frame < 0 || scheduled.frame >= songFrames(p))
            throw std::runtime_error("A note falls outside the song.");
        if (event.velocity > 0)
            callback(scheduled.frame, event, scheduled.eventId);
    }
}

void writeAll(QIODevice& file, const char* data, qint64 bytes) {
    while (bytes > 0) {
        const qint64 n = file.write(data, std::min<qint64>(bytes, 1024 * 1024));
        if (n <= 0)
            throw std::runtime_error(file.errorString().toStdString());
        data += n;
        bytes -= n;
    }
}
void writeRaw(const QString& path, const std::vector<float>& samples) {
    if (path.isEmpty())
        throw std::runtime_error("No worker output path was provided.");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error(file.errorString().toStdString());
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    writeAll(file, reinterpret_cast<const char*>(samples.data()),
             static_cast<qint64>(samples.size() * sizeof(float)));
#else
    std::vector<quint32> converted(16384);
    for (size_t i = 0; i < samples.size();) {
        const size_t count = std::min(converted.size(), samples.size() - i);
        for (size_t j = 0; j < count; ++j) {
            quint32 bits;
            std::memcpy(&bits, &samples[i + j], sizeof(bits));
            converted[j] = qToLittleEndian(bits);
        }
        writeAll(file, reinterpret_cast<const char*>(converted.data()),
                 static_cast<qint64>(count * sizeof(quint32)));
        i += count;
    }
#endif
    if (!file.commit())
        throw std::runtime_error(file.errorString().toStdString());
}
} // namespace

std::vector<float> renderTrack(const Project& project, int trackIndex) {
    const int frames = checkedFrames(project);
    if (trackIndex < 0 || trackIndex >= TrackCount)
        throw std::runtime_error("Invalid track index.");
    const Track& track = project.tracks[trackIndex];
    if (track.instrument < 0 || track.instrument >= TrackCount)
        throw std::runtime_error("Invalid instrument type.");
    const double volume = bounded(track.volume, 0, 1.5), pan = bounded(track.pan, -1, 1);
    const double left = std::sqrt(1.0 - pan), right = std::sqrt(1.0 + pan);
    std::vector<float> output(static_cast<size_t>(frames) * 2, 0);
    std::unique_ptr<Sample> sample;
    std::map<int, std::vector<float>> pitched;
    if (!track.samplePath.isEmpty()) {
        sample = std::make_unique<Sample>(loadSample(track.samplePath));
        // Bound CPU work as well as allocations before generating any voices.
        // Every referenced event contributes its exact pitched duration; each
        // distinct note needs one 32-tap conversion and fits within the cache.
        constexpr uint64_t MaxMixedFrames = 96'000'000;
        constexpr uint64_t MaxResampleTaps = 100'000'000;
        uint64_t mixedFrames = 0, resampleTaps = 0;
        std::array<bool, 128> seen{};
        events(project, trackIndex, [&](int onset, const Note& event, uint64_t) {
            const size_t length = sampleVoiceFrames(*sample, event.note, 60);
            mixedFrames += std::min(length, static_cast<size_t>(frames - onset));
            if (!seen[event.note]) {
                resampleTaps += static_cast<uint64_t>(length) * 32;
                seen[event.note] = true;
            }
            if (mixedFrames > MaxMixedFrames)
                throw std::runtime_error("Sample track exceeds the 96-million-frame mixing budget. Shorten "
                                         "the sample, use fewer active steps, or raise its pitch.");
            if (resampleTaps > MaxResampleTaps)
                throw std::runtime_error(
                    "Sample track exceeds the 100-million-tap resampling budget. Shorten the sample, use "
                    "fewer different notes, or raise its pitch.");
        });
    }
    size_t cacheBytes = 0;
    events(project, trackIndex, [&](int onset, const Note& event, uint64_t eventId) {
        const double level = event.velocity * volume;
        if (sample) {
            auto existing = pitched.find(event.note);
            if (existing == pitched.end()) {
                auto audio = resample(*sample, event.note, 60);
                const size_t bytes = audio.size() * sizeof(float);
                while (!pitched.empty() && cacheBytes + bytes > 64 * 1024 * 1024) {
                    cacheBytes -= pitched.begin()->second.size() * sizeof(float);
                    pitched.erase(pitched.begin());
                }
                cacheBytes += bytes;
                existing = pitched.emplace(event.note, std::move(audio)).first;
            }
            const auto& audio = existing->second;
            const double decay = bounded(track.decay, 0, 1), drive = bounded(track.drive, 0, 1);
            // Sample envelopes preserve the imported sound at long decay; short
            // settings make a tight one-shot without modifying the source file.
            const double release = .045 + decay * decay * 8;
            const double gain = 1.0 + drive * 2;
            Biquad leftFilter, rightFilter;
            const double cutoff = 900 + bounded(track.tone, 0, 1) * 18100;
            leftFilter.lowpass(cutoff, SampleRate);
            rightFilter.lowpass(cutoff, SampleRate);
            const size_t length = std::min(audio.size() / 2, static_cast<size_t>(frames - onset));
            for (size_t i = 0; i < length; ++i) {
                const size_t destination = static_cast<size_t>(onset) + i;
                const double envelope = std::exp(-static_cast<double>(i) / (release * SampleRate));
                const double a = leftFilter.next(std::tanh(audio[i * 2] * gain) / std::tanh(gain));
                const double b = rightFilter.next(std::tanh(audio[i * 2 + 1] * gain) / std::tanh(gain));
                output[destination * 2] += static_cast<float>(a * level * left * envelope);
                output[destination * 2 + 1] += static_cast<float>(b * level * right * envelope);
            }
        } else {
            Voice voice(track.instrument, track, event.note,
                        0xa341316cu ^ static_cast<uint32_t>(eventId * 747796405u + trackIndex * 2891336453u));
            const int duration = static_cast<int>(std::ceil(voice.duration * SampleRate));
            for (int i = 0; i < std::min(duration, frames - onset); ++i) {
                const size_t destination = static_cast<size_t>(onset + i);
                const auto [a, b] = voice.next(i);
                output[destination * 2] += static_cast<float>(a * level * left);
                output[destination * 2 + 1] += static_cast<float>(b * level * right);
            }
        }
    });
    const double amount = bounded(project.duck, 0, 1);
    if (track.instrument == 1 && amount > 0) {
        std::vector<float> duck(static_cast<size_t>(frames), 1);
        for (int kickTrack = 0; kickTrack < TrackCount; ++kickTrack) {
            const auto& kick = project.tracks[kickTrack];
            if (kick.instrument != 0 || !audible(project, kickTrack))
                continue;
            events(project, kickTrack, [&](int onset, const Note& event, uint64_t) {
                const double release = .035 + bounded(kick.decay, 0, 1) * .10;
                const int duration = static_cast<int>(release * 7 * SampleRate);
                const double strength = amount * .96 * event.velocity * bounded(kick.volume, 0, 1);
                for (int i = 0; i < std::min(duration, frames - onset); ++i) {
                    const size_t destination = static_cast<size_t>(onset + i);
                    const float gain = static_cast<float>(1 - strength * std::exp(-i / (release * SampleRate)));
                    duck[destination] = std::min(duck[destination], gain);
                }
            });
        }
        for (int i = 0; i < frames; ++i) {
            output[static_cast<size_t>(i) * 2] *= duck[i];
            output[static_cast<size_t>(i) * 2 + 1] *= duck[i];
        }
    }
    return output;
}

namespace {
void accumulateStem(const Project& project, int track, const std::vector<float>& stem,
                    std::vector<float>& output, std::vector<float>& send) {
    if (stem.size() != output.size())
        throw std::runtime_error("A rendered track has the wrong duration.");
    if (!audible(project, track))
        return;
    const int instrument = project.tracks[track].instrument;
    const bool delaySend = instrument == 2 || instrument == 3 || instrument == 4;
    for (size_t i = 0; i < output.size(); ++i) {
        const float value = stem[i];
        if (!std::isfinite(value))
            throw std::runtime_error("A rendered track contains non-finite audio.");
        output[i] += value;
        if (delaySend)
            send[i] += value;
    }
}

void finishMix(const Project& project, std::vector<float>& output, const std::vector<float>& send) {
    const int frames = static_cast<int>(output.size() / 2);
    const double master = bounded(project.master, 0, 1), delay = bounded(project.delay, 0, 1);
    if (delay > 0) {
        // Causal feed-forward taps continue into the export tail. No sound from
        // the end of a song can reappear before its source at the beginning.
        const int interval = static_cast<int>(std::llround(SampleRate * 60.0 / project.bpm * .75));
        for (int tap = 1; tap <= 5; ++tap) {
            const double gain = delay * .38 * std::pow(.47, tap - 1);
            const int offset = interval * tap;
            for (int i = 0; i < frames - offset; ++i) {
                const size_t dest = static_cast<size_t>(i + offset) * 2;
                const size_t src = static_cast<size_t>(i) * 2;
                output[dest] += static_cast<float>(send[src + (tap % 2)] * gain);
                output[dest + 1] += static_cast<float>(send[src + 1 - (tap % 2)] * gain);
            }
        }
    }
    if (project.metronome) {
        for (int beat = 0; beat < project.songBars * 4; ++beat) {
            const int onset = static_cast<int>(std::llround(beat * SampleRate * 60.0 / project.bpm));
            for (int j = 0; j < std::min(SampleRate / 35, frames - onset); ++j) {
                const double t = j / static_cast<double>(SampleRate);
                const double click =
                    std::sin(Tau * (beat % 4 == 0 ? 1760 : 1174.66) * t) * std::exp(-t / .004) * .16;
                const size_t dest = static_cast<size_t>(onset + j) * 2;
                output[dest] += static_cast<float>(click);
                output[dest + 1] += static_cast<float>(click);
            }
        }
    }
    // Causal DC blocking leaves a silent intro silent, unlike subtracting a
    // mean measured over the complete song, which creates a pre-echo everywhere.
    Biquad leftDc, rightDc;
    leftDc.highpass(10, SampleRate);
    rightDc.highpass(10, SampleRate);
    for (int frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < 2; ++channel) {
            const size_t i = static_cast<size_t>(frame) * 2 + channel;
            const double x = (channel ? rightDc : leftDc).next(output[i]) * master;
            // Unity gain through the normal range; a smooth knee handles
            // accidental overlaps while retaining the track balance.
            const double a = std::abs(x), threshold = .82;
            const double limited = a <= threshold ? a : threshold + .16 * std::tanh((a - threshold) / .16);
            // A user may request no tail or a tail shorter than a held sample.
            // Release inside the finite song so the WAV ends at exact silence.
            constexpr int EndFadeFrames = 128;
            const double endGain = std::min(1.0, (frames - frame - 1) / double(EndFadeFrames - 1));
            output[i] = static_cast<float>(std::copysign(limited, x) * endGain);
        }
    }
}
} // namespace

std::vector<float> mixTracks(const Project& project,
                             const std::array<std::vector<float>, TrackCount>& stems) {
    const size_t samples = static_cast<size_t>(checkedFrames(project)) * 2;
    std::vector<float> output(samples, 0), send(samples, 0);
    for (int track = 0; track < TrackCount; ++track)
        accumulateStem(project, track, stems[track], output, send);
    finishMix(project, output, send);
    return output;
}

std::vector<float> renderProject(const Project& project) {
    const size_t samples = static_cast<size_t>(checkedFrames(project)) * 2;
    std::vector<float> output(samples, 0), send(samples, 0);
    // Keep one stem at a time: a full-length song does not require six complete
    // intermediate stereo buffers to coexist in the standalone renderer.
    for (int track = 0; track < TrackCount; ++track)
        accumulateStem(project, track, renderTrack(project, track), output, send);
    finishMix(project, output, send);
    return output;
}

bool writeWave(const QString& path, const std::vector<float>& stereo, QString& error, int repeats) {
    error.clear();
    if (stereo.empty() || stereo.size() % 2 || repeats < 1 || repeats > 64 ||
        stereo.size() / 2 > static_cast<size_t>(SampleRate) * 3600 / static_cast<size_t>(repeats)) {
        error = "Invalid export duration or repeat count.";
        return false;
    }
    for (float value : stereo)
        if (!std::isfinite(value)) {
            error = "Cannot export non-finite audio.";
            return false;
        }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    SF_INFO info{};
    info.samplerate = SampleRate;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_24;
    SNDFILE* wave = sf_open_fd(file.handle(), SFM_WRITE, &info, SF_FALSE);
    if (!wave) {
        error = QString::fromUtf8(sf_strerror(nullptr));
        file.cancelWriting();
        return false;
    }
    sf_command(wave, SFC_SET_CLIPPING, nullptr, SF_TRUE);
    bool success = true;
    const size_t frames = stereo.size() / 2;
    for (int repeat = 0; repeat < repeats && success; ++repeat)
        for (size_t offset = 0; offset < frames;) {
            const sf_count_t count = static_cast<sf_count_t>(std::min<size_t>(frames - offset, 16384));
            if (sf_writef_float(wave, stereo.data() + offset * 2, count) != count) {
                error = QString::fromUtf8(sf_strerror(wave));
                success = false;
                break;
            }
            offset += static_cast<size_t>(count);
        }
    const int closeError = sf_close(wave);
    if (closeError && success) {
        error = QString::fromUtf8(sf_error_number(closeError));
        success = false;
    }
    if (!success) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

int workerMain() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        QJsonObject result;
        result["ok"] = false;
        result["frames"] = 0;
        result["id"] = QJsonValue::Null;
        result["path"] = QString();
        result["error"] = QString();
        try {
            // A song can contain 128 phrases per track; the project loader
            // permits 16 MiB, plus a small IPC envelope and output path.
            if (line.size() > 17 * 1024 * 1024)
                throw std::runtime_error("Worker request is too large.");
            QJsonParseError parseError{};
            const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                throw std::runtime_error("Invalid worker JSON request.");
            const auto job = document.object();
            result["id"] = job.contains("id") ? job.value("id") : QJsonValue(QJsonValue::Null);
            const QString path = job.value("path").toString();
            result["path"] = path;
            const QJsonValue trackValue = job.value("track");
            const double trackNumber = trackValue.toDouble(-1);
            if (!trackValue.isDouble() || trackNumber < 0 || trackNumber >= TrackCount ||
                trackNumber != std::floor(trackNumber))
                throw std::runtime_error("Invalid worker track index.");
            if (!job.value("project").isObject())
                throw std::runtime_error("Worker request has no project.");
            Project project;
            QString error;
            if (!projectFromJson(job.value("project").toObject(), project, error))
                throw std::runtime_error(error.toStdString());
            const auto samples = renderTrack(project, static_cast<int>(trackNumber));
            writeRaw(path, samples);
            result["ok"] = true;
            result["frames"] = static_cast<qint64>(samples.size() / 2);
        } catch (const std::exception& error) {
            result["error"] = QString::fromUtf8(error.what());
        }
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n' << std::flush;
    }
    return std::cin.bad() ? 1 : 0;
}
} // namespace fc
