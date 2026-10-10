#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include <utility>

#include "MidiQuantisation.h"

namespace midi
{
	// Pure, loop-local editor geometry. A resolved take setting and its transport
	// start are captured together; callers discard this value when either changes.
	struct LoopGridGeometry
	{
		static constexpr std::size_t MaxCells = 8192u;

		struct Cell
		{
			std::size_t Time = 0u;
			std::uint8_t Pitch = 0u;
			bool operator==(const Cell& other) const noexcept
			{
				return Time == other.Time && Pitch == other.Pitch;
			}
		};

		// Includes the two physical loop edges. Interior values are every reachable
		// snapped onset from raw samples [0, length), wrapped into loop-local time.
		// This includes a boundary beyond the physical end for non-dividing loops.
		// Zero-width cells are removed.
		std::vector<std::uint32_t> Boundaries;
		// Nonzero only when the two physical edge fragments belong to one
		// repeating grid cell. These are presentation aliases, not extra events.
		std::uint32_t SeamHeadEnd = 0u, SeamTailStart = 0u;

		std::pair<std::uint32_t, std::uint32_t> EditorSpan(std::uint32_t start,
			std::uint32_t end) const noexcept
		{
			if (!SeamHeadEnd) return {start, end};
			if (start < SeamHeadEnd && end <= SeamTailStart)
				return {SeamTailStart, Boundaries.back() + end};
			if (start >= SeamTailStart && end == Boundaries.back())
				return {start, end + SeamHeadEnd};
			return {start, end};
		}

		// Presentation only: cut a whole-grain loop at the nearest master grain.
		// Explicit phase offsets and non-repeating geometry retain physical edges.
		static std::uint32_t DisplayOrigin(std::uint32_t length,
			const MidiQuantisationSettings& settings, std::uint64_t transportStart) noexcept
		{
			if (!length || !settings.Enabled || settings.PhaseOffsetSamps != 0
				|| !settings.GrainSamps || settings.HasRemoteGrid() || settings.HasBaseGrid()
				|| length % settings.GrainSamps != 0u)
				return 0u;
			const auto grain = settings.GrainSamps;
			const auto phase = transportStart % grain;
			const auto delta = phase * 2u < grain
				? -static_cast<std::int64_t>(phase) : static_cast<std::int64_t>(grain - phase);
			return static_cast<std::uint32_t>((delta % length + length) % length);
		}

		static std::uint32_t DisplaySample(std::uint32_t sample, std::uint32_t length,
			std::uint32_t origin) noexcept
		{
			return length ? static_cast<std::uint32_t>((static_cast<std::uint64_t>(sample)
				+ length - origin) % length) : 0u;
		}

