#include "NativeMidiSidecar.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace io;

bool NativeMidiSidecar::ToStream(const Stream& stream, std::ostream& out, std::string* error)
{
	if (!Validate(stream, error))
		return false;

	static constexpr char magic[] = "JAMMIDI1";
	const auto eventCount = static_cast<std::uint32_t>(stream.Events.size());
	const auto laneCount = static_cast<std::uint32_t>(stream.Lanes.size());
	if (!WriteBytes(out, magic, sizeof(magic) - 1u)
		|| !WriteBytes(out, &CurrentMajor, sizeof(CurrentMajor))
		|| !WriteBytes(out, &CurrentMinor, sizeof(CurrentMinor))
		|| !WriteBytes(out, &CurrentPatch, sizeof(CurrentPatch))
		|| !WriteBytes(out, &stream.LogicalLength, sizeof(stream.LogicalLength))
		|| !WriteBytes(out, &stream.AutomationGlobalSampleOrigin, sizeof(stream.AutomationGlobalSampleOrigin))
		|| !WriteBytes(out, &eventCount, sizeof(eventCount))
		|| !WriteBytes(out, &laneCount, sizeof(laneCount)))
	{
		SetError(error, "could not write MIDI sidecar header");
		return false;
	}

	for (const auto& event : stream.Events)
	{
		if (!WriteBytes(out, &event.SampleOffset, sizeof(event.SampleOffset))
			|| !WriteBytes(out, &event.Status, sizeof(event.Status))
			|| !WriteBytes(out, &event.Data1, sizeof(event.Data1))
			|| !WriteBytes(out, &event.Data2, sizeof(event.Data2)))
		{
			SetError(error, "could not write MIDI event");
			return false;
		}
	}

	for (const auto& lane : stream.Lanes)
	{
		const auto mapping = static_cast<std::uint8_t>(lane.Mapping);
		const auto scope = static_cast<std::uint8_t>(lane.TargetScope == "station" ? 0u : lane.TargetScope == "take" ? 1u : 2u);
		const auto pointCount = static_cast<std::uint32_t>(lane.Points.size());
		if (!WriteBytes(out, &mapping, sizeof(mapping)) || !WriteBytes(out, &lane.Channel, sizeof(lane.Channel))
			|| !WriteBytes(out, &lane.Controller, sizeof(lane.Controller)) || !WriteBytes(out, &scope, sizeof(scope))
			|| !WriteBytes(out, &lane.TargetPluginIndex, sizeof(lane.TargetPluginIndex))
			|| !WriteBytes(out, &lane.TargetLoopIndex, sizeof(lane.TargetLoopIndex))
			|| !WriteBytes(out, &lane.TargetParameterIndex, sizeof(lane.TargetParameterIndex))
			|| !WriteBytes(out, &pointCount, sizeof(pointCount)))
		{
			SetError(error, "could not write MIDI automation lane");
			return false;
		}
		for (const auto& point : lane.Points)
		{
			if (!WriteBytes(out, &point.Fraction, sizeof(point.Fraction)) || !WriteBytes(out, &point.Value, sizeof(point.Value)))
			{
				SetError(error, "could not write MIDI automation point");
				return false;
			}
		}
	}
	return true;
}

