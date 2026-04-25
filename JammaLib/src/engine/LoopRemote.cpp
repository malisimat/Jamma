#include "LoopRemote.h"
#include <algorithm>
#include <cmath>

namespace engine {

LoopRemote::LoopRemote(LoopTakeParams params, audio::AudioMixerParams mixerParams)
    : LoopTake(params, mixerParams), _currentMeasureFrames(44100 * 4), _measurePos(0) {
    _envelopeData.resize(128, 0.0f);
}

LoopRemote::~LoopRemote() {
}

void LoopRemote::IngestAudioBlock(const float* buffer, unsigned int numFrames) {
    if (numFrames == 0) return;

    // Calculate RMS or peak for the block
    float maxVal = 0.0f;
    for (unsigned int i = 0; i < numFrames; ++i) {
        float val = std::abs(buffer[i]);
        if (val > maxVal) maxVal = val;
    }

    // Map to envelope
    unsigned int envIndex = static_cast<unsigned int>((static_cast<unsigned long long>(_measurePos) * _envelopeData.size()) / _currentMeasureFrames);
    if (envIndex < _envelopeData.size()) {
        _envelopeData[envIndex] = std::max(_envelopeData[envIndex], maxVal);
    }

    _measurePos = (_measurePos + numFrames) % _currentMeasureFrames;

    // Slowly decay envelope or clear on wrap
    if (_measurePos < numFrames) {
        std::fill(_envelopeData.begin(), _envelopeData.end(), 0.0f);
    }

    _UpdateVisualMesh();
}

void LoopRemote::_UpdateVisualMesh() {
    // Generate a ring mesh using _envelopeData
    // ...
}


}
