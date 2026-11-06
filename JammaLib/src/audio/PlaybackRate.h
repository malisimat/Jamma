#pragma once

#include "InterpolatedValue.h"
#include <cstdint>
#include <atomic>
#include <optional>
#include <vector>

namespace audio
{
	class SourcePhaseCorrection
	{
	public:
		static constexpr double PositionTolerance = 1e-6;
		static constexpr double MaximumFraction = 0.25;
		void SetError(double sourceSamples) noexcept;
		double Apply(double baseIncrement, double damping) noexcept;
		double Error() const noexcept { return _error; }
		bool IsSettled() const noexcept { return _error == 0.0; }
	private:
		double _error = 0.0;
	};

	// Ideal progress stays independent of audible progress. Velocity smoothing
	// contributes phase debt, which the same positive-motion correction closes.
	class SourceVelocityTransition
	{
	public:
		void Initialise(double sourceIncrement, double idealIncrement = 0.0) noexcept;
		void SetPhaseError(double sourceSamples) noexcept { _phase.SetError(sourceSamples); }
		double Next(double idealIncrement, double damping) noexcept;
		double Velocity() const noexcept { return _velocity; }
		double PhaseError() const noexcept { return _phase.Error(); }
		bool IsSettled(double idealIncrement) const noexcept;
	private:
		double _velocity = 1.0;
		double _previousIdeal = 0.0;
		SourcePhaseCorrection _phase;
	};
	// Frame d is read at U(d); its increment is the newly smoothed G(d).
	// Therefore N calls to Next advance from the block's first edge to edge N.
	class PlaybackRateCursor
	{
	public:
		static constexpr double SettleTolerance = 1e-12;
		PlaybackRateCursor(double initial, double target, double damping, double sourceScale = 1.0,
			bool hasSourceTransition = false, double initialSourceVelocity = 1.0,
			double initialSourcePhaseError = 0.0) noexcept;
		double Next() noexcept;
		double Current() const noexcept { return _value.Current(); }
		double SourceVelocity() const noexcept;
		double SourcePhaseError() const noexcept { return _transition.PhaseError(); }
		double BaseIncrement() const noexcept { return _baseIncrement; }
		double Correction() const noexcept { return _correction; }
	private:
		static InterpolatedValueExp::ExponentialParams Parameters(double damping) noexcept;
		InterpolatedValueExp _value;
		double _target;
		double _damping;
		double _sourceScale;
		bool _hasSourceTransition;
		SourceVelocityTransition _transition;
		double _baseIncrement = 1.0;
		double _correction = 0.0;
	};

	struct PlaybackRateBlock
	{
		double InitialRate = 1.0;
		double TargetRate = 1.0;
		double Damping = 1.0;
		unsigned int Frames = 0u;
		std::uint64_t Revision = 0u;
		double SourceScale = 1.0;
		bool HasSourceTransition = false;
		double InitialSourceVelocity = 1.0;
		double InitialSourcePhaseError = 0.0;
		std::uint64_t RemoteIntervalLengthSamps = 0u;
		unsigned int RemoteBpi = 0u;
		std::uint64_t MasterPositionWhole = 0u;
		double MasterPositionFraction = 0.0;
		double MasterRecordingRate = 1.0;
		std::uint32_t MasterLength = 0u;
		PlaybackRateCursor Cursor() const noexcept;
		double Advance(unsigned int frames, double recordingRate = 1.0) const noexcept;
		double RateAt(unsigned int frame) const noexcept;
		bool IsConstant() const noexcept { return InitialRate == TargetRate && !HasSourceTransition; }
	};

	// Only the callback owner commits. Readers replay a descriptor independently.
	class PlaybackRate
	{
	public:
		static constexpr double Minimum = 0.5;
		static constexpr double Maximum = 2.0;
		static bool IsValid(double rate) noexcept;
		bool Configure(unsigned int sampleRate, double timeConstantSeconds = 0.010) noexcept;
		bool SetTarget(double rate, std::uint64_t revision) noexcept;
		bool Jump(double rate, std::uint64_t revision = 0u) noexcept;
		PlaybackRateBlock DescribeBlock(unsigned int frames) const noexcept;
		bool Commit(const PlaybackRateBlock& block) noexcept;
		double Current() const noexcept { return _state.Current(); }
		double Target() const noexcept { return _target; }
		std::uint64_t Revision() const noexcept { return _revision; }
		bool IsSettled() const noexcept { return _state.Current() == _target; }
	private:
		PlaybackRateCursor _state{ 1.0, 1.0, 1.0 };
		double _target = 1.0;
		double _damping = 1.0;
		std::uint64_t _revision = 0u;
	};

