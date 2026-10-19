#pragma once
#include "project.h"
#include <vector>

namespace fc {
// Interleaved stereo, one complete arrangement, 48 kHz float32.
std::vector<float> renderTrack(const Project& project, int trackIndex);
std::vector<float> mixTracks(const Project& project, const std::array<std::vector<float>, TrackCount>& stems);
std::vector<float> renderProject(const Project& project);
bool writeWave(const QString& path, const std::vector<float>& stereo, QString& error, int repeats = 1);
// Persistent newline-delimited JSON worker. Input: {id,project,track,path};
// output: {id,ok,path,frames,error}. Render stereo f32 LE to path atomically.
int workerMain();
} // namespace fc