		static std::optional<LoopGridGeometry> Resolve(std::uint32_t length,
			const MidiQuantisationSettings& settings, std::uint64_t transportStart)
		{
			// MSVC long double has double precision. Keep absolute arithmetic in
			// its exact-integer range before translating back to local samples.
			constexpr std::uint64_t ExactLimit = 1ull << 50u;
			if (length == 0u || !settings.Enabled || transportStart > ExactLimit ||
				(settings.HasRemoteGrid() &&
					(settings.RemoteOriginSamps > static_cast<std::int64_t>(ExactLimit) ||
					settings.RemoteOriginSamps < -static_cast<std::int64_t>(ExactLimit))))
				return std::nullopt;
			const bool remote = settings.HasRemoteGrid();
			const std::uint64_t interval = settings.GridInterval();
			const std::uint64_t divisions = settings.GridDivisions();
			if (interval == 0u || divisions == 0u || divisions > MaxCells)
				return std::nullopt;
			const std::int64_t start = static_cast<std::int64_t>(transportStart);
			const std::int64_t origin = remote ? settings.RemoteOriginSamps : 0;
			const std::int64_t phase = settings.PhaseOffsetSamps;
			// Avoid overflow in relative coordinates and BoundarySampleAt's product.
			const long double firstRelative = static_cast<long double>(start) - origin;
			const long double lastRelative = firstRelative + length - 1u;
			if (firstRelative < static_cast<long double>((std::numeric_limits<std::int64_t>::min)()) + 2 * interval ||
				lastRelative > static_cast<long double>((std::numeric_limits<std::int64_t>::max)()) - 2 * interval ||
				(std::max)(std::abs(firstRelative), std::abs(lastRelative)) / interval * divisions >
					static_cast<long double>((std::numeric_limits<std::int64_t>::max)()) - 2 * divisions)
				return std::nullopt;
			const auto first = MidiQuantisation::NearestBoundaryIndex(
				static_cast<std::int64_t>(firstRelative), interval, divisions);
			const auto last = MidiQuantisation::NearestBoundaryIndex(
				static_cast<std::int64_t>(lastRelative), interval, divisions);
			if (last < first || static_cast<std::uint64_t>(last - first) > MaxCells)
				return std::nullopt;
			const auto productLimit = ((std::numeric_limits<std::int64_t>::max)() -
				static_cast<std::int64_t>(divisions / 2u)) / static_cast<std::int64_t>(interval);
			if (first < -productLimit + 2 || last > productLimit - 2)
				return std::nullopt;
			LoopGridGeometry grid;
			grid.Boundaries.reserve(static_cast<std::size_t>(last - first) + 3u);
			grid.Boundaries.push_back(0u);
			for (auto index = first; index <= last; ++index)
			{
				const long double local = static_cast<long double>(origin)
					+ MidiQuantisation::BoundarySampleAt(index, interval, divisions) - start + phase;
				const auto whole = static_cast<std::int64_t>(local);
				const auto wrapped = (whole % static_cast<std::int64_t>(length) + length) % length;
				if (wrapped > 0)
					grid.Boundaries.push_back(static_cast<std::uint32_t>(wrapped));
			}
			grid.Boundaries.push_back(length);
			std::sort(grid.Boundaries.begin(), grid.Boundaries.end());
			grid.Boundaries.erase(std::unique(grid.Boundaries.begin(), grid.Boundaries.end()), grid.Boundaries.end());
			// A repeating sample grid can be cut inside a cell. Do not link edges
			// for non-repeating lengths: their fragments are different grid cells.
			if (grid.Boundaries.size() > 2u && static_cast<std::uint64_t>(length) * divisions % interval == 0u)
			{
				const auto relative = start - origin - phase;
				const auto boundary = MidiQuantisation::NearestBoundaryIndex(relative, interval, divisions);
				if (MidiQuantisation::BoundarySampleAt(boundary, interval, divisions) != relative)
				{
					grid.SeamHeadEnd = grid.Boundaries[1u];
					grid.SeamTailStart = grid.Boundaries[grid.Boundaries.size() - 2u];
				}
			}
			return grid.Boundaries.size() <= MaxCells + 1u ? std::optional<LoopGridGeometry>(std::move(grid)) : std::nullopt;
		}

		std::size_t CellAt(std::uint32_t sample) const noexcept
		{
			if (Boundaries.size() < 2u)
				return 0u;
			const auto it = std::upper_bound(Boundaries.begin(), Boundaries.end() - 1,
				std::min(sample, Boundaries.back() - 1u));
			return static_cast<std::size_t>(it - Boundaries.begin() - 1);
		}

		static double SampleU(std::uint32_t sample, std::uint32_t length) noexcept
		{
			return length == 0u ? 0.0 : static_cast<double>(std::min(sample, length)) / length;
		}

		static std::uint32_t SampleAtU(double u, std::uint32_t length) noexcept
		{
			if (length == 0u || !std::isfinite(u))
				return 0u;
			return static_cast<std::uint32_t>((std::min)(
				std::floor(std::clamp(u, 0.0, 1.0) * length), static_cast<double>(length - 1u)));
		}

