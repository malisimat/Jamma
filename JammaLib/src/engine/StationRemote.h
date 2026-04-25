#pragma once
#include "Station.h"

namespace engine {

class StationRemote : public Station {
public:
    StationRemote(StationParams params, audio::AudioMixerParams mixerParams);
    virtual ~StationRemote();

    void SetRemoteUserName(const std::string& name);
    std::string GetRemoteUserName() const;

    void IngestStereoBlock(const float* left, const float* right, unsigned int numFrames);

private:
    std::string _remoteUserName;
};

}
