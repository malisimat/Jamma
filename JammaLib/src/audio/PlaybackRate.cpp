#include "PlaybackRate.h"
#include <algorithm>
#include <cmath>
#include <limits>

using namespace audio;

InterpolatedValueExp::ExponentialParams PlaybackRateCursor::Parameters(double damping) noexcept
{
	InterpolatedValueExp::ExponentialParams parameters;
	parameters.Damping = std::isfinite(damping) && damping >= 1.0 ? damping : 1.0;
	return parameters;
}

PlaybackRateCursor::PlaybackRateCursor(double initial, double target, double damping, double sourceScale,
	bool hasSourceTransition, double initialSourceVelocity, double initialSourcePhaseError) noexcept :
	_value(Parameters(damping)), _target(target), _damping(Parameters(damping).Damping),
	_sourceScale(sourceScale), _hasSourceTransition(hasSourceTransition)
{
	_value.Jump(initial);
	_value.SetTarget(target);
	_transition.Initialise(initialSourceVelocity, initial * sourceScale);
	_transition.SetPhaseError(initialSourcePhaseError);
	_baseIncrement = SourceVelocity();
}

double PlaybackRateCursor::Next() noexcept
{
	if (_value.Current() != _target)
	{
		const auto next = _value.Next();
		if (std::abs(next - _target) <= SettleTolerance) _value.Jump(_target);
	}
	const auto ideal = _value.Current() * _sourceScale;
	const auto advance = _hasSourceTransition ? _transition.Next(ideal, _damping) : ideal;
	_baseIncrement = SourceVelocity();
	_correction = advance - _baseIncrement;
	return advance;
}

double PlaybackRateCursor::SourceVelocity() const noexcept
{
	return _hasSourceTransition ? _transition.Velocity() : _value.Current() * _sourceScale;
}

PlaybackRateCursor PlaybackRateBlock::Cursor() const noexcept
{
	return { InitialRate, TargetRate, Damping, SourceScale, HasSourceTransition,
		InitialSourceVelocity, InitialSourcePhaseError };
}

double PlaybackRateBlock::Advance(unsigned int frames, double recordingRate) const noexcept
{
	if (!std::isfinite(recordingRate) || recordingRate <= 0.0)
		return 0.0;
	frames = std::min(frames, Frames);
	if (IsConstant())
		return static_cast<double>(frames) * (TargetRate * SourceScale / recordingRate);
	auto cursor = Cursor();
	double progress = 0.0;
	for (unsigned int frame = 0u; frame < frames; ++frame)
		progress += cursor.Next() / recordingRate;
	return progress;
}

double PlaybackRateBlock::RateAt(unsigned int frame) const noexcept
{
	if (IsConstant()) return TargetRate * SourceScale;
	auto cursor = Cursor();
	// The terminal edge is queryable, but never extrapolate a history block.
	const auto count = std::min(frame, Frames);
	for (unsigned int index = 0u; index < count; ++index) cursor.Next();
	return frame < Frames ? cursor.Next() : cursor.BaseIncrement() + cursor.Correction();
}

bool PlaybackRate::IsValid(double rate) noexcept
{
	return std::isfinite(rate) && rate >= Minimum && rate <= Maximum;
}

bool PlaybackRate::Configure(unsigned int sampleRate, double timeConstantSeconds) noexcept
{
	if (sampleRate == 0u || !std::isfinite(timeConstantSeconds) || timeConstantSeconds <= 0.0)
		return false;
	const auto alpha = -std::expm1(-1.0 / (sampleRate * timeConstantSeconds));
	if (!std::isfinite(alpha) || alpha <= 0.0) return false;
	_damping = 1.0 / alpha;
	_state = PlaybackRateCursor(_state.Current(), _target, _damping);
	return true;
}

bool PlaybackRate::SetTarget(double rate, std::uint64_t revision) noexcept
{
	if (!IsValid(rate) || revision < _revision) return false;
	if (revision == _revision && rate != _target) return false;
	_target = rate;
	_revision = revision;
	_state = PlaybackRateCursor(_state.Current(), _target, _damping);
	return true;
}

bool PlaybackRate::Jump(double rate, std::uint64_t revision) noexcept
{
	if (!IsValid(rate)) return false;
	_target = rate;
	_state = PlaybackRateCursor(rate, rate, _damping);
	_revision = revision;
	return true;
}

PlaybackRateBlock PlaybackRate::DescribeBlock(unsigned int frames) const noexcept
{
	return { _state.Current(), _target, _damping, frames, _revision };
}

