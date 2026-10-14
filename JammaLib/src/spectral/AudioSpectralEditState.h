#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "AudioSpectralDsp.h"
#include "AudioSpectralGainField.h"
#include "AudioSpectralEnvelope.h"

namespace spectral
{
	// Committed editing content, independent of the audible preview's progress.
	// Value replacement is off audio; source/gain/envelope handles are immutable.
	class AudioSpectralEditState
	{
	public:
		static std::shared_ptr<const AudioSpectralEditState> Create(
			std::shared_ptr<const AudioSpectralSource> source, AudioSpectralTransformConfig transform,
			std::shared_ptr<const AudioSpectralGainField> gains,
			std::shared_ptr<const AudioSpectralEnvelope> envelope,
			std::uint64_t revision, std::uint64_t spectralRevision,
			std::uint64_t envelopeRevision, std::string& reason);
		const std::shared_ptr<const AudioSpectralSource> Source;
		const AudioSpectralTransformConfig Transform;
		const std::shared_ptr<const AudioSpectralGainField> Gains;
		const std::shared_ptr<const AudioSpectralEnvelope> Envelope; // Null is unity.
		const std::uint64_t Revision;
		const std::uint64_t SpectralRevision;
		const std::uint64_t EnvelopeRevision;

	private:
		AudioSpectralEditState(std::shared_ptr<const AudioSpectralSource> source,
			AudioSpectralTransformConfig transform, std::shared_ptr<const AudioSpectralGainField> gains,
			std::shared_ptr<const AudioSpectralEnvelope> envelope, std::uint64_t revision,
			std::uint64_t spectralRevision, std::uint64_t envelopeRevision);
	};
}
