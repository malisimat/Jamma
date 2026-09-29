#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "LoopGridGeometry.h"
#include "MidiEditOperations.h"

namespace midi
{
	// UI-thread transaction. Every pointer sample transforms a detached source;
	// only the completed Working state is eligible for publication.
	class MidiGridGesture
	{
	public:
		enum class Kind { None, Paint, Create, Move, TrimLeft, TrimRight };
		struct Point
		{
			std::uint32_t Sample = 0u;
			std::uint8_t Pitch = 60u;
			double U = 0.0;
		};
		struct PreviewSpan
		{
			std::uint32_t Start = 0u, End = 0u;
			std::uint8_t Pitch = 60u;
			bool Fill = true;
		};

		bool Begin(const MidiLoop::EditState& source, Point point, std::uint8_t channel,
			double pixelsPerSample = 0.0)
		{
			Cancel();
			if (!source.LoopLengthSamps || point.Sample >= source.LoopLengthSamps
				|| point.Pitch > 127u || channel >= 16u) return false;
			_before = source;
			_working = source;
			_anchor = point;
			_last = point;
			_channel = channel;
			_grid = LoopGridGeometry::Resolve(source.LoopLengthSamps,
				source.Quantisation, source.QuantisationTransportStartSamps);
			if (_grid)
			{
				_kind = Kind::Paint;
				_lastCell = { _grid->CellAt(point.Sample), point.Pitch };
				_visited.resize((_grid->Boundaries.size() - 1u) * 128u, false);
				_occupied.resize(_visited.size(), false);
				BuildOccupancy(source);
				_fill = !_occupied[_lastCell.Time * 128u + _lastCell.Pitch];
				return Paint(_lastCell);
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
				return true;
			}
			_kind = Kind::Create;
			const auto duration = std::max(1u, std::min(source.LoopLengthSamps / 32u,
				source.LoopLengthSamps - point.Sample));
			_createDefaultDuration = duration;
			_dirty = MidiEditOperations::Create(_working, point.Sample, duration,
				channel, point.Pitch);
			if (_dirty) _preview.push_back({ point.Sample, point.Sample + duration,
				point.Pitch, true });
			return _dirty;
		}

		bool Update(Point point)
		{
			if (_kind == Kind::None || _rejected || point.Pitch > 127u) return false;
			point.Sample = std::min(point.Sample, _before.LoopLengthSamps - 1u);
			if (_kind == Kind::Paint)
			{
				const LoopGridGeometry::Cell current{ _grid->CellAt(point.Sample), point.Pitch };
				int seam = 0;
				if (_last.U > 0.90 && point.U < 0.10) seam = 1;
				else if (_last.U < 0.10 && point.U > 0.90) seam = -1;
				for (const auto cell : _grid->CrossedCells(_lastCell, current, seam))
					if (!Paint(cell)) return false;
				_lastCell = current;
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
					static_cast<std::uint32_t>(end - start), _channel, _anchor.Pitch);
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
			_kind = Kind::None; _grid.reset(); _visited.clear(); _occupied.clear();
			_dirty = false;
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

	private:
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
					if (next.Channel() != on.Channel() || next.data1 != on.data1) continue;
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
		void BuildOccupancy(const MidiLoop::EditState& source)
		{
			std::vector<MidiEvent> quantised(source.EventCount);
			MidiQuantisation::BuildQuantisedPlaybackEvents(source.Events.data(), source.EventCount,
				source.LoopLengthSamps, source.Quantisation,
				source.QuantisationTransportStartSamps, quantised.data());
			for (const auto& note : MidiNote::ExtractSpans(quantised.data(), quantised.size(),
				source.LoopLengthSamps))
			{
				for (std::size_t cell = _grid->CellAt(note.StartSample);
					cell + 1u < _grid->Boundaries.size(); ++cell)
				{
					const auto midpoint = _grid->Boundaries[cell]
						+ (_grid->Boundaries[cell + 1u] - _grid->Boundaries[cell]) / 2u;
					if (midpoint >= note.StartSample + note.DurationSamples) break;
					if (midpoint >= note.StartSample)
						_occupied[cell * 128u + note.Note] = true;
				}
			}
		}
		bool Paint(LoopGridGeometry::Cell cell)
		{
			const auto index = cell.Time * 128u + cell.Pitch;
			if (_visited[index]) return true;
			_visited[index] = true;
			if (_occupied[index] == _fill) return true;
			if (!MidiEditOperations::SetCell(_working, _grid->Boundaries[cell.Time],
				_grid->Boundaries[cell.Time + 1u], cell.Pitch, _channel, _fill,
				_before.Quantisation, _before.QuantisationTransportStartSamps))
			{
				_rejected = true;
				_working = _before;
				_preview.clear();
				return false;
			}
			_dirty = true;
			_preview.push_back({ _grid->Boundaries[cell.Time],
				_grid->Boundaries[cell.Time + 1u], cell.Pitch, _fill });
			return true;
		}

		MidiLoop::EditState _before{};
		MidiLoop::EditState _working{};
		std::optional<LoopGridGeometry> _grid;
		std::vector<bool> _visited;
		std::vector<bool> _occupied;
		std::vector<PreviewSpan> _preview;
		Kind _kind = Kind::None;
		Point _anchor{}, _last{};
		LoopGridGeometry::Cell _lastCell{};
		std::size_t _onIndex = 0u;
		std::uint32_t _start = 0u, _end = 0u;
		std::uint32_t _createDefaultDuration = 1u;
		std::uint8_t _pitch = 60u, _channel = 0u;
		bool _fill = true, _dirty = false, _rejected = false;
	};
}
