#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <stop_token>
#include <string>
#include "AudioSpectralDsp.h"
#include "AudioSpectralEnvelope.h"

namespace spectral
{
	// Complete immutable content revision. Envelope-only revisions share PCM.
	// Create and owning destruction are off audio; callbacks borrow SampleAt().
	class AudioSpectralPlaybackSnapshot
	{
	public:
		struct OverviewRange { float Minimum = 0.0f; float Maximum = 0.0f; };
		static constexpr unsigned int OverviewSegments = 2048u;
		const std::array<OverviewRange, OverviewSegments>& Overview() const noexcept { return _overview; }
		static std::shared_ptr<const AudioSpectralPlaybackSnapshot> Create(
			std::shared_ptr<const AudioSpectralPcm> pcm,
			std::shared_ptr<const AudioSpectralEnvelope> envelope,
			std::uint64_t revision, std::uint64_t spectralRevision,
			std::uint64_t envelopeRevision, std::string& reason, std::stop_token stop = {});
		const std::shared_ptr<const AudioSpectralPcm> Pcm;
		const std::shared_ptr<const AudioSpectralEnvelope> Envelope; // Null means unity.
		const std::uint64_t Revision;
		const std::uint64_t SpectralRevision;
		const std::uint64_t EnvelopeRevision;
		const AudioSpectralSource& Source() const noexcept { return *Pcm->Source; }
		bool UsesOriginal() const noexcept { return Pcm->UsesOriginal() && (!Envelope || Envelope->IsUnity()); }
		float SampleAt(unsigned long storageIndex) const noexcept;

	private:
		std::array<OverviewRange, OverviewSegments> _overview{};
		AudioSpectralPlaybackSnapshot(std::shared_ptr<const AudioSpectralPcm> pcm,
			std::shared_ptr<const AudioSpectralEnvelope> envelope,
			std::uint64_t revision, std::uint64_t spectralRevision, std::uint64_t envelopeRevision);
	};
}
