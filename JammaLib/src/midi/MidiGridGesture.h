#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "LoopGridGeometry.h"
#include "MidiEditOperations.h"
#include "MidiGridTargets.h"

namespace midi
{
	// UI-thread transaction. Every pointer sample transforms a detached source;
	// only the completed Working state is eligible for publication.
	class MidiGridGesture
	{
	public:
		static constexpr std::uint8_t DefaultCreationVelocity = 100u;
		enum class Kind { None, Paint, Create, Move, TrimLeft, TrimRight, Velocity, SnappedMove };
		struct Point
		{
			std::uint32_t Sample = 0u;
			std::uint8_t Pitch = 60u;
			double U = 0.0;
			double V = 0.0;
		};
		struct PreviewSpan
		{
			std::uint32_t Start = 0u, End = 0u;
			std::uint8_t Pitch = 60u;
			bool Fill = true;
		};

		bool BeginVelocity(const MidiLoop::EditState& source, Point point)
		{
			return BeginCaptured(source, point, Kind::Velocity);
		}
		bool BeginSnappedMove(const MidiLoop::EditState& source, Point point)
		{
			return BeginCaptured(source, point, Kind::SnappedMove);
		}
		// Upward-positive relative device units: 3-unit dead zone, 4 units/value.
		// Foreground raw input excludes cursor recenter messages and OS acceleration.
		bool UpdateRelative(double dy)
		{
			if (_kind != Kind::Velocity || _rejected || !std::isfinite(dy)) return false;
			_velocityDelta = std::clamp(_velocityDelta + dy, -1000000.0, 1000000.0);
			const auto steps = std::abs(_velocityDelta) < 3.0 ? 0 : static_cast<int>(std::clamp(std::trunc(_velocityDelta / 4.0), -127.0, 127.0));
			_proposedVelocity = std::clamp(_initialVelocity + steps, 1, 127);
			_working = _before;
			_dirty = _proposedVelocity != _initialVelocity;
			return MidiEditOperations::SetVelocity(_working, _onIndex, _proposedVelocity);
		}
		std::optional<std::size_t> CapturedNoteIndex() const noexcept { return _captured.NoteIndex; }
		const MidiGridTargets::Target& CapturedTarget() const noexcept { return _captured; }
		int ProposedVelocity() const noexcept { return _proposedVelocity; }

		bool Begin(const MidiLoop::EditState& source, Point point, std::uint8_t channel,
			double pixelsPerSample = 0.0, std::uint8_t velocity = DefaultCreationVelocity)
		{
			Cancel();
			if (!source.LoopLengthSamps || point.Sample >= source.LoopLengthSamps
				|| source.EventCount > MidiLoop::DefaultCapacity
				|| point.Pitch > 127u || channel >= 16u || velocity == 0u || velocity > 127u) return false;
			_before = source;
			_working = source;
			_anchor = point;
			_last = point;
			_channel = channel;
			_creationVelocity = velocity;
			_grid = LoopGridGeometry::Resolve(source.LoopLengthSamps,
				source.Quantisation, source.QuantisationTransportStartSamps);
			_targets = MidiGridTargets::Build(source);
			if (_grid)
			{
				_kind = Kind::Paint;
				_visited.resize((_grid->Boundaries.size() - 1u) * 128u, false);
				_removed.resize(source.EventCount, false);
				_fill = !_targets.Resolve(point.Sample, point.Pitch, &*_grid).NoteIndex;
				return Paint(point);
			}
			const auto hit = PickNote(source, point, pixelsPerSample);
			if (hit)
			{
				_onIndex = hit->Index;
				_start = hit->Start;
				_end = hit->End;
				_pitch = hit->Pitch;
				_kind = hit->Zone == LoopGridGeometry::HitZone::LeftEdge ? Kind::TrimLeft
					: hit->Zone == LoopGridGeometry::HitZone::RightEdge ? Kind::TrimRight : Kind::Move;
				_preview.push_back({ _start, _end, _pitch, true });
				return true;
			}
			_kind = Kind::Create;
			const auto duration = std::max(1u, std::min(source.LoopLengthSamps / 32u,
				source.LoopLengthSamps - point.Sample));
			_createDefaultDuration = duration;
			_dirty = MidiEditOperations::Create(_working, point.Sample, duration,
				channel, point.Pitch, _creationVelocity);
			if (_dirty) _preview.push_back({ point.Sample, point.Sample + duration,
				point.Pitch, true });
			return _dirty;
		}

