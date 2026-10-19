#include "audio.h"
#include "project.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <portaudio.h>

namespace fc {
namespace {
constexpr unsigned CrossfadeFrames = 256;
constexpr unsigned TransportRampFrames = 128;
constexpr uint64_t NoSeek = std::numeric_limits<uint64_t>::max();
enum class BufferState : unsigned { Free, Building, Owned, Retired };

// These types must never fall back to a library lock in the audio callback.
static_assert(std::atomic<void*>::is_always_lock_free);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<unsigned long>::is_always_lock_free);
static_assert(std::atomic<float>::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<BufferState>::is_always_lock_free);

float finiteSample(float value) noexcept {
    return std::isfinite(value) ? value : 0.0f;
}
QString paError(const char* operation, PaError error) {
    return QString::fromLatin1(operation) + ": " + QString::fromUtf8(Pa_GetErrorText(error));
}
} // namespace

struct AudioEngine::Impl {
    struct Buffer {
        std::atomic<BufferState> state{BufferState::Free};
        std::vector<float> samples;
        uint64_t frames = 0;
        uint64_t revision = 0;
        double barFrames = 1.0;
        double bpm = 200.0;
    };

    // At most two callback-owned buffers, one pending buffer, and one being
    // prepared by the control thread. Replacing pending never grows a queue.
    std::array<Buffer, 4> buffers;
    std::atomic<Buffer*> pending{nullptr};
    // Bit zero is Play; higher bits identify each control-thread command. The
    // callback can stop at song end without overwriting a newer Play request.
    std::atomic<uint64_t> requestedTransport{0};
    std::atomic<bool> requestedLooping{false};
    std::atomic<float> requestedGain{0.8f};
    std::atomic<uint64_t> seekRequest{NoSeek};
    std::atomic<uint64_t> clearRequest{0};
    std::atomic<uint64_t> publishedPosition{0};
    std::atomic<uint64_t> publishedFrames{0};
    std::atomic<uint64_t> publishedRevision{0};
    std::atomic<double> publishedTempo{200.0};
    std::atomic<float> publishedPeak{0.0f};
    std::atomic<unsigned long> underflows{0};

    // Only the callback touches the following rendering state.
    Buffer* active = nullptr;
    Buffer* fading = nullptr;
    uint64_t cursor = 0;
    uint64_t fadeCursor = 0;
    uint64_t nextBarFrame = 0;
    uint64_t handledTransport = 0;
    uint64_t handledClear = 0;
    unsigned fadeRemaining = 0;
    unsigned seekRemaining = 0;
    float seekLeft = 0.0f, seekRight = 0.0f;
    float lastLeft = 0.0f, lastRight = 0.0f;
    float stopTailLeft = 0.0f, stopTailRight = 0.0f;
    unsigned stopTailRemaining = 0;
    float transportGain = 0.0f;
    float outputGain = 0.8f;

    // Stream lifecycle and strings belong to the control thread.
    PaStream* stream = nullptr;
    bool initialized = false;
    QString device;
    uint64_t nextRevision = 0;

    static void retire(Buffer* buffer) noexcept {
        if (buffer)
            buffer->state.store(BufferState::Retired, std::memory_order_release);
    }

    void finishFade() noexcept {
        retire(fading);
        fading = nullptr;
        fadeRemaining = 0;
    }

    static uint64_t barBoundary(const Buffer* buffer, uint64_t position, bool after) noexcept {
        uint64_t bar = static_cast<uint64_t>(position / buffer->barFrames);
        uint64_t boundary = static_cast<uint64_t>(std::llround(bar * buffer->barFrames));
        if (boundary < position) {
            boundary = static_cast<uint64_t>(std::llround(++bar * buffer->barFrames));
        }
        if (after && boundary == position) {
            boundary = static_cast<uint64_t>(std::llround((bar + 1) * buffer->barFrames));
        }
        return boundary;
    }

