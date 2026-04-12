///////////////////////////////////////////////////////////
//
// LatencyMeasure — ASIO loopback latency utility
//
// Emits a 10-bit MLS (primitive polynomial x^10+x^7+1,
// period 1023 samples) on output channel 0, simultaneously
// records input channel 0, then cross-correlates to find
// the roundtrip hardware latency.
//
// Usage: LatencyMeasure.exe [bufferSize]
//   bufferSize  ASIO buffer size in samples (default 512)
//
// Setup: patch output channel 0 to input channel 0 on the
//        interface with a physical cable before running.
//
///////////////////////////////////////////////////////////

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <limits>
#include <cassert>
#include <cstdint>
#include <thread>
#include <chrono>

#include "rtaudio/RtAudio.h"

// ============================================================
// MLS configuration
//
// Degree 10, primitive polynomial x^10 + x^7 + 1.
// Taps at 1-indexed positions {10, 7}, 0-indexed {9, 6}.
// Period: 2^10 - 1 = 1023 samples (~23 ms at 44100 Hz).
// ============================================================
static constexpr int   MlsDegree = 10;
static constexpr int   MlsLength = (1 << MlsDegree) - 1;  // 1023
static constexpr int   MlsMask   = (1 << MlsDegree) - 1;  // 0x3FF
static constexpr float MlsAmp    = 0.5f;  // leave 6 dB headroom

// Search up to this many samples for the correlation peak (~185 ms at 44100)
static constexpr int MaxLag = 8192;

// ============================================================
// Generate one period of the MLS
// ============================================================
static std::vector<float> GenerateMls()
{
    std::vector<float> seq(MlsLength);
    uint32_t reg = MlsMask;  // non-zero seed

    for (int i = 0; i < MlsLength; ++i)
    {
        seq[i] = (reg & 1u) ? MlsAmp : -MlsAmp;
        // Fibonacci LFSR: feedback = tap9 XOR tap6
        uint32_t fb = ((reg >> 9) ^ (reg >> 6)) & 1u;
        reg = ((reg >> 1) | (fb << 9)) & MlsMask;
    }

    return seq;
}

// ============================================================
// Measurement state — pre-allocated before stream start;
// emitPos/captureCount are callback-thread-private,
// phase is the only field shared with the main thread.
// ============================================================
enum class Phase { Warmup, Record, Done };

struct MeasureState
{
    const float*       reference    = nullptr;
    int                referenceLen = 0;
    float*             captured     = nullptr;
    int                captureTarget = 0;   // = referenceLen + MaxLag

    int                emitPos      = 0;    // callback-thread only
    int                captureCount = 0;    // callback-thread only

    std::atomic<Phase> phase{ Phase::Warmup };
    int                warmupLeft   = 20;   // callbacks to ignore at startup

    int                numInChannels  = 1;
    int                numOutChannels = 1;
};

// ============================================================
// RtAudio callback
//
// WARMUP: silence output, wait for stream to stabilise.
// RECORD: emit reference on output ch0; simultaneously
//         record input ch0 into captured[]; stop when
//         captureTarget samples accumulated.
// DONE:   silence output; return 1 to stop the stream.
// ============================================================
static int AudioCallback(
    void*               outBuf,
    void*               inBuf,
    unsigned int        nFrames,
    double              /*streamTime*/,
    RtAudioStreamStatus /*status*/,
    void*               userData)
{
    auto* s   = static_cast<MeasureState*>(userData);
    auto* out = static_cast<float*>(outBuf);
    auto* in  = static_cast<float*>(inBuf);

    Phase current = s->phase.load(std::memory_order_relaxed);

    // Zero the output unconditionally; fill selectively below.
    for (unsigned int i = 0; i < nFrames * (unsigned int)s->numOutChannels; ++i)
        out[i] = 0.0f;

    switch (current)
    {
    case Phase::Warmup:
        if (--s->warmupLeft <= 0)
            s->phase.store(Phase::Record, std::memory_order_release);
        break;

    case Phase::Record:
    {
        // Write reference to output ch0 (interleaved; other channels stay 0)
        for (unsigned int i = 0; i < nFrames; ++i)
        {
            if (s->emitPos < s->referenceLen)
                out[i * (unsigned int)s->numOutChannels] = s->reference[s->emitPos++];
        }

        // Capture input ch0 into the pre-allocated buffer
        if (in)
        {
            for (unsigned int i = 0; i < nFrames && s->captureCount < s->captureTarget; ++i)
                s->captured[s->captureCount++] = in[i * (unsigned int)s->numInChannels];
        }

        if (s->captureCount >= s->captureTarget)
            s->phase.store(Phase::Done, std::memory_order_release);

        break;
    }

    case Phase::Done:
        return 1;  // signal RtAudio to stop the stream
    }

    return 0;
}