		// Bottom pitch and visible row count are independent of viewport pixels.
		static std::optional<std::uint8_t> PitchAtY(double y, double height,
			int bottomPitch, int visibleRows) noexcept
		{
			if (!std::isfinite(y) || !std::isfinite(height) || height <= 0 ||
				visibleRows <= 0 || bottomPitch < 0 || bottomPitch + visibleRows > 128 || y < 0 || y >= height)
				return std::nullopt;
			return static_cast<std::uint8_t>(bottomPitch + visibleRows - 1 -
				static_cast<int>(y * visibleRows / height));
		}

		static double PitchY(std::uint8_t pitch, double height, int bottomPitch,
			int visibleRows) noexcept
		{
			return (bottomPitch + visibleRows - pitch - 0.5) * height / visibleRows;
		}

		// Walk a sampled drag through every intervening time cell and pitch row.
		// The caller explicitly identifies a seam crossing: +1 travels forward
		// through cell zero, -1 backward through the final cell, 0 stays on plane.
		std::vector<Cell> CrossedCells(Cell from, Cell to, int seamTurns = 0) const
		{
			std::vector<Cell> cells;
			const auto count = static_cast<int>(Boundaries.size()) - 1;
			if (count <= 0 || from.Time >= static_cast<std::size_t>(count) ||
				to.Time >= static_cast<std::size_t>(count) || from.Pitch > 127 || to.Pitch > 127 ||
				seamTurns < -1 || seamTurns > 1 ||
				(seamTurns == 1 && from.Time <= to.Time) ||
				(seamTurns == -1 && from.Time >= to.Time))
				return cells;
			const int deltaTime = static_cast<int>(to.Time) - static_cast<int>(from.Time)
				+ seamTurns * count;
			const int deltaPitch = static_cast<int>(to.Pitch) - from.Pitch;
			const int steps = (std::max)(std::abs(deltaTime), std::abs(deltaPitch));
			cells.reserve(static_cast<std::size_t>(steps) + 1u);
			for (int step = 0; step <= steps; ++step)
			{
				const int time = static_cast<int>(from.Time) + (steps == 0 ? 0 :
					static_cast<int>(std::round(static_cast<double>(deltaTime) * step / steps)));
				const int pitch = static_cast<int>(from.Pitch) + (steps == 0 ? 0 :
					static_cast<int>(std::round(static_cast<double>(deltaPitch) * step / steps)));
				const Cell cell{ static_cast<std::size_t>((time % count + count) % count),
					static_cast<std::uint8_t>(pitch) };
				// Both coordinates advance monotonically before wrapping, so a
				// repeated sampled cell can only equal the previous one.
				if (cells.empty() || !(cells.back() == cell))
					cells.push_back(cell);
			}
			return cells;
		}

		enum class HitZone { LeftEdge, Body, RightEdge };
		static HitZone NoteHitZone(double pointerX, double leftX, double rightX,
			double minimumEdgePixels = 6.0) noexcept
		{
			const double width = rightX - leftX;
			if (width <= 0 || !std::isfinite(width))
				return HitZone::Body;
			const double edge = (std::max)(minimumEdgePixels, (std::min)(width / 3.0, width * 0.12));
			if (width <= 2.0 * edge)
				return pointerX - leftX < width / 2.0 ? HitZone::LeftEdge : HitZone::RightEdge;
			if (pointerX - leftX <= edge)
				return HitZone::LeftEdge;
			return rightX - pointerX <= edge ? HitZone::RightEdge : HitZone::Body;
		}

		static bool ValidNoteSpan(std::uint32_t start, std::uint32_t duration,
			std::uint32_t length, int pitch, int channel, int velocity) noexcept
		{
			return length > 0u && start < length && duration > 0u &&
				duration <= length - start && pitch >= 0 && pitch <= 127 &&
				channel >= 0 && channel < 16 && velocity > 0 && velocity <= 127;
		}
	};
}
