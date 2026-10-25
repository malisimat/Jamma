#include "Compositor.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <glm/ext.hpp>
#include "Constants.h"
#include "../gui/GuiPanel.h"

using namespace graphics;
using namespace gui;

Compositor::Compositor()
{
	GuiLabelParams labelParams;
	labelParams.String = "BPM --- / BPI ---";
	labelParams.Ellipsize = true;
	labelParams.ClipText = true;
	labelParams.VerticalAlign = GuiTextVerticalAlign::Center;
	labelParams.Position = { 0, 3 };
	labelParams.TextInsetX = GuiStyle::TextPadding;
	labelParams.Size = { GuiStyle::TempoColumnMaxWidth, 22u };
	_tempoLabel = std::make_unique<GuiLabel>(labelParams);
	labelParams.String = "Jamma v" LIB_VERSION;
	_versionLabel = std::make_unique<GuiLabel>(labelParams);
}

void Compositor::InitResources(resources::ResourceLib& resources, bool forceInit)
{
	_skybox.InitResources(resources, forceInit);
	_tempoLabel->InitResources(resources, forceInit);
	_versionLabel->InitResources(resources, forceInit);
}

void Compositor::ReleaseResources()
{
	_skybox.ReleaseResources();
	_tempoLabel->ReleaseResources();
	_versionLabel->ReleaseResources();
}

void Compositor::Resize(utils::Size2d viewport)
{
	const auto hScale = viewport.Width > 0 ? 2.0f / static_cast<float>(viewport.Width) : 1.0f;
	const auto vScale = viewport.Height > 0 ? 2.0f / static_cast<float>(viewport.Height) : 1.0f;
	_overlayViewProjection = glm::mat4(1.0);
	_overlayViewProjection = glm::translate(_overlayViewProjection, glm::vec3(-1.0f, -1.0f, -1.0f));
	_overlayViewProjection = glm::scale(_overlayViewProjection, glm::vec3(hScale, vScale, 1.0f));
}

void Compositor::BeginOverlay(GlDrawContext& context) const
{
	glDisable(GL_DEPTH_TEST);
	context.ClearMvp();
	context.PushMvp(_overlayViewProjection);
}

void Compositor::EndOverlay(GlDrawContext& context)
{
	context.PopMvp();
}

void Compositor::DrawStatus(base::DrawContext& context, utils::Size2d viewport, std::optional<Tempo> tempo)
{
	const int statusWidth = GuiStyle::StatusBarWidth(static_cast<int>(viewport.Width));
	const int tempoWidth = GuiStyle::TempoColumnWidth(statusWidth);
	std::string tempoText = "BPM --- / BPI ---";
	if (tempo)
	{
		std::ostringstream text;
		text << std::fixed << std::setprecision(0) << "BPM " << std::setw(3) << tempo->Bpm
			<< " / BPI " << std::setw(3) << tempo->Bpi;
		tempoText = text.str();
	}
	_tempoLabel->SetString(tempoText);
	const int statusHeight = std::min(GuiStyle::StatusBarHeight, static_cast<int>(viewport.Height));
	_tempoLabel->SetPosition({ 0, std::max(0, (statusHeight - 22) / 2) });
	_tempoLabel->SetSize({ static_cast<unsigned int>(tempoWidth), 22u });
	if (tempoWidth > 0 && statusHeight >= 22) _tempoLabel->Draw(context);
	const int versionWidth = GuiStyle::VersionColumnWidth(statusWidth);
	_versionLabel->SetPosition({ statusWidth - versionWidth, std::max(0, (statusHeight - 22) / 2) });
	_versionLabel->SetSize({ static_cast<unsigned int>(versionWidth), 22u });
	if (versionWidth > 0 && statusHeight >= 22) _versionLabel->Draw(context);
}

void Compositor::DrawBackground(GlDrawContext& context, const Camera& camera, utils::Size2d viewport)
{
	const auto aspect = viewport.Height > 0 ? static_cast<float>(viewport.Width) / viewport.Height : 1.0f;
	const auto view = camera.ViewMatrix();
	glEnable(GL_DEPTH_TEST);
	if (!_skyboxStarted)
	{
		_skyboxStartTime = utils::Timer::GetTime();
		_skyboxStarted = true;
	}
	const auto elapsed = static_cast<float>(utils::Timer::GetElapsedSeconds(_skyboxStartTime, utils::Timer::GetTime()));
	const auto rotation = Skybox::AnimationTransform(elapsed);
	const auto skyboxViewProjection = camera.SkyboxProjection(aspect) * glm::mat4(glm::mat3(view)) * rotation;
	context.ClearMvp();
	context.PushMvp(skyboxViewProjection);
	_skybox.Draw(context);
	context.PopMvp();
}

glm::mat4 Compositor::BeginWorld(GlDrawContext& context, const Camera& camera,
	utils::Size2d viewport, utils::Position3d centre, base::DrawPass pass)
{
	const auto aspect = viewport.Height > 0 ? static_cast<float>(viewport.Width) / viewport.Height : 1.0f;
	const auto projection = camera.Projection(aspect, centre);
	const auto view = camera.ViewMatrix();
	if (pass == base::PASS_SCENE) glEnable(GL_DEPTH_TEST);
	else glDisable(GL_DEPTH_TEST);
	context.ClearMvp();
	context.PushMvp(projection);
	context.PushMvp(view);
	return projection * view;
}

bool Compositor::SetWorldPresentation(GlDrawContext& context, const WorldPresentation& presentation) const
{
	context.SetUniform("SelectionActive", presentation.SelectionActive ? 1.0f : 0.0f);
	context.SetUniform("SceneDim", presentation.SurroundingDim);
	context.SetUniform("EditorMorph", 0.0f);
	context.SetUniform("EditorTime", _skyboxStarted
		? static_cast<float>(utils::Timer::GetElapsedSeconds(_skyboxStartTime, utils::Timer::GetTime())) : 0.0f);
	const auto probeId = presentation.UseProbe ? _skybox.CubemapId() : 0u;
	// Cubemap and material samplers require distinct units even without lighting.
	context.SetUniform("ProbeSampler", 3);
	context.SetUniform("ProbeStrength", probeId != 0u ? 1.0f : 0.0f);
	if (probeId != 0u)
	{
		context.SetUniform("EditorProbeEye", presentation.ProbeEye);
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_CUBE_MAP, probeId);
		glActiveTexture(GL_TEXTURE0);
	}
	return probeId != 0u;
}

void Compositor::EndProjection(GlDrawContext& context)
{
	context.PopMvp();
	context.PopMvp();
}

void Compositor::EndWorld(GlDrawContext& context, bool probeBound)
{
	context.SetGroupPreview(false, glm::vec3(1.0f), {});
	context.ApplyGroupPreview({});
	context.SetUniform("SelectionActive", 0.0f);
	context.SetUniform("SceneDim", 1.0f);
	if (probeBound)
	{
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		glActiveTexture(GL_TEXTURE0);
	}
	EndProjection(context);
}