		bool Update(Point point)
		{
			if (_kind == Kind::None || _rejected || point.Pitch > 127u) return false;
			point.Sample = std::min(point.Sample, _before.LoopLengthSamps - 1u);
			if (_kind == Kind::Velocity) return true;
			if (_kind == Kind::SnappedMove)
			{
				const auto grabbedOffset = static_cast<std::int64_t>(_grid->CellAt(_anchor.Sample)) - _grid->CellAt(_start);
				const auto targetCell = std::clamp<std::int64_t>(static_cast<std::int64_t>(_grid->CellAt(point.Sample)) - grabbedOffset,
					0, _grid->Boundaries.size() - 2u);
				const auto duration = _end - _start;
				auto cell = static_cast<std::size_t>(targetCell);
				while (cell > 0 && duration > _before.LoopLengthSamps - _grid->Boundaries[cell]) --cell;
				const auto start = _grid->CellAt(point.Sample) == _grid->CellAt(_anchor.Sample)
					? _start : _grid->Boundaries[cell];
				const auto pitch = static_cast<std::uint8_t>(std::clamp(static_cast<int>(_pitch) + static_cast<int>(point.Pitch) - _anchor.Pitch, 0, 127));
				_working = _before; _preview.clear();
				if (start == _start && pitch == _pitch) { _dirty = false; _preview.push_back({_start, _end, _pitch, true}); return true; }
				_preview.push_back({start, start + duration, pitch, true});
				// An invalid destination is transient: keep the preview and allow retry.
				_dirty = MidiEditOperations::MoveSnappedReplacingOverlaps(_working,
					_onIndex, _offIndex, start, start + duration, pitch);
				return _dirty;
			}
			if (_kind == Kind::Paint)
			{
				int seam = 0;
				if (_last.U > 0.90 && point.U < 0.10) seam = 1;
				else if (_last.U < 0.10 && point.U > 0.90) seam = -1;
				if (!Traverse(point, seam)) return false;
				_last = point;
				return true;
			}
			_working = _before;
			_preview.clear();
			const auto delta = static_cast<std::int64_t>(point.Sample)
				- static_cast<std::int64_t>(_anchor.Sample);
			const auto length = static_cast<std::int64_t>(_before.LoopLengthSamps);
			if (_kind == Kind::Create)
			{
				const auto start = std::min(point.Sample, _anchor.Sample);
				const auto end = point.Sample == _anchor.Sample
					? std::min(length, static_cast<std::int64_t>(_anchor.Sample)
						+ _createDefaultDuration)
					: static_cast<std::int64_t>(std::max(point.Sample, _anchor.Sample)) + 1u;
				_dirty = MidiEditOperations::Create(_working, start,
					static_cast<std::uint32_t>(end - start), _channel, _anchor.Pitch, _creationVelocity);
				if (_dirty) _preview.push_back({ start,
					static_cast<std::uint32_t>(end), _anchor.Pitch, true });
			}
			else
			{
				std::int64_t start = _start, end = _end;
				int pitch = _pitch;
				if (_kind == Kind::Move)
				{
					const auto shift = std::clamp(delta, -start, length - end);
					start += shift; end += shift;
					pitch = std::clamp(static_cast<int>(_pitch) + static_cast<int>(point.Pitch)
						- static_cast<int>(_anchor.Pitch), 0, 127);
				}
				else if (_kind == Kind::TrimLeft)
					start = std::clamp<std::int64_t>(start + delta, 0, end - 1);
				else
					end = std::clamp<std::int64_t>(end + delta, start + 1, length);
				if (start == _start && end == _end && pitch == _pitch)
				{
					_dirty = false;
					_preview.push_back({ _start, _end, _pitch, true });
					_last = point;
					return true;
				}
				_dirty = MidiEditOperations::MoveOrTrim(_working, _onIndex,
					static_cast<std::uint32_t>(start), static_cast<std::uint32_t>(end),
					static_cast<std::uint8_t>(pitch));
				if (_dirty) _preview.push_back({ static_cast<std::uint32_t>(start),
					static_cast<std::uint32_t>(end), static_cast<std::uint8_t>(pitch), true });
			}
			_rejected = !_dirty;
			_last = point;
			return _dirty;
		}

		void Cancel() noexcept
		{
			_kind = Kind::None; _grid.reset(); _visited.clear(); _removed.clear(); _targets.Notes.clear();
			_dirty = false;
			_captured = {}; _velocityDelta = 0.0;
			_rejected = false;
			_preview.clear();
		}
		Kind Mode() const noexcept { return _kind; }
		bool Dirty() const noexcept { return _dirty && !_rejected; }
		bool Rejected() const noexcept { return _rejected; }
		bool Filling() const noexcept { return _fill; }
		const MidiLoop::EditState& Before() const noexcept { return _before; }
		const MidiLoop::EditState& Working() const noexcept { return _working; }
		const std::optional<LoopGridGeometry>& Grid() const noexcept { return _grid; }
		const std::vector<PreviewSpan>& Preview() const noexcept { return _preview; }
		bool MatchesPublished(const MidiLoop::EditState& state) const noexcept
		{
			return _kind != Kind::None && state.Revision == _before.Revision
				&& state.LoopLengthSamps == _before.LoopLengthSamps
				&& state.Quantisation == _before.Quantisation
				&& state.QuantisationTransportStartSamps
					== _before.QuantisationTransportStartSamps;
		}

