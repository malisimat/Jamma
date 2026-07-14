#include "StationModel.h"

#include <cmath>
#include <algorithm>

#include "../../include/Constants.h"
#include "../utils/VecUtils.h"
#include "GlDeleteQueue.h"
#include "GlDrawContext.h"
#include "glm/glm.hpp"

using namespace graphics;
using base::DrawContext;
using base::DrawPass;

// -------------------------------------------------------------------------
// Geometry constants
// -------------------------------------------------------------------------
namespace
{
	constexpr unsigned int  DefaultNumSides    = 32u;
	constexpr unsigned int  DefaultNumRibs     = 0u;
	constexpr unsigned int  SideVerticalSections = 12u;
	constexpr float         DeckRadius         = 9.6f;
	constexpr float         BevelWidth         = 2.0f;
	constexpr float         BevelHeight        = 10.0f;
	constexpr float         SideHeight         = 450.0f;

	// Part-kind UVs (y channel)
	constexpr float UV_TOP   = 0.0f;
	constexpr float UV_BEVEL = 1.0f;
	constexpr float UV_SIDE  = 2.0f;
	constexpr float UV_RIB   = 3.0f;
	constexpr float UV_STATE_RING_BRIGHT = 4.0f;
	constexpr float UV_STATE_RING_DARK = 5.0f;

	constexpr unsigned int StateRingSides = 64u;
	constexpr unsigned int StateRingOccluderInstances = 20u;
	constexpr float StateRingScale = 5.0f;
	constexpr float StateRingOccluderInnerRadius = 9.0f;
	constexpr float StateRingOccluderOuterRadius = 17.0f;
	constexpr float StateRingTopY = -2.0f;
	constexpr float StateRingBottomY = -(2.0f * BevelHeight + SideHeight) + 2.0f;

	constexpr RingProfilePoint StateRingProfile[] = {
		{ 9.30f, 0.00f }, { 10.80f, -1.25f }, { 15.80f, -3.50f },
		{ 15.80f, -9.50f }, { 14.90f, -13.50f }, { 11.20f, -16.00f }
	};
	constexpr RingProfilePoint StateRingBottomProfile[] = {
		{ 9.30f, 0.00f }, { 11.60f, 1.10f }, { 16.60f, 3.20f },
		{ 16.60f, 8.20f }, { 15.30f, 12.40f }, { 12.00f, 17.50f },
		{ 10.20f, 19.00f }
	};

	void PushTri(std::vector<float>& verts,
		std::vector<float>& uvs,
		const glm::vec3& a, float ua, float va,
		const glm::vec3& b, float ub, float vb,
		const glm::vec3& c, float uc, float vc)
	{
		verts.push_back(a.x); verts.push_back(a.y); verts.push_back(a.z);
		verts.push_back(b.x); verts.push_back(b.y); verts.push_back(b.z);
		verts.push_back(c.x); verts.push_back(c.y); verts.push_back(c.z);

		uvs.push_back(ua); uvs.push_back(va);
		uvs.push_back(ub); uvs.push_back(vb);
		uvs.push_back(uc); uvs.push_back(vc);
	}

	void PushQuad(std::vector<float>& verts,
		std::vector<float>& uvs,
		const glm::vec3& a, float ua, float va,
		const glm::vec3& b, float ub, float vb,
		const glm::vec3& c, float uc, float vc,
		const glm::vec3& d, float ud, float vd)
	{
		// Triangle 1: a, b, c
		PushTri(verts, uvs, a, ua, va, b, ub, vb, c, uc, vc);
		// Triangle 2: a, c, d
		PushTri(verts, uvs, a, ua, va, c, uc, vc, d, ud, vd);
	}
}

