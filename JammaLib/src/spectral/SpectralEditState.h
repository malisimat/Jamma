#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "SpectralDsp.h"
#include "SpectralGainField.h"
#include "SpectralEnvelope.h"

namespace spectral
{
	// Committed editing content, independent of the audible preview's progress.
	// Value replacement is off audio; source/gain/envelope handles are immutable.
	class SpectralEditState
	{
	public:
		static std::shared_ptr<const SpectralEditState> Create(
			std::shared_ptr<const SpectralSource> source, SpectralTransformConfig transform,
			std::shared_ptr<const SpectralGainField> gains,
			std::shared_ptr<const SpectralEnvelope> envelope,
			std::uint64_t revision, std::uint64_t spectralRevision,
			std::uint64_t envelopeRevision, std::string& reason);
		const std::shared_ptr<const SpectralSource> Source;
		const SpectralTransformConfig Transform;
		const std::shared_ptr<const SpectralGainField> Gains;
		const std::shared_ptr<const SpectralEnvelope> Envelope; // Null is unity.
		const std::uint64_t Revision;
		const std::uint64_t SpectralRevision;
		const std::uint64_t EnvelopeRevision;

	private:
		SpectralEditState(std::shared_ptr<const SpectralSource> source,
			SpectralTransformConfig transform, std::shared_ptr<const SpectralGainField> gains,
			std::shared_ptr<const SpectralEnvelope> envelope, std::uint64_t revision,
			std::uint64_t spectralRevision, std::uint64_t envelopeRevision);
	};
}
