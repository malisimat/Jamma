#pragma once
#include <memory>
#include <vector>
#include "LoopTake.h"

namespace engine {

class LoopRemote : public LoopTake {
public:
    LoopRemote(LoopTakeParams params, audio::AudioMixerParams mixerParams);
    virtual ~LoopRemote();

    void IngestAudioBlock(const float* buffer, unsigned int numFrames);

private:
    unsigned int _currentMeasureFrames;
    unsigned int _measurePos;
    std::vector<float> _envelopeData;

    void _UpdateVisualMesh();
};


}