bool PlaybackRate::Commit(const PlaybackRateBlock& block) noexcept
{
	if (block.Revision != _revision || block.InitialRate != _state.Current() ||
		block.TargetRate != _target || block.Damping != _damping ||
		block.SourceScale != 1.0 || block.HasSourceTransition) return false;
	auto cursor = block.Cursor();
	if (!block.IsConstant())
		for (unsigned int frame = 0u; frame < block.Frames; ++frame) cursor.Next();
	_state = cursor;
	return true;
}

bool SourcePosition::Advance(double samples) noexcept
{
	if (!std::isfinite(samples) || !std::isfinite(Fraction) || Fraction < 0.0 || Fraction >= 1.0)
		return false;
	const auto delta = std::floor(samples + Fraction);
	const auto next = static_cast<long double>(Whole) + static_cast<long double>(delta);
	if (next < static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
		next > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) return false;
	// Check delta before casting, including platforms where long double is double.
	if (delta <= -9223372036854775808.0 || delta >= 9223372036854775808.0) return false;
	const auto integral = static_cast<std::int64_t>(delta);
	if ((integral > 0 && Whole > std::numeric_limits<std::int64_t>::max() - integral) ||
		(integral < 0 && Whole < std::numeric_limits<std::int64_t>::min() - integral)) return false;
	Fraction = samples + Fraction - delta;
	Whole += integral;
	return true;
}

double SourcePosition::Difference(const SourcePosition& earlier) const noexcept
{
	// Subtract unsigned magnitudes before conversion: nearby positions remain
	// exact even after the integral coordinate has exceeded double precision.
	const auto difference = Whole >= earlier.Whole
		? static_cast<double>(static_cast<std::uint64_t>(Whole) - static_cast<std::uint64_t>(earlier.Whole))
		: -static_cast<double>(static_cast<std::uint64_t>(earlier.Whole) - static_cast<std::uint64_t>(Whole));
	return difference + Fraction - earlier.Fraction;
}

void SourcePhaseCorrection::SetError(double sourceSamples) noexcept
{
	if (std::isfinite(sourceSamples)) _error = sourceSamples;
}

double SourcePhaseCorrection::Apply(double baseIncrement, double damping) noexcept
{
	if (!std::isfinite(baseIncrement) || baseIncrement <= 0.0 || !std::isfinite(damping) || damping < 1.0)
		return 0.0;
	const auto cap = MaximumFraction * baseIncrement;
	const auto desired = std::abs(_error) <= PositionTolerance ? _error : _error / damping;
	const auto correction = std::clamp(desired, -cap, cap);
	_error -= correction;
	return correction;
}

void SourceVelocityTransition::Initialise(double sourceIncrement, double idealIncrement) noexcept
{
	if (std::isfinite(sourceIncrement) && sourceIncrement > 0.0)
	{
		_velocity = sourceIncrement;
		_previousIdeal = std::isfinite(idealIncrement) && idealIncrement > 0.0 ? idealIncrement : 0.0;
		_phase.SetError(0.0);
	}
}

double SourceVelocityTransition::Next(double idealIncrement, double damping) noexcept
{
	if (!std::isfinite(idealIncrement) || idealIncrement <= 0.0 || !std::isfinite(damping) || damping < 1.0)
		return 0.0;
	// Decay only the authority-switch velocity difference. A moving common G
	// trajectory must pass through unchanged, rather than be smoothed twice.
	const auto previousIdeal = _previousIdeal > 0.0 ? _previousIdeal : idealIncrement;
	_velocity = idealIncrement + (_velocity - previousIdeal) * (1.0 - 1.0 / damping);
	_previousIdeal = idealIncrement;
	if (std::abs(_velocity - idealIncrement) <= PlaybackRateCursor::SettleTolerance)
		_velocity = idealIncrement;
	_phase.SetError(_phase.Error() + idealIncrement - _velocity);
	return _velocity + _phase.Apply(_velocity, damping);
}

bool SourceVelocityTransition::IsSettled(double idealIncrement) const noexcept
{
	return _velocity == idealIncrement && _phase.IsSettled();
}

PlaybackRateHistory::PlaybackRateHistory(std::size_t capacity) : _segments(capacity) {}

void PlaybackRateHistory::Prepare(std::size_t capacity)
{
	std::vector<Segment> prepared(capacity);
	_segments.swap(prepared);
	Clear();
}