std::optional<NativeMidiSidecar::Stream> NativeMidiSidecar::FromStream(std::istream& in, std::string* error)
{
	std::size_t consumed = 0u;
	char magic[8]{};
	std::uint16_t major = 0u;
	std::uint16_t minor = 0u;
	std::uint16_t patch = 0u;
	Stream stream;
	std::uint32_t eventCount = 0u;
	std::uint32_t laneCount = 0u;
	if (!ReadBytes(in, magic, sizeof(magic), consumed) || std::memcmp(magic, "JAMMIDI1", sizeof(magic)) != 0
		|| !ReadBytes(in, &major, sizeof(major), consumed) || !ReadBytes(in, &minor, sizeof(minor), consumed)
		|| !ReadBytes(in, &patch, sizeof(patch), consumed) || !ReadBytes(in, &stream.LogicalLength, sizeof(stream.LogicalLength), consumed)
		|| !ReadBytes(in, &stream.AutomationGlobalSampleOrigin, sizeof(stream.AutomationGlobalSampleOrigin), consumed)
		|| !ReadBytes(in, &eventCount, sizeof(eventCount), consumed) || !ReadBytes(in, &laneCount, sizeof(laneCount), consumed))
	{
		SetError(error, "truncated or malformed MIDI sidecar header");
		return std::nullopt;
	}
	if (major > CurrentMajor || (major == CurrentMajor && (minor != CurrentMinor || patch != CurrentPatch)))
	{
		SetError(error, "unsupported MIDI sidecar version");
		return std::nullopt;
	}
	if (eventCount > MaxEvents || laneCount > MaxLanes)
	{
		SetError(error, "MIDI sidecar count exceeds limit");
		return std::nullopt;
	}
	stream.Events.resize(eventCount);
	for (auto& event : stream.Events)
	{
		if (!ReadBytes(in, &event.SampleOffset, sizeof(event.SampleOffset), consumed)
			|| !ReadBytes(in, &event.Status, sizeof(event.Status), consumed) || !ReadBytes(in, &event.Data1, sizeof(event.Data1), consumed)
			|| !ReadBytes(in, &event.Data2, sizeof(event.Data2), consumed))
		{
			SetError(error, "truncated MIDI event payload");
			return std::nullopt;
		}
	}
	stream.Lanes.resize(laneCount);
	for (auto& lane : stream.Lanes)
	{
		std::uint8_t mapping = 0u;
		std::uint8_t scope = 0u;
		std::uint32_t pointCount = 0u;
		if (!ReadBytes(in, &mapping, sizeof(mapping), consumed) || !ReadBytes(in, &lane.Channel, sizeof(lane.Channel), consumed)
			|| !ReadBytes(in, &lane.Controller, sizeof(lane.Controller), consumed) || !ReadBytes(in, &scope, sizeof(scope), consumed)
			|| !ReadBytes(in, &lane.TargetPluginIndex, sizeof(lane.TargetPluginIndex), consumed)
			|| !ReadBytes(in, &lane.TargetLoopIndex, sizeof(lane.TargetLoopIndex), consumed)
			|| !ReadBytes(in, &lane.TargetParameterIndex, sizeof(lane.TargetParameterIndex), consumed)
			|| !ReadBytes(in, &pointCount, sizeof(pointCount), consumed))
		{
			SetError(error, "truncated MIDI automation lane");
			return std::nullopt;
		}
		if (mapping > static_cast<std::uint8_t>(JamFile::AutomationLane::MappingType::Editor) || scope > 2u || pointCount > MaxPointsPerLane)
		{
			SetError(error, "invalid MIDI automation lane");
			return std::nullopt;
		}
		lane.Mapping = static_cast<JamFile::AutomationLane::MappingType>(mapping);
		lane.TargetScope = scope == 0u ? "station" : scope == 1u ? "take" : "loop";
		lane.Points.resize(pointCount);
		for (auto& point : lane.Points)
		{
			if (!ReadBytes(in, &point.Fraction, sizeof(point.Fraction), consumed) || !ReadBytes(in, &point.Value, sizeof(point.Value), consumed))
			{
				SetError(error, "truncated MIDI automation point");
				return std::nullopt;
			}
		}
	}
	if (!Validate(stream, error))
		return std::nullopt;
	// The asset format has no padding or extension area.  Reject any tail so a
	// malformed/oversized sidecar is never accepted as a valid prefix.
	if (in.peek() != std::char_traits<char>::eof())
	{
		SetError(error, "trailing MIDI sidecar data");
		return std::nullopt;
	}
	return stream;
}

bool NativeMidiSidecar::Validate(const Stream& stream, std::string* error) noexcept
{
	if (stream.LogicalLength == 0u || stream.Events.size() > MaxEvents || stream.Lanes.size() > MaxLanes)
	{
		SetError(error, "invalid MIDI stream dimensions");
		return false;
	}
	for (const auto& event : stream.Events)
	{
		const auto statusType = event.Status & 0xf0u;
		if (event.SampleOffset >= stream.LogicalLength || statusType < 0x80u || statusType > 0xe0u || event.Data1 > 127u || event.Data2 > 127u)
		{
			SetError(error, "invalid MIDI event");
			return false;
		}
	}
	for (const auto& lane : stream.Lanes)
	{
		if (lane.Points.size() > MaxPointsPerLane || (lane.Mapping == JamFile::AutomationLane::MappingType::Cc && (lane.Channel > 15u || lane.Controller > 127u))
			|| (lane.TargetScope != "station" && lane.TargetScope != "take" && lane.TargetScope != "loop"))
		{
			SetError(error, "invalid MIDI automation lane");
			return false;
		}
		for (const auto& point : lane.Points)
		{
			if (!std::isfinite(point.Fraction) || !std::isfinite(point.Value) || point.Fraction < 0.0 || point.Fraction > 1.0)
			{
				SetError(error, "invalid MIDI automation point");
				return false;
			}
		}
	}
	return true;
}

bool NativeMidiSidecar::WriteBytes(std::ostream& out, const void* data, std::size_t size) noexcept
{
	out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
	return static_cast<bool>(out);
}

bool NativeMidiSidecar::ReadBytes(std::istream& in, void* data, std::size_t size, std::size_t& consumed) noexcept
{
	if (size > MaxAssetBytes - consumed)
		return false;
	in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
	if (in.gcount() != static_cast<std::streamsize>(size))
		return false;
	consumed += size;
	return true;
}

void NativeMidiSidecar::SetError(std::string* error, const char* text) noexcept
{
	if (error)
		*error = text;
}