	// Integral part never passes through double. Overflow rejects advancement;
	// callers reset/rebase only at their existing quiescent lifecycle boundary.
	struct SourcePosition
	{
		std::int64_t Whole = 0;
		double Fraction = 0.0;
		bool Advance(double samples) noexcept;
		double Difference(const SourcePosition& earlier) const noexcept;
	};

	struct PlaybackRateHistoryPoint
	{
		double Rate = 1.0;
		SourcePosition Coordinate;
		std::uint64_t Revision = 0u;
	};

	struct PlaybackMusicalHistoryPoint
	{
		SourcePosition Coordinate;
		double RecordingRate = 1.0;
		std::uint32_t MasterLength = 0u;
	};

	// Construct outside audio. Append/queries allocate nothing. The owner sets
	// the oldest required frame, accounting for history window AND live users.
	// A false Append leaves the accepted history untouched: defer the edit.
	// Append/Clear/RetainFrom have one audio writer. Queries are bounded coherent
	// atomic reads and may fail during publication; readers must not guess.
	class PlaybackRateHistory
	{
	public:
		static_assert(std::atomic<double>::is_always_lock_free);
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
		explicit PlaybackRateHistory(std::size_t capacity);
		// Quiescent lifecycle only: no audio OR job reader may retain this history.
		void Prepare(std::size_t capacity);
		void Clear() noexcept;
		void RetainFrom(std::uint64_t deviceFrame) noexcept { _retainFrom = deviceFrame; }
		bool Append(std::uint64_t deviceStart, const PlaybackRateBlock& block) noexcept;
		bool CanAppend(std::uint64_t deviceStart, const PlaybackRateBlock& block, bool allowContinuation = true) const noexcept;
		std::size_t Capacity() const noexcept { return _segments.size(); }
		std::optional<PlaybackMusicalHistoryPoint> QueryMusicalPosition(std::uint64_t deviceFrame) const noexcept;
		std::optional<PlaybackRateHistoryPoint> Query(std::uint64_t deviceFrame) const noexcept;
		// Replay starts at this edge and ends at the containing segment boundary.
		std::optional<PlaybackRateBlock> DescribeAt(std::uint64_t deviceFrame) const noexcept;
		std::optional<double> QueryDifference(std::uint64_t start, std::uint64_t end,
			double recordingRate = 1.0) const noexcept;
		std::size_t Size() const noexcept { return _publishedCount.load(); }
	private:
		struct SegmentValue
		{
			std::uint64_t Start = 0u;
			PlaybackRateBlock Block;
			SourcePosition Origin;
		};
		struct Segment
		{
			std::atomic<std::uint64_t> Start{ 0u }, Revision{ 0u };
			std::atomic<unsigned int> Frames{ 0u };
			std::atomic<double> InitialRate{ 1.0 }, TargetRate{ 1.0 }, Damping{ 1.0 }, Fraction{ 0.0 };
			std::atomic<double> SourceScale{ 1.0 }, SourceVelocity{ 1.0 }, SourcePhaseError{ 0.0 };
			std::atomic<std::uint64_t> RemoteInterval{ 0u };
			std::atomic<unsigned int> RemoteBpi{ 0u };
			std::atomic<std::uint64_t> MasterWhole{ 0u };
			std::atomic<double> MasterFraction{ 0.0 }, MasterRecordingRate{ 1.0 };
			std::atomic<std::uint32_t> MasterLength{ 0u };
			std::atomic<bool> HasSourceTransition{ false };
			std::atomic<std::int64_t> Whole{ 0 };
			SegmentValue Read() const noexcept;
			void Write(const SegmentValue& value) noexcept;
		};
		std::optional<SegmentValue> Find(std::uint64_t deviceFrame, bool terminalEdge,
			std::size_t first, std::size_t count) const noexcept;
		static std::optional<PlaybackRateHistoryPoint> Point(const SegmentValue& segment,
			std::uint64_t deviceFrame) noexcept;
		bool CanContinue(const SegmentValue& previous, const PlaybackRateBlock& next) const noexcept;
		void UpdateWriterEndpoint(const SegmentValue& segment, unsigned int appendedFrames, bool continuing) noexcept;
		static std::optional<PlaybackMusicalHistoryPoint> MusicalPoint(const SegmentValue& segment,
			std::uint64_t deviceFrame) noexcept;
		std::vector<Segment> _segments;
		std::atomic<std::uint64_t> _sequence{ 0u };
		std::atomic<std::size_t> _publishedFirst{ 0u }, _publishedCount{ 0u };
		std::size_t _first = 0u;
		std::size_t _count = 0u;
		std::uint64_t _retainFrom = 0u;
		// Sole-writer cache; never published or read by history query threads.
		PlaybackRateCursor _writerCursor{ 1.0, 1.0, 1.0 };
		PlaybackRateCursor _writerMusicalCursor{ 1.0, 1.0, 1.0 };
		double _writerProgress = 0.0;
		double _writerMusicalProgress = 0.0;
	};
}
