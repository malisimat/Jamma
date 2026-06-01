#include "StationModel.h"

#include <cmath>
#include <algorithm>

#include "../../include/Constants.h"
#include "../utils/VecUtils.h"
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
	constexpr unsigned int  DefaultNumRibs     = 8u;
	constexpr float         DeckRadius         = 190.0f;
	constexpr float         BevelWidth         = 18.0f;
	constexpr float         BevelHeight        = 12.0f;
	constexpr float         SideHeight         = 14.0f;
	constexpr float         RibInnerFrac       = 0.18f;
	constexpr float         RibOuterFrac       = 0.90f;
	constexpr float         RibHeight          = 6.0f;
	constexpr float         RibHalfWidthRad    = 0.045f; // ~2.6 degrees per rib face

	// Part-kind UVs (y channel)
	constexpr float UV_TOP   = 0.0f;
	constexpr float UV_BEVEL = 1.0f;
	constexpr float UV_SIDE  = 2.0f;
	constexpr float UV_RIB   = 3.0f;

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

	verts.reserve(numSides * 6 * 3);
	uvs.reserve(numSides * 6 * 2);

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

		const glm::vec3 t0(c0 * r, yTop, s0 * r);
		const glm::vec3 t1(c1 * r, yTop, s1 * r);
		const glm::vec3 b0(c0 * r, yBot, s0 * r);
		const glm::vec3 b1(c1 * r, yBot, s1 * r);

		// Outward-facing: t0, b0, b1, t1
		PushQuad(verts, uvs,
			t0, 1.0f, UV_SIDE,
			b0, 1.0f, UV_SIDE,
			b1, 1.0f, UV_SIDE,
			t1, 1.0f, UV_SIDE);
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
StationModel::BuildAllGeometry(unsigned int numSides, float radius, unsigned int numRibs)
{
	auto [tv, tu] = BuildDeckTop(numSides, radius);
	auto [bv, bu] = BuildBevel(numSides, radius, BevelWidth, BevelHeight);
	auto [sv, su] = BuildSide(numSides, radius, SideHeight);
	auto [rv, ru] = BuildRibs(numSides, radius,
		numRibs,
		radius * RibInnerFrac,
		radius * RibOuterFrac,
		RibHeight, RibHalfWidthRad);

	// Concatenate
	std::vector<float> verts;
	std::vector<float> uvs;
	verts.reserve(tv.size() + bv.size() + sv.size() + rv.size());
	uvs.reserve(tu.size() + bu.size() + su.size() + ru.size());

	for (auto& v : { &tv, &bv, &sv, &rv })
		verts.insert(verts.end(), v->begin(), v->end());

	for (auto& u : { &tu, &bu, &su, &ru })
		uvs.insert(uvs.end(), u->begin(), u->end());

	return { verts, uvs };
}

// -------------------------------------------------------------------------
// StationModel ctor / Draw3d
// -------------------------------------------------------------------------

StationModel::StationModel() :
	GuiModel(gui::GuiModelParams()),
	_lastPass(base::PASS_SCENE),
	_ownerGlobalId(),
	_ownerSelected(false),
	_ownerPicking(false)
{
	_modelParams.ModelShaders = { "station", "picker" };
	SetVisible(false);

	auto [verts, uvs] = BuildAllGeometry(DefaultNumSides, DeckRadius, DefaultNumRibs);
	SetGeometry(std::move(verts), std::move(uvs));
}

void StationModel::SetOwnerState(const std::vector<unsigned int>& ownerGlobalId, bool selected, bool picking)
{
	_ownerGlobalId = ownerGlobalId;
	_ownerSelected = selected;
	_ownerPicking = picking;
}

std::weak_ptr<resources::ShaderResource> StationModel::GetShader()
{
	if (_lastPass == base::PASS_PICKER)
	{
		if (_modelShaders.size() >= 2)
			return _modelShaders[1];
	}

	// scene and highlight both use shaders[0]
	if (!_modelShaders.empty())
		return _modelShaders[0];

	return {};
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
	if (_instanceAttributesNeedUpdating)
	{
		if (!SyncInstanceAttributes())
			_resourcesNeedInitialising = true;
	}

	auto modelShader = GetShader();
	auto shader = modelShader.lock();

	if (!shader || 0u == _vertexArray || 0u == _numTris)
		return;

	// Set pass-specific uniforms before binding the program.
	switch (pass)
	{
	case base::PASS_PICKER:
	{
		auto idVec = _ownerGlobalId.empty() ? GlobalId() : _ownerGlobalId;
		idVec.resize(3);
		for (auto& idPart : idVec)
			idPart += 1;
		const auto id = utils::VecToId(idVec);
		glCtx.SetUniform("ObjectId", id);
		break;
	}
	case base::PASS_HIGHLIGHT:
		glCtx.SetUniform("Highlight", _ownerSelected ? 1.0f : 0.0f);
		glCtx.SetUniform("StationHover", _ownerPicking ? 1.0f : 0.0f);
		break;
	case base::PASS_SCENE:
	default:
		glCtx.SetUniform("Highlight", _ownerSelected ? 0.35f : 0.0f);
		glCtx.SetUniform("StationHover", _ownerPicking ? 1.0f : 0.0f);
		break;
	}

	glUseProgram(shader->GetId());
	shader->SetUniforms(glCtx);

	glBindVertexArray(_vertexArray);
	glDrawArrays(GL_TRIANGLES, 0, _numTris * 3);
	glBindVertexArray(0);
	glUseProgram(0);
}