PlaybackRateHistory::SegmentValue PlaybackRateHistory::Segment::Read() const noexcept
{
	return { Start.load(), { InitialRate.load(), TargetRate.load(), Damping.load(),
		Frames.load(), Revision.load(), SourceScale.load(), HasSourceTransition.load(),
		SourceVelocity.load(), SourcePhaseError.load(), RemoteInterval.load(), RemoteBpi.load(),
		MasterWhole.load(), MasterFraction.load(), MasterRecordingRate.load(), MasterLength.load() }, { Whole.load(), Fraction.load() } };
}

void PlaybackRateHistory::Segment::Write(const SegmentValue& value) noexcept
{
	Start.store(value.Start);
	InitialRate.store(value.Block.InitialRate);
	TargetRate.store(value.Block.TargetRate);
	Damping.store(value.Block.Damping);
	Frames.store(value.Block.Frames);
	Revision.store(value.Block.Revision);
	SourceScale.store(value.Block.SourceScale);
	HasSourceTransition.store(value.Block.HasSourceTransition);
	SourceVelocity.store(value.Block.InitialSourceVelocity);
	SourcePhaseError.store(value.Block.InitialSourcePhaseError);
	RemoteInterval.store(value.Block.RemoteIntervalLengthSamps);
	RemoteBpi.store(value.Block.RemoteBpi);
	MasterWhole.store(value.Block.MasterPositionWhole);
	MasterFraction.store(value.Block.MasterPositionFraction);
	MasterRecordingRate.store(value.Block.MasterRecordingRate);
	MasterLength.store(value.Block.MasterLength);
	Whole.store(value.Origin.Whole);
	Fraction.store(value.Origin.Fraction);
}

void PlaybackRateHistory::Clear() noexcept
{
	_sequence.fetch_add(1u);
	_first = _count = 0u;
	_retainFrom = 0u;
	_writerCursor = PlaybackRateCursor(1.0, 1.0, 1.0);
	_writerMusicalCursor = PlaybackRateCursor(1.0, 1.0, 1.0);
	_writerProgress = _writerMusicalProgress = 0.0;
	_publishedFirst.store(0u);
	_publishedCount.store(0u);
	_sequence.fetch_add(1u);
}

std::optional<PlaybackMusicalHistoryPoint> PlaybackRateHistory::MusicalPoint(
	const SegmentValue& segment, std::uint64_t deviceFrame) noexcept
{
	const auto& block = segment.Block;
	if (!block.MasterLength || block.MasterPositionWhole > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
		|| !std::isfinite(block.MasterRecordingRate) || block.MasterRecordingRate <= 0.0) return std::nullopt;
	SourcePosition position{ static_cast<std::int64_t>(block.MasterPositionWhole), block.MasterPositionFraction };
	const auto frames = static_cast<unsigned int>(deviceFrame - segment.Start);
	auto musical = block;
	musical.SourceScale = 1.0;
	musical.HasSourceTransition = false;
	if (!position.Advance(block.RemoteIntervalLengthSamps ? static_cast<double>(frames)
		: musical.Advance(frames, block.MasterRecordingRate))) return std::nullopt;
	return PlaybackMusicalHistoryPoint{ position, block.MasterRecordingRate, block.MasterLength };
}

void PlaybackRateHistory::UpdateWriterEndpoint(const SegmentValue& segment,
	unsigned int appendedFrames, bool continuing) noexcept
{
	const auto& block = segment.Block;
	auto musical = block;
	musical.SourceScale = 1.0;
	musical.HasSourceTransition = false;
	if (!continuing)
	{
		_writerCursor = block.Cursor();
		_writerMusicalCursor = musical.Cursor();
		_writerProgress = _writerMusicalProgress = 0.0;
	}
	if (block.IsConstant()) _writerProgress = block.Advance(block.Frames);
	else for (unsigned int frame = 0u; frame < appendedFrames; ++frame)
		_writerProgress += _writerCursor.Next();
	if (block.RemoteIntervalLengthSamps) _writerMusicalProgress = static_cast<double>(block.Frames);
	else if (block.MasterLength && std::isfinite(block.MasterRecordingRate) && block.MasterRecordingRate > 0.0)
	{
		if (musical.IsConstant()) _writerMusicalProgress = musical.Advance(block.Frames, block.MasterRecordingRate);
		else for (unsigned int frame = 0u; frame < appendedFrames; ++frame)
			_writerMusicalProgress += _writerMusicalCursor.Next() / block.MasterRecordingRate;
	}
}

