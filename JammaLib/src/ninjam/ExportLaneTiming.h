///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2026 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

namespace ninjam
{
	// Inputs to one block's export-lane timing computation.
	// n/pos/length are all in samples, same convention as NJClient::GetPosition.
	struct ExportLaneTimingInput
	{
		// Running write-cursor tick shared by both delay lines, sampled
		// before this block's content was written (i.e. the delay lines'
		// write index prior to this call's EndWrite).
		unsigned int n = 0u;
		unsigned int numFrames = 0u;
		unsigned int pos = 0u;
		unsigned int length = 0u;
		unsigned int inLatencySamps = 0u;
		unsigned int outLatencySamps = 0u;
	};

	// Carries the previous block's observation forward so each call can
	// detect generation changes (tempo/BPI vote, reconnect) and anomalous
	// position jumps without any audio-thread or NJClient state.
	struct ExportLaneTimingState
	{
		bool primed = false;
		unsigned int lastLength = 0u;
		unsigned int predictedPos = 0u;
	};

	struct ExportLaneTimingResult
	{
		// K_dac/K_adc plus numFrames, ready to pass directly to
		// AudioBuffer::Delay(...) (accounts for AudioBuffer's post-write
		// cursor convention -- see AudioBuffer::EndWrite).
		unsigned int dacDelaySamps = 0u;
		unsigned int adcDelaySamps = 0u;

		// True when length was 0 (no NJClient timing yet); dacDelaySamps/
		// adcDelaySamps are meaningless in that case and the caller should
		// mute rather than read the delay lines.
		bool valid = false;

		// True when the caller should Reset() both delay lines and mute
		// until they re-prime (see doc/ninjam-live-loop-latency-sync-planC.md
		// §3.1): first block, an interval-length change (tempo/BPI vote), or
		// an anomalous position jump (§3.2).
		bool generationReset = false;

		// True when generationReset fired because of an anomalous position
		// jump (§3.2 case 3) rather than an ordinary length change or the
		// very first block -- a rate-limited, non-realtime diagnostic signal
		// that GetPosition/AudioProc behaved unexpectedly.
		bool anomalyDetected = false;
	};

	// Pure helper computing the DAC/ADC export delay-line offsets described in
	// doc/ninjam-live-loop-latency-sync-planC.md §1-3. Deliberately has no
	// dependency on NJClient, audio buffers, or thread state so it can be unit
	// tested directly.
	class ExportLaneTiming
	{
	public:
		static ExportLaneTimingResult Compute(const ExportLaneTimingInput& input,
			ExportLaneTimingState& state);
	};
}
