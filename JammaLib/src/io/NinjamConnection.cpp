#include "NinjamConnection.h"
#include "njclient.h"
#include "PathUtils.h"
#include <iostream>
#include <windows.h>

namespace jamma {
namespace io {

NinjamConnection::NinjamConnection() : _client(nullptr), _scratchBuffers(nullptr), _maxChannels(0), _bufferFrames(0) {
    _client = new NJClient();
    char temp[MAX_PATH];
    GetTempPathA(MAX_PATH, temp);
    _workDir = std::string(temp) + "ninjam_jamma";
    CreateDirectoryA(_workDir.c_str(), NULL);
    _client->SetWorkDir(const_cast<char*>(_workDir.c_str()));
}

NinjamConnection::~NinjamConnection() {
    Disconnect();
    if (_client) {
        delete _client;
        _client = nullptr;
    }
    if (_scratchBuffers) {
        for (unsigned int i = 0; i < _maxChannels; ++i) {
            delete[] _scratchBuffers[i];
        }
        delete[] _scratchBuffers;
    }
}

bool NinjamConnection::Connect(const std::string& host, const std::string& user, const std::string& pass) {
    if (!_client) return false;
    _client->Connect(host.c_str(), user.c_str(), pass.c_str());
    return true;
}

void NinjamConnection::Disconnect() {
    if (_client) {
        _client->Disconnect();
    }
}

void NinjamConnection::Pump() {
    if (_client) {
        _client->Run();
        _UpdateSnapshot();
    }
}

void NinjamConnection::SetAudioFormat(unsigned int sampleRate, unsigned int numFrames) {
    // Basic setup if NJClient needed it, usually format is passed per-block
}

void NinjamConnection::ProcessAudioBlock(unsigned int numFrames, unsigned int sampleRate) {
    if (!_client) return;

    // Get number of users to determine required channels (2 per user for simplicity)
    int maxCh = _client->GetNumUsers() * 2;
    if (maxCh < 2) maxCh = 2; // Minimum stereo output
    
    _EnsureBuffers(numFrames, maxCh);

    // Clear output buffers
    for(unsigned int i=0; i < _maxChannels; ++i) {
        memset(_scratchBuffers[i], 0, sizeof(float) * numFrames);
    }

    _client->AudioProc(nullptr, 0, _scratchBuffers, _maxChannels, numFrames, sampleRate);
}

void NinjamConnection::_EnsureBuffers(unsigned int frames, unsigned int maxChannels) {
    if (frames > _bufferFrames || maxChannels > _maxChannels) {
        if (_scratchBuffers) {
            for (unsigned int i = 0; i < _maxChannels; ++i) {
                delete[] _scratchBuffers[i];
            }
            delete[] _scratchBuffers;
        }

        _maxChannels = maxChannels;
        _bufferFrames = frames;

        _scratchBuffers = new float*[_maxChannels];
        for (unsigned int i = 0; i < _maxChannels; ++i) {
            _scratchBuffers[i] = new float[_bufferFrames];
        }
    }
}

void NinjamConnection::_UpdateSnapshot() {
    std::lock_guard<std::mutex> lock(_snapshotMutex);
    _snapshot.clear();
    
    if (!_client) return;

    int numUsers = _client->GetNumUsers();
    int currentOutIndex = 0;

    for (int i = 0; i < numUsers; ++i) {
        const char* username = _client->GetUserState(i);
        if (username) {
            RemoteUserSnapshot snap;
            snap.name = username;
            
            int chCount = 0;
            while (_client->EnumUserChannels(i, chCount) >= 0) {
               chCount++;
            }
            snap.channels = chCount;
            snap.firstOutputIndex = currentOutIndex;

            // Route this user's channels to currentOutIndex
            for (int ch = 0; ch < snap.channels; ++ch) {
                 _client->SetUserChannelState(i, _client->EnumUserChannels(i, ch), false, false, false, 0.0f, false, 0.0f, false, false, false, false, true, currentOutIndex);
            }
            _snapshot.push_back(snap);
            currentOutIndex += 2; // Allocate stereo pair per user
        }
    }
}

std::vector<RemoteUserSnapshot> NinjamConnection::Snapshot() const {
    std::lock_guard<std::mutex> lock(_snapshotMutex);
    return _snapshot;
}

bool NinjamConnection::ConsumeRemoteMixForUser(const std::string& user, float*& left, float*& right, unsigned int& numFrames) {
    std::lock_guard<std::mutex> lock(_snapshotMutex);
    for (const auto& snap : _snapshot) {
        if (snap.name == user) {
            if (snap.firstOutputIndex + 1 < (int)_maxChannels) {
                left = _scratchBuffers[snap.firstOutputIndex];
                right = _scratchBuffers[snap.firstOutputIndex + 1];
                numFrames = _bufferFrames;
                return true;
            }
        }
    }
    return false;
}

}
}