		MidiGridTargets::Target TargetAt(Point point) const
		{
			return _targets.Resolve(point.Sample, point.Pitch, _grid ? &*_grid : nullptr);
		}

	private:
		bool BeginCaptured(const MidiLoop::EditState& source, Point point, Kind kind)
		{
			Cancel();
			if (!source.LoopLengthSamps || point.Sample >= source.LoopLengthSamps || point.Pitch > 127u || source.EventCount > MidiLoop::DefaultCapacity) return false;
			_before = source; _working = source; _anchor = point; _last = point;
			_grid = LoopGridGeometry::Resolve(source.LoopLengthSamps, source.Quantisation, source.QuantisationTransportStartSamps);
			_targets = MidiGridTargets::Build(source);
			_captured = _targets.Resolve(point.Sample, point.Pitch, _grid ? &*_grid : nullptr);
			if (!_captured.NoteIndex || (kind == Kind::SnappedMove && !_grid)) { Cancel(); return false; }
			const auto& note = _targets.Notes[*_captured.NoteIndex];
			if (note.Ambiguous) { Cancel(); _rejected = true; return false; }
			_onIndex = note.On; _offIndex = note.Off; _start = note.Start; _end = note.End; _pitch = note.Pitch;
			_initialVelocity = source.Events[note.On].data2; _proposedVelocity = _initialVelocity;
			_kind = kind; if (kind == Kind::SnappedMove) _preview.push_back({_start, _end, _pitch, true}); return true;
		}
		struct NoteHit
		{
			std::size_t Index;
			std::uint32_t Start, End;
			std::uint8_t Pitch;
			LoopGridGeometry::HitZone Zone;
		};
		static std::optional<NoteHit> PickNote(const MidiLoop::EditState& source,
			Point point, double pixelsPerSample)
		{
			std::optional<NoteHit> result;
			double bestDistance = 1e30;
			for (std::size_t i = 0u; i < source.EventCount; ++i)
			{
				const auto& on = source.Events[i];
				if (!on.IsNoteOn() || on.data1 != point.Pitch)
					continue;
				std::uint32_t end = source.LoopLengthSamps;
				for (std::size_t j = i + 1u; j < source.EventCount; ++j)
				{
					const auto& next = source.Events[j];
					if (next.PairingSlot() != on.PairingSlot()) continue;
					if (next.IsNoteOn()) { end = on.sampleOffset; break; }
					if (next.IsNoteOff()) { end = next.sampleOffset; break; }
				}
				const auto pixel = point.Sample * pixelsPerSample;
				const auto leftPixel = on.sampleOffset * pixelsPerSample;
				const auto rightPixel = end * pixelsPerSample;
				const auto distance = pixel < leftPixel ? leftPixel - pixel
					: pixel > rightPixel ? pixel - rightPixel : 0.0;
				if (distance > 6.0 || (!pixelsPerSample &&
					(point.Sample < on.sampleOffset || point.Sample >= end))) continue;
				if (distance > bestDistance) continue;
				const auto zone = LoopGridGeometry::NoteHitZone(point.Sample * pixelsPerSample,
					leftPixel, rightPixel);
				// Last NoteOn in canonical order wins for overlapping channels/notes.
				result = NoteHit{ i, on.sampleOffset, end, point.Pitch, zone };
				bestDistance = distance;
			}
			return result;
		}