    void takePending(bool crossfade, bool loop) noexcept {
        // No pointer obtained by a plain load is ever dereferenced. The atomic
        // exchange transfers exclusive ownership from publisher to callback.
        Buffer* next = pending.exchange(nullptr, std::memory_order_acq_rel);
        if (!next)
            return;
        finishFade();
        uint64_t nextCursor = 0;
        if (active && next->frames) {
            // Preserve the musical position across tempo changes. A playing
            // exchange occurs exactly at an integer bar boundary.
            nextCursor = static_cast<uint64_t>(std::llround(cursor * next->barFrames / active->barFrames));
            nextCursor = loop ? nextCursor % next->frames : std::min(nextCursor, next->frames);
        }
        if (crossfade && active && active->frames) {
            fading = active;
            fadeCursor = cursor;
            fadeRemaining = CrossfadeFrames;
        } else {
            retire(active);
        }
        active = next;
        cursor = nextCursor;
        nextBarFrame = barBoundary(active, cursor, false);
        publishedFrames.store(active->frames, std::memory_order_relaxed);
        publishedTempo.store(active->bpm, std::memory_order_relaxed);
        publishedRevision.store(active->revision, std::memory_order_release);
    }

    static float channel(const Buffer* buffer, uint64_t frame, unsigned side) noexcept {
        return buffer && frame < buffer->frames ? finiteSample(buffer->samples[frame * 2 + side]) : 0.0f;
    }

