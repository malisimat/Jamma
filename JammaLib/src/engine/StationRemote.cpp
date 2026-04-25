#include "StationRemote.h"
#include "LoopTake.h"

namespace engine {

StationRemote::StationRemote(StationParams params, audio::AudioMixerParams mixerParams)
    : Station(params, mixerParams) {
    // A remote station doesn't use standard looping takes
    // We could clear _loopTakes if needed, but Station starts with 0 takes anyway.
}

StationRemote::~StationRemote() {
}

void StationRemote::SetRemoteUserName(const std::string& name) {
    _remoteUserName = name;
    SetName(name);
}

std::string StationRemote::GetRemoteUserName() const {
    return _remoteUserName;
}

void StationRemote::IngestStereoBlock(const float* left, const float* right, unsigned int numFrames) {
    if (NumBusChannels() >= 2 && numFrames > 0) {
        base::AudioWriteRequest reqL;
        reqL.samples = left;
        reqL.numSamps = numFrames;
        reqL.source = base::Audible::AUDIOSOURCE_MIXER;

        base::AudioWriteRequest reqR;
        reqR.samples = right;
        reqR.numSamps = numFrames;
        reqR.source = base::Audible::AUDIOSOURCE_MIXER;

        OnBlockWriteChannel(0, reqL, 0);
        OnBlockWriteChannel(1, reqR, 0);

        EndMultiWrite(numFrames, true, base::Audible::AUDIOSOURCE_MIXER);
    }
}


}
