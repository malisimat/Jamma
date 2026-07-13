///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#define MAJOR_VERSION       5
#define MINOR_VERSION       0
#define PATCH_VERSION       2

#define QUOTE_(x) #x
#define QUOTE(x) QUOTE_(x)
#define LIB_VERSION			QUOTE(MAJOR_VERSION.MINOR_VERSION.PATCH_VERSION)

namespace constants
{
	constexpr auto TWOPI = 6.283185307179586476925286766559;
	const unsigned int MaxBlockSize = 4096u;
	const unsigned int MaxLoopFadeSamps = 70000u;
	const unsigned long MaxLoopBufferSize = 40000000ul;
	const unsigned int GrainSamps = 300u;
	const unsigned int DefaultFadeSamps = 20u;
	const unsigned int DefaultPreDelaySamps = 0u;
	const unsigned int DefaultDebounceSamps = 280u;
	const unsigned int DefaultSeedGrainMinMs = 300u;
	const unsigned int DefaultSeedGrainTargetMaxMs = 3000u;
	const unsigned int DefaultSeedBpmMin = 80u;
	const unsigned int DefaultSampleRate = 44100u;
	const unsigned int DefaultBufferSizeSamps = 512u;

	// Upper bound on a NINJAM interval length, in samples, used to size the
	// fixed-capacity export delay lines in NinjamConnection (see
	// doc/ninjam-live-loop-latency-sync-planC.md). Comfortably covers any
	// realistic BPM/BPI/sample-rate combination: BPM as low as 20, BPI as
	// high as 32, sample rate as high as 96kHz => (60/20)*32*96000 = 5,760,000
	// samples. Rounded up with margin.
	const unsigned int MaxNinjamIntervalSamps = 6000000u;

	// Plausibility bounds for a NINJAM remote tempo reading, matching the
	// realistic BPM/BPI assumption above. Used to reject njclient's transient
	// pre-handshake GetActualBPM()/GetBPI() readings (observed to report
	// nonsensical values such as bpm=2646, bpi=1 for the brief window before
	// the server's real CONFIG_CHANGE_NOTIFY message has been parsed) so they
	// are never mistaken for an authoritative tempo change.
	const float MinPlausibleNinjamBpm = 20.0f;
	const float MaxPlausibleNinjamBpm = 400.0f;
	const int MinPlausibleNinjamBpi = 1;
	const int MaxPlausibleNinjamBpi = 32;
}