    void process(float* output, unsigned long frameCount, PaStreamCallbackFlags flags) noexcept {
        if (flags & paOutputUnderflow)
            underflows.fetch_add(1, std::memory_order_relaxed);
        if (!output)
            return;

        const uint64_t clear = clearRequest.load(std::memory_order_acquire);
        if (clear != handledClear) {
            handledClear = clear;
            finishFade();
            retire(active);
            active = nullptr;
            cursor = nextBarFrame = 0;
            transportGain = 0.0f;
            seekRemaining = stopTailRemaining = 0;
            lastLeft = lastRight = 0.0f;
            publishedFrames.store(0, std::memory_order_relaxed);
            publishedRevision.store(0, std::memory_order_release);
        }

        uint64_t command = requestedTransport.load(std::memory_order_acquire);
        bool run = (command & 1) != 0;
        const bool newPlay = run && command != handledTransport;
        handledTransport = command;
        const bool loop = requestedLooping.load(std::memory_order_relaxed);
        const float targetGain = requestedGain.load(std::memory_order_relaxed);
        uint64_t seek = seekRequest.exchange(NoSeek, std::memory_order_acq_rel);

        if ((!run && transportGain == 0.0f) || !active || !active->frames ||
            (!loop && cursor >= active->frames)) {
            // A stopped or finished song can receive a longer arrangement
            // immediately; it has no future bar boundary to wait for.
            finishFade();
            takePending(false, loop && run);
        }
        if (seek != NoSeek && !active) {
            // The UI can seek before its first asynchronous render arrives.
            // Return the request to the mailbox unless a newer seek replaced it.
            uint64_t empty = NoSeek;
            seekRequest.compare_exchange_strong(empty, seek, std::memory_order_acq_rel);
            seek = NoSeek;
        }
        if (seek != NoSeek || (newPlay && active && active->frames && cursor >= active->frames)) {
            cursor = seek != NoSeek ? std::min(seek, active ? active->frames : uint64_t(0)) : 0;
            finishFade();
            nextBarFrame = active ? barBoundary(active, cursor, false) : 0;
            if (run && active && cursor < active->frames) {
                seekLeft = lastLeft;
                seekRight = lastRight;
                seekRemaining = TransportRampFrames;
                stopTailRemaining = 0;
            } else {
                // A paused seek keeps its exact position while the previous
                // output sample releases independently of any source buffer.
                if (transportGain > 0.0f || stopTailRemaining) {
                    stopTailLeft = lastLeft;
                    stopTailRight = lastRight;
                    stopTailRemaining = TransportRampFrames;
                }
                seekRemaining = 0;
                transportGain = 0.0f;
            }
        }
        if (run && stopTailRemaining && active && cursor < active->frames) {
            seekLeft = lastLeft;
            seekRight = lastRight;
            seekRemaining = TransportRampFrames;
            stopTailRemaining = 0;
        }

        const auto stopAtEnd = [&]() noexcept {
            run = false;
            // A compare/exchange protects a newer command published while this
            // callback was rendering the last frames of the song.
            requestedTransport.compare_exchange_strong(command, command & ~uint64_t(1),
                                                       std::memory_order_acq_rel);
            transportGain = 0.0f;
            seekRemaining = 0;
            finishFade();
        };
        const auto resolveEnd = [&]() noexcept {
            if (!run || !active || cursor < active->frames)
                return;
            if (!active->frames) {
                stopAtEnd();
            } else if (loop) {
                cursor = 0;
                nextBarFrame = 0;
            } else {
                takePending(false, false);
                if (active && cursor >= active->frames)
                    stopAtEnd();
            }
        };
        // Explicitly seeking to the end stops instead of turning into a loop.
        if (seek != NoSeek && active && cursor >= active->frames) {
            if (run)
                stopAtEnd();
        } else {
            resolveEnd();
        }

        float blockPeak = 0.0f;
        const float gainStep =
            (targetGain - outputGain) / static_cast<float>(std::max<unsigned long>(1, frameCount));
        for (unsigned long frame = 0; frame < frameCount; ++frame) {
            if (run && !fading && active && active->frames && (cursor == 0 || cursor == nextBarFrame)) {
                takePending(transportGain > 0.0f, loop);
                nextBarFrame = barBoundary(active, cursor, true);
                resolveEnd();
            }
            if (run && active && cursor < active->frames)
                transportGain = std::min(1.0f, transportGain + 1.0f / TransportRampFrames);
            else
                transportGain = std::max(0.0f, transportGain - 1.0f / TransportRampFrames);
            outputGain += gainStep;

            float left = 0.0f, right = 0.0f;
            if (transportGain > 0.0f && active && cursor < active->frames) {
                const uint64_t remaining = active->frames - cursor;
                left = channel(active, cursor, 0);
                right = channel(active, cursor, 1);
                ++cursor;
                if (fading) {
                    const float mix =
                        static_cast<float>(CrossfadeFrames - fadeRemaining + 1) / CrossfadeFrames;
                    left = channel(fading, fadeCursor, 0) * (1.0f - mix) + left * mix;
                    right = channel(fading, fadeCursor, 1) * (1.0f - mix) + right * mix;
                    if (fadeCursor < fading->frames)
                        ++fadeCursor;
                    if (loop && fadeCursor >= fading->frames)
                        fadeCursor = 0;
                    if (--fadeRemaining == 0)
                        finishFade();
                }
                // Release the final source samples inside the song, leaving
                // exact silence after its end rather than an extra audio tail.
                const float endGain = !loop ? std::min(1.0f, static_cast<float>(remaining) /
                                                               TransportRampFrames) : 1.0f;
                left *= outputGain * transportGain * endGain;
                right *= outputGain * transportGain * endGain;
                if (seekRemaining) {
                    const float mix =
                        static_cast<float>(TransportRampFrames - seekRemaining + 1) / TransportRampFrames;
                    left = seekLeft * (1.0f - mix) + left * mix;
                    right = seekRight * (1.0f - mix) + right * mix;
                    --seekRemaining;
                }
            } else if (!run && stopTailRemaining) {
                const float release = static_cast<float>(--stopTailRemaining) / TransportRampFrames;
                left = stopTailLeft * release;
                right = stopTailRight * release;
            }
            left = std::clamp(finiteSample(left), -1.0f, 1.0f);
            right = std::clamp(finiteSample(right), -1.0f, 1.0f);
            output[frame * 2] = lastLeft = left;
            output[frame * 2 + 1] = lastRight = right;
            blockPeak = std::max(blockPeak, std::max(std::abs(left), std::abs(right)));
            resolveEnd();
        }
        outputGain = targetGain;
        if (!run && transportGain == 0.0f) {
            finishFade();
            takePending(false, false);
        }
        publishedPosition.store(cursor, std::memory_order_relaxed);
        publishedPeak.store(blockPeak, std::memory_order_relaxed);
    }

