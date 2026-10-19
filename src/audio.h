#pragma once
#include <QString>
#include <QStringList>
#include <cstdint>
#include <memory>
#include <vector>

namespace fc {
class AudioEngine {
  public:
    AudioEngine();
    ~AudioEngine();
    bool open(QString& error, int device = -1);
    void close();
    // Control thread only. Retired immutable buffers are reclaimed off callback.
    // Returns a nonzero publication ID, or zero if no buffer slot is available.
    uint64_t submit(const std::vector<float>& samples, double bpm);
    // Active publication ID; zero before initial acquisition or after clear.
    uint64_t revision() const;
    void collect();
    // Invalidate the session at the next callback before any audio is emitted.
    void clear();
    void setPlaying(bool playing);
    bool playing() const;
    void setLooping(bool looping);
    bool looping() const;
    // Absolute song position; applied by the callback and clamped to song end.
    void seek(uint64_t frame);
    void rewind();
    void setGain(float gain);
    uint64_t position() const;
    uint64_t frames() const;
    double tempo() const;
    float peak() const;
    unsigned long xruns() const;
    double cpuLoad() const;
    QString deviceName() const;
    static QStringList devices();
    // Runs exactly the callback code without requiring audio hardware.
    void processForTest(float* output, unsigned long frames);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace fc