// ============================================================
// Cross-correlation
//
// corr[lag] = sum_{i=0}^{N-1} ref[i] * captured[i + lag]
//
// The peak lag equals the roundtrip hardware latency because
// captured[t] = ref[t - L] where L is the latency:
//   corr[L] = sum ref[i] * ref[i+L-L] = sum ref[i]^2  (max)
// ============================================================
struct CorrResult
{
    int   lagSamples;
    float peakNormalized;  // 1.0 = perfect correlation
};

static CorrResult CrossCorrelate(
    const std::vector<float>& ref,
    const std::vector<float>& captured,
    int maxLag)
{
    float refEnergy = 0.0f;
    for (float v : ref)
        refEnergy += v * v;

    if (refEnergy == 0.0f)
        return { -1, 0.0f };

    float bestCorr = -std::numeric_limits<float>::max();
    int   bestLag  = 0;
    int   refLen   = (int)ref.size();
    int   capLen   = (int)captured.size();

    for (int lag = 0; lag < maxLag; ++lag)
    {
        float corr = 0.0f;
        int   count = std::min(refLen, capLen - lag);
        if (count <= 0)
            break;
        for (int i = 0; i < count; ++i)
            corr += ref[i] * captured[i + lag];
        if (corr > bestCorr)
        {
            bestCorr = corr;
            bestLag  = lag;
        }
    }

    return { bestLag, bestCorr / refEnergy };
}