		bool Reject()
		{
			_rejected = true; _working = _before; _preview.clear();
			return false;
		}
		void AppendPreview(std::uint32_t start, std::uint32_t end, std::uint8_t pitch, bool fill)
		{
			const auto span = _grid ? _grid->EditorSpan(start, end) : std::pair{start, end};
			_preview.push_back({span.first, span.second, pitch, fill});
		}
		bool Paint(Point point)
		{
			const auto target = TargetAt(point);
			if (target.NoteIndex)
			{
				const auto& note = _targets.Notes[*target.NoteIndex];
				if (_removed[note.On]) return true;
				if (_fill)
				{
					// Existing notes crossed by add paint are held too, without editing them.
					_removed[note.On] = true;
					AppendPreview(note.Start, note.End, note.Pitch, true);
					return true;
				}
				if (note.Ambiguous) return Reject();
				_removed[note.On] = true;
				// Rebuild from frozen source identities, deleting in descending order.
				_working = _before;
				for (std::size_t i = _removed.size(); i-- > 0u;)
					if (_removed[i] && !MidiEditOperations::RemoveNote(_working, i)) return Reject();
				_dirty = true;
				AppendPreview(note.Start, note.End, note.Pitch, false);
				return true;
			}
			if (!_fill) return true;
			const auto physicalCell = _grid->CellAt(point.Sample);
			// Both fragments paint one canonical source note in the tail cell.
			const auto cell = _grid->SeamHeadEnd && physicalCell == 0u
				? _grid->Boundaries.size() - 2u : physicalCell;
			const auto index = cell * 128u + point.Pitch;
			if (_visited[index]) return true;
			_visited[index] = true;
			const auto cellStart = _grid->Boundaries[cell], cellEnd = _grid->Boundaries[cell + 1u];
			// Subtract the union of frozen displayed notes, across all channels.
			std::vector<std::pair<std::uint32_t, std::uint32_t>> covered;
			for (const auto& note : _targets.Notes)
				if (note.Pitch == point.Pitch && note.Start < cellEnd && note.End > cellStart)
					covered.emplace_back(std::max(note.Start, cellStart), std::min(note.End, cellEnd));
			std::sort(covered.begin(), covered.end());
			auto start = cellStart;
			const auto add = [&](std::uint32_t end) {
				if (start >= end) return true;
				if (!MidiEditOperations::CreateExact(_working, start, end, _channel, point.Pitch, _creationVelocity)) return false;
				AppendPreview(start, end, point.Pitch, true); _dirty = true;
				return true;
			};
			for (const auto& span : covered)
			{
				if (!add(span.first)) return Reject();
				start = std::max(start, span.second);
			}
			if (!add(cellEnd)) return Reject();
			return true;
		}
		bool Traverse(Point point, int seam)
		{
			const double length = _before.LoopLengthSamps;
			const double from = _last.Sample;
			const double delta = static_cast<double>(point.Sample) + seam * length - from;
			const double pitchDelta = static_cast<int>(point.Pitch) - static_cast<int>(_last.Pitch);
			std::vector<double> cuts{0.0, 1.0};
			const auto cut = [&](double boundary) {
				if (delta == 0.0) return;
				const auto t = (boundary - from) / delta;
				if (t > 0.0 && t < 1.0) cuts.push_back(t);
			};
			for (int turn = -1; turn <= 1; ++turn)
			{
				for (auto boundary : _grid->Boundaries) cut(boundary + turn * length);
				for (const auto& note : _targets.Notes)
				{
					cut(note.Start + turn * length); cut(note.End + turn * length);
				}
			}
			if (pitchDelta != 0.0)
				for (int pitch = std::min(_last.Pitch, point.Pitch); pitch < std::max(_last.Pitch, point.Pitch); ++pitch)
					cuts.push_back((pitch + 0.5 - _last.Pitch) / pitchDelta);
			std::sort(cuts.begin(), cuts.end());
			cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
			// Every open interval has one constant note/cell/row classification.
			// Midpoints ensure even a one-sample note skipped by hardware is visited.
			for (std::size_t i = 1; i < cuts.size(); ++i)
			{
				const auto t = (cuts[i - 1u] + cuts[i]) * 0.5;
				auto sample = std::fmod(from + delta * t + length, length);
				const Point traversed{static_cast<std::uint32_t>(sample),
					static_cast<std::uint8_t>(std::round(_last.Pitch + pitchDelta * t)), sample / length};
				if (!Paint(traversed)) return false;
			}
			return Paint(point);
		}

		MidiLoop::EditState _before{};
		MidiLoop::EditState _working{};
		std::optional<LoopGridGeometry> _grid;
		std::vector<bool> _visited;
		std::vector<bool> _removed;
		MidiGridTargets _targets;
		std::vector<PreviewSpan> _preview;
		Kind _kind = Kind::None;
		Point _anchor{}, _last{};
		std::size_t _onIndex = 0u, _offIndex = 0u;
		MidiGridTargets::Target _captured{};
		double _velocityDelta = 0.0;
		int _initialVelocity = 96, _proposedVelocity = 96;
		std::uint32_t _start = 0u, _end = 0u;
		std::uint32_t _createDefaultDuration = 1u;
		std::uint8_t _pitch = 60u, _channel = 0u;
		std::uint8_t _creationVelocity = DefaultCreationVelocity;
		bool _fill = true, _dirty = false, _rejected = false;
	};
}
