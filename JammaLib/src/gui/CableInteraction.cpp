#include "CableInteraction.h"

#include <algorithm>
#include <cmath>

using namespace gui;

std::vector<int> CableInteraction::Spread(int first, int last, size_t count)
{
	std::vector<int> values;
	values.reserve(count);
	if (count == 0u)
		return values;
	if (count == 1u)
	{
		values.push_back(first + (last - first) / 2);
		return values;
	}
	for (size_t i = 0u; i < count; ++i)
		values.push_back(first + static_cast<int>((static_cast<long long>(last - first) * i) / (count - 1u)));
	return values;
}

float CableInteraction::_DistanceSquared(utils::Position2d lhs, utils::Position2d rhs)
{
	const auto dx = static_cast<float>(lhs.X - rhs.X);
	const auto dy = static_cast<float>(lhs.Y - rhs.Y);
	return dx * dx + dy * dy;
}

bool CableInteraction::HitTest(const Endpoint& endpoint, utils::Position2d point, float radius)
{
	return _DistanceSquared(endpoint.Position, point) <= radius * radius;
}

std::optional<size_t> CableInteraction::HitEndpoint(const std::vector<Endpoint>& endpoints,
	utils::Position2d point,
	float radius)
{
	std::optional<size_t> nearest;
	float nearestDistance = radius * radius;
	for (size_t i = 0u; i < endpoints.size(); ++i)
	{
		const auto distance = _DistanceSquared(endpoints[i].Position, point);
		if (distance <= nearestDistance)
		{
			nearest = i;
			nearestDistance = distance;
		}
	}
	return nearest;
}

CableInteraction::End CableInteraction::ClosestEnd(const Cable& cable, utils::Position2d point)
{
	return _DistanceSquared(cable.Start.Position, point) <= _DistanceSquared(cable.Finish.Position, point)
		? End::Start : End::Finish;
}

std::optional<size_t> CableInteraction::HitCable(const std::vector<Cable>& cables,
	utils::Position2d point,
	float radius)
{
	std::optional<size_t> nearest;
	float nearestDistance = radius * radius;
	for (size_t i = 0u; i < cables.size(); ++i)
	{
		const auto& a = cables[i].Start.Position;
		const auto& b = cables[i].Finish.Position;
		const auto dx = static_cast<float>(b.X - a.X);
		const auto dy = static_cast<float>(b.Y - a.Y);
		const auto lengthSquared = dx * dx + dy * dy;
		const auto projection = lengthSquared > 0.0f
			? std::clamp(((point.X - a.X) * dx + (point.Y - a.Y) * dy) / lengthSquared, 0.0f, 1.0f)
			: 0.0f;
		const utils::Position2d closest{
			static_cast<int>(std::lround(a.X + projection * dx)),
			static_cast<int>(std::lround(a.Y + projection * dy)) };
		const auto distance = _DistanceSquared(closest, point);
		if (distance <= nearestDistance)
		{
			nearest = i;
			nearestDistance = distance;
		}
	}
	return nearest;
}

std::optional<std::pair<size_t, CableInteraction::End>> CableInteraction::HitCableEnd(
	const std::vector<Cable>& cables, utils::Position2d point, float radius)
{
	std::optional<std::pair<size_t, End>> nearest;
	float nearestDistance = radius * radius;
	for (size_t i = 0u; i < cables.size(); ++i)
	{
		for (const auto end : { End::Start, End::Finish })
		{
			const auto distance = _DistanceSquared(
				end == End::Start ? cables[i].Start.Position : cables[i].Finish.Position, point);
			if (distance < nearestDistance)
			{
				nearest = std::make_pair(i, end);
				nearestDistance = distance;
			}
		}
	}
	return nearest;
}

bool CableInteraction::Related(const Cable& cable, const Endpoint& endpoint)
{
	switch (endpoint.Kind)
	{
	case EndpointKind::AdcSource:
	case EndpointKind::MidiSource:
		return cable.Start.Source.has_value() && endpoint.Source.has_value() &&
			_SameSource(cable.Start.Source.value(), endpoint.Source.value());
	case EndpointKind::TriggerInput:
		return cable.Route.Kind == RouteKind::Capture &&
			cable.Finish.TriggerIndex == endpoint.TriggerIndex;
	case EndpointKind::TriggerOutput:
		return cable.Route.Kind == RouteKind::Station &&
			cable.Start.TriggerIndex == endpoint.TriggerIndex;
	case EndpointKind::Station:
		return cable.Route.Kind == RouteKind::Station &&
			cable.Finish.StationIndex == endpoint.StationIndex &&
			(!endpoint.TriggerIndex.has_value() || cable.Route.TriggerIndex == endpoint.TriggerIndex.value());
	}
	return false;
}

bool CableInteraction::_SameSource(const io::RigFileRouting::Source& lhs,
	const io::RigFileRouting::Source& rhs)
{
	return lhs.Kind == rhs.Kind && lhs.AdcChannel == rhs.AdcChannel && lhs.MidiDevice == rhs.MidiDevice;
}

