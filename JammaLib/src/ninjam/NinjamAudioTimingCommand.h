#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "../utils/Timer.h"

namespace ninjam
{
	// Command semantics for the single audio-boundary transport fan-out.
	//   ReplaceTiming   - coherent absolute seed length / grain / quantisation / phase replacement.
	//   JoinAlignment   - one deliberate signed circular delta (may be up to half an interval).
	//   PhaseDiscipline - small bounded signed delta under the steady-state safety policy.
	//   Invalidate      - cancel prior generations without moving phase.
	enum class NinjamTimingCommandType : std::uint8_t
	{
		ReplaceTiming,
		JoinAlignment,
		PhaseDiscipline,
		Invalidate
	};

	// One immutable transport command published by the job thread and consumed
	// exactly once at the top of the audio callback, before any station playback
	// advancement. The Timer and every active local take apply the same local
	// copy in the same block so no job-thread publication can split consumers.
	struct NinjamAudioTimingCommand
	{
		std::uint64_t Sequence = 0u;
		std::uint64_t Generation = 0u;
		NinjamTimingCommandType Type = NinjamTimingCommandType::Invalidate;
		unsigned long SeedLengthSamps = 0ul;
		unsigned int QuantiseSamps = 0u;
		utils::Timer::QuantisationType Quantisation = utils::Timer::QUANTISE_OFF;
		unsigned int AbsolutePhaseSamps = 0u;
		long long PhaseDeltaSamps = 0;
	};

	// Single-writer (job thread) / single-reader (audio thread) latest-command
	// mailbox. A monotonically increasing even sequence brackets one coherent
	// command; an odd sequence means the writer is mid-publication. The reader
	// tracks the last sequence it applied, so each publication is consumed at
	// most once with bounded, allocation-free work.
	class NinjamAudioTimingCommandMailbox
	{
	public:
		void Publish(const NinjamAudioTimingCommand& command) noexcept
		{
			const auto writingSequence = _sequence.fetch_add(1u, std::memory_order_acq_rel) + 1u;
			_generation.store(command.Generation, std::memory_order_relaxed);
			_type.store(command.Type, std::memory_order_relaxed);
			_seedLengthSamps.store(command.SeedLengthSamps, std::memory_order_relaxed);
			_quantiseSamps.store(command.QuantiseSamps, std::memory_order_relaxed);
			_quantisation.store(command.Quantisation, std::memory_order_relaxed);
			_absolutePhaseSamps.store(command.AbsolutePhaseSamps, std::memory_order_relaxed);
			_phaseDeltaSamps.store(command.PhaseDeltaSamps, std::memory_order_relaxed);
			_sequence.store(writingSequence + 1u, std::memory_order_release);
			_hasPublication.store(true, std::memory_order_release);
		}

		// Returns the latest command if a new one has been published since the
		// previous Consume, otherwise std::nullopt. Audio-thread only.
		std::optional<NinjamAudioTimingCommand> Consume() noexcept
		{
			if (!_hasPublication.load(std::memory_order_acquire))
				return std::nullopt;

			for (unsigned int attempt = 0u; attempt < _MaxReadAttempts; ++attempt)
			{
				const auto before = _sequence.load(std::memory_order_acquire);
				if ((before & 1u) != 0u)
					continue;
				if (before == _consumedSequence)
					return std::nullopt;

				NinjamAudioTimingCommand command;
				command.Sequence = before;
				command.Generation = _generation.load(std::memory_order_relaxed);
				command.Type = _type.load(std::memory_order_relaxed);
				command.SeedLengthSamps = _seedLengthSamps.load(std::memory_order_relaxed);
				command.QuantiseSamps = _quantiseSamps.load(std::memory_order_relaxed);
				command.Quantisation = _quantisation.load(std::memory_order_relaxed);
				command.AbsolutePhaseSamps = _absolutePhaseSamps.load(std::memory_order_relaxed);
				command.PhaseDeltaSamps = _phaseDeltaSamps.load(std::memory_order_relaxed);

				const auto after = _sequence.load(std::memory_order_acquire);
				if (before == after)
				{
					_consumedSequence = before;
					return command;
				}
			}

			return std::nullopt;
		}

		std::uint64_t PublishedSequence() const noexcept
		{
			return _sequence.load(std::memory_order_acquire);
		}

	private:
		static constexpr unsigned int _MaxReadAttempts = 4u;
		std::atomic<std::uint64_t> _sequence{ 0u };
		std::atomic_bool _hasPublication{ false };
		std::atomic<std::uint64_t> _generation{ 0u };
		std::atomic<NinjamTimingCommandType> _type{ NinjamTimingCommandType::Invalidate };
		std::atomic<unsigned long> _seedLengthSamps{ 0ul };
		std::atomic<unsigned int> _quantiseSamps{ 0u };
		std::atomic<utils::Timer::QuantisationType> _quantisation{ utils::Timer::QUANTISE_OFF };
		std::atomic<unsigned int> _absolutePhaseSamps{ 0u };
		std::atomic<long long> _phaseDeltaSamps{ 0 };
		std::uint64_t _consumedSequence = 0u;
	};
}