bool PlaybackRateHistory::CanContinue(const SegmentValue& previous, const PlaybackRateBlock& next) const noexcept
{
	const auto& old = previous.Block;
	if (old.TargetRate != next.TargetRate || old.Revision != next.Revision || old.Damping != next.Damping
		|| old.SourceScale != next.SourceScale || old.HasSourceTransition != next.HasSourceTransition
		|| old.RemoteIntervalLengthSamps != next.RemoteIntervalLengthSamps || old.RemoteBpi != next.RemoteBpi
		|| old.MasterLength != next.MasterLength || old.MasterRecordingRate != next.MasterRecordingRate
		|| next.Frames > (std::numeric_limits<unsigned int>::max)() - old.Frames) return false;
	// Retain the existing bounded query replay and admission policy.
	if (!(old.IsConstant() && next.IsConstant()) && old.Frames + next.Frames > 4096u) return false;
	if (old.MasterLength)
	{
		if (old.MasterPositionWhole > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
			|| !std::isfinite(old.MasterRecordingRate) || old.MasterRecordingRate <= 0.0) return false;
		SourcePosition end{ static_cast<std::int64_t>(old.MasterPositionWhole), old.MasterPositionFraction };
		if (!end.Advance(_writerMusicalProgress) || end.Whole < 0 || static_cast<std::uint64_t>(end.Whole) != next.MasterPositionWhole
			|| std::abs(end.Fraction - next.MasterPositionFraction) > 1e-9) return false;
	}
	if (old.IsConstant() && next.IsConstant()) return old.TargetRate == next.InitialRate;
	return _writerCursor.Current() == next.InitialRate && (!next.HasSourceTransition
		|| (_writerCursor.SourceVelocity() == next.InitialSourceVelocity && _writerCursor.SourcePhaseError() == next.InitialSourcePhaseError));
}

bool PlaybackRateHistory::CanAppend(std::uint64_t deviceStart, const PlaybackRateBlock& block, bool allowContinuation) const noexcept
{
	if (_segments.empty() || !block.Frames) return false;
	if (!_count) return true;
	const auto last = _segments[(_first + _count - 1u) % _segments.size()].Read();
	if (last.Start + last.Block.Frames != deviceStart) return false;
	if (_count < _segments.size() || (allowContinuation && CanContinue(last, block))) return true;
	const auto oldest = _segments[_first].Read();
	return oldest.Start + oldest.Block.Frames <= _retainFrom;
}

bool PlaybackRateHistory::Append(std::uint64_t deviceStart, const PlaybackRateBlock& block) noexcept
{
	if (_segments.empty() || block.Frames == 0u ||
		deviceStart > std::numeric_limits<std::uint64_t>::max() - block.Frames ||
		!std::isfinite(block.InitialRate) || block.InitialRate <= 0.0 ||
		!std::isfinite(block.TargetRate) || block.TargetRate <= 0.0 ||
		!std::isfinite(block.Damping) || block.Damping < 1.0 ||
		!std::isfinite(block.SourceScale) || block.SourceScale <= 0.0 ||
		!std::isfinite(block.InitialSourceVelocity) || block.InitialSourceVelocity <= 0.0 ||
		!std::isfinite(block.InitialSourcePhaseError)) return false;
	SourcePosition origin;
	if (_count != 0u)
	{
		const auto lastIndex = (_first + _count - 1u) % _segments.size();
		auto last = _segments[lastIndex].Read();
		if (last.Start + last.Block.Frames != deviceStart) return false;
		if (CanContinue(last, block))
		{
			last.Block.Frames += block.Frames;
			UpdateWriterEndpoint(last, block.Frames, true);
			_sequence.fetch_add(1u);
			_segments[lastIndex].Write(last);
			_sequence.fetch_add(1u);
			return true;
		}
		origin = last.Origin;
		if (!origin.Advance(_writerProgress)) return false;
	}
	if (_count == _segments.size())
	{
		const auto oldest = _segments[_first].Read();
		if (oldest.Start + oldest.Block.Frames > _retainFrom) return false;
	}
	UpdateWriterEndpoint({ deviceStart, block, origin }, block.Frames, false);
	_sequence.fetch_add(1u);
	if (_count == _segments.size())
	{
		_first = (_first + 1u) % _segments.size();
		--_count;
	}
	_segments[(_first + _count) % _segments.size()].Write({ deviceStart, block, origin });
	++_count;
	_publishedFirst.store(_first);
	_publishedCount.store(_count);
	_sequence.fetch_add(1u);
	return true;
}