// -------------------------------------------------------------------------
// Static geometry builders
// -------------------------------------------------------------------------

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildDeckTop(unsigned int numSides, float radius)
{
	std::vector<float> verts;
	std::vector<float> uvs;

	verts.reserve(numSides * 3 * 3);
	uvs.reserve(numSides * 3 * 2);

	const float deckY = 0.0f;
	const glm::vec3 center(0.0f, deckY, 0.0f);
	const float uCenter = 0.0f;

	for (unsigned int i = 0; i < numSides; ++i)
	{
		const float a0 = static_cast<float>(constants::TWOPI) * static_cast<float>(i) / static_cast<float>(numSides);
		const float a1 = static_cast<float>(constants::TWOPI) * static_cast<float>(i + 1u) / static_cast<float>(numSides);

		const glm::vec3 p0(std::cos(a0) * radius, deckY, std::sin(a0) * radius);
		const glm::vec3 p1(std::cos(a1) * radius, deckY, std::sin(a1) * radius);

		const float u0 = 1.0f;  // outer edge radialFrac
		const float u1 = 1.0f;

		// Winding: center, p1, p0 gives +Y normal
		PushTri(verts, uvs,
			center, uCenter, UV_TOP,
			p1,     u1,      UV_TOP,
			p0,     u0,      UV_TOP);
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildBevel(unsigned int numSides, float radius, float bevelWidth, float bevelHeight)
{
	std::vector<float> verts;
	std::vector<float> uvs;

	verts.reserve(numSides * 6 * 3);
	uvs.reserve(numSides * 6 * 2);

	const float yTop   = 0.0f;
	const float yBot   = -bevelHeight;
	const float rInner = radius;
	const float rOuter = radius + bevelWidth;

	for (unsigned int i = 0; i < numSides; ++i)
	{
		const float a0 = static_cast<float>(constants::TWOPI) * static_cast<float>(i) / static_cast<float>(numSides);
		const float a1 = static_cast<float>(constants::TWOPI) * static_cast<float>(i + 1u) / static_cast<float>(numSides);
		const float c0 = std::cos(a0), s0 = std::sin(a0);
		const float c1 = std::cos(a1), s1 = std::sin(a1);

		// Inner top edge (deck rim)
		const glm::vec3 it0(c0 * rInner, yTop, s0 * rInner);
		const glm::vec3 it1(c1 * rInner, yTop, s1 * rInner);
		// Outer bottom edge (bevel base)
		const glm::vec3 ob0(c0 * rOuter, yBot, s0 * rOuter);
		const glm::vec3 ob1(c1 * rOuter, yBot, s1 * rOuter);

		const float uIT = rInner / rOuter; // slightly inside 1
		const float uOB = 1.0f;

		// Winding outward: it0, ob0, ob1, it1 gives outward-facing normal
		PushQuad(verts, uvs,
			it0, uIT, UV_BEVEL,
			ob0, uOB, UV_BEVEL,
			ob1, uOB, UV_BEVEL,
			it1, uIT, UV_BEVEL);
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildSide(unsigned int numSides, float radius, float sideHeight)
{
	std::vector<float> verts;
	std::vector<float> uvs;

	verts.reserve(numSides * SideVerticalSections * 6 * 3);
	uvs.reserve(numSides * SideVerticalSections * 6 * 2);

	const float bevelDrop = BevelHeight;   // top of side = bottom of bevel
	const float yTop = -bevelDrop;
	const float yBot = yTop - sideHeight;

	// side radius = deck radius + bevel width
	const float r = radius + BevelWidth;

	for (unsigned int i = 0; i < numSides; ++i)
	{
		const float a0 = static_cast<float>(constants::TWOPI) * static_cast<float>(i) / static_cast<float>(numSides);
		const float a1 = static_cast<float>(constants::TWOPI) * static_cast<float>(i + 1u) / static_cast<float>(numSides);
		const float c0 = std::cos(a0), s0 = std::sin(a0);
		const float c1 = std::cos(a1), s1 = std::sin(a1);

		for (unsigned int section = 0u; section < SideVerticalSections; ++section)
		{
			const float t0 = static_cast<float>(section) / static_cast<float>(SideVerticalSections);
			const float t1 = static_cast<float>(section + 1u) / static_cast<float>(SideVerticalSections);

			const float y0 = glm::mix(yTop, yBot, t0);
			const float y1 = glm::mix(yTop, yBot, t1);

			const glm::vec3 p0(c0 * r, y0, s0 * r);
			const glm::vec3 p1(c1 * r, y0, s1 * r);
			const glm::vec3 p2(c1 * r, y1, s1 * r);
			const glm::vec3 p3(c0 * r, y1, s0 * r);
			const float uvTop = 1.0f - t0;
			const float uvBottom = 1.0f - t1;

			// Outward-facing: p0, p1, p2, p3
			PushQuad(verts, uvs,
				p0, uvTop, UV_SIDE,
				p1, uvTop, UV_SIDE,
				p2, uvBottom, UV_SIDE,
				p3, uvBottom, UV_SIDE);
		}
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildBottomBevel(unsigned int numSides, float radius,
	float bevelWidth, float bevelHeight, float sideHeight)
{
	std::vector<float> verts;
	std::vector<float> uvs;

	verts.reserve(numSides * 6 * 3);
	uvs.reserve(numSides * 6 * 2);

	const float yTop = -(bevelHeight + sideHeight);
	const float yBot = yTop - bevelHeight;
	const float rOuter = radius + bevelWidth;
	const float rInner = radius;
	const float uInner = rInner / rOuter;

	for (unsigned int i = 0; i < numSides; ++i)
	{
		const float a0 = static_cast<float>(constants::TWOPI) * static_cast<float>(i) / static_cast<float>(numSides);
		const float a1 = static_cast<float>(constants::TWOPI) * static_cast<float>(i + 1u) / static_cast<float>(numSides);
		const float c0 = std::cos(a0), s0 = std::sin(a0);
		const float c1 = std::cos(a1), s1 = std::sin(a1);

		const glm::vec3 ot0(c0 * rOuter, yTop, s0 * rOuter);
		const glm::vec3 ot1(c1 * rOuter, yTop, s1 * rOuter);
		const glm::vec3 ib0(c0 * rInner, yBot, s0 * rInner);
		const glm::vec3 ib1(c1 * rInner, yBot, s1 * rInner);

		// Outward-facing lower frustum: ot0, ot1, ib1, ib0.
		PushQuad(verts, uvs,
			ot0, 1.0f, UV_BEVEL,
			ot1, 1.0f, UV_BEVEL,
			ib1, uInner, UV_BEVEL,
			ib0, uInner, UV_BEVEL);
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildDeckBottom(unsigned int numSides, float radius,
	float bevelHeight, float sideHeight)
{
	std::vector<float> verts;
	std::vector<float> uvs;

	verts.reserve(numSides * 3 * 3);
	uvs.reserve(numSides * 3 * 2);

	const float deckY = -(2.0f * bevelHeight + sideHeight);
	const glm::vec3 center(0.0f, deckY, 0.0f);
	const float uCenter = 0.0f;

	for (unsigned int i = 0; i < numSides; ++i)
	{
		const float a0 = static_cast<float>(constants::TWOPI) * static_cast<float>(i) / static_cast<float>(numSides);
		const float a1 = static_cast<float>(constants::TWOPI) * static_cast<float>(i + 1u) / static_cast<float>(numSides);

		const glm::vec3 p0(std::cos(a0) * radius, deckY, std::sin(a0) * radius);
		const glm::vec3 p1(std::cos(a1) * radius, deckY, std::sin(a1) * radius);

		// Winding: center, p0, p1 gives -Y normal.
		PushTri(verts, uvs,
			center, uCenter, UV_SIDE,
			p0, 1.0f, UV_SIDE,
			p1, 1.0f, UV_SIDE);
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildRibs(unsigned int numSides, float radius,
	unsigned int numRibs,
	float ribInnerRadius, float ribOuterRadius,
	float ribHeight, float ribHalfWidth)
{
	(void)numSides;

	std::vector<float> verts;
	std::vector<float> uvs;

	// Each rib: 2 long-face quads (inner/outer walls) + 1 top quad = 6 tris
	verts.reserve(numRibs * 18 * 3);
	uvs.reserve(numRibs * 18 * 2);

	const float deckY    = 0.0f;
	const float ribTopY  = deckY + ribHeight;

	for (unsigned int r = 0u; r < numRibs; ++r)
	{
		const float centrAngle = static_cast<float>(constants::TWOPI) * static_cast<float>(r) / static_cast<float>(numRibs);
		const float aL = centrAngle - ribHalfWidth;
		const float aR = centrAngle + ribHalfWidth;

		// Four corners at deck level, then four at rib-top level
		auto corner = [&](float angle, float rad, float y) -> glm::vec3 {
			return { std::cos(angle) * rad, y, std::sin(angle) * rad };
		};

		const auto iL_b = corner(aL, ribInnerRadius, deckY);
		const auto iR_b = corner(aR, ribInnerRadius, deckY);
		const auto oL_b = corner(aL, ribOuterRadius, deckY);
		const auto oR_b = corner(aR, ribOuterRadius, deckY);
		const auto iL_t = corner(aL, ribInnerRadius, ribTopY);
		const auto iR_t = corner(aR, ribInnerRadius, ribTopY);
		const auto oL_t = corner(aL, ribOuterRadius, ribTopY);
		const auto oR_t = corner(aR, ribOuterRadius, ribTopY);

		const float uInner = ribInnerRadius / radius;
		const float uOuter = ribOuterRadius / radius;

		// Top face (facing +Y): oL_t, oR_t, iR_t, iL_t
		PushQuad(verts, uvs,
			oL_t, uOuter, UV_RIB,
			oR_t, uOuter, UV_RIB,
			iR_t, uInner, UV_RIB,
			iL_t, uInner, UV_RIB);

		// Left side face
		PushQuad(verts, uvs,
			iL_b, uInner, UV_RIB,
			iL_t, uInner, UV_RIB,
			oL_t, uOuter, UV_RIB,
			oL_b, uOuter, UV_RIB);

		// Right side face
		PushQuad(verts, uvs,
			iR_b, uInner, UV_RIB,
			oR_b, uOuter, UV_RIB,
			oR_t, uOuter, UV_RIB,
			iR_t, uInner, UV_RIB);
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildLathedProfileGeometry(unsigned int numSides,
	std::span<const RingProfilePoint> profile,
	float yOffset, bool invertY, float partKind, float profileScale)
{
	std::vector<float> verts;
	std::vector<float> uvs;
	if (numSides < 3u || profile.size() < 2u)
		return { verts, uvs };

	verts.reserve(numSides * (profile.size() - 1u) * 6u * 3u);
	uvs.reserve(numSides * (profile.size() - 1u) * 6u * 2u);
	const auto profileY = [yOffset, invertY, profileScale](float y) { return yOffset + (invertY ? -y : y) * profileScale; };

	for (unsigned int side = 0u; side < numSides; ++side)
	{
		const float a0 = static_cast<float>(constants::TWOPI) * static_cast<float>(side) / static_cast<float>(numSides);
		const float a1 = static_cast<float>(constants::TWOPI) * static_cast<float>(side + 1u) / static_cast<float>(numSides);
		const float u0 = static_cast<float>(side) / static_cast<float>(numSides);
		const float u1 = static_cast<float>(side + 1u) / static_cast<float>(numSides);
		for (std::size_t profileIndex = 0u; profileIndex + 1u < profile.size(); ++profileIndex)
		{
			const auto& inner = profile[profileIndex];
			const auto& outer = profile[profileIndex + 1u];
			const glm::vec3 p00(std::cos(a0) * inner.Radius * profileScale, profileY(inner.Y), std::sin(a0) * inner.Radius * profileScale);
			const glm::vec3 p01(std::cos(a0) * outer.Radius * profileScale, profileY(outer.Y), std::sin(a0) * outer.Radius * profileScale);
			const glm::vec3 p11(std::cos(a1) * outer.Radius * profileScale, profileY(outer.Y), std::sin(a1) * outer.Radius * profileScale);
			const glm::vec3 p10(std::cos(a1) * inner.Radius * profileScale, profileY(inner.Y), std::sin(a1) * inner.Radius * profileScale);
			PushQuad(verts, uvs, p00, u0, partKind, p01, u0, partKind,
				p11, u1, partKind, p10, u1, partKind);
		}
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildOccluderPrismGeometry(float innerRadius, float outerRadius,
	float partKind)
{
	std::vector<float> verts;
	std::vector<float> uvs;
	verts.reserve(2u * 12u * 3u * 3u);
	uvs.reserve(2u * 12u * 3u * 2u);
	for (unsigned int bar = 0u; bar < 2u; ++bar)
	{
		const auto prismVertex = [innerRadius, outerRadius](bool outer, bool high, bool next)
		{
			return glm::vec3(outer ? outerRadius : innerRadius,
				high ? 1.0f : 0.0f, next ? 1.0f : 0.0f);
		};
		const auto pushFace = [&verts, &uvs, partKind, bar](const glm::vec3& a,
			const glm::vec3& b, const glm::vec3& c, const glm::vec3& d)
		{
			const auto barKind = static_cast<float>(bar);
			PushQuad(verts, uvs, a, barKind, partKind, b, barKind, partKind,
				c, barKind, partKind, d, barKind, partKind);
		};
		const auto il = prismVertex(false, false, false);
		const auto ol = prismVertex(true, false, false);
		const auto ih = prismVertex(false, true, false);
		const auto oh = prismVertex(true, true, false);
		const auto iln = prismVertex(false, false, true);
		const auto oln = prismVertex(true, false, true);
		const auto ihn = prismVertex(false, true, true);
		const auto ohn = prismVertex(true, true, true);
		pushFace(il, ol, oh, ih);
		pushFace(iln, ihn, ohn, oln);
		pushFace(ol, oln, ohn, oh);
		pushFace(il, ih, ihn, iln);
		pushFace(ih, oh, ohn, ihn);
		pushFace(il, iln, oln, ol);
	}

	return { verts, uvs };
}

std::tuple<std::vector<float>, std::vector<float>>
StationModel::BuildAllGeometry(unsigned int numSides, float radius, unsigned int numRibs)
{
	(void)numRibs;

	auto [tv, tu] = BuildDeckTop(numSides, radius);
	auto [bv, bu] = BuildBevel(numSides, radius, BevelWidth, BevelHeight);
	auto [sv, su] = BuildSide(numSides, radius, SideHeight);
	auto [lbv, lbu] = BuildBottomBevel(numSides, radius, BevelWidth, BevelHeight, SideHeight);
	auto [cv, cu] = BuildDeckBottom(numSides, radius, BevelHeight, SideHeight);

	// Concatenate
	std::vector<float> verts;
	std::vector<float> uvs;
	verts.reserve(tv.size() + bv.size() + sv.size() + lbv.size() + cv.size());
	uvs.reserve(tu.size() + bu.size() + su.size() + lbu.size() + cu.size());

	for (auto& v : { &tv, &bv, &sv, &lbv, &cv })
		verts.insert(verts.end(), v->begin(), v->end());

	for (auto& u : { &tu, &bu, &su, &lbu, &cu })
		uvs.insert(uvs.end(), u->begin(), u->end());

	return { verts, uvs };
}

// -------------------------------------------------------------------------
// StationModel ctor / Draw3d
// -------------------------------------------------------------------------

StationModel::StationModel() :
	GuiModel(gui::GuiModelParams()),
	_lastPass(base::PASS_SCENE),
	_stationGlobalId(),
	_stationSelected(false),
	_stationPicking(false),
	_stationLevel(0.0f),
	_stationVisualState(0u),
	_stationFallRate(0.0f),
	_topRing(),
	_bottomRing(),
	_ringOccluder(),
	_ringsNeedInitialising(true)
{
	_modelParams.ModelShaders = { "station", "picker", "station_ring" };
	SetVisible(false);

	auto [verts, uvs] = BuildAllGeometry(DefaultNumSides, DeckRadius, DefaultNumRibs);
	SetGeometry(std::move(verts), std::move(uvs));
	std::tie(_topRing.Verts, _topRing.Uvs) = BuildLathedProfileGeometry(
		StateRingSides, StateRingProfile, StateRingTopY, false, UV_STATE_RING_BRIGHT, StateRingScale);
	std::tie(_bottomRing.Verts, _bottomRing.Uvs) = BuildLathedProfileGeometry(
		StateRingSides, StateRingBottomProfile, StateRingBottomY, false, UV_STATE_RING_BRIGHT, StateRingScale);
	std::tie(_ringOccluder.Verts, _ringOccluder.Uvs) = BuildOccluderPrismGeometry(
		StateRingOccluderInnerRadius * StateRingScale,
		StateRingOccluderOuterRadius * StateRingScale,
		UV_STATE_RING_DARK);
}

void StationModel::SetStationState(const std::vector<unsigned int>& stationGlobalId,
		bool selected,
		bool picking,
		float level,
		std::uint8_t visualState)
	{
		_stationGlobalId = stationGlobalId;
		_stationSelected = selected;
		_stationPicking = picking;
		_stationVisualState = visualState;
		const auto targetLevel = std::clamp(level, 0.0f, 1.0f);
		const auto decayRate = std::max(_stationFallRate, 0.0f);
		_stationLevel = _ApplySoftDecay(_stationLevel, targetLevel, decayRate);
}

void StationModel::SetParams(float fallRate) noexcept
	{
		_stationFallRate = std::max(fallRate, 0.0f);
	}

	void StationModel::ResetStationLevel() noexcept
	{
		_stationLevel = 0.0f;
}

float StationModel::_ApplySoftDecay(float current, float target, float fallRate) noexcept
{
	if (target > current)
	{
		// Rise: move 30% of the way toward target
		return current + 0.3f * (target - current);
	}
	else if (target < current)
	{
		// Fall: apply soft movement with fallRate limit
		const auto softFall = 0.5f * fallRate;
		const auto next = current - softFall;
		return std::max(next, target);  // Don't fall below target
	}

	return current;
}

void StationModel::_InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	GuiModel::_InitResources(resourceLib, forceInit);
	if (!_ringsNeedInitialising || !HasCurrentGlContext())
		return;

	_InitRingMesh(_topRing);
	_InitRingMesh(_bottomRing);
	_InitRingMesh(_ringOccluder);
	_ringsNeedInitialising = false;
}

void StationModel::_ReleaseResources()
{
	GuiModel::_ReleaseResources();
	_ReleaseRingMesh(_topRing);
	_ReleaseRingMesh(_bottomRing);
	_ReleaseRingMesh(_ringOccluder);
	_ringsNeedInitialising = true;
}

void StationModel::_InitRingMesh(RingMesh& mesh)
{
	if (mesh.Verts.empty() || mesh.Uvs.empty())
		return;

	_ReleaseRingMesh(mesh);
	mesh.NumTris = static_cast<unsigned int>(mesh.Verts.size() / 9u);
	std::vector<GLfloat> normals;
	normals.reserve(mesh.NumTris * 9u);
	for (unsigned int triangle = 0u; triangle < mesh.NumTris; ++triangle)
	{
		const auto v1 = glm::vec3(mesh.Verts[(triangle * 3u + 0u) * 3u], mesh.Verts[(triangle * 3u + 0u) * 3u + 1u], mesh.Verts[(triangle * 3u + 0u) * 3u + 2u]);
		const auto v2 = glm::vec3(mesh.Verts[(triangle * 3u + 1u) * 3u], mesh.Verts[(triangle * 3u + 1u) * 3u + 1u], mesh.Verts[(triangle * 3u + 1u) * 3u + 2u]);
		const auto v3 = glm::vec3(mesh.Verts[(triangle * 3u + 2u) * 3u], mesh.Verts[(triangle * 3u + 2u) * 3u + 1u], mesh.Verts[(triangle * 3u + 2u) * 3u + 2u]);
		const auto normal = glm::normalize(glm::cross(v2 - v1, v3 - v1));
		for (unsigned int vertex = 0u; vertex < 3u; ++vertex)
		{
			normals.push_back(normal.x);
			normals.push_back(normal.y);
			normals.push_back(normal.z);
		}
	}

	glGenVertexArrays(1, &mesh.VertexArray);
	glBindVertexArray(mesh.VertexArray);
	glGenBuffers(3, mesh.VertexBuffers);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.VertexBuffers[0]);
	glBufferData(GL_ARRAY_BUFFER, mesh.Verts.size() * sizeof(GLfloat), mesh.Verts.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.VertexBuffers[1]);
	glBufferData(GL_ARRAY_BUFFER, mesh.Uvs.size() * sizeof(GLfloat), mesh.Uvs.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glBindBuffer(GL_ARRAY_BUFFER, mesh.VertexBuffers[2]);
	glBufferData(GL_ARRAY_BUFFER, normals.size() * sizeof(GLfloat), normals.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, 0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
}

void StationModel::_ReleaseRingMesh(RingMesh& mesh)
{
	if (!HasCurrentGlContext())
		return;
	graphics::GlDeleteQueue::DeleteBuffers(3, mesh.VertexBuffers);
	mesh.VertexBuffers[0] = 0u;
	mesh.VertexBuffers[1] = 0u;
	mesh.VertexBuffers[2] = 0u;
	graphics::GlDeleteQueue::DeleteVertexArrays(1, &mesh.VertexArray);
	mesh.VertexArray = 0u;
	mesh.NumTris = 0u;
}

void StationModel::_DrawRingMesh(const RingMesh& mesh)
{
	if (mesh.VertexArray == 0u || mesh.NumTris == 0u)
		return;
	glBindVertexArray(mesh.VertexArray);
	glDrawArrays(GL_TRIANGLES, 0, mesh.NumTris * 3u);
}

void StationModel::_DrawRingOccluder(const RingMesh& mesh)
{
	if (mesh.VertexArray == 0u || mesh.NumTris == 0u)
		return;
	glBindVertexArray(mesh.VertexArray);
	glDrawArraysInstanced(GL_TRIANGLES, 0, mesh.NumTris * 3u, StateRingOccluderInstances);
}

std::weak_ptr<resources::ShaderResource> StationModel::GetShader()
{
	return GetShaderAt(_lastPass == base::PASS_PICKER ? 1u : 0u);
}

void StationModel::Draw3d(DrawContext& ctx,
	unsigned int /*numInstances*/,
	DrawPass pass)
{
	if (!_isVisible)
		return;

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	// Stash pass so GetShader() returns the right one.
	_lastPass = pass;

	// Ensure resources are present (lazy init like quantisation model).
	if (!SyncInstanceAttributes())
		_resourcesNeedInitialising = true;

	auto modelShader = GetShader();
	auto shader = modelShader.lock();

	if (!shader || 0u == _vertexArray || 0u == _numTris)
		return;

	const auto stationLevel = std::clamp(_stationLevel, 0.0f, 1.0f);
	const glm::vec3 stationStateColors[] = {
		{ 0.34f, 0.78f, 0.89f },
		{ 0.94f, 0.20f, 0.22f },
		{ 0.20f, 0.96f, 0.38f },
		{ 0.24f, 0.68f, 0.98f },
		{ 0.95f, 0.54f, 0.16f },
		{ 0.71f, 0.33f, 0.93f }
	};
	const auto stationStateIndex = std::min<std::size_t>(_stationVisualState,
		std::size(stationStateColors) - 1u);

	// Set pass-specific uniforms before binding the program.
	switch (pass)
	{
	case base::PASS_PICKER:
	{
		auto idVec = _stationGlobalId.empty() ? GlobalId() : _stationGlobalId;
		idVec.resize(3);
		for (auto& idPart : idVec)
			idPart += 1;
		const auto id = utils::VecToId(idVec);
		glCtx.SetUniform("ObjectId", id);
		break;
	}
	case base::PASS_HIGHLIGHT:
			glCtx.SetUniform("Highlight", _stationSelected ? 1.0f : 0.0f);
			glCtx.SetUniform("StationHover", _stationPicking ? 1.0f : 0.0f);
			glCtx.SetUniform("StationLevel", stationLevel);
			glCtx.SetUniform("StationStateColor", stationStateColors[stationStateIndex]);
			break;
		case base::PASS_SCENE:
		default:
			glCtx.SetUniform("Highlight", _stationSelected ? 0.35f : 0.0f);
			glCtx.SetUniform("StationHover", _stationPicking ? 1.0f : 0.0f);
		glCtx.SetUniform("StationLevel", stationLevel);
		glCtx.SetUniform("StationStateColor", stationStateColors[stationStateIndex]);
		break;
	}

	glUseProgram(shader->GetId());
	shader->SetUniforms(glCtx);

	glBindVertexArray(_vertexArray);
	glDrawArrays(GL_TRIANGLES, 0, _numTris * 3);
	glBindVertexArray(0);
	glUseProgram(0);

	if (pass == base::PASS_PICKER)
	{
		glUseProgram(shader->GetId());
		shader->SetUniforms(glCtx);
		_DrawRingMesh(_topRing);
		_DrawRingMesh(_bottomRing);
		glBindVertexArray(0);
		glUseProgram(0);
		return;
	}

	auto ringShader = GetShaderAt(2u).lock();
	if (!ringShader)
		return;

	glCtx.SetUniform("Highlight", _stationSelected ? (pass == base::PASS_HIGHLIGHT ? 1.0f : 0.35f) : 0.0f);
	glCtx.SetUniform("StationHover", _stationPicking ? 1.0f : 0.0f);
	glCtx.SetUniform("StationLevel", stationLevel);
	glCtx.SetUniform("StationStateColor", stationStateColors[stationStateIndex]);
	glCtx.SetUniform("StationVisualState", static_cast<int>(stationStateIndex));
	glCtx.SetUniform("RingScale", StateRingScale);
	glUseProgram(ringShader->GetId());
	ringShader->SetUniforms(glCtx);
	_DrawRingMesh(_topRing);
	_DrawRingMesh(_bottomRing);
	glCtx.SetUniform("RingCapY", StateRingTopY);
	glCtx.SetUniform("RingDirection", 1.0f);
	ringShader->SetUniforms(glCtx);
	_DrawRingOccluder(_ringOccluder);
	glCtx.SetUniform("RingCapY", StateRingBottomY);
	glCtx.SetUniform("RingDirection", -1.0f);
	ringShader->SetUniforms(glCtx);
	_DrawRingOccluder(_ringOccluder);
	glBindVertexArray(0);
	glUseProgram(0);
}
