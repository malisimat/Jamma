#pragma once

#include "SceneSelector.h"
#include "../io/IoInputSubsystem.h"
#include "../engine/HierarchySelection.h"
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
		void AddChild(std::shared_ptr<base::GuiElement> child);
		gui::SceneSettingsControls& Settings() noexcept { return *_settingsControls; }
		const gui::SceneSettingsControls& Settings() const noexcept { return *_settingsControls; }
		const std::shared_ptr<gui::GuiMainPanel>& MainPanel() const noexcept { return _mainPanel; }
		const std::shared_ptr<gui::GuiMainPanel>& SelectionPanel() const noexcept { return _selectionPanel; }
		const std::shared_ptr<gui::GuiHud>& Hud() const noexcept { return _hudPanel; }
		gui::GuiPopupManager& Popups() noexcept { return _popupManager; }
		const gui::GuiPopupManager& Popups() const noexcept { return _popupManager; }
		gui::RuntimeDialogs& Notices() noexcept { return _runtimeDialogs; }
		const gui::RuntimeDialogs& Notices() const noexcept { return _runtimeDialogs; }
		graphics::CtrlHandleOverlay& CtrlOverlay() noexcept { return _ctrlHandleOverlay; }
		const graphics::CtrlHandleOverlay& CtrlOverlay() const noexcept { return _ctrlHandleOverlay; }

		void SetFeedback(std::string text) { _quantisationFeedback = std::move(text); }
		const std::string& QuantisationFeedback() const noexcept { return _quantisationFeedback; }

		struct StationAccess
		{
			std::mutex& Mutex;
			const std::vector<std::shared_ptr<engine::Station>>& Stations;
			std::vector<std::shared_ptr<engine::Station>> Snapshot() const
			{ std::scoped_lock lock(Mutex); return Stations; }
		};
		struct InputPresentation
		{
			const io::UserConfig& Config;
			audio::AudioStreamParams Stream;
			bool Quitting, WriterHeld, RigSaveDialogOpen, QuantisationResetRequested, Verbose;
		};
		struct InputCommands
		{
			std::optional<GuiStationControls::Intent> StationControl;
			std::optional<bool> MuteAll;
			bool CancelTakeDitch = false, CancelStationRemoval = false;
		};
		void InitializeEditor(std::mutex& mutex, const std::vector<std::shared_ptr<engine::Station>>& stations, std::function<void()> onOpened);
		midi::LoopGridEditor& Editor() noexcept { return *_editor; }
		const midi::LoopGridEditor& Editor() const noexcept { return *_editor; }
		actions::ActionUndoHistory& Undo() noexcept { return _undoHistory; }
		engine::QuantisationInteractionContext InteractionContextLocked(StationAccess access, const InputPresentation& presentation) const;
		enum class KeyboardCommand { TapTempo, ApplyKey };
		using KeyboardHandler = std::function<std::optional<actions::ActionResult>(KeyboardCommand, const actions::KeyAction&, bool)>;
		actions::ActionResult OnKeyboard(actions::KeyAction action, StationAccess access, const InputPresentation& presentation,
			bool contextual, bool suppressedKey, bool freshLearnKey, io::IoInputSubsystem& input, const KeyboardHandler& handleKeyboard);
		void InitializeRacks(VstRackControls::Host host) { _racks = std::make_unique<VstRackControls>(std::move(host)); }
		VstRackControls& Racks() noexcept { return *_racks; }
		struct TakeDitchTarget { std::shared_ptr<engine::Station> Station; std::shared_ptr<engine::LoopTake> Take; std::uint64_t Revision = 0u; };
		TakeDitchTarget TakeDitchTargetLocked() { return std::exchange(_takeDitchTarget, {}); }
		void AttachStationLabel(const std::shared_ptr<engine::Station>& station, std::function<std::string(const std::string&, std::uint64_t)> submit);
		void OpenTakeDitch(std::shared_ptr<engine::Station> station, std::shared_ptr<engine::LoopTake> take, std::uint64_t revision, const std::shared_ptr<base::ActionReceiver>& receiver);
		void OpenStationRemoval(const std::string& name, const std::shared_ptr<base::ActionReceiver>& receiver);
		void CompleteMembershipPresentationLocked(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const std::shared_ptr<engine::Station>& removed, const std::shared_ptr<engine::Station>& added, std::size_t affectedBindings);
		static utils::Position3d StationCentre(const std::vector<std::shared_ptr<engine::Station>>& stations);
		void AdvanceCamera(Time now, float deltaSeconds, const graphics::Camera::Presentation& presentation,
			const engine::CameraRecordingObservation& recordings, bool controlsAvailable, StationAccess access, const InputPresentation& input);
		bool PrepareKeyboard(actions::KeyAction& action, StationAccess access);
		void ResetKeyboardObservation();
		void OnFocusLostLocked(StationAccess access, const InputPresentation& presentation);
		void InitReceivers(const std::shared_ptr<base::ActionReceiver>& receiver);
		void ClearReceivers();
		bool SpaceHeld() const noexcept { return _spaceHeld; }
		bool CtrlHeld() const noexcept { return _ctrlHeld; }
		bool Touching() const noexcept { return _isSceneTouching; }
		void Init() { _selector->Init(); }
		bool PopupOpen() const noexcept { return _popupManager.IsOpen(); }
		bool IsEditingText() const noexcept { return _focusManager.IsEditingText(); }
		bool CameraDragging() const noexcept { return _camera.IsBackgroundDragging(); }
		bool HasPointerCapture() const noexcept { return _pointer.HasCapture(); }
		void CommitFocus() { _focusManager.ClearFocus(); }
		void CancelBackgroundClick() { _selector->ClearBackgroundClick(); }
		base::SelectDepth PresentationMode() const { return _viewMode; }
		base::SelectDepth SelectionDepth() const { return _selector->CurrentSelectDepth(); }
		std::vector<unsigned char> CurrentHover() const { return _selector->CurrentHover(); }
		void SetSelectionDepthLocked(base::SelectDepth depth) { _selector->SetSelectDepth(depth); }
		void OnCameraTriggerActivity() { _camera.TriggerActivity(utils::Timer::GetTime()); }
		void OnHumanActivity() { _camera.HumanActivity(utils::Timer::GetTime()); }
		graphics::Camera::FrameTime BeginCameraPresentation(Time now) { return _camera.BeginPresentation(now); }
		void SetCameraInteractionEligibility(bool eligible, bool held, const engine::CameraRecordingObservation& recordings)
		{ _camera.SetInteractionEligibility(eligible, held, recordings, utils::Timer::GetTime()); }
		void RegisterStationLocked(std::size_t index, const std::shared_ptr<engine::Station>& station)
		{ _camera.RegisterStation(index, station, station->LoopTakeRevision()); }
		void UpdateEditorUi() { _editor->UpdateUi(_viewProj); }
		void CloseTakeDitch() { _popupManager.Close(_takeDitchDialog); }
		void CloseStationRemoval() { _popupManager.Close(_stationDeleteDialog); }
		void PumpNativeControls() { _racks->Pump(); _runtimeDialogs.Pump(_popupManager, _size, _runtimeRendererReady); }
		void ResetRackControls() { _racks->Reset(); }
		void FinishRenderer() { _runtimeRendererReady = false; _runtimeDialogs.Pump(_popupManager, _size, false); }
		void InitNoticeResources(resources::ResourceLib& resources, bool forceInit)
		{ _runtimeRendererReady = _runtimeDialogs.InitResources(resources, forceInit); if (!_runtimeRendererReady) _runtimeDialogs.Pump(_popupManager, _size, false); }
		void SetViewMode(unsigned int mode, StationAccess access, const InputPresentation& presentation);
		auto TakeTempoDecision() { return _tempoPrompt.TakeDecision(_popupManager); }
		void SynchronizeTempoPrompt(const std::optional<ninjam::NinjamTempoProposal>& prompt) { _tempoPrompt.Synchronize(prompt, _popupManager, _size); }
		glm::mat4 ViewProjection() const noexcept { return _viewProj; }
		bool RequestFocus(const std::shared_ptr<base::GuiElement>& editor) { return _focusManager.RequestFocus(editor); }
		void OpenPopup(std::shared_ptr<base::GuiElement> popup) { _popupManager.Open(std::move(popup)); }
		void ClosePopups() { _popupManager.CloseAll(); }
		void CloseTopPopup() { _popupManager.Close(); }
		bool IsSelectionMutePressed() const { return _selector->IsMutePressed(); }
		bool IsPaintingMute() const { return _selector->IsPaintingMute(); }
		bool OwnsBackgroundClick() const { return _selector->OwnsBackgroundClick(); }
		const std::shared_ptr<GuiPopup>& StationRemovalConfirmation() const { return _stationDeleteDialog; }
		void BindQuantisation(engine::Quantiser& quantiser, engine::QuantiserController& interaction);
		InputCommands TakeInputCommands() { return std::exchange(_inputCommands, {}); }
		actions::ActionResult OnTouch(actions::TouchAction action, StationAccess access, const InputPresentation& presentation);
		actions::ActionResult OnMove(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation);
		void SetHover3d(std::vector<unsigned char> path, base::Action::Modifiers modifiers, std::optional<utils::Position2d> pointerPosition, StationAccess access, const InputPresentation& presentation);
		void ApplyDeferredHoverUpdates(StationAccess access, const InputPresentation& presentation);

		void OnSettingsHidden(const std::shared_ptr<base::GuiElement>& subtree);

		bool BackgroundPreviewActive(StationAccess access, const InputPresentation& presentation) const;

		bool BackgroundSelectionPreviewActive(StationAccess access, const InputPresentation& presentation) const;

		void UpdateSelection(actions::ActionResultType res, StationAccess access, const InputPresentation& presentation);

		void EndBackgroundDrag(StationAccess access, const InputPresentation& presentation);

		bool EditControlsSuppressed() const;

		void InvalidateHover2d();

		void ApplyHoverPath2d(const std::vector<std::weak_ptr<base::GuiElement>>& nextPath);

		void ReleaseQuantisationInput(StationAccess access, const InputPresentation& presentation);

		void ReleaseQuantisationInputLocked(StationAccess access, const InputPresentation& presentation);

		void OnLoopGridEditorOpened(StationAccess access, const InputPresentation& presentation);

		void ApplyCameraSelectDepthChange(graphics::Camera::SelectDepthChange change, StationAccess access, const InputPresentation& presentation);

		void CycleCameraView(StationAccess access, const InputPresentation& presentation);

		void UpdateSelectDepth(unsigned int depth, StationAccess access, const InputPresentation& presentation);

		void SetRelativePointerHost(std::function<bool(int, utils::Position2d)> begin, std::function<void(int)> end, std::function<float()> logicalPixelScale, StationAccess access, const InputPresentation& presentation);

	private:
		friend class WorkspaceTestAccess;
		std::optional<actions::ActionResult> _RouteQuantisationMove(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation);

		std::optional<actions::ActionResult> _RouteQuantisationTouch(actions::TouchAction action, StationAccess access, const InputPresentation& presentation);

		bool _HasQuantisationHover(StationAccess access, const InputPresentation& presentation) const;

		void _UpdateStationQuantisation(std::shared_ptr<base::GuiElement> candidate, base::SelectDepth depth, bool confirmCandidate, StationAccess access, const InputPresentation& presentation);

		bool _TrySetMasterFromHover(bool confirm, StationAccess access, const InputPresentation& presentation);

		actions::ActionResult _UpdateBackgroundDrag(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation);

		actions::ActionResult _BeginBackgroundDrag(actions::TouchAction action, StationAccess access, const InputPresentation& presentation);

		base::Tweakable::TweakState _SelectionTweakStateLocked(const std::shared_ptr<base::GuiElement>& target, StationAccess access, const InputPresentation& presentation) const;

		void _ResolveHoverPath2d(std::vector<std::weak_ptr<base::GuiElement>>& outPath, StationAccess access, const InputPresentation& presentation);

		void _SelectAllLocalLocked(StationAccess access, const InputPresentation& presentation);

		std::optional<actions::ActionResult> _RouteSettingsTouch(actions::TouchAction action, StationAccess access, const InputPresentation& presentation);

		actions::ActionResult _OnPointerMove(actions::TouchMoveAction action, StationAccess access, const InputPresentation& presentation);

		actions::ActionResult _OnPointerAction(actions::TouchAction action, StationAccess access, const InputPresentation& presentation);

		static std::shared_ptr<base::GuiElement> _ChildFromPathLocked(const std::vector<unsigned char>& path, StationAccess access);
		static std::shared_ptr<base::GuiElement> _ChildFromPath(const std::vector<unsigned char>& path, StationAccess access);
		bool _HasSelection(StationAccess access) const;
		engine::QuantisationInteractionContext _InteractionContext(StationAccess access, const InputPresentation& presentation) const;
		static bool _HasSelectionLocked(StationAccess access);

		engine::Quantiser* _quantiser = nullptr; // Scene-owned services; bound before input/resources.
		engine::QuantiserController* _interaction = nullptr;
		actions::ActionUndoHistory _undoHistory;
		InputCommands _inputCommands;
		TakeDitchTarget _takeDitchTarget;
		gui::PointerInteraction _pointer;
		std::optional<Time> _lastPanelAnimationTime;
		base::SelectDepth _viewMode = base::DEPTH_STATION;
		bool _isSceneTouching = false;
		bool _ctrlHeld = false;
		bool _spaceHeld = false;
		std::array<bool, 256u> _learnKeysDown{};
		utils::Position2d _cursorPos{};
		bool _cameraPointerPreservesIdle = false;
		std::weak_ptr<base::GuiElement> _hoverElement3d;
		std::vector<unsigned char> _hoverPath3d;
		std::vector<unsigned char> _lastLoggedHoverPath;
		std::string _quantisationFeedback;
		std::shared_ptr<gui::GuiPopup> _stationDeleteDialog;
		std::shared_ptr<gui::GuiPopup> _takeDitchDialog;
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
		std::function<bool(int, utils::Position2d)> _beginRelativePointer;
		std::function<void(int)> _endRelativePointer;
		std::function<float()> _logicalPixelScale;
		graphics::Camera _camera;
		std::unique_ptr<VstRackControls> _racks;
		// Host references above and in Scene are released explicitly before Scene member destruction.
		std::unique_ptr<midi::LoopGridEditor> _editor;
	};
}