std::optional<PlaybackRateHistory::SegmentValue> PlaybackRateHistory::Find(
	std::uint64_t deviceFrame, bool terminalEdge, std::size_t first, std::size_t count) const noexcept
{
	std::size_t lower = 0u, upper = count;
	while (lower < upper)
	{
		const auto middle = lower + (upper - lower) / 2u;
		if (_segments[(first + middle) % _segments.size()].Start.load() <= deviceFrame) lower = middle + 1u;
		else upper = middle;
	}
	if (!lower) return std::nullopt;
	const auto segment = _segments[(first + lower - 1u) % _segments.size()].Read();
	const auto delta = deviceFrame - segment.Start;
	if (delta > segment.Block.Frames || (!terminalEdge && delta == segment.Block.Frames)) return std::nullopt;
	return segment;
}

std::optional<PlaybackRateHistoryPoint> PlaybackRateHistory::Point(
	const SegmentValue& segment, std::uint64_t deviceFrame) noexcept
{
	const auto offset = static_cast<unsigned int>(deviceFrame - segment.Start);
	auto coordinate = segment.Origin;
	if (!coordinate.Advance(segment.Block.Advance(offset))) return std::nullopt;
	return PlaybackRateHistoryPoint{ segment.Block.RateAt(offset), coordinate, segment.Block.Revision };
}

std::optional<PlaybackRateHistoryPoint> PlaybackRateHistory::Query(std::uint64_t deviceFrame) const noexcept
{
	for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
	{
		const auto before = _sequence.load();
		if (before & 1u) continue;
		const auto segment = Find(deviceFrame, true, _publishedFirst.load(), _publishedCount.load());
		if (_sequence.load() != before) continue;
		return segment ? Point(*segment, deviceFrame) : std::nullopt;
	}
	return std::nullopt;
}

std::optional<double> PlaybackRateHistory::QueryDifference(std::uint64_t start, std::uint64_t end,
	double recordingRate) const noexcept
{
	if (!std::isfinite(recordingRate) || recordingRate <= 0.0) return std::nullopt;
	for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
	{
		const auto before = _sequence.load();
		if (before & 1u) continue;
		const auto firstIndex = _publishedFirst.load();
		const auto count = _publishedCount.load();
		const auto first = Find(start, true, firstIndex, count);
		const auto last = Find(end, true, firstIndex, count);
		if (_sequence.load() != before) continue;
		if (!first || !last) return std::nullopt;
		const auto firstPoint = Point(*first, start);
		const auto lastPoint = Point(*last, end);
		if (!firstPoint || !lastPoint) return std::nullopt;
		return lastPoint->Coordinate.Difference(firstPoint->Coordinate) / recordingRate;
	}
	return std::nullopt;
}

std::optional<PlaybackMusicalHistoryPoint> PlaybackRateHistory::QueryMusicalPosition(std::uint64_t deviceFrame) const noexcept
{
	for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
	{
		const auto before = _sequence.load();
		if (before & 1u) continue;
		const auto segment = Find(deviceFrame, true, _publishedFirst.load(), _publishedCount.load());
		if (_sequence.load() != before) continue;
		return segment ? MusicalPoint(*segment, deviceFrame) : std::nullopt;
	}
	return std::nullopt;
}

std::optional<PlaybackRateBlock> PlaybackRateHistory::DescribeAt(std::uint64_t deviceFrame) const noexcept
{
	for (unsigned int attempt = 0u; attempt < 4u; ++attempt)
	{
		const auto before = _sequence.load();
		if (before & 1u) continue;
		const auto segment = Find(deviceFrame, false, _publishedFirst.load(), _publishedCount.load());
		if (_sequence.load() != before) continue;
		if (!segment) return std::nullopt;
		const auto offset = static_cast<unsigned int>(deviceFrame - segment->Start);
		auto block = segment->Block;
		if (offset > 0u)
		{
			if (const auto master = MusicalPoint(*segment, deviceFrame))
			{
				block.MasterPositionWhole = static_cast<std::uint64_t>(master->Coordinate.Whole);
				block.MasterPositionFraction = master->Coordinate.Fraction;
			}
			auto cursor = block.Cursor();
			if (!block.IsConstant())
				for (unsigned int frame = 0u; frame < offset; ++frame) cursor.Next();
			block.InitialRate = cursor.Current();
			block.InitialSourceVelocity = cursor.SourceVelocity();
			block.InitialSourcePhaseError = cursor.SourcePhaseError();
		}
		block.Frames -= offset;
		return block;
	}
	return std::nullopt;
}