bool CableInteraction::Compatible(const Drag& drag, const Endpoint& candidate, const io::RigFile& rig)
{
	if (!candidate.Available || !drag.Fixed.Available ||
		(candidate.Source.has_value() && !candidate.Source->Available) ||
		(drag.Fixed.Source.has_value() && !drag.Fixed.Source->Available))
		return false;
	if (drag.Route.Kind == RouteKind::Capture)
	{
		const auto triggerIndex = drag.MovingEnd == End::Finish && candidate.TriggerIndex.has_value()
			? candidate.TriggerIndex.value() : drag.Route.TriggerIndex;
		if (triggerIndex >= rig.Triggers.size())
			return false;
		if (drag.MovingEnd == End::Start)
		{
			if ((candidate.Kind != EndpointKind::AdcSource && candidate.Kind != EndpointKind::MidiSource) ||
				!candidate.Source.has_value())
				return false;
			const auto& trigger = rig.Triggers[triggerIndex];
			const auto& source = candidate.Source.value();
			if (source.Kind == io::RigFileRouting::SourceKind::Adc)
				return std::find(trigger.InputChannels.begin(), trigger.InputChannels.end(), source.AdcChannel) == trigger.InputChannels.end() ||
					(drag.OriginalSource.has_value() && _SameSource(drag.OriginalSource.value(), source));
			return std::find(trigger.MidiInputDevices.begin(), trigger.MidiInputDevices.end(), source.MidiDevice) == trigger.MidiInputDevices.end() ||
				(drag.OriginalSource.has_value() && _SameSource(drag.OriginalSource.value(), source));
		}
		if (candidate.Kind != EndpointKind::TriggerInput || !candidate.TriggerIndex.has_value())
			return false;
		const auto& trigger = rig.Triggers[triggerIndex];
		if (!drag.Fixed.Source.has_value())
			return false;
		const auto& source = drag.Fixed.Source.value();
		if (source.Kind == io::RigFileRouting::SourceKind::Adc)
			return std::find(trigger.InputChannels.begin(), trigger.InputChannels.end(), source.AdcChannel) == trigger.InputChannels.end();
		if (source.MidiDevice == "*")
			return trigger.MidiInputs != io::RigFile::Trigger::MidiInputMode::Any;
		return std::find(trigger.MidiInputDevices.begin(), trigger.MidiInputDevices.end(), source.MidiDevice) == trigger.MidiInputDevices.end();
	}
	if (drag.MovingEnd == End::Start)
		return candidate.Kind == EndpointKind::TriggerOutput && candidate.TriggerIndex == drag.Route.TriggerIndex;
	return candidate.Kind == EndpointKind::Station && candidate.StationIndex.has_value();
}

std::optional<size_t> CableInteraction::NearestViable(const Drag& drag,
	const std::vector<Endpoint>& endpoints,
	const io::RigFile& rig,
	float radius)
{
	std::optional<size_t> nearest;
	float nearestDistance = radius * radius;
	for (size_t i = 0u; i < endpoints.size(); ++i)
	{
		if (!Compatible(drag, endpoints[i], rig))
			continue;
		const auto distance = _DistanceSquared(endpoints[i].Position, drag.Pointer);
		if (distance <= nearestDistance)
		{
			nearest = i;
			nearestDistance = distance;
		}
	}
	return nearest;
}

void CableInteraction::Update(Drag& drag,
	utils::Position2d pointer,
	const std::vector<Endpoint>& endpoints,
	const io::RigFile& rig,
	float radius,
	float hysteresis)
{
	drag.Pointer = pointer;
	if (drag.Snap.has_value() && Compatible(drag, drag.Snap.value(), rig) &&
		HitTest(drag.Snap.value(), pointer, radius + hysteresis))
		return;
	const auto nearest = NearestViable(drag, endpoints, rig, radius);
	drag.Snap = nearest.has_value() ? std::optional<Endpoint>(endpoints[nearest.value()]) : std::nullopt;
}

std::pair<utils::Position2d, utils::Position2d> CableInteraction::Preview(const Drag& drag)
{
	const auto moving = drag.Snap.has_value() ? drag.Snap->Position : drag.Pointer;
	return drag.MovingEnd == End::Start
		? std::make_pair(moving, drag.Fixed.Position)
		: std::make_pair(drag.Fixed.Position, moving);
}

void CableInteraction::Cancel(std::optional<Drag>& drag)
{
	drag.reset();
}

