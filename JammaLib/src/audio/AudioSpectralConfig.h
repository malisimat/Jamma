#pragma once

#include <cstdint>

namespace audio
{
	// V1 prototype tuning. The transform contract is explicit; sidecar versioning
	// must be frozen only after the deferred listening/performance validation.
	struct AudioSpectralConfig
	{
		static constexpr std::uint32_t TransformVersion = 1u;
		static constexpr unsigned int WindowSamps = 4096u;
		static constexpr unsigned int PreferredOverlap = 16u; // Tune window/overlap together.
		static constexpr unsigned int BaselineOverlap = 4u;
        static_assert(PreferredOverlap >= BaselineOverlap && (PreferredOverlap & (PreferredOverlap - 1u)) == 0u);
        static_assert(WindowSamps >= PreferredOverlap && (WindowSamps & (WindowSamps - 1u)) == 0u);
        static constexpr unsigned int BaselineHopSamps = WindowSamps / BaselineOverlap;
		static constexpr unsigned int HopSamps = WindowSamps / PreferredOverlap;
		static constexpr float MinimumGainDb = -60.0f;
		static constexpr float MaximumGainDb = 24.0f; // Broadband bar headroom.
        static constexpr float MinimumSpectralGainDb = -1200.0f;
        static constexpr float MaximumSpectralGainDb = 1200.0f;
        static constexpr float SilenceGainDb = -2048.0f; // Finite authored-zero tag.
        static constexpr double SpectralFloorDb = -90.0;
        static constexpr double SpectralCeilingDb = 0.0;
		static constexpr double BrushTimeRadiusSeconds = 0.05;
		static constexpr double BrushFrequencyRadiusOctaves = 0.25;
		static constexpr double BrushDbPerSecond = 60.0; // 2 dB at the nominal 30 Hz publication rate.
        static constexpr double MaximumBrushExposureSeconds = (MaximumSpectralGainDb - MinimumSpectralGainDb) / BrushDbPerSecond;
		static constexpr double StrokeUpdateSeconds = 1.0 / 30.0; // Audible queue cadence.
        static constexpr double VisualUpdateSeconds = 1.0 / 60.0;
        // Two active maps and at most one cancelled in-flight map; each has reserved dirty indices.
        static constexpr std::uint64_t InputMapBytesPerCoefficient = 3u * (sizeof(double) + sizeof(std::uint32_t));
		static constexpr std::uint64_t BandMapBytesPerSample = 3u * (2u * sizeof(double) + 2u * sizeof(std::uint32_t));
		static constexpr double BarTimeRadiusSeconds = 0.05;
		static constexpr double BarSilenceDragPixels = 160.0;
		static constexpr double BarBoostDragPixels = 160.0;
		static constexpr unsigned int MaxPlaybackTransitionSamps = 48000u;
		static constexpr double PlaybackTransitionSeconds = 0.0075;
		static constexpr double EnvelopeRampSeconds = 0.003;
		static constexpr double DisplayMinimumFrequencyHz = 20.0;
		static constexpr float DisplayFloorDb = -90.0f;
		static constexpr float DisplayCeilingDb = 24.0f;
		static constexpr float DisplayReferenceMagnitude = 1.0f;
		static constexpr unsigned int MaxTimeSegments = 4095u;
		static constexpr unsigned int MaxFrequencySegments = 256u;
		static constexpr unsigned long SourceCopySamplesPerBlock = 4096ul;
		static constexpr std::uint64_t MaxSourceBytes = 256ull * 1024ull * 1024ull;
		static constexpr std::uint64_t MaxWorkingBytes = 512ull * 1024ull * 1024ull;
		static constexpr std::uint64_t DisplayWorkingBytes = 64ull * 1024ull * 1024ull;
		static constexpr std::uint64_t MaxHistoryBytes = 64ull * 1024ull * 1024ull;
        // Display/history plus short-source transform, gain and input-map scratch.
        // Only higher-detail admission leaves this headroom; baseline may use the full cap.
        static constexpr std::uint64_t ShortSourceWorkingBytes = 32ull * 1024ull * 1024ull;
        static constexpr std::uint64_t PreferredWorkingBytes = MaxWorkingBytes
            - DisplayWorkingBytes - MaxHistoryBytes - ShortSourceWorkingBytes;
	};

	struct AudioSpectralTransformConfig
	{
		std::uint32_t Version = AudioSpectralConfig::TransformVersion;
		unsigned int WindowSamps = AudioSpectralConfig::WindowSamps;
		unsigned int HopSamps = AudioSpectralConfig::HopSamps;
		// Periodic Hann analysis/synthesis windows, real float FFT, and explicit
		// 1/N inverse FFT normalization followed by samplewise sum(window^2).
		bool IsValid() const noexcept
		{
			return Version == AudioSpectralConfig::TransformVersion
				&& WindowSamps >= 4u && WindowSamps <= 16384u
				&& (WindowSamps & (WindowSamps - 1u)) == 0u
				&& HopSamps > 0u && HopSamps <= WindowSamps / 2u;
		}
		bool operator==(const AudioSpectralTransformConfig&) const = default;
	};
}
