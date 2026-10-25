#pragma once

#include "SceneSelector.h"
#include "VstRackControls.h"
#include "PointerInteraction.h"
#include "SceneSettingsControls.h"
#include "GuiHud.h"
#include "GuiFocusManager.h"
#include "GuiPopupManager.h"
#include "GuiStationControls.h"
#include "NinjamTempoPrompt.h"
#include "RuntimeDialogs.h"
#include "../graphics/Compositor.h"
#include "../graphics/CtrlHandleOverlay.h"
#include "../engine/PerformanceGroupController.h"
#include "../engine/Quantiser.h"
#include "../midi/LoopGridEditor.h"

namespace gui
{
	// Window-owned composition. Locked phases synchronously borrow the caller's
	// station view; they never retain it or acquire the Scene lock themselves.
	class Workspace
	{
	public:
		explicit Workspace(utils::Size2d size);
		void Initialize(GuiHudParams hud, std::uint8_t channel, double offset,
			io::JamFile::GlobalMidiQuantState quantisation,
			std::function<void(const std::shared_ptr<base::GuiElement>&)> beforeHide,
			std::function<void(bool, engine::GroupCommand, std::size_t)> groupCommand);
		struct OverlayPresentation
		{
			bool RigSaveEnabled = false;
			std::string QuantisationFeedback;
			std::uint8_t ForcedChannel = 0u;
			audio::AudioStreamParams Stream;
			std::vector<float> AudioPeaks, MidiPeaks;
			std::optional<graphics::Compositor::Tempo> Tempo;
		};
		struct WorldPresentation
		{
			bool HasSelection, BackgroundPreview, BackgroundSelectionPreview, EditControlsSuppressed;
		};
		struct AnimationPresentation
		{
			bool CtrlHeld, Quitting, RigSaveInProgress, StationChangePending, RigEditsEnabled, StationControlsAvailable;
			std::uint64_t RigRevision;
			std::string GroupFeedback;
		};
		bool AdvancePresentationLocked(const std::vector<std::shared_ptr<engine::Station>>& stations, midi::LoopGridEditor& editor,
			VstRackControls& racks, const engine::PerformanceGroups& groups, const AnimationPresentation& presentation);
		void DrawOverlayLocked(base::DrawContext& ctx, const std::vector<std::shared_ptr<engine::Station>>& stations,
			midi::LoopGridEditor& editor, engine::QuantiserController& quantisation, const OverlayPresentation& presentation);
		void DrawWorldLocked(base::DrawContext& ctx, base::DrawPass pass,
			const std::vector<std::shared_ptr<engine::Station>>& stations, midi::LoopGridEditor& editor,
			engine::QuantiserController& quantisation, const engine::PerformanceGroups& groups, WorldPresentation presentation);
		graphics::Camera::Presentation ObserveCameraLocked(const std::vector<std::shared_ptr<engine::Station>>& stations, bool diagnosticDue);
		void DrawBackground(base::DrawContext& ctx);
		void InitResourcesLocked(resources::ResourceLib& resources, bool forceInit, midi::LoopGridEditor& editor,
			const std::vector<std::shared_ptr<engine::Station>>& stations);
		void InitDynamicResourcesLocked(resources::ResourceLib& resources, const std::vector<std::shared_ptr<engine::Station>>& stations,
			bool ctrlHeld, bool controlsAvailable);
		void ReleaseResources(midi::LoopGridEditor& editor);
		void ResizeLocked(utils::Size2d size, midi::LoopGridEditor& editor,
			const std::vector<std::shared_ptr<engine::Station>>& stations);
		void UpdateViewProjectionLocked(const std::vector<std::shared_ptr<engine::Station>>& stations);
		void UpdateHudStationAnchorsLocked(const std::vector<std::shared_ptr<engine::Station>>& stations);
		void UpdateRackVisibilityLocked(const std::vector<std::shared_ptr<engine::Station>>& stations, bool editorEngaged);
		void CollapseRacksLocked(const std::vector<std::shared_ptr<engine::Station>>& stations);
		gui::PointerInteraction& Pointer() noexcept { return _pointer; }
		const gui::PointerInteraction& Pointer() const noexcept { return _pointer; }
		void AddChild(std::shared_ptr<base::GuiElement> child);
		glm::mat4& ViewProjection() noexcept { return _viewProj; }
		const glm::mat4& ViewProjection() const noexcept { return _viewProj; }
		std::unique_ptr<gui::SceneSettingsControls>& Settings() noexcept { return _settingsControls; }
		const std::unique_ptr<gui::SceneSettingsControls>& Settings() const noexcept { return _settingsControls; }
		std::unique_ptr<gui::SceneSelector>& Selector() noexcept { return _selector; }
		const std::unique_ptr<gui::SceneSelector>& Selector() const noexcept { return _selector; }
		std::shared_ptr<gui::GuiMainPanel>& MainPanel() noexcept { return _mainPanel; }
		const std::shared_ptr<gui::GuiMainPanel>& MainPanel() const noexcept { return _mainPanel; }
		std::shared_ptr<gui::GuiMainPanel>& SelectionPanel() noexcept { return _selectionPanel; }
		const std::shared_ptr<gui::GuiMainPanel>& SelectionPanel() const noexcept { return _selectionPanel; }
		std::shared_ptr<gui::GuiHud>& Hud() noexcept { return _hudPanel; }
		const std::shared_ptr<gui::GuiHud>& Hud() const noexcept { return _hudPanel; }
		std::vector<std::shared_ptr<base::GuiElement>>& Children() noexcept { return _guiChildren; }
		const std::vector<std::shared_ptr<base::GuiElement>>& Children() const noexcept { return _guiChildren; }
		gui::GuiFocusManager& Focus() noexcept { return _focusManager; }
		const gui::GuiFocusManager& Focus() const noexcept { return _focusManager; }
		gui::GuiPopupManager& Popups() noexcept { return _popupManager; }
		const gui::GuiPopupManager& Popups() const noexcept { return _popupManager; }
		gui::RuntimeDialogs& Notices() noexcept { return _runtimeDialogs; }
		const gui::RuntimeDialogs& Notices() const noexcept { return _runtimeDialogs; }
		bool& RendererReady() noexcept { return _runtimeRendererReady; }
		const bool& RendererReady() const noexcept { return _runtimeRendererReady; }
		gui::NinjamTempoPrompt& TempoPrompt() noexcept { return _tempoPrompt; }
		const gui::NinjamTempoPrompt& TempoPrompt() const noexcept { return _tempoPrompt; }
		gui::GuiStationControls& StationControls() noexcept { return _stationControls; }
		const gui::GuiStationControls& StationControls() const noexcept { return _stationControls; }
		graphics::CtrlHandleOverlay& CtrlOverlay() noexcept { return _ctrlHandleOverlay; }
		const graphics::CtrlHandleOverlay& CtrlOverlay() const noexcept { return _ctrlHandleOverlay; }
		graphics::Camera& Camera() noexcept { return _camera; }
		const graphics::Camera& Camera() const noexcept { return _camera; }

	private:
		static utils::Position3d _StationCentre(const std::vector<std::shared_ptr<engine::Station>>& stations);
		gui::PointerInteraction _pointer;
		std::optional<Time> _lastPanelAnimationTime;
		utils::Size2d _size;
		glm::mat4 _viewProj{};
		graphics::Compositor _compositor;
		std::unique_ptr<gui::SceneSettingsControls> _settingsControls;
		std::unique_ptr<gui::SceneSelector> _selector;
		std::shared_ptr<gui::GuiMainPanel> _mainPanel;
		std::shared_ptr<gui::GuiMainPanel> _selectionPanel;
		std::shared_ptr<gui::GuiHud> _hudPanel;
		std::vector<std::shared_ptr<base::GuiElement>> _guiChildren;
		gui::GuiFocusManager _focusManager;
		gui::GuiPopupManager _popupManager;
		gui::RuntimeDialogs _runtimeDialogs;
		bool _runtimeRendererReady = false;
		gui::NinjamTempoPrompt _tempoPrompt;
		gui::GuiStationControls _stationControls;
		graphics::CtrlHandleOverlay _ctrlHandleOverlay;
		graphics::Camera _camera;
	};
}
