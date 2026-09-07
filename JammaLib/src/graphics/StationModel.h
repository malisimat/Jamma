#pragma once

#include <cstdint>
#include <glm/vec3.hpp>
#include <span>
#include <tuple>
#include <vector>
#include "../gui/GuiModel.h"

namespace graphics
{
	struct RingProfilePoint
	{
		float Radius;
		float Y;
	};

	// Procedural "halo deck" geometry for a Station.
	// Static mesh built once at construction; no per-frame work.
	// UV layout: x = radialFrac (0..1), y = partKind (0=top,1=bevel,2=side,3=rib).
	//
	// Shader selection:
	//   ModelShaders[0] = "station"   (scene pass and highlight pass)
	//   ModelShaders[1] = "picker"    (picker pass)
	//
	// Draw3d checks the pass and routes to the correct shader, then packs
	// GlobalId() as ObjectId so picker clicks select the owning station.
	class StationModel :
		public gui::GuiModel
	{
	public:
		StationModel();

		// Copy
		StationModel(const StationModel&) = delete;
		StationModel& operator=(const StationModel&) = delete;

		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;
		void SetStationState(const std::vector<unsigned int>& stationGlobalId,
			bool selected,
			bool picking,
			float level = 0.0f,
			std::uint8_t visualState = 0u);
		void SetParams(float fallRate) noexcept;
		void ResetStationLevel() noexcept;

		// --- Pure geometry builders (no OpenGL; testable without a GL context) ---

		// Build the deck top (flat polygon fan, normal +Y).
		// Returns interleaved {verts, uvs} where verts are xyz triples and uvs are xy pairs.
		// numSides: polygon approximation (8..64).
		// radius: outer radius of the deck.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildDeckTop(unsigned int numSides, float radius);

		// Build the beveled outer ring connecting the deck top to the deck side.
		// bevelWidth: radial width of the bevel strip.
		// bevelHeight: vertical drop of the bevel.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildBevel(unsigned int numSides, float radius, float bevelWidth, float bevelHeight);

		// Build the vertical cylindrical side.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildSide(unsigned int numSides, float radius, float sideHeight);

		// Build the lower bevel ring to close the capsule silhouette.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildBottomBevel(unsigned int numSides, float radius,
				float bevelWidth, float bevelHeight, float sideHeight);

		// Build the bottom cap (flat polygon fan, normal -Y).
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildDeckBottom(unsigned int numSides, float radius,
				float bevelHeight, float sideHeight);

		// Build raised radial ribs on the deck top surface.
		// numRibs: number of ribs (evenly spaced).
		// ribInnerRadius / ribOuterRadius: extent of each rib.
		// ribHeight: how far the rib protrudes above deckY.
		// ribHalfWidth: angular half-width (radians) of each rib face.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildRibs(unsigned int numSides, float radius,
				unsigned int numRibs,
				float ribInnerRadius, float ribOuterRadius,
				float ribHeight, float ribHalfWidth);

		// Revolve a profile around the station axis. yOffset anchors the profile
		// at a cap; invertY mirrors its local Y direction for the lower collar.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildLathedProfileGeometry(unsigned int numSides,
				std::span<const RingProfilePoint> profile,
				float yOffset, bool invertY, float partKind,
				float profileScale = 1.0f);

		// Build the upper and lower closed-prism templates used by every
		// instanced state-ring occluder segment.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildOccluderPrismGeometry(float innerRadius, float outerRadius,
				float partKind);

		// Convenience: build all geometry and concatenate into one pair.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildAllGeometry(unsigned int numSides, float radius,
				unsigned int numRibs);

		// Number of triangles in the last built geometry (0 before init).
		unsigned int NumTris() const noexcept { return _numTris; }

	protected:
		virtual std::weak_ptr<resources::ShaderResource> GetShader() override;

	private:
		static constexpr unsigned int DefaultNumSides = 32u;
		static constexpr unsigned int DefaultNumRibs = 0u;
		static constexpr unsigned int SideVerticalSections = 12u;
		static constexpr float DeckRadius = 30.0f;
		static constexpr float BevelWidth = 2.0f;
		static constexpr float BevelHeight = 10.0f;
		static constexpr float SideHeight = 450.0f;
		// Part-kind UVs (y channel).
		static constexpr float UV_TOP = 0.0f;
		static constexpr float UV_BEVEL = 1.0f;
		static constexpr float UV_SIDE = 2.0f;
		static constexpr float UV_RIB = 3.0f;
		static constexpr float UV_STATE_RING_BRIGHT = 4.0f;
		static constexpr float UV_STATE_RING_DARK = 5.0f;
		static constexpr unsigned int StateRingSides = 64u;
		static constexpr unsigned int StateRingOccluderInstances = 20u;
		static constexpr float StateRingScale = 5.0f;
		static constexpr float StateRingOccluderInnerRadius = 9.0f;
		static constexpr float StateRingOccluderOuterRadius = 17.0f;
		static constexpr float StateRingTopY = -2.0f;
		static constexpr float StateRingBottomY = -(2.0f * BevelHeight + SideHeight) + 2.0f;
		static constexpr RingProfilePoint StateRingProfile[] = {
			{ 9.30f, 0.00f }, { 10.80f, -1.25f }, { 15.80f, -3.50f },
			{ 15.80f, -9.50f }, { 14.90f, -13.50f }, { 11.20f, -16.00f }
		};
		static constexpr RingProfilePoint StateRingBottomProfile[] = {
			{ 9.30f, 0.00f }, { 11.60f, 1.10f }, { 16.60f, 3.20f },
			{ 16.60f, 8.20f }, { 15.30f, 12.40f }, { 12.00f, 17.50f },
			{ 10.20f, 19.00f }
		};
		static void PushTri(std::vector<float>& verts,
			std::vector<float>& uvs,
			const glm::vec3& a, float ua, float va,
			const glm::vec3& b, float ub, float vb,
			const glm::vec3& c, float uc, float vc);
		static void PushQuad(std::vector<float>& verts,
			std::vector<float>& uvs,
			const glm::vec3& a, float ua, float va,
			const glm::vec3& b, float ub, float vb,
			const glm::vec3& c, float uc, float vc,
			const glm::vec3& d, float ud, float vd);
		// Index into _modelShaders for each draw pass.
		// 0 = station (scene/highlight), 1 = picker.
		base::DrawPass _lastPass;
		std::vector<unsigned int> _stationGlobalId;
		bool _stationSelected;
		bool _stationPicking;
		float _stationLevel;
		std::uint8_t _stationVisualState;
		float _stationFallRate;
		struct RingMesh
		{
			std::vector<float> Verts;
			std::vector<float> Uvs;
			GLuint VertexArray = 0u;
			GLuint VertexBuffers[3] = { 0u, 0u, 0u };
			unsigned int NumTris = 0u;
		};
		RingMesh _topRing;
		RingMesh _bottomRing;
		RingMesh _ringOccluder;
		bool _ringsNeedInitialising;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		static void _InitRingMesh(RingMesh& mesh);
		static void _ReleaseRingMesh(RingMesh& mesh);
		static void _DrawRingMesh(const RingMesh& mesh);
		static void _DrawRingOccluder(const RingMesh& mesh);
		static float _ApplySoftDecay(float current, float target, float fallRate) noexcept;
	};
}
