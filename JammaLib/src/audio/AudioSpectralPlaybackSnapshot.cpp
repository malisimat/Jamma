#include "AudioSpectralPlaybackSnapshot.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <limits>
#include <utility>

using namespace audio;

AudioSpectralPlaybackSnapshot::AudioSpectralPlaybackSnapshot(
	std::shared_ptr<const AudioSpectralPcm> pcm, std::shared_ptr<const AudioSpectralEnvelope> envelope,
	std::uint64_t revision, std::uint64_t spectralRevision, std::uint64_t envelopeRevision) :
	Pcm(std::move(pcm)), Envelope(std::move(envelope)), Revision(revision),
	SpectralRevision(spectralRevision), EnvelopeRevision(envelopeRevision)
{
}

std::shared_ptr<const AudioSpectralPlaybackSnapshot> AudioSpectralPlaybackSnapshot::Create(
	std::shared_ptr<const AudioSpectralPcm> pcm, std::shared_ptr<const AudioSpectralEnvelope> envelope,
	std::uint64_t revision, std::uint64_t spectralRevision, std::uint64_t envelopeRevision,
	std::string& reason, std::stop_token stop)
{
	reason.clear();
	if (!pcm || !pcm->Source || !pcm->Source->Identity || pcm->Source->Generation == 0u
		|| !pcm->Source->Layout.IsValid() || revision == 0u
		|| pcm->Source->Samples.size() != pcm->Source->Layout.StoredSampleCount()
		|| pcm->Samples().size() != pcm->Source->Layout.StoredSampleCount()
		|| (envelope && envelope->Length() != pcm->Source->Layout.LogicalLength)
        || (pcm->EnvelopeComposed && (envelope || pcm->ComposedSpectralRevision != spectralRevision
            || pcm->ComposedEnvelopeRevision != envelopeRevision)))
	{
		reason = "Edited playback content does not match its audio source";
		return nullptr;
	}
	try
	{
		auto snapshot = std::shared_ptr<AudioSpectralPlaybackSnapshot>(
			new AudioSpectralPlaybackSnapshot(std::move(pcm), std::move(envelope),
				revision, spectralRevision, envelopeRevision));
		// Off-worker validation also covers envelope multiplication overflow. No
		// limiter/renormalization is introduced into the callback read path.
		const auto length = snapshot->Source().Layout.StoredSampleCount();
		for (unsigned long first = 0ul; first < length; first += 4096ul)
		{
			if (stop.stop_requested()) { reason = "Edited playback preparation cancelled"; return nullptr; }
			const auto end = (std::min)(length, first + 4096ul);
			for (auto index = first; index < end; ++index)
				if (!std::isfinite(snapshot->SampleAt(index)))
				{
					reason = "Edited playback exceeded finite float range";
					return nullptr;
				}
		}
		// Prepared alongside finite validation on the worker. Ordinary overview
		// updates consume this bounded value and never reread writable recording banks.
		const auto& layout = snapshot->Source().Layout;
		for (unsigned int segment = 0u; segment < OverviewSegments; ++segment)
		{
			if (stop.stop_requested()) { reason = "Edited playback preparation cancelled"; return nullptr; }
			auto first = static_cast<unsigned long>(static_cast<std::uint64_t>(segment)
				* layout.LogicalLength / OverviewSegments);
			auto end = static_cast<unsigned long>(static_cast<std::uint64_t>(segment + 1u)
				* layout.LogicalLength / OverviewSegments);
			if (end <= first) { first = (std::min)(first, layout.LogicalLength - 1ul); end = first + 1ul; }
			auto minimum = std::numeric_limits<float>::max();
			auto maximum = std::numeric_limits<float>::lowest();
			for (auto index = first; index < end; ++index)
			{
				if ((index & 4095ul) == 0ul && stop.stop_requested())
				{ reason = "Edited playback preparation cancelled"; return nullptr; }
				const auto sample = snapshot->SampleAt(layout.BodyOffset + index);
				minimum = (std::min)(minimum, sample);
				maximum = (std::max)(maximum, sample);
			}
			snapshot->_overview[segment] = { minimum, maximum };
		}
		return snapshot;
	}
	catch (const std::bad_alloc&) { reason = "Not enough memory for edited playback"; }
	return nullptr;
}

float AudioSpectralPlaybackSnapshot::SampleAt(unsigned long storageIndex) const noexcept
{
	const auto samples = Pcm->Samples();
	if (storageIndex >= samples.size()) return 0.0f;
	const auto sample = samples[storageIndex];
	if (!Envelope || Envelope->IsUnity()) return sample;
	// In the existing seam read the prefix and ending indices differ by L,
	// hence both receive exactly the same wrapped envelope gain.
	return sample * Envelope->GainAt(Source().Layout.WrappedBodyIndex(storageIndex));
}
