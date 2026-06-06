#pragma once

#include <vector>
#include <tuple>
#include "../gui/GuiModel.h"

namespace graphics
{
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
			float level = 0.0f);
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

		// Convenience: build all geometry and concatenate into one pair.
		static std::tuple<std::vector<float>, std::vector<float>>
			BuildAllGeometry(unsigned int numSides, float radius,
				unsigned int numRibs);

		// Number of triangles in the last built geometry (0 before init).
		unsigned int NumTris() const noexcept { return _numTris; }

	protected:
		virtual std::weak_ptr<resources::ShaderResource> GetShader() override;

	private:
		// Index into _modelShaders for each draw pass.
		// 0 = station (scene/highlight), 1 = picker.
		base::DrawPass _lastPass;
		std::vector<unsigned int> _stationGlobalId;
		bool _stationSelected;
		bool _stationPicking;
		float _stationLevel;
		float _stationFallRate;
		static float _ApplySoftDecay(float current, float target, float fallRate) noexcept;
	};
}