    static int callback(const void*, void* output, unsigned long frames, const PaStreamCallbackTimeInfo*,
                        PaStreamCallbackFlags flags, void* user) noexcept {
        static_cast<Impl*>(user)->process(static_cast<float*>(output), frames, flags);
        return paContinue;
    }
};

AudioEngine::AudioEngine() : d(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() {
    close();
}

bool AudioEngine::open(QString& error, int selectedDevice) {
    close();
    error.clear();
    PaError result = Pa_Initialize();
    if (result != paNoError) {
        error = paError("Cannot initialize Linux audio", result);
        return false;
    }
    d->initialized = true;
    const int count = Pa_GetDeviceCount();
    if (count < 0) {
        error = paError("Cannot enumerate audio devices", count);
        close();
        return false;
    }
    PaDeviceIndex index = selectedDevice < 0 ? Pa_GetDefaultOutputDevice() : selectedDevice;
    if (selectedDevice < 0 && index == paNoDevice) {
        for (int i = 0; i < count; ++i) {
            const auto* info = Pa_GetDeviceInfo(i);
            if (info && info->maxOutputChannels >= 2) {
                index = i;
                break;
            }
        }
    }
    if (index < 0 || index >= count) {
        error = "No stereo audio output is available. Start PipeWire/JACK or connect an output device; WAV "
                "export still works.";
        close();
        return false;
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
    if (!info || info->maxOutputChannels < 2) {
        error = "The selected audio device does not provide stereo output.";
        close();
        return false;
    }
    PaStreamParameters output{};
    output.device = index;
    output.channelCount = 2;
    output.sampleFormat = paFloat32;
    output.suggestedLatency = std::max(info->defaultLowOutputLatency, 256.0 / SampleRate);
    result = Pa_IsFormatSupported(nullptr, &output, SampleRate);
    if (result != paFormatIsSupported) {
        error = QString("%1 cannot play 48 kHz stereo float audio: %2")
                    .arg(QString::fromUtf8(info->name), QString::fromUtf8(Pa_GetErrorText(result)));
        close();
        return false;
    }
    d->device = QString::fromUtf8(info->name);
    result = Pa_OpenStream(&d->stream, nullptr, &output, SampleRate, 256, paClipOff | paDitherOff,
                           Impl::callback, d.get());
    if (result == paNoError)
        result = Pa_StartStream(d->stream);
    if (result != paNoError) {
        error = paError("Cannot start audio output", result);
        close();
        return false;
    }
    return true;
}

void AudioEngine::close() {
    setPlaying(false);
    if (d->stream) {
        Pa_StopStream(d->stream);
        Pa_CloseStream(d->stream);
        d->stream = nullptr;
    }
    if (d->initialized) {
        Pa_Terminate();
        d->initialized = false;
    }
    // The stream has joined its callback, so these fields can now be reset by
    // the control thread. Reconnecting while paused must never play a tail.
    d->finishFade();
    d->transportGain = 0.0f;
    d->seekRemaining = d->stopTailRemaining = 0;
    d->lastLeft = d->lastRight = 0.0f;
    d->outputGain = d->requestedGain.load(std::memory_order_relaxed);
    d->publishedPeak.store(0.0f, std::memory_order_relaxed);
    d->device.clear();
}

void AudioEngine::collect() {
    for (auto& buffer : d->buffers) {
        BufferState state = BufferState::Retired;
        if (buffer.state.compare_exchange_strong(state, BufferState::Building, std::memory_order_acquire)) {
            // Destruction and allocation happen exclusively on this thread.
            std::vector<float>().swap(buffer.samples);
            buffer.state.store(BufferState::Free, std::memory_order_release);
        }
    }
}

void AudioEngine::clear() {
    setPlaying(false);
    Impl::retire(d->pending.exchange(nullptr, std::memory_order_acq_rel));
    d->seekRequest.store(NoSeek, std::memory_order_release);
    // Publish after pending invalidation. A later submit remains available for
    // the callback to acquire after it has discarded the previous session.
    d->clearRequest.fetch_add(1, std::memory_order_release);
}

uint64_t AudioEngine::submit(const std::vector<float>& samples, double bpm) {
    collect();
    Impl::Buffer* destination = nullptr;
    for (auto& buffer : d->buffers) {
        BufferState state = BufferState::Free;
        if (buffer.state.compare_exchange_strong(state, BufferState::Building, std::memory_order_acquire)) {
            destination = &buffer;
            break;
        }
    }
    // The four-slot invariant ensures a free slot even during a crossfade.
    if (!destination)
        return 0;
    try {
        destination->samples.assign(samples.begin(), samples.begin() + (samples.size() & ~size_t(1)));
    } catch (...) {
        destination->state.store(BufferState::Retired, std::memory_order_release);
        throw;
    }
    destination->frames = destination->samples.size() / 2;
    if (++d->nextRevision == 0)
        ++d->nextRevision;
    const uint64_t revision = destination->revision = d->nextRevision;
    const double tempo = std::isfinite(bpm) ? std::clamp(bpm, 20.0, 1000.0) : 200.0;
    destination->barFrames = SampleRate * 240.0 / tempo;
    destination->bpm = tempo;
    destination->state.store(BufferState::Owned, std::memory_order_release);
    Impl::retire(d->pending.exchange(destination, std::memory_order_acq_rel));
    return revision;
}

uint64_t AudioEngine::revision() const {
    return d->publishedRevision.load(std::memory_order_acquire);
}

void AudioEngine::setPlaying(bool playing) {
    uint64_t previous = d->requestedTransport.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = ((previous + 2) & ~uint64_t(1)) | uint64_t(playing);
    } while (!d->requestedTransport.compare_exchange_weak(previous, next, std::memory_order_release,
                                                         std::memory_order_relaxed));
}
bool AudioEngine::playing() const {
    return (d->requestedTransport.load(std::memory_order_relaxed) & 1) != 0;
}
void AudioEngine::setLooping(bool looping) {
    d->requestedLooping.store(looping, std::memory_order_relaxed);
}
bool AudioEngine::looping() const {
    return d->requestedLooping.load(std::memory_order_relaxed);
}
void AudioEngine::seek(uint64_t frame) {
    // NoSeek is a mailbox marker; an enormous frame still seeks past any
    // realizable buffer and is clamped to its end by the callback.
    d->seekRequest.store(std::min(frame, NoSeek - 1), std::memory_order_release);
}
void AudioEngine::rewind() {
    seek(0);
}
void AudioEngine::setGain(float gain) {
    d->requestedGain.store(std::isfinite(gain) ? std::clamp(gain, 0.0f, 2.0f) : 0.0f,
                           std::memory_order_relaxed);
}
uint64_t AudioEngine::position() const {
    return d->publishedPosition.load(std::memory_order_relaxed);
}
uint64_t AudioEngine::frames() const {
    return d->publishedFrames.load(std::memory_order_relaxed);
}
double AudioEngine::tempo() const {
    return d->publishedTempo.load(std::memory_order_relaxed);
}
float AudioEngine::peak() const {
    return d->publishedPeak.load(std::memory_order_relaxed);
}
unsigned long AudioEngine::xruns() const {
    return d->underflows.load(std::memory_order_relaxed);
}
double AudioEngine::cpuLoad() const {
    return d->stream ? Pa_GetStreamCpuLoad(d->stream) : 0.0;
}
QString AudioEngine::deviceName() const {
    return d->device;
}

QStringList AudioEngine::devices() {
    QStringList result;
    if (Pa_Initialize() != paNoError)
        return result;
    const int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info && info->maxOutputChannels >= 2) {
            const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
            result << QString("%1: %2 (%3)")
                          .arg(i)
                          .arg(QString::fromUtf8(info->name),
                               host ? QString::fromUtf8(host->name) : QString("audio"));
        }
    }
    Pa_Terminate();
    return result;
}

void AudioEngine::processForTest(float* output, unsigned long frames) {
    d->process(output, frames, 0);
}
} // namespace fc