// ============================================================
// main
// ============================================================
int main(int argc, char* argv[])
{
    unsigned int bufSize = 512;
    if (argc >= 2)
    {
        try { bufSize = (unsigned int)std::stoul(argv[1]); }
        catch (...) { std::cerr << "Invalid buffer size argument.\n"; return 1; }
    }

    constexpr unsigned int SampleRate = 44100;

    std::cout << "=== Jamma Loopback Latency Measurement ===\n";
    std::cout << "Requested buffer size : " << bufSize << " samples\n";
    std::cout << "MLS length            : " << MlsLength << " samples ("
              << std::fixed << std::setprecision(1)
              << MlsLength * 1000.0 / SampleRate << " ms)\n";
    std::cout << "Max lag searched      : " << MaxLag << " samples ("
              << std::fixed << std::setprecision(1)
              << MaxLag * 1000.0 / SampleRate << " ms)\n\n";
    std::cout << "SETUP: patch output ch0 to input ch0 before running.\n\n";

    // Generate reference and pre-allocate capture buffer
    auto reference = GenerateMls();
    std::vector<float> captured(MlsLength + MaxLag, 0.0f);

    // Initialise state
    MeasureState state;
    state.reference     = reference.data();
    state.referenceLen  = (int)reference.size();
    state.captured      = captured.data();
    state.captureTarget = MlsLength + MaxLag;

    // Open ASIO stream
    RtAudio rtAudio(RtAudio::WINDOWS_ASIO);

    if (rtAudio.getDeviceCount() == 0)
    {
        std::cerr << "No ASIO devices found.\n";
        return 1;
    }

    unsigned int inDevId  = rtAudio.getDefaultInputDevice();
    unsigned int outDevId = rtAudio.getDefaultOutputDevice();
    auto         inInfo   = rtAudio.getDeviceInfo(inDevId);
    auto         outInfo  = rtAudio.getDeviceInfo(outDevId);

    std::cout << "Input device  : " << inInfo.name
              << " (" << inInfo.inputChannels << " ch)\n";
    std::cout << "Output device : " << outInfo.name
              << " (" << outInfo.outputChannels << " ch)\n\n";

    if (inInfo.inputChannels == 0 || outInfo.outputChannels == 0)
    {
        std::cerr << "Device has no input or output channels.\n";
        return 1;
    }

    state.numInChannels  = 1;
    state.numOutChannels = 1;

    RtAudio::StreamParameters inParams;
    inParams.deviceId     = inDevId;
    inParams.nChannels    = 1;
    inParams.firstChannel = 0;

    RtAudio::StreamParameters outParams;
    outParams.deviceId     = outDevId;
    outParams.nChannels    = 1;
    outParams.firstChannel = 0;

    RtAudio::StreamOptions opts;
    opts.numberOfBuffers = 2;

    try
    {
        rtAudio.openStream(
            &outParams, &inParams,
            RTAUDIO_FLOAT32,
            SampleRate,
            &bufSize,
            AudioCallback,
            &state,
            &opts,
            nullptr);
    }
    catch (RtAudioError& err)
    {
        std::cerr << "Failed to open stream: " << err.getMessage() << "\n";
        return 1;
    }

    long long reportedIn  = rtAudio.getInputStreamLatency();
    long long reportedOut = rtAudio.getOutputStreamLatency();

    std::cout << "Actual buffer size       : " << bufSize << " samples\n";
    std::cout << "Reported input latency   : " << std::setw(5) << reportedIn
              << " samples  (" << std::fixed << std::setprecision(2)
              << reportedIn * 1000.0 / SampleRate << " ms)\n";
    std::cout << "Reported output latency  : " << std::setw(5) << reportedOut
              << " samples  (" << std::fixed << std::setprecision(2)
              << reportedOut * 1000.0 / SampleRate << " ms)\n";
    std::cout << "Reported roundtrip (sum) : " << std::setw(5) << (reportedIn + reportedOut)
              << " samples  (" << std::fixed << std::setprecision(2)
              << (reportedIn + reportedOut) * 1000.0 / SampleRate << " ms)\n\n";

    std::cout << "Running measurement...\n";

    rtAudio.startStream();

    // Wait for the callback to finish — poll, yielding the CPU between checks.
    // This is a one-shot utility; a spin-sleep is appropriate here.
    while (state.phase.load(std::memory_order_acquire) != Phase::Done)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (rtAudio.isStreamRunning())
        rtAudio.stopStream();
    if (rtAudio.isStreamOpen())
        rtAudio.closeStream();

    std::cout << "Capture complete. Computing cross-correlation...\n\n";

    auto result = CrossCorrelate(reference, captured, MaxLag);

    if (result.lagSamples < 0 || result.peakNormalized < 0.05f)
    {
        std::cerr << "ERROR: No signal detected in capture buffer.\n"
                  << "Check cable connection, gain levels, and that the\n"
                  << "input channel is not muted or locked by the ASIO driver.\n";
        return 1;
    }

    double measuredMs = result.lagSamples * 1000.0 / SampleRate;

    std::cout << "=== Results ===\n";
    std::cout << "Measured roundtrip       : " << std::setw(5) << result.lagSamples
              << " samples  (" << std::fixed << std::setprecision(2)
              << measuredMs << " ms)\n";
    std::cout << "Correlation peak         : " << std::fixed << std::setprecision(4)
              << result.peakNormalized
              << "  (1.0 = perfect; >0.5 is good)\n";

    long long delta = result.lagSamples - (reportedIn + reportedOut);
    std::cout << "Measured - reported      : " << std::showpos << delta
              << std::noshowpos << " samples  ("
              << std::fixed << std::setprecision(2)
              << delta * 1000.0 / SampleRate << " ms)"
              << "  [converter pipeline not reported by ASIO]\n";

    if (result.peakNormalized < 0.3f)
    {
        std::cout << "\nWARNING: Low correlation peak. Results may be unreliable.\n"
                  << "Check cable, interface gain, and ASIO panel settings.\n";
    }

    return 0;
}
