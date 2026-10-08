#pragma once

#include <cstdint>
#include <array>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <string>
#include <vector>
#include "../io/RigFile.h"
#include "../utils/CommonTypes.h"

namespace gui
{
	struct CableInteraction
	{
		enum class EndpointKind { AdcSource, MidiSource, TriggerInput, TriggerOutput, Station };
		enum class RouteKind { Capture, Station };
		enum class End { Start, Finish };

		struct Handle
		{
			std::uint64_t Revision = 0u;
			size_t TriggerIndex = 0u;
			RouteKind Kind = RouteKind::Capture;
			size_t RouteIndex = 0u;
		};

		struct Endpoint
		{
			EndpointKind Kind = EndpointKind::AdcSource;
			utils::Position2d Position{};
			std::optional<size_t> TriggerIndex;
			std::optional<size_t> StationIndex;
			std::string StationName;
			std::optional<io::RigFileRouting::Source> Source;
			bool Available = true;
			// Real socket hit/snap bounds, in the same space as Position.
			std::optional<utils::Rect2d> HitBounds;
			bool Continuation = false;
			std::optional<glm::dvec2> ActualPosition;
		};

		struct Cable
		{
			Handle Route;
			Endpoint Start;
			Endpoint Finish;
		};

		struct Drag
		{
			Handle Route;
			End MovingEnd = End::Start;
			Endpoint Fixed;
			std::optional<io::RigFileRouting::Source> OriginalSource;
			utils::Position2d Pointer{};
			std::optional<Endpoint> Snap;
			bool FromCable = false;
		};

		struct Release
		{
			std::optional<io::RigFile> Candidate;
			bool Changed = false;
		};

		static std::vector<int> Spread(int first, int last, size_t count);
		static constexpr int CurveVertexCount = 24;
		using Curve = std::array<glm::vec2, 4>;
		static Curve CurveControls(RouteKind kind, utils::Position2d start, utils::Position2d finish);
		static glm::vec2 EvaluateCurve(const Curve& curve, float t);
		struct BoundaryPoint
		{
			utils::Position2d Position;
			bool Clipped = false;
		};
		// GL clip convention: positive W and depth inside [-W, W]. X/Y may
		// be offscreen; keep them floating point until bounded for presentation.
		static std::optional<glm::dvec2> ProjectAnchor(glm::vec4 clip, utils::Size2d window);
		static std::optional<BoundaryPoint> ResolveBoundary(glm::dvec2 point, utils::Rect2d bounds);
		static glm::dvec2 FannedPoint(glm::dvec2 point, double offset, bool horizontal, utils::Rect2d bounds);
		static bool HitTest(const Endpoint& endpoint, utils::Position2d point, float radius);
		static std::optional<size_t> HitEndpoint(const std::vector<Endpoint>& endpoints,
			utils::Position2d point,
			float radius);
		static End ClosestEnd(const Cable& cable, utils::Position2d point);
		static std::optional<size_t> HitCable(const std::vector<Cable>& cables,
			utils::Position2d point,
			float radius);
		static std::optional<std::pair<size_t, End>> HitCableEnd(const std::vector<Cable>& cables,
			utils::Position2d point,
			float radius);
		static bool Related(const Cable& cable, const Endpoint& endpoint);
		static bool Revealed(const Cable& cable, bool revealHeld, bool dragging,
			const std::optional<Endpoint>& hoveredSocket);
		static bool CanGrabEnd(const Cable& cable, End end, bool loopEditor, bool revealHeld);
		static bool Compatible(const Drag& drag, const Endpoint& candidate, const io::RigFile& rig);
		static std::optional<size_t> NearestViable(const Drag& drag,
			const std::vector<Endpoint>& endpoints,
			const io::RigFile& rig,
			float radius);
		static void Update(Drag& drag,
			utils::Position2d pointer,
			const std::vector<Endpoint>& endpoints,
			const io::RigFile& rig,
			float radius,
			float hysteresis);
		static std::pair<utils::Position2d, utils::Position2d> Preview(const Drag& drag);
		static void Cancel(std::optional<Drag>& drag);
		static Release ReleaseToCandidate(const Drag& drag, const io::RigFile& rig);

	private:
		static float _DistanceSquared(utils::Position2d lhs, utils::Position2d rhs);
		static bool _SameSource(const io::RigFileRouting::Source& lhs,
			const io::RigFileRouting::Source& rhs);
	};
}
