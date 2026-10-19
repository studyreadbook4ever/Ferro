#include "audio.h"
#include "project.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

int audioTests() {
    int failures = 0;
    const auto check = [&](bool ok, const char* description) {
        if (!ok) {
            ++failures;
            std::cerr << "Audio: " << description << '\n';
        }
    };
    const auto silent = [](const std::vector<float>& values) {
        return std::all_of(values.begin(), values.end(), [](float v) { return v == 0.0f; });
    };
    const auto bounded = [](const std::vector<float>& values) {
        return std::all_of(values.begin(), values.end(),
                           [](float v) { return std::isfinite(v) && std::abs(v) <= 1.0f; });
    };
    {
        fc::AudioEngine audio;
        std::vector<float> out(512, 1.0f);
        audio.processForTest(out.data(), 256);
        check(silent(out), "new stopped engine emits silence");
        check(audio.position() == 0 && audio.frames() == 0, "new transport is empty at frame zero");

        constexpr uint64_t barFrames = 48000; // 240 BPM, four quarter notes.
        std::vector<float> positive(barFrames * 8, 0.5f);
        audio.submit(positive, 240.0);
        audio.processForTest(out.data(), 256);
        check(audio.frames() == barFrames * 4 && silent(out), "stopped submission activates without sound");
        audio.setGain(1.0f);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        check(audio.playing() && audio.position() == 256, "play advances sample position");
        check(std::abs(out.back() - 0.5f) < 0.0001f, "startup and master gain settle to the source");

        std::vector<float> negative(barFrames * 8, -0.5f);
        audio.submit(negative, 240.0);
        // Repeated edits retain only the last pending buffer while preserving
        // the currently audible immutable buffer.
        for (int i = 0; i < 250; ++i) {
            std::vector<float> discarded(200 + i * 2, 0.9f);
            audio.submit(discarded, 240.0);
        }
        audio.submit(negative, 240.0);
        std::vector<float> untilBoundary((barFrames - 256) * 2);
        audio.processForTest(untilBoundary.data(), barFrames - 256);
        check(std::all_of(untilBoundary.begin(), untilBoundary.end(), [](float v) { return v == 0.5f; }),
              "rapid pending replacement does not mutate active audio before the bar");
        check(audio.position() == barFrames, "pending swap waits for the complete current bar");
        audio.processForTest(out.data(), 256);
        check(out.front() > 0.49f && std::abs(out.back() + 0.5f) < 0.0001f,
              "bar exchange crossfades the old and new immutable arrangements");
        float largestStep = 0.0f;
        for (size_t i = 2; i < out.size(); i += 2)
            largestStep = std::max(largestStep, std::abs(out[i] - out[i - 2]));
        check(largestStep < 0.005f, "arrangement replacement has no discontinuous sample jump");
        audio.collect();

        // A tempo replacement maps bar two at 240 BPM to bar two at 120 BPM.
        std::vector<float> slower(384000 * 2, 0.25f);
        audio.submit(slower, 120.0);
        check(audio.tempo() == 240.0, "pending tempo is not exposed before audible exchange");
        std::vector<float> restOfBar((barFrames - 256) * 2);
        audio.processForTest(restOfBar.data(), barFrames - 256);
        audio.processForTest(out.data(), 256);
        check(audio.frames() == 384000 && audio.position() == 192256,
              "tempo and song-length exchange preserves musical bar position");
        check(std::abs(out.back() - 0.25f) < 0.0001f, "tempo exchange renders the replacement");
        check(audio.tempo() == 120.0, "published tempo follows the active arrangement");

        audio.rewind();
        audio.processForTest(out.data(), 256);
        check(audio.position() == 256, "rewind while playing resets the cursor before rendering");
        audio.setPlaying(false);
        audio.rewind();
        audio.processForTest(out.data(), 256);
        check(audio.position() == 0 && out.front() > 0.24f && out.front() < 0.25f &&
                  std::all_of(out.begin() + 256, out.end(), [](float v) { return v == 0.0f; }),
              "Stop plus rewind fades the last sample and keeps the cursor at zero");
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        audio.setPlaying(false);
        audio.processForTest(out.data(), 256);
        check(std::all_of(out.begin() + 256, out.end(), [](float v) { return v == 0.0f; }),
              "stop releases in 128 frames and then emits exact silence");
        const auto stoppedPosition = audio.position();
        audio.processForTest(out.data(), 256);
        check(silent(out) && audio.position() == stoppedPosition,
              "stopped playback remains silent without moving");
        audio.rewind();
        audio.processForTest(out.data(), 256);
        check(audio.position() == 0 && silent(out), "stopped rewind resets to frame zero silently");
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        audio.close();
        audio.processForTest(out.data(), 256);
        check(silent(out) && !audio.playing(), "closing output clears tails before a paused reconnect");

        std::vector<float> unsafe{std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::infinity(), 8.0f, -8.0f, 0.25f};
        audio.submit(unsafe, std::numeric_limits<double>::quiet_NaN());
        audio.processForTest(out.data(), 256);
        check(audio.frames() == 2, "an incomplete stereo frame is discarded");
        audio.setGain(2.0f);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        check(bounded(out), "nonfinite and over-range source samples never reach the device");
        check(audio.position() == audio.frames() && !audio.playing() && audio.peak() <= 1.0f,
              "a short song stops at its exact end and peak remains in bounds");
        audio.setGain(0.0f);
        audio.processForTest(out.data(), 256);
        audio.processForTest(out.data(), 256);
        check(silent(out), "master gain zero produces exact silence");
        audio.setGain(std::numeric_limits<float>::quiet_NaN());
        audio.processForTest(out.data(), 256);
        check(silent(out), "nonfinite master gain is safely muted");

        audio.setPlaying(false);
        audio.rewind();
        audio.submit({}, 200.0);
        audio.processForTest(out.data(), 256);
        check(audio.frames() == 0 && audio.position() == 0 && silent(out),
              "empty arrangements are safe and silent");
    }
    {
        fc::AudioEngine audio;
        constexpr uint64_t bar = 48000;
        std::vector<float> source(bar * 4, 0.5f), out(512);
        const auto first = audio.submit(source, 240.0);
        check(first != 0 && audio.revision() == 0,
              "submission returns a nonzero revision before the callback acknowledges it");
        audio.processForTest(out.data(), 256);
        check(audio.revision() == first && audio.frames() == bar * 2,
              "buffer acquisition publishes its revision and frame metadata");
        const auto discarded = audio.submit(source, 220.0);
        const auto latest = audio.submit(source, 200.0);
        check(first < discarded && discarded < latest && audio.revision() == first,
              "replacement publications are monotonic and remain unacknowledged while pending");
        audio.processForTest(out.data(), 256);
        check(audio.revision() == latest && audio.tempo() == 200.0,
              "overwriting a pending buffer acknowledges only the latest acquired publication");
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        const auto nextBarRevision = audio.submit(source, 180.0);
        audio.processForTest(out.data(), 256);
        check(audio.revision() == latest,
              "a pending playing replacement is not acknowledged before its bar exchange");
        audio.seek(57600); // Next exact bar at 200 BPM.
        audio.processForTest(out.data(), 256);
        check(audio.revision() == nextBarRevision && audio.tempo() == 180.0,
              "playing bar exchange acknowledges the replacement actually acquired");
        audio.clear();
        check(audio.revision() == nextBarRevision, "clear acknowledgement waits for the callback");
        audio.processForTest(out.data(), 256);
        check(audio.revision() == 0 && audio.frames() == 0 && audio.position() == 0,
              "consuming clear resets the active revision and song metadata");
        const auto newSession = audio.submit(source, 120.0);
        audio.processForTest(out.data(), 256);
        check(newSession > nextBarRevision && audio.revision() == newSession,
              "publication IDs remain monotonic across session clear");
        audio.clear();
        const auto afterClear = audio.submit(source, 140.0);
        audio.processForTest(out.data(), 256);
        check(audio.revision() == afterClear && audio.frames() == bar * 2,
              "clear preserves and acknowledges a newly submitted session buffer");
    }
    {
        fc::AudioEngine audio;
        constexpr uint64_t length = 777;
        std::vector<float> source(length * 2, 0.5f), out(1024 * 2);
        check(!audio.looping(), "song looping is disabled by default");
        audio.submit(source, 120.0);
        audio.setGain(1.0f);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 1024);
        check(audio.position() == length && !audio.playing(),
              "a song ending in the middle of a callback retains its exact final frame and stops");
        check(std::all_of(out.begin() + length * 2, out.end(), [](float v) { return v == 0.0f; }),
              "all samples after the song end are exact silence");
        check(out[(length - 1) * 2] > 0.0f && out[(length - 1) * 2] < 0.005f,
              "song end releases inside its final frames instead of cutting off abruptly");
        audio.processForTest(out.data(), 1024);
        check(silent(out) && audio.position() == length && !audio.playing(),
              "the next callback cannot accidentally restart a finished song");

        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        check(audio.position() == 256 && audio.playing() && out[510] > 0.49f,
              "Play after song end explicitly starts again from frame zero");
        audio.seek(600);
        audio.processForTest(out.data(), 400);
        check(audio.position() == length && !audio.playing() && bounded(out),
              "a seek while playing can reach song end safely in the same callback");
        check(std::all_of(out.begin() + 177 * 2, out.begin() + 400 * 2,
                          [](float v) { return v == 0.0f; }),
              "seeking while playing does not introduce wrapped samples after the end");

        audio.seek(120);
        audio.processForTest(out.data(), 256);
        check(audio.position() == 120 && silent(std::vector<float>(out.begin(), out.begin() + 512)),
              "a paused seek selects an exact frame without playing sound");
        audio.setPlaying(true);
        audio.processForTest(out.data(), 128);
        check(audio.position() == 248 && audio.playing(), "Play resumes at a paused seek position");
        audio.setPlaying(false);
        audio.seek(500);
        audio.processForTest(out.data(), 256);
        check(audio.position() == 500 && out.front() > 0.0f &&
                  std::all_of(out.begin() + 256, out.begin() + 512, [](float v) { return v == 0.0f; }),
              "Stop and seek release the previous sample without advancing the requested frame");
        audio.seek(std::numeric_limits<uint64_t>::max());
        audio.processForTest(out.data(), 256);
        check(audio.position() == length && !audio.playing(), "seeking past the song clamps to its end");
        audio.setPlaying(true);
        audio.processForTest(out.data(), 128);
        check(audio.position() == 128 && audio.playing(), "Play from a clamped end rewinds exactly once");
        audio.seek(400);
        audio.processForTest(out.data(), 128);
        check(audio.position() == 528, "an absolute playing seek advances from its target");
        audio.seek(400);
        audio.processForTest(out.data(), 128);
        check(audio.position() == 528, "repeated seeks to the same frame are independent commands");
    }
    {
        fc::AudioEngine audio;
        std::vector<float> source(4096, 0.5f), out(256);
        audio.submit(source, 120.0);
        audio.seek(1000);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 128);
        check(audio.position() == 1128 && audio.playing(),
              "a seek before the first callback is applied after acquiring the pending render");

        fc::AudioEngine waiting;
        waiting.seek(1000);
        waiting.setPlaying(true);
        waiting.processForTest(out.data(), 128);
        check(waiting.position() == 0 && silent(out), "Play can wait silently for its first render");
        waiting.submit(source, 120.0);
        waiting.processForTest(out.data(), 128);
        check(waiting.position() == 1128,
              "a seek is retained when the asynchronous first render has not arrived yet");
    }
    {
        constexpr uint64_t bar = 48000;
        fc::AudioEngine audio;
        std::vector<float> original(bar * 4 * 2, 0.5f), shortSong(bar * 2, -0.5f);
        std::vector<float> longSong(bar * 8 * 2, 0.25f), out(512);
        audio.submit(original, 240.0);
        audio.seek(bar + 128);
        audio.setGain(1.0f);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        audio.submit(shortSong, 240.0);
        const auto untilBar = bar * 2 - audio.position();
        std::vector<float> remainder(untilBar * 2);
        audio.processForTest(remainder.data(), untilBar);
        check(audio.position() == bar * 2 && audio.playing(),
              "a shorter pending song still waits for the next bar boundary");
        audio.processForTest(out.data(), 256);
        check(audio.frames() == bar && audio.position() == bar && !audio.playing() && silent(out),
              "shortening past the playhead clamps to the new end instead of wrapping to its start");
        audio.submit(longSong, 240.0);
        audio.processForTest(out.data(), 256);
        check(audio.frames() == bar * 8 && audio.position() == bar && !audio.playing() && silent(out),
              "a longer render activates at song end without a future bar and preserves the position");
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        check(audio.position() == bar + 256 && audio.playing(),
              "extending a finished song lets Play resume from the preserved musical position");

        audio.setPlaying(false);
        audio.seek(bar * 3 + 123);
        audio.processForTest(out.data(), 256);
        audio.submit(std::vector<float>(bar * 4, 0.5f), 120.0);
        audio.processForTest(out.data(), 256);
        check(audio.position() == bar * 2 && audio.frames() == bar * 2 && audio.tempo() == 120.0,
              "a paused tempo change maps musical position then clamps to a shorter song end");
    }
    {
        fc::AudioEngine audio;
        constexpr uint64_t length = 1000;
        std::vector<float> source(length * 2, 0.5f), out(1400 * 2);
        audio.submit(source, 120.0);
        audio.setGain(1.0f);
        audio.setLooping(true);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 1400);
        check(audio.looping() && audio.playing() && audio.position() == 400 && out.back() > 0.49f,
              "explicit whole-song looping wraps and continues without the one-shot end release");
        audio.setLooping(false);
        audio.processForTest(out.data(), 800);
        check(!audio.looping() && !audio.playing() && audio.position() == length &&
                  std::all_of(out.begin() + 1200, out.begin() + 1600, [](float v) { return v == 0.0f; }),
              "disabling looping completes the current pass and stops at its exact end");
        audio.setLooping(true);
        audio.processForTest(out.data(), 256);
        check(!audio.playing() && audio.position() == length,
              "enabling looping alone never restarts a stopped song");
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        check(audio.position() == 256 && audio.playing(), "explicit loop Play also restarts from song end");
        audio.seek(length);
        audio.processForTest(out.data(), 256);
        check(audio.position() == length && !audio.playing(),
              "an explicit seek to song end stops even with whole-song looping enabled");
    }
    {
        constexpr uint64_t bar = 48000;
        fc::AudioEngine audio;
        std::vector<float> original(bar * 4 * 2, 0.5f), replacement(bar * 4 * 2, -0.5f), out(512);
        audio.submit(original, 240.0);
        audio.setGain(1.0f);
        audio.setLooping(true);
        audio.seek(bar * 4 - 128);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        check(audio.position() == 128 && audio.playing(), "loop wrap resets the bar scheduler");
        audio.submit(replacement, 240.0);
        std::vector<float> remainder((bar - 128) * 2);
        audio.processForTest(remainder.data(), bar - 128);
        check(std::all_of(remainder.begin(), remainder.end(), [](float v) { return v == 0.5f; }),
              "a pending edit after loop wrap waits for the next musical bar");
        audio.processForTest(out.data(), 256);
        check(std::abs(out.back() + 0.5f) < 0.0001f && audio.position() == bar + 256,
              "bar-boundary crossfades continue correctly after explicit loop wrap");
    }
    {
        fc::AudioEngine audio;
        const double period = fc::SampleRate * 240.0 / 203.0;
        const uint64_t total = static_cast<uint64_t>(std::llround(period * 8));
        std::vector<float> original(total * 2, 0.5f), replacement(total * 2, -0.5f);
        std::vector<float> scratch(512);
        audio.submit(original, 203.0);
        audio.setGain(1.0f);
        audio.setPlaying(true);
        audio.processForTest(scratch.data(), 256);
        const auto nearThirdBar = static_cast<uint64_t>(std::llround(period * 2)) + 17;
        scratch.resize((nearThirdBar - 256) * 2);
        audio.processForTest(scratch.data(), nearThirdBar - 256);
        audio.submit(replacement, 203.0);
        const auto thirdBar = static_cast<uint64_t>(std::llround(period * 3));
        scratch.resize((thirdBar - nearThirdBar) * 2);
        audio.processForTest(scratch.data(), thirdBar - nearThirdBar);
        check(std::all_of(scratch.begin(), scratch.end(), [](float v) { return v == 0.5f; }),
              "fractional-tempo exchange waits for the globally rounded bar boundary");
        scratch.resize(512);
        audio.processForTest(scratch.data(), 256);
        check(std::abs(scratch.back() + 0.5f) < 0.0001f,
              "fractional-tempo bars do not accumulate one-frame rounding drift");
        audio.submit(original, 203.0);
        const auto fourthBar = static_cast<uint64_t>(std::llround(period * 4));
        const auto remaining = fourthBar - audio.position();
        scratch.resize(remaining * 2);
        audio.processForTest(scratch.data(), remaining);
        check(std::all_of(scratch.begin(), scratch.end(), [](float v) { return v == -0.5f; }),
              "the next fractional-tempo bar remains scheduled after a rounded-down boundary");
        scratch.resize(512);
        audio.processForTest(scratch.data(), 256);
        check(std::abs(scratch.back() - 0.5f) < 0.0001f,
              "consecutive fractional-tempo exchanges each occur at their musical bar");
    }
    {
        fc::AudioEngine audio;
        std::vector<float> oldSession(4096, 0.75f), newSession(2048, -0.25f), out(512);
        audio.submit(oldSession, 200.0);
        audio.setGain(1.0f);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        audio.submit(oldSession, 210.0); // A previous session's pending render.
        audio.clear();
        audio.setPlaying(true); // User presses Play before the callback runs.
        audio.processForTest(out.data(), 256);
        check(silent(out) && audio.frames() == 0 && audio.position() == 0,
              "clearing then immediately playing cannot replay active or pending old-session audio");
        audio.submit(newSession, 250.0);
        audio.processForTest(out.data(), 256);
        check(audio.frames() == 1024 && audio.tempo() == 250.0 && std::abs(out.back() + 0.25f) < 0.0001f,
              "a new render after clear becomes audible immediately");
        audio.clear();
        audio.submit(oldSession, 270.0);
        audio.setPlaying(true);
        audio.processForTest(out.data(), 256);
        check(audio.frames() == 2048 && audio.tempo() == 270.0 && std::abs(out.back() - 0.75f) < 0.0001f,
              "clear preserves a newer pending render submitted before the callback acknowledges clear");
    }
    {
        // Exercise the real publisher/callback ownership boundary concurrently.
        // ASan/TSan can use this same test without a sound card or a Qt loop.
        fc::AudioEngine audio;
        std::vector<float> source(1024, 0.25f);
        audio.submit(source, 1000.0);
        audio.setLooping(true);
        audio.setPlaying(true);
        std::atomic<bool> done{false};
        std::atomic<bool> invalid{false};
        std::thread renderer([&] {
            std::vector<float> out(128 * 2);
            while (!done.load(std::memory_order_acquire)) {
                audio.processForTest(out.data(), 128);
                if (!bounded(out))
                    invalid.store(true, std::memory_order_relaxed);
            }
        });
        for (int edit = 0; edit < 3000; ++edit) {
            std::vector<float> next((128 + edit % 100) * 2, (edit % 2 ? -0.75f : 0.75f));
            audio.submit(next, 200.0 + edit % 100);
            if (edit % 37 == 0)
                audio.rewind();
            if (edit % 41 == 0)
                audio.seek(edit % 311);
            if (edit % 59 == 0)
                audio.setLooping(!audio.looping());
            if (edit % 73 == 0)
                audio.setPlaying(!audio.playing());
            if (edit % 101 == 0)
                audio.clear();
            audio.collect();
        }
        done.store(true, std::memory_order_release);
        renderer.join();
        audio.collect();
        check(!invalid.load(),
              "concurrent publication, recycling, seek, loop changes and playback stay finite and bounded");
    }
    return failures;
}
