#include "CableInteraction.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

using namespace gui;

CableInteraction::Curve CableInteraction::CurveControls(RouteKind kind,
	utils::Position2d start, utils::Position2d finish)
{
	const glm::vec2 a{ start.X, start.Y };
	const glm::vec2 b{ finish.X, finish.Y };
	if (kind == RouteKind::Station)
	{
		const float pull = std::max(50.0f, std::abs(a.x - b.x) * 0.45f);
		const float drop = std::max(50.0f, std::abs(a.y - b.y) * 0.45f);
		return { a, glm::vec2{ a.x - pull, a.y },
			glm::vec2{ b.x, b.y + (a.y < b.y ? -drop : drop) }, b };
	}
	return { a, glm::vec2{ a.x, a.y + std::max(50.0f, (b.y - a.y) * 0.55f) },
		glm::vec2{ b.x - std::max(50.0f, (b.x - a.x) * 0.40f), b.y }, b };
}

glm::vec2 CableInteraction::EvaluateCurve(const Curve& curve, float t)
{
	const float u = 1.0f - t;
	return u * u * u * curve[0] + 3.0f * u * u * t * curve[1] +
		3.0f * u * t * t * curve[2] + t * t * t * curve[3];
}

std::optional<glm::dvec2> CableInteraction::ProjectAnchor(glm::vec4 clip, utils::Size2d window)
{
	if (window.Width == 0u || window.Height == 0u ||
		!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.z) || !std::isfinite(clip.w) ||
		clip.w <= 1e-6f || clip.z < -clip.w || clip.z > clip.w)
		return std::nullopt;
	return glm::dvec2{
		(static_cast<double>(clip.x) / clip.w + 1.0) * 0.5 * window.Width,
		(static_cast<double>(clip.y) / clip.w + 1.0) * 0.5 * window.Height };
}

std::optional<CableInteraction::BoundaryPoint> CableInteraction::ResolveBoundary(
	glm::dvec2 point, utils::Rect2d bounds)
{
	if (bounds.IsEmpty() || !std::isfinite(point.x) || !std::isfinite(point.y))
		return std::nullopt;
	const bool clipped = point.x < bounds.Left || point.x >= bounds.Right ||
		point.y < bounds.Bottom || point.y >= bounds.Top;
	// Clamp before conversion: distant projected anchors cannot overflow pixels.
	return BoundaryPoint{ {
		static_cast<int>(std::clamp(point.x, static_cast<double>(bounds.Left), static_cast<double>(bounds.Right - 1))),
		static_cast<int>(std::clamp(point.y, static_cast<double>(bounds.Bottom), static_cast<double>(bounds.Top - 1))) }, clipped };
}

glm::dvec2 CableInteraction::FannedPoint(glm::dvec2 point, double offset, bool horizontal, utils::Rect2d bounds)
{
	// Turn a fan toward the boundary tangent over its own radius as a socket
	// leaves view. This preserves separation at distant edges and continuity
	// at entry, without animation that could detach a visible cable/socket.
	const double clearance = horizontal
		? std::min(point.x - bounds.Left, (bounds.Right - 1) - point.x)
		: std::min(point.y - bounds.Bottom, (bounds.Top - 1) - point.y);
	const double outside = std::max(0.0, -clearance);
	const double turn = std::clamp(outside / 7.0, 0.0, 1.0);
	// A visible socket near the edge contracts its fan symmetrically; a fan
	// offset alone must never turn that visible socket into a continuation.
	const double spread = std::clamp(clearance / 7.0, 0.0, 1.0);
	return point + (horizontal ? glm::dvec2{ offset * spread, offset * turn }
		: glm::dvec2{ offset * turn, offset * spread });
}

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
	return !endpoint.Continuation && (!endpoint.HitBounds || endpoint.HitBounds->Contains(point)) &&
		_DistanceSquared(endpoint.Position, point) <= radius * radius;
}