CableInteraction::Release CableInteraction::ReleaseToCandidate(const Drag& drag, const io::RigFile& rig)
{
	if (drag.Route.Kind == RouteKind::Station)
	{
		if (drag.Route.TriggerIndex >= rig.Triggers.size())
			return {};
		if (!drag.Snap.has_value())
			return { io::RigFileRouting::WithStationTarget(rig, drag.Route.TriggerIndex, ""), true };
		if (drag.Snap->Kind == EndpointKind::TriggerOutput)
			return { rig, false };
		if (!drag.Snap->StationIndex.has_value() || drag.Snap->StationName.empty())
			return {};
		const auto& current = rig.Triggers[drag.Route.TriggerIndex].StationTarget;
		if (current.has_value() && current.value() == drag.Snap->StationName)
			return { rig, false };
		return { io::RigFileRouting::WithStationTarget(rig, drag.Route.TriggerIndex, drag.Snap->StationName), true };
	}

	if (drag.MovingEnd == End::Finish)
	{
		if (!drag.Fixed.Available || !drag.Fixed.Source.has_value() || !drag.Fixed.Source->Available)
			return {};
		const auto& source = drag.Fixed.Source.value();
		std::optional<io::RigFile> candidate;
		if (drag.OriginalSource.has_value())
		{
			if (drag.Route.TriggerIndex >= rig.Triggers.size())
				return {};
			if (source.Kind == io::RigFileRouting::SourceKind::Adc)
				candidate = io::RigFileRouting::WithoutAdcInput(rig, drag.Route.TriggerIndex, source.AdcChannel);
			else if (source.MidiDevice == "*")
			{
				candidate = rig;
				auto& trigger = candidate->Triggers[drag.Route.TriggerIndex];
				trigger.MidiInputs = io::RigFile::Trigger::MidiInputMode::None;
				trigger.MidiInputDevices.clear();
			}
			else
				candidate = io::RigFileRouting::WithoutMidiInput(rig, drag.Route.TriggerIndex, source.MidiDevice);
			if (!candidate.has_value())
				return {};
		}
		if (!drag.Snap.has_value())
			return candidate.has_value() ? Release{ std::move(candidate), true } : Release{};
		if (!drag.Snap->Available || drag.Snap->Kind != EndpointKind::TriggerInput)
			return {};
		const auto triggerIndex = drag.Snap->TriggerIndex.value_or(drag.Route.TriggerIndex);
		const auto& base = candidate.has_value() ? candidate.value() : rig;
		if (triggerIndex >= base.Triggers.size())
			return {};
		if (source.Kind == io::RigFileRouting::SourceKind::Midi && source.MidiDevice == "*")
		{
			auto replacement = base;
			auto& trigger = replacement.Triggers[triggerIndex];
			trigger.MidiInputs = io::RigFile::Trigger::MidiInputMode::Any;
			trigger.MidiInputDevices.clear();
			return { std::move(replacement), true };
		}
		if (source.Kind == io::RigFileRouting::SourceKind::Adc)
			return { io::RigFileRouting::WithAdcInput(base, triggerIndex, source.AdcChannel), true };
		return { io::RigFileRouting::WithMidiInput(base, triggerIndex, source.MidiDevice), true };
	}
	const auto original = drag.OriginalSource;
	if (drag.Route.TriggerIndex >= rig.Triggers.size())
		return {};
	if (!original.has_value() && drag.Snap.has_value() && drag.Snap->Source.has_value())
	{
		const auto& source = drag.Snap->Source.value();
		if (source.Kind == io::RigFileRouting::SourceKind::Adc)
			return { io::RigFileRouting::WithAdcInput(rig, drag.Route.TriggerIndex, source.AdcChannel), true };
		if (source.MidiDevice == "*")
		{
			auto candidate = rig;
			auto& trigger = candidate.Triggers[drag.Route.TriggerIndex];
			trigger.MidiInputs = io::RigFile::Trigger::MidiInputMode::Any;
			trigger.MidiInputDevices.clear();
			return { std::move(candidate), true };
		}
		return { io::RigFileRouting::WithMidiInput(rig, drag.Route.TriggerIndex, source.MidiDevice), true };
	}
	if (!original.has_value())
		return {};
	std::optional<io::RigFile> removed;
	if (original->Kind == io::RigFileRouting::SourceKind::Adc)
		removed = io::RigFileRouting::WithoutAdcInput(rig, drag.Route.TriggerIndex, original->AdcChannel);
	else if (original->MidiDevice == "*")
	{
		removed = rig;
		auto& trigger = removed->Triggers[drag.Route.TriggerIndex];
		trigger.MidiInputs = io::RigFile::Trigger::MidiInputMode::None;
		trigger.MidiInputDevices.clear();
	}
	else
		removed = io::RigFileRouting::WithoutMidiInput(rig, drag.Route.TriggerIndex, original->MidiDevice);
	if (!removed.has_value())
		return {};
	if (!drag.Snap.has_value())
		return { std::move(removed), true };
	const auto& replacement = drag.Snap->Source;
	if (!replacement.has_value() || _SameSource(original.value(), replacement.value()))
		return { rig, false };
	if (replacement->Kind == io::RigFileRouting::SourceKind::Adc)
		return { io::RigFileRouting::WithAdcInput(removed.value(), drag.Route.TriggerIndex, replacement->AdcChannel), true };
	if (replacement->MidiDevice == "*")
	{
		auto candidate = removed.value();
		auto& trigger = candidate.Triggers[drag.Route.TriggerIndex];
		trigger.MidiInputs = io::RigFile::Trigger::MidiInputMode::Any;
		trigger.MidiInputDevices.clear();
		return { std::move(candidate), true };
	}
	return { io::RigFileRouting::WithMidiInput(removed.value(), drag.Route.TriggerIndex, replacement->MidiDevice), true };
}
