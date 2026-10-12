#pragma once

#include <cstdint>

namespace ninjam
{
	struct NinjamTempoChange
	{
		unsigned int RemoteMasterIntervalLengthSamps = 0u;
		unsigned int SourceSampleRate = 0u;
		unsigned int RemoteGridStepSamps = 0u;
		float Bpm = 0.0f;
		unsigned int Bpi = 0u;
		unsigned int IntervalPositionSamps = 0u;
		std::uint64_t RemotePhaseDeviceSample = 0u;

		bool HasSameProposalIdentity(const NinjamTempoChange& other) const noexcept;
	};

	struct NinjamTempoProposalId
	{
		std::uint64_t SessionEpoch = 0u;
		std::uint64_t Revision = 0u;
		bool operator==(const NinjamTempoProposalId&) const = default;
	};

	struct NinjamTempoProposal
	{
		NinjamTempoProposalId Id;
		NinjamTempoChange Change;
	};

	enum class NinjamTempoChoice { FollowServer, StayLocal };

	struct NinjamTempoDecision
	{
		NinjamTempoProposalId Id;
		NinjamTempoChoice Choice;
	};
}
