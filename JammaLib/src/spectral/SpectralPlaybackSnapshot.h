#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <stop_token>
#include <string>
#include "SpectralDsp.h"
#include "SpectralEnvelope.h"

namespace spectral
{
	// Complete immutable content revision. Envelope-only revisions share PCM.
	// Create and owning destruction are off audio; callbacks borrow SampleAt().
	class SpectralPlaybackSnapshot
	{
	public:
		struct OverviewRange { float Minimum = 0.0f; float Maximum = 0.0f; };
		static constexpr unsigned int OverviewSegments = 2048u;
		const std::array<OverviewRange, OverviewSegments>& Overview() const noexcept { return _overview; }
		static std::shared_ptr<const SpectralPlaybackSnapshot> Create(
			std::shared_ptr<const SpectralPcm> pcm,
			std::shared_ptr<const SpectralEnvelope> envelope,
			std::uint64_t revision, std::uint64_t spectralRevision,
			std::uint64_t envelopeRevision, std::string& reason, std::stop_token stop = {});
		const std::shared_ptr<const SpectralPcm> Pcm;
		const std::shared_ptr<const SpectralEnvelope> Envelope; // Null means unity.
		const std::uint64_t Revision;
		const std::uint64_t SpectralRevision;
		const std::uint64_t EnvelopeRevision;
		const SpectralSource& Source() const noexcept { return *Pcm->Source; }
		bool UsesOriginal() const noexcept { return Pcm->UsesOriginal() && (!Envelope || Envelope->IsUnity()); }
		float SampleAt(unsigned long storageIndex) const noexcept;

	private:
		std::array<OverviewRange, OverviewSegments> _overview{};
		SpectralPlaybackSnapshot(std::shared_ptr<const SpectralPcm> pcm,
			std::shared_ptr<const SpectralEnvelope> envelope,
			std::uint64_t revision, std::uint64_t spectralRevision, std::uint64_t envelopeRevision);
	};
}
