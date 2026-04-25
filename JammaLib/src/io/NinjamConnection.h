#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>

class NJClient;

namespace jamma {
namespace io {

struct RemoteUserSnapshot {
    std::string name;
    int channels;
    int firstOutputIndex;
};

class NinjamConnection {
public:
    NinjamConnection();
    ~NinjamConnection();

    bool Connect(const std::string& host, const std::string& user, const std::string& pass);
    void Disconnect();
    void Pump();

    void SetAudioFormat(unsigned int sampleRate, unsigned int numFrames);
    void ProcessAudioBlock(unsigned int numFrames, unsigned int sampleRate);

    std::vector<RemoteUserSnapshot> Snapshot() const;
    bool ConsumeRemoteMixForUser(const std::string& user, float*& left, float*& right, unsigned int& numFrames);

private:
    NJClient* _client;
    std::string _workDir;
    std::vector<RemoteUserSnapshot> _snapshot;
    mutable std::mutex _snapshotMutex;

    float** _scratchBuffers;
    unsigned int _maxChannels;
    unsigned int _bufferFrames;

    void _UpdateSnapshot();
    void _EnsureBuffers(unsigned int frames, unsigned int maxChannels);
};

}
}