std::optional<size_t> CableInteraction::HitEndpoint(const std::vector<Endpoint>& endpoints,
	utils::Position2d point,
	float radius)
{
	std::optional<size_t> nearest;
	float nearestDistance = radius * radius;
	for (size_t i = 0u; i < endpoints.size(); ++i)
	{
		if (!HitTest(endpoints[i], point, radius))
			continue;
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
		const auto curve = CurveControls(cables[i].Route.Kind, cables[i].Start.Position, cables[i].Finish.Position);
		const glm::vec2 pointer{ point.X, point.Y };
		float distance = radius * radius + 1.0f;
		auto a = curve[0];
		// Match the shader's sampled line strip, including its vertex count.
		for (int vertex = 1; vertex < CurveVertexCount; ++vertex)
		{
			const auto b = EvaluateCurve(curve, static_cast<float>(vertex) / (CurveVertexCount - 1));
			const auto delta = b - a;
			const float lengthSquared = glm::dot(delta, delta);
			const float fraction = lengthSquared > 0.0f
				? std::clamp(glm::dot(pointer - a, delta) / lengthSquared, 0.0f, 1.0f) : 0.0f;
			const auto offset = pointer - (a + fraction * delta);
			distance = std::min(distance, glm::dot(offset, offset));
			a = b;
		}
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
			const auto& endpoint = end == End::Start ? cables[i].Start : cables[i].Finish;
			if (!HitTest(endpoint, point, radius))
				continue;
			const auto distance = _DistanceSquared(endpoint.Position, point);
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
	if (candidate.Continuation || !candidate.Available || !drag.Fixed.Available ||
		(candidate.Source.has_value() && !candidate.Source->Available) ||
		(drag.Fixed.Source.has_value() && !drag.Fixed.Source->Available))
		return false;
	if ((candidate.Source.has_value() && candidate.Source->Kind == io::RigFileRouting::SourceKind::Midi &&
		candidate.Source->MidiDevice == "*") ||
		(drag.Fixed.Source.has_value() && drag.Fixed.Source->Kind == io::RigFileRouting::SourceKind::Midi &&
			drag.Fixed.Source->MidiDevice == "*"))
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
		if (!Compatible(drag, endpoints[i], rig) || !HitTest(endpoints[i], drag.Pointer, radius))
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
	if (drag.Snap)
	{
		const auto current = std::find_if(endpoints.begin(), endpoints.end(), [&drag](const auto& endpoint)
		{
			const auto& saved = *drag.Snap;
			return endpoint.Kind == saved.Kind && endpoint.TriggerIndex == saved.TriggerIndex &&
				endpoint.StationIndex == saved.StationIndex && endpoint.StationName == saved.StationName &&
				endpoint.Source.has_value() == saved.Source.has_value() &&
				(!endpoint.Source || _SameSource(*endpoint.Source, *saved.Source));
		});
		// Scrolling/resizing may move or hide the socket during a captured drag.
		// Hysteresis applies to the current real socket, never the saved geometry.
		if (current != endpoints.end() && Compatible(drag, *current, rig) &&
			HitTest(*current, pointer, radius + hysteresis))
		{
			drag.Snap = *current;
			return;
		}
	}
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
		return { io::RigFileRouting::WithMidiInput(rig, drag.Route.TriggerIndex, source.MidiDevice), true };
	}
	if (!original.has_value())
		return {};
	std::optional<io::RigFile> removed;
	if (original->Kind == io::RigFileRouting::SourceKind::Adc)
		removed = io::RigFileRouting::WithoutAdcInput(rig, drag.Route.TriggerIndex, original->AdcChannel);
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
	return { io::RigFileRouting::WithMidiInput(removed.value(), drag.Route.TriggerIndex, replacement->MidiDevice), true };
}

// Reveal intent, rather than fade-out alpha, defines which routes accept input.
bool CableInteraction::Revealed(const Cable& cable, bool revealHeld, bool dragging,
	const std::optional<Endpoint>& hoveredSocket)
{
	return revealHeld || dragging || (hoveredSocket && Related(cable, *hoveredSocket));
}

bool CableInteraction::CanGrabEnd(const Cable& cable, End end, bool loopEditor, bool revealHeld)
{
	return !loopEditor || revealHeld || cable.Route.Kind != RouteKind::Station || end == End::Start;
}
