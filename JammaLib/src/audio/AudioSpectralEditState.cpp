#include "AudioSpectralEditState.h"

#include <new>
#include <utility>

using namespace audio;

AudioSpectralEditState::AudioSpectralEditState(
	std::shared_ptr<const AudioSpectralSource> source, AudioSpectralTransformConfig transform,
	std::shared_ptr<const AudioSpectralGainField> gains, std::shared_ptr<const AudioSpectralEnvelope> envelope,
	std::uint64_t revision, std::uint64_t spectralRevision, std::uint64_t envelopeRevision) :
	Source(std::move(source)), Transform(transform), Gains(std::move(gains)), Envelope(std::move(envelope)),
	Revision(revision), SpectralRevision(spectralRevision), EnvelopeRevision(envelopeRevision)
{
}

std::shared_ptr<const AudioSpectralEditState> AudioSpectralEditState::Create(
	std::shared_ptr<const AudioSpectralSource> source, AudioSpectralTransformConfig transform,
	std::shared_ptr<const AudioSpectralGainField> gains, std::shared_ptr<const AudioSpectralEnvelope> envelope,
	std::uint64_t revision, std::uint64_t spectralRevision, std::uint64_t envelopeRevision, std::string& reason)
{
	reason.clear();
	if (!source || !source->Identity || source->Generation == 0u || revision == 0u || !gains
		|| source->Samples.size() != source->Layout.StoredSampleCount())
	{
		reason = "Audio edit state has no complete source or gain field";
		return nullptr;
	}
	const auto estimate = AudioSpectralDsp::EstimateWorkingMemory(source->Layout, transform, reason);
	if (!estimate) return nullptr;
	if (gains->FrameCount() != estimate->Geometry.FrameCount || gains->BinCount() != estimate->Geometry.BinCount
		|| (envelope && envelope->Length() != source->Layout.LogicalLength)
		|| (spectralRevision == 0u && !gains->IsUnity())
		|| (envelopeRevision == 0u && envelope && !envelope->IsUnity()))
	{
		reason = "Audio edit layers do not match their source coordinates";
		return nullptr;
	}
	try
	{
		return std::shared_ptr<const AudioSpectralEditState>(new AudioSpectralEditState(
			std::move(source), transform, std::move(gains), std::move(envelope),
			revision, spectralRevision, envelopeRevision));
	}
	catch (const std::bad_alloc&) { reason = "Not enough memory for audio edit state"; }
	return nullptr;
}
