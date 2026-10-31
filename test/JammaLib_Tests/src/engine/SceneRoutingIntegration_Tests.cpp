#include "../gui/WorkspaceTestAccess.h"
#include "gtest/gtest.h"
#include "../../TestPaths.h"
#include <algorithm>
#include <bitset>
#include <filesystem>
#include <fstream>
#include "engine/Scene.h"
#include "gui/GuiScrollPanel.h"
#include "gui/SceneSelector.h"
#include "graphics/Window.h"
#include "io/NativeMidiSidecar.h"
#include "io/IoSessionExporter.h"
#include "utils/StringUtils.h"

namespace gui
{
    class GuiMainPanelTestAccess
    {
    public:
        static std::shared_ptr<GuiButton> RigSaveButton(GuiMainPanel& panel) { panel.SetPage(SettingsPage::Session); return panel._saveRigButton; }
        static std::shared_ptr<GuiButton> OrdinaryRigSave(GuiMainPanel& panel) { return panel._ordinaryRigSaveButton; }
        static std::shared_ptr<GuiButton> RigSaveChoice(GuiMainPanel& panel, unsigned int choice) { return panel._additionalRigSaveButtons.at(choice); }

    };
}
namespace engine
{
    class NativeKeyboardWindow : public graphics::Window
    {
    public:
        NativeKeyboardWindow(Scene& scene, resources::ResourceLib& resources) : Window(scene, resources) {}
        std::bitset<256u> NativeKeys;
    protected:
        // Direct WindowProcedure calls do not update the OS queued key state.
        bool _IsNativeKeyDown(unsigned int key, bool) const override
        { return key < NativeKeys.size() && NativeKeys.test(key); }
    };

    class PressPickWindow : public graphics::Window
    {
    public:
        PressPickWindow(Scene& scene, resources::ResourceLib& resources) : Window(scene, resources) {}
        std::vector<unsigned char> PressPath;
        utils::Position2d PickPosition{};
        unsigned int PickCount = 0;
    protected:
        std::optional<unsigned int> _PickPointerPress(utils::Position2d position) override
        {
            PickPosition = position;
            ++PickCount;
            if (PressPath.empty()) return 0u; // Cleared picker pixel.
            unsigned int objectId = 0;
            for (unsigned int segment = 0; segment < 3u; ++segment)
                objectId |= (segment < PressPath.size() ? PressPath[segment] : 0xffu) << (16u - 8u * segment);
            return objectId;
        }
    };

    class RemoteSelectionStation : public Station
    {
    public:
        RemoteSelectionStation(StationParams params, audio::AudioMixerParams mixer) : Station(params, mixer) {}
        bool IsRemote() const noexcept override { return true; }
    };

    class SourceLossIntegrationTestAccess
    {
    public:
		static unsigned int InteractionSampleRate(Scene& scene)
		{
			std::scoped_lock lock(scene._sceneMutex);
			return scene._workspace->InteractionContextLocked({ scene._sceneMutex, scene._stations }, scene._WorkspaceInputPresentation()).SampleRate;
		}
		static std::shared_ptr<base::GuiElement> SelectionHandle(Scene& scene) { return scene._workspace->SelectionPanel()->TryGetChild(1u); }
        static std::shared_ptr<gui::GuiMainPanel> SettingsPanel(Scene& scene) { return scene._workspace->MainPanel(); }
        static std::vector<unsigned char> CurrentHover(Scene& scene) { return scene._workspace->CurrentHover(); }
		struct MidiToken { std::uint64_t SceneId, OperationId, Revision; std::vector<io::UserConfig::MidiSettings> Desired; };
		static bool Quitting(const Scene& scene) { return scene._isSceneQuitting.load(std::memory_order_acquire); }
		static void PrepareMidiDraft(Scene& scene)
		{
			scene._workspace->Settings().SetMidiDraft({{"Keys", true, "keys"}});
			scene._operation.InitializeCandidates(scene._workspace->Settings().AudioDraft(), scene._workspace->Settings().MidiDraft());
			scene._operation._presentation.PagesChanged = true;
			scene._workspace->ApplyControlPresentation(scene._operation.TakePresentation(), scene.shared_from_this());
		}
		static void ToggleMidi(Scene& scene, bool enabled)
		{ scene._workspace->Settings().MidiDevice(0u)->SetToggleState(enabled ? gui::GuiToggleParams::TOGGLE_ON : gui::GuiToggleParams::TOGGLE_OFF, false); }
		static bool MidiEnabled(const Scene& scene) { return scene._workspace->Settings().MidiDraft().at(0u).Enabled; }
		static bool MidiOperation(const Scene& scene) { return scene._operation.Current() == SessionControl::Midi; }
		static MidiToken TakeMidiRequest(Scene& scene)
		{
			const auto request = scene._operation._pendingMidiDeviceRequest.exchange({}, std::memory_order_acq_rel);
			EXPECT_TRUE(request);
			return request ? MidiToken{request->SceneId, request->OperationId, request->DraftRevision, request->Desired} : MidiToken{};
		}
		static void CompleteMidi(Scene& scene, const MidiToken& token, unsigned int mismatch = 0u)
		{
			scene._operation._completedMidiDeviceRequest.store(std::make_shared<const SessionControl::MidiDeviceCompletion>(SessionControl::MidiDeviceCompletion{
				token.SceneId + (mismatch == 1u), token.OperationId + (mismatch == 2u), token.Revision + (mismatch == 3u),
				"Test-owned completion", true, true, token.Desired}), std::memory_order_release);
		}
		static std::uint64_t AudioRevision(const Scene& scene) { return scene._workspace->Settings().AudioDraftRevision(); }
		static void FocusAudioText(Scene& scene, const std::string& text)
		{
			scene._workspace->MainPanel()->SetPage(gui::SettingsPage::Audio);
			auto field = scene._workspace->Settings().AudioInput(0u);
			ASSERT_TRUE(scene._workspace->RequestFocus(field));
			field->SetText(text, false);
		}
		static SessionControl::Presentation AudioCompletion(Scene& scene, io::UserConfig::AudioSettings settings, std::uint64_t revision)
		{
			scene._operation._presentation.AudioAccepted = SessionControl::Candidate<io::UserConfig::AudioSettings>{std::move(settings), revision};
			scene._operation._presentation.PagesChanged = true;
			return scene._operation.TakePresentation();
		}
		static void ApplyPresentation(Scene& scene, SessionControl::Presentation presentation)
		{ scene._workspace->ApplyControlPresentation(std::move(presentation), scene.shared_from_this()); }
		static bool ControlIdle(const Scene& scene) { return scene._operation.Current() == SessionControl::None && !scene._operation.HoldsWriter(); }
		static bool InputGateOpen(Scene& scene) { const auto rig = scene.AcceptedRigSnapshot(); return rig && scene._inputSubsystem->TryAcceptUiRigTriggerInput(rig->Revision); }
        static utils::Position2d CanvasPoint(Scene& scene, utils::Position2d offset = {})
        {
            // The former (560,300) lies inside stacked Inputs at 800x600.
            // These tests need actual free canvas before supplying a picker path.
            const auto point = utils::Position2d{560,200} + offset;
            EXPECT_TRUE(utils::Size2d::RectTest(scene._sizeParams.Size, point));
            EXPECT_FALSE(scene._workspace->Hud()->CapturesPointer(scene._workspace->Hud()->GlobalToLocal(point)));
            EXPECT_FALSE(scene._workspace->SelectionPanel()->RouteHitTest(scene._workspace->SelectionPanel()->GlobalToLocal(point)));
            EXPECT_FALSE(scene._workspace->MainPanel()->RouteHitTest(scene._workspace->MainPanel()->GlobalToLocal(point)));
            return point;
        }
        static std::shared_ptr<gui::GuiButton> RigSaveButton(Scene& scene)
        { return gui::GuiMainPanelTestAccess::RigSaveButton(*scene._workspace->MainPanel()); }
        static std::shared_ptr<gui::GuiButton> RigSaveChoice(Scene& scene, unsigned int choice)
        { return gui::GuiMainPanelTestAccess::RigSaveChoice(*scene._workspace->MainPanel(), choice); }
        static std::shared_ptr<gui::GuiButton> OrdinaryRigSave(Scene& scene)
        { return gui::GuiMainPanelTestAccess::OrdinaryRigSave(*scene._workspace->MainPanel()); }
        static std::shared_ptr<gui::GuiButton> SessionButton(Scene& scene, unsigned int command)
        {
            for (const auto& entry : scene._workspace->Settings().SettingsEntries())
                if (entry.Command == command) return std::dynamic_pointer_cast<gui::GuiButton>(entry.Control);
            return {};
        }
        static void PickDirectory(Scene& scene, std::function<std::wstring(const std::wstring&)> pick)
        { scene._operation._pickSessionDirectory = std::move(pick); }
        static void Save(Scene& scene, bool choose = false) { scene._operation.RequestSave(choose, false, scene._ControlEffects()); }
        static void FinishPickedSave(Scene& scene, bool success, std::wstring folder, std::wstring path)
        { scene._operation.CompleteSessionSave(success, path, "test failure", std::move(folder), false); scene._operation.Begin(SessionControl::None); }
        static std::wstring PendingFolder(Scene& scene) { return scene._operation._pendingSaveFolder; }
        static std::wstring SaveFolder(Scene& scene) { return scene._operation.SessionSaveFolder(); }
        static std::string FileStatus(Scene& scene) { return scene._operation.TakePresentation().FileStatus; }
        static void SetFileControls(Scene& scene, bool enabled) { scene._workspace->Settings().SetSessionEnabled(enabled, enabled); scene._workspace->MainPanel()->SetRigSaveEnabled(enabled); }
        static gui::RuntimeDialogDecision ResetDecision(Scene& scene) { return scene._resetAllDecision; }
        static void ResolveReset(Scene& scene, bool accept) { ASSERT_TRUE(scene._resetAllDecision); *scene._resetAllDecision = accept; scene.AdvanceUiAnimations(); }
        static unsigned long ClockLength(Scene& scene) { return scene._quantisation.Clock()->SeedSourceLength(); }
        static void SetClock(Scene& scene) { scene._quantisation.Clock()->SetSeedSourceLength(96000ul); }
        static void SetResetQuantisationState(Scene& scene)
        {
            scene._quantisation.SetGlobalPhaseOffsetSamps(321, scene._stations);
            scene._quantisation.SetDefaultMidiQuantisationFraction(midi::MidiQuantisationFraction::Sixth, scene._stations, false);
            scene._SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::All);
        }
        static std::int32_t GlobalPhase(Scene& scene) { return scene._quantisation.GlobalPhaseOffsetSamps(); }
        static midi::MidiQuantisationFraction DefaultFraction(Scene& scene) { return scene._quantisation.DefaultMidiQuantisationFraction(); }
        static io::JamFile::GlobalMidiQuantState GlobalQuantisationState(Scene& scene) { return scene._globalMidiQuantState; }

        static bool ReclockArmed(Scene& scene) { return scene._quantisation.IsArmedForReclock(); }
        static bool RigSaveRequested(const Scene& scene) { return scene._operation._rigSaveRequested; }
        static void ChooseAudio(Scene& scene, int index) { scene._workspace->Settings().AudioDevice()->SetSelectedIndex(index, true); }
        static void ChooseSampleRate(Scene& scene, int index) { scene._workspace->Settings().AudioSampleRate()->SetSelectedIndex(index, true); }
        static std::string SampleRateText(const Scene& scene) { return scene._workspace->Settings().AudioSampleRate()->SelectedText(); }
        static void RefreshDevicePage(Scene& scene)
        {
            scene._operation._presentation.PagesChanged = true;
            scene._workspace->ApplyControlPresentation(scene._operation.TakePresentation(), scene.shared_from_this());
        }
        static io::UserConfig::AudioSettings SubmittedAudio(const Scene& scene) { return scene._operation._submittedAudio.Value; }
		static bool ScheduleAudioRefresh(Scene& scene, SessionControl::Eligibility eligibility = {}, bool inventoryBusy = false)
		{
			SessionControl::Effects effects;
			effects.CheckEligibility = [eligibility] { return eligibility; };
			return scene._operation._ScheduleWindowOperation(std::chrono::steady_clock::now(), scene._audioEngine->GetStreamParams(), effects, inventoryBusy);
		}
		static bool AudioRefreshPending(const Scene& scene) { return scene._operation._audioRefreshRequested.load(); }
		static bool AudioRefreshOperation(const Scene& scene) { return scene._operation.Current() == SessionControl::AudioRefresh; }
		static bool ScheduleRefreshWithOpenStream(Scene& scene) { return scene._operation._ScheduleAudioRefresh(true); }
		static bool ManualAudioRescanOperation(const Scene& scene) { return scene._operation.Current() == SessionControl::AudioRescan; }
		static void RequestManualAudioRescan(Scene& scene) { scene._operation.RefreshAudioFromWindow(); }
		static SessionControl::Presentation DevicePresentation(Scene& scene) { return scene._operation.TakePresentation(); }
		static io::UserConfig::AudioSettings ConfiguredAudio(const Scene& scene) { return scene._userConfig.Audio; }
		static void SetHostInventory(Scene& scene, audio::AsioInventory inventory) { scene._audioEngine->_asioInventory = std::move(inventory); }
		static bool ValidateAudioDraft(Scene& scene, bool hasTakes, std::string& error)
		{
			SessionControl::Effects effects;
			effects.CheckEligibility = [hasTakes] { return SessionControl::Eligibility{false, hasTakes, false}; };
			return scene._operation._ValidateAudioDraft(error, effects);
		}
		static void FinishTestAudioRefresh(Scene& scene) { scene._operation.Begin(SessionControl::None); }
		static void AcceptRescannedAudioDraft(Scene& scene, const io::UserConfig::AudioSettings& settings)
		{
			scene._operation._admittedAudio.Revision = scene._workspace->Settings().AudioDraftRevision();
			scene._operation._submittedAudio = {scene._workspace->Settings().AudioDraft(), scene._operation._admittedAudio.Revision};
			scene._operation._AcceptAudioDraft(settings, true, true);
			scene._operation._presentation.PagesChanged = true;
			scene._workspace->ApplyControlPresentation(scene._operation.TakePresentation(), scene.shared_from_this());
		}
		static io::UserConfig::AudioSettings FailAudioRefreshWithoutHardware(Scene& scene, audio::AudioStreamParams actual = {})
		{
			// The existing uncertain-backend guard rejects before constructing any
			// RtAudio object. Exercise request choice/failure scheduling hermetically.
			scene._audioEngine->_backendLifetimeUncertain = true;
			std::string error;
			scene._operation._ApplyAudioCandidate(actual, std::chrono::steady_clock::now(), {scene._sceneMutex, scene._stations}, {}, error);
			EXPECT_FALSE(error.empty());
			scene._audioEngine->_backendLifetimeUncertain = false;
			return scene._operation._admittedAudio.Value;
		}
		static void SetUnsubmittedAudioDraft(Scene& scene, io::UserConfig::AudioSettings draft)
		{ scene._operation._submittedAudio = {std::move(draft), 123u}; }
        static void AudioChannels(Scene& scene, unsigned int input, double value) { scene._workspace->Settings().AudioInput(input)->SetValue(value, true); }
        static std::shared_ptr<Station> RemovalTarget(Scene& scene)
        { std::scoped_lock lock(scene._sceneMutex); return scene._stationManagement.Target; }
        static std::shared_ptr<gui::GuiButton> RemovalButton(Scene& scene, unsigned int ordinal)
        {
            if (!scene._workspace->StationRemovalConfirmation()) return {};
            for (unsigned int index = 0u; index < 256u; ++index)
            {
                const auto child = scene._workspace->StationRemovalConfirmation()->TryGetChild(static_cast<unsigned char>(index));
                if (!child) break;
                if (const auto button = std::dynamic_pointer_cast<gui::GuiButton>(child))
                    if (ordinal-- == 0u) return button;
            }
            return {};
        }
        static void BeginDeviceWriter(Scene& scene)
        { scene._operation._writer = std::unique_lock(scene._operation.LifecycleMutex()); scene._operation.Begin(SessionControl::Save); }
        static void EndDeviceWriter(Scene& scene)
        { scene._operation.Begin(SessionControl::None); scene._operation.ReleaseWriter(); }
        static std::unique_lock<std::mutex> ExcludeJob(Scene& scene)
        { return std::unique_lock(scene._operation.LifecycleMutex()); }
        static std::string StartedAudioSelection(Scene& scene,
            const audio::AsioInventory& inventory, const audio::AudioStreamParams& params)
        {
            scene._operation._deviceInventory = inventory;
            scene._workspace->Settings().SetAudioDraftFromStream(params);
            scene._operation._presentation.PagesChanged = true;
            scene._workspace->ApplyControlPresentation(scene._operation.TakePresentation(), scene.shared_from_this());
            return scene._workspace->Settings().AudioDevice()->SelectedText();
        }
        static io::UserConfig::AudioSettings AudioDraft(const Scene& scene) { return scene._workspace->Settings().AudioDraft(); }
        static void WhileSceneLocked(Scene& scene, const std::function<void()>& callback)
        {
            std::scoped_lock lock(scene._sceneMutex);
            callback();
        }
        static bool ControlHeld(const Scene& scene) { return scene._workspace->CtrlHeld(); }
        static void SetDepth(Scene& scene, unsigned int depth) { scene._workspace->UpdateSelectDepth(depth, { scene._sceneMutex, scene._stations }, scene._WorkspaceInputPresentation()); }
        static void AddStation(Scene& scene, const std::shared_ptr<Station>& station)
        {
            std::scoped_lock lock(scene._sceneMutex);
            scene._stations.push_back(station);
        }
        static bool SelectionPreviewActive(const Scene& scene) { return scene._workspace->BackgroundSelectionPreviewActive({ scene._sceneMutex, scene._stations }, scene._WorkspaceInputPresentation()); }
        static void SelectTakeDepth(Scene& scene) { scene._workspace->UpdateSelectDepth(Scene::VIEW_LOOPTAKE, { scene._sceneMutex, scene._stations }, scene._WorkspaceInputPresentation()); }
        static void RefreshRackVisibility(Scene& scene)
        {
            std::scoped_lock lock(scene._sceneMutex);
            gui::WorkspaceTestAccess::SetProjection(*scene._workspace, glm::mat4(1.0f));
            scene._workspace->UpdateRackVisibilityLocked(scene._stations, scene._workspace->Editor().IsEngaged());
        }
        static bool ItemMutePressed(const Scene& scene) { return scene._workspace->IsSelectionMutePressed(); }
        static bool ItemMutePainting(const Scene& scene) { return scene._workspace->IsPaintingMute(); }
        static bool BackgroundPreviewActive(const Scene& scene) { return scene._workspace->BackgroundPreviewActive({ scene._sceneMutex, scene._stations }, scene._WorkspaceInputPresentation()); }
        static graphics::Camera::Pose CameraPose(const Scene& scene) { return gui::WorkspaceTestAccess::Camera(*scene._workspace).CurrentPose(); }
        static bool BackgroundOwned(const Scene& scene) { return scene._workspace->OwnsBackgroundClick(); }
        static bool CameraBackgroundActive(const Scene& scene) { return gui::WorkspaceTestAccess::Camera(*scene._workspace).IsBackgroundDragging(); }
        static bool CameraPointerHeld(const Scene& scene) { return gui::WorkspaceTestAccess::Camera(*scene._workspace).IsBackgroundPointerHeld(); }
        static bool ApplyMuteBoundary(Scene& scene)
        {
            scene._audioEngine->CloseCallbackAdmission();
            return scene._audioEngine->ApplyPendingRigWhileQuiescent();
        }
        static void Confirm(Scene& scene, const midi::MidiConnectionResult& previous,
            const midi::MidiConnectionResult& refreshed, const std::vector<std::string>& names)
        {
            scene._operation.ConfirmMidiConnectionChange(previous, refreshed, names, { scene._sceneMutex, scene._stations });
        }
        static void RemoteGrid(Scene& scene, const engine::RemoteTransportGeometry& geometry, std::int64_t origin)
        { std::scoped_lock lock(scene._sceneMutex); scene._quantisation.SetRemoteMidiGrid(geometry, origin, scene._stations); }
        static bool Export(Scene& scene, const std::wstring& directory)
        {
            return io::IoSessionExporter::ExportSessionToDirectory(scene.SnapshotStations(), scene.AcceptedRigSnapshot(),
                scene._quantisation, scene._globalMidiQuantState, scene._transportOffsetLoopFrac,
                scene._userConfig, scene._audioEngine->GetStreamParams(), nullptr, scene._sceneMutex, nullptr, directory);
        }
        static void Manage(Scene& scene, bool prepend, std::shared_ptr<Station> remove = {})
        {
			// Existing topology tests have no render pump; finish the previous
			// presentation interval before their next independent operation.
			FinishMotion(scene);
			scene._RequestStationControl({prepend, std::move(remove)});
		}
		static void FinishMotion(Scene& scene)
		{ std::scoped_lock lock(scene._sceneMutex); for (const auto& station : scene._stations) station->FinishPresentationMove(); }
		static bool ControlsAvailable(Scene& scene)
		{ std::scoped_lock lock(scene._sceneMutex); return scene._StationManagementAvailableLocked(); }
		static bool ConfirmationOpen(const Scene& scene) { return scene._workspace->PopupOpen(); }
		static void RequestTakeDitch(Scene& scene, const std::shared_ptr<Station>& station, const std::shared_ptr<LoopTake>& take)
		{ scene._RequestTakeDitch({ false, {}, station, take }); }
		static void AdvanceAcceptedRevision(Scene& scene)
		{
			std::scoped_lock lock(scene._sceneMutex);
			const auto accepted = scene._rigCoordinator.Accepted();
			const auto updated = scene._rigCoordinator.RefreshRuntimeAvailability(accepted->AvailableAdcChannels, accepted->AvailableMidiDevices);
			ASSERT_TRUE(updated);
			ASSERT_GT(updated->Revision, accepted->Revision);
			scene._audioEngine->PublishPendingRigSnapshot(updated);
			scene._inputSubsystem->PublishRigInputDispatch(updated);
		}
        static void ConfirmRemoval(Scene& scene) { scene._workspace->CloseTopPopup(); scene._AcceptStationRemoval(true); }
        static bool Busy(Scene& scene) { std::scoped_lock lock(scene._sceneMutex); return scene._stationManagement.Busy(); }
        static void Recover(Scene& scene) { scene._PumpSourceLossRecovery(); }
        static bool RecoveryGateIsClosed(Scene& scene)
        {
            std::scoped_lock lock(scene._sceneMutex);
            return scene._operation.SourceRecoveryGateClosed();
        }
		static std::shared_ptr<gui::GuiNumericInput> FocusSetting(Scene& scene, const std::string& text)
		{
			auto field = scene._workspace->Settings().Offset();
			// Focus manager and text edit state belong to the window owner. Commit
			// remains deferred until actual Scene focus loss invokes FinalizeEdits.
			EXPECT_TRUE(scene._workspace->RequestFocus(field));
			field->SetText(text, false);
			return field;
		}
		static unsigned int ForcedChannel(const Scene& scene) { return scene._inputSubsystem->ForcedChannelOverride(); }
        static void OpenKeyboardModal(Scene& scene) { scene._workspace->OpenPopup(std::make_shared<gui::GuiPopup>()); }
        static void CloseKeyboardModal(Scene& scene) { scene._workspace->ClosePopups(); }

		static void StaleProjection(Scene& scene) { gui::WorkspaceTestAccess::SetProjection(*scene._workspace, glm::mat4(0.0f)); }
		static bool HasCurrentProjection(const Scene& scene)
		{
			const auto aspect = static_cast<float>(scene._sizeParams.Size.Width) / scene._sizeParams.Size.Height;
			const auto current = gui::WorkspaceTestAccess::Camera(*scene._workspace).Projection(aspect, gui::Workspace::StationCentre(scene._stations)) * gui::WorkspaceTestAccess::Camera(*scene._workspace).ViewMatrix();
			return scene._workspace->ViewProjection() == current;
		}
		static std::shared_ptr<gui::GuiHud> PrepareHud(Scene& scene)
		{
			std::scoped_lock lock(scene._sceneMutex);
			const bool alreadyClosed = (scene._audioEngine->_callbackAdmission.load(std::memory_order_acquire)
				& audio::AudioHost::_CallbackAdmissionClosed) != 0u;
			scene._audioEngine->CloseCallbackAdmission();
			scene._audioEngine->PublishPendingRigSnapshot(scene._rigCoordinator.Accepted());
			scene._audioEngine->ApplyPendingRigWhileQuiescent();
			if (!alreadyClosed) scene._audioEngine->ReopenCallbackAdmission();
			EXPECT_EQ(scene._rigCoordinator.Accepted()->Revision, scene._audioEngine->AppliedRigRevision());
			// Supply only the liveness hint. Publication still runs through the real
			// producer gate, stopped-reader barrier and input acknowledgement.
			AudioHeartbeat(scene);
			scene._workspace->Hud()->SetCableRevealHeld(true);
			scene._workspace->Hud()->SetStationAnchors({ {0u, scene._stations[0]->Name(), glm::dvec2{600, 350}, glm::vec4{1.0f}},
				{1u, scene._stations[1]->Name(), glm::dvec2{800, 350}, glm::vec4{1.0f}} });
			return scene._workspace->Hud();
		}
		static void AudioHeartbeat(Scene& scene)
		{ scene._audioEngine->_audioCallbackHeartbeat.fetch_add(1u); }
		static gui::RoutingEditAvailability RoutingAvailability(Scene& scene)
		{ std::scoped_lock lock(scene._sceneMutex); return scene._RoutingEditAvailability(); }
		static void AudioBoundary(Scene& scene)
		{
			std::scoped_lock lock(scene._sceneMutex);
			const bool alreadyClosed = (scene._audioEngine->_callbackAdmission.load(std::memory_order_acquire)
				& audio::AudioHost::_CallbackAdmissionClosed) != 0u;
			scene._audioEngine->CloseCallbackAdmission();
			scene._audioEngine->ApplyPendingRigWhileQuiescent();
			if (!alreadyClosed) scene._audioEngine->ReopenCallbackAdmission();
		}
        static void CloseRecoveryGate(Scene& scene)
        {
            std::scoped_lock lock(scene._sceneMutex);
            scene._audioEngine->CloseCallbackAdmission();
            scene._operation.CloseSourceRecoveryGateLocked();
        }
        static void StartRecording(Scene& scene, const std::shared_ptr<Trigger>& trigger,
            const io::UserConfig& cfg)
        {
            // The real job runner also consumes structural commands. Keep the
            // test's simulated audio/job handoff in the same critical section
            // as Scene's production structural pump.
            std::scoped_lock lock(scene._sceneMutex);
            trigger->AddInputChannel(0u);
            base::Action action;
            trigger->QueueExternalControlAction(true, true, action);
            trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
            trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
            trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
        }
    };
}

class SceneRoutingIntegrationTest : public testing::Test
{
protected:
	static void LayoutHud(const std::shared_ptr<base::GuiElement>& element)
	{
		if (auto stack = std::dynamic_pointer_cast<gui::GuiStackPanel>(element)) stack->ComputeLayout();
		if (auto scroll = std::dynamic_pointer_cast<gui::GuiScrollPanel>(element))
			if (scroll->Content()) LayoutHud(scroll->Content());
		for (unsigned int i = 0; i < 256; ++i)
		{ auto child = element->TryGetChild(static_cast<unsigned char>(i)); if (!child) break; LayoutHud(child); }
	}
	static void Settle(const std::shared_ptr<engine::Scene>& scene)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while ((!scene->CaptureCommittedRig() || engine::SourceLossIntegrationTestAccess::Busy(*scene)) && std::chrono::steady_clock::now() < deadline)
		{ engine::SourceLossIntegrationTestAccess::AudioBoundary(*scene); scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
	}
	static io::JamFile::Station Station(std::string name)
	{
		io::JamFile::Station station{};
		station.Name = std::move(name);
		return station;
	}

	static io::RigFile::Trigger Trigger(std::string name, std::string target)
	{
		io::RigFile::Trigger trigger{};
		trigger.Id = name + "-id";
		trigger.Name = std::move(name);
		if (!target.empty()) trigger.StationTarget = std::move(target);
		return trigger;
	}

	static std::shared_ptr<engine::Scene> FreshScene(std::vector<io::JamFile::Station> stations,
		std::vector<io::RigFile::Trigger> triggers,
		std::function<bool(const io::RigFile&)> saveRig = [](const io::RigFile&) { return true; },
		std::vector<io::JamFile::TriggerHistory> histories = {},
		std::function<std::chrono::steady_clock::time_point()> routingNow = [] {
			return std::chrono::steady_clock::time_point{std::chrono::seconds(1)}; })
	{
		io::JamFile jam{};
		jam.Stations = std::move(stations);
		jam.TriggerHistories = std::move(histories);
		auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson)).value();
		rig.Triggers = std::move(triggers);
		for (const auto& trigger : rig.Triggers)
			for (const auto& id : trigger.CaptureInputs.Midi)
				if (std::none_of(rig.User.Midi.Devices.begin(), rig.User.Midi.Devices.end(),
					[&id](const auto& device) { return device.Id == id; }))
					rig.User.Midi.Devices.push_back({ id, true, id });
		engine::SceneParams params({ "" }, {}, { 640u, 480u });
		// No audio callback runs here; test-owned time keeps routing independent
		// of layout, gesture duration and machine scheduling.
		params.RoutingEditNow = std::move(routingNow);
		auto scene = engine::Scene::FromFile(params, std::move(jam), std::move(rig), L"", std::move(saveRig));
		return scene.has_value() ? scene.value() : nullptr;
	}

	static std::vector<std::pair<std::string, std::size_t>> Memberships(const std::shared_ptr<engine::Scene>& scene)
	{
		std::vector<std::pair<std::string, std::size_t>> result;
		const auto stations = scene->SnapshotStations();
		const auto rig = scene->AcceptedRigSnapshot();
		for (std::size_t stationIndex = 0u; stationIndex < stations.size(); ++stationIndex)
		{
			const auto count = rig ? static_cast<std::size_t>(std::count_if(rig->Triggers.begin(),
				rig->Triggers.end(), [stationIndex](const engine::RigSnapshotTrigger& trigger)
				{
					return trigger.StationIndex == stationIndex;
				})) : 0u;
			result.emplace_back(stations[stationIndex]->Name(), count);
		}
		return result;
	}
};

TEST_F(SceneRoutingIntegrationTest, StartedAudioSelectionShowsResolvedDeviceAndNegotiatedGeometry)
{
	io::RigFile rig{};
	rig.Name = "Audio selection";
	rig.User.Audio = { "default", 44100u, 64u, 10u, 20u, 4u, 8u, 8u };
	io::JamFile jam{};
	jam.Stations = { Station("Mic") };
	engine::SceneParams params({ "" }, {}, { 640u, 480u });
	unsigned int saves = 0u;
	auto created = engine::Scene::FromFile(params, jam, rig, L"",
		[&saves](const io::RigFile&) { ++saves; return true; });
	ASSERT_TRUE(created.has_value());
	auto scene = *created;
	audio::AsioInventory inventory;
	inventory.Devices = {{ 0u, "Primary ASIO device", true, "", 18u, 20u, {44100u, 48000u}, 48000u }};
	const audio::AudioStreamParams actual{ "Primary ASIO device", 48000u, 2u, 128u, 177u, 177u, 8u, 8u };
	EXPECT_EQ("Primary ASIO device", engine::SourceLossIntegrationTestAccess::StartedAudioSelection(*scene, inventory, actual));
	const auto draft = engine::SourceLossIntegrationTestAccess::AudioDraft(*scene);
	EXPECT_EQ(48000u, draft.SampleRate);
	EXPECT_EQ(128u, draft.BufSize);
	EXPECT_EQ(2u, draft.NumBuffers);
	EXPECT_EQ(8u, draft.NumChannelsIn);
	EXPECT_EQ(8u, draft.NumChannelsOut);
	ASSERT_TRUE(scene->AcceptedRigSnapshot());
	EXPECT_EQ("default", scene->AcceptedRigSnapshot()->Rig.User.Audio.Name);
	EXPECT_EQ(64u, scene->AcceptedRigSnapshot()->Rig.User.Audio.BufSize);
	EXPECT_EQ(0u, saves);
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SampleRateChoicesFollowDeviceAndSupersedePendingRate)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		audio::AsioInventory inventory;
		inventory.Devices = {{0u, "Device A", true, "", 2u, 2u, {44100u, 48000u}, 44100u},
			{1u, "Device B", true, "", 2u, 2u, {32000u, 96000u}, 96000u},
			{2u, "Device C", true, "", 2u, 2u, {44100u, 48000u}, 48000u}};
		const audio::AudioStreamParams actual{"Device A", 44100u, 2u, 128u, 0u, 0u, 2u, 2u};
		Access::StartedAudioSelection(*scene, inventory, actual);
		EXPECT_EQ("44100 Hz", Access::SampleRateText(*scene));
		Access::ChooseSampleRate(*scene, 1);
		EXPECT_EQ(48000u, Access::SubmittedAudio(*scene).SampleRate);
		const auto pendingRevision = Access::AudioRevision(*scene);
		Access::ChooseAudio(*scene, 2);
		EXPECT_GT(Access::AudioRevision(*scene), pendingRevision);
		// C also supports 48000 Hz, but a pending edit on A must not carry over.
		EXPECT_EQ(44100u, Access::SubmittedAudio(*scene).SampleRate);
		Access::RefreshDevicePage(*scene);
		EXPECT_EQ("44100 Hz", Access::SampleRateText(*scene));
		Access::ChooseSampleRate(*scene, 1);

		Access::ChooseAudio(*scene, 1);
		EXPECT_EQ("Device B", Access::SubmittedAudio(*scene).Name);
		EXPECT_EQ(96000u, Access::SubmittedAudio(*scene).SampleRate);
		// The old dropdown can still send an action until the next UI refresh.
		Access::ChooseSampleRate(*scene, 0);
		EXPECT_EQ(96000u, Access::SubmittedAudio(*scene).SampleRate);
		Access::RefreshDevicePage(*scene);
		EXPECT_EQ("96000 Hz", Access::SampleRateText(*scene));
		Access::ChooseSampleRate(*scene, 0);
		EXPECT_EQ(32000u, Access::SubmittedAudio(*scene).SampleRate);
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, DeviceWithIncompatibleRateKeepsRunningRateVisibleWithTakes)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		audio::AsioInventory inventory;
		inventory.Devices = {{0u, "Device A", true, "", 2u, 2u, {44100u}, 44100u},
			{1u, "Device B", true, "", 2u, 2u, {48000u}, 48000u}};
		const audio::AudioStreamParams actual{"Device A", 44100u, 2u, 128u, 0u, 0u, 2u, 2u};
		Access::StartedAudioSelection(*scene, inventory, actual);
		auto station = scene->SnapshotStations().front();
		station->AddTake();
		scene->CommitChanges();
		Access::ChooseAudio(*scene, 1);
		EXPECT_EQ(44100u, Access::SubmittedAudio(*scene).SampleRate);
		Access::RefreshDevicePage(*scene);
		EXPECT_EQ("44100 Hz (unavailable)", Access::SampleRateText(*scene));
		const auto revision = Access::AudioRevision(*scene);
		Access::ChooseSampleRate(*scene, 1);
		EXPECT_EQ(revision, Access::AudioRevision(*scene));
		EXPECT_EQ(44100u, Access::SubmittedAudio(*scene).SampleRate);
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ChannelControlsUseSelectedDeviceLimits)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		audio::AsioInventory inventory;
		inventory.Devices = {{0u, "Device A", true, "", 4u, 6u, {44100u}, 44100u},
			{1u, "Device B", true, "", 1u, 2u, {44100u}, 44100u}};
		const audio::AudioStreamParams actual{"Device A", 44100u, 2u, 128u, 0u, 0u, 2u, 2u};
		Access::StartedAudioSelection(*scene, inventory, actual);
		Access::AudioChannels(*scene, 0u, 99.0);
		Access::AudioChannels(*scene, 1u, 99.0);
		EXPECT_EQ(4u, Access::AudioDraft(*scene).NumChannelsIn);
		EXPECT_EQ(6u, Access::AudioDraft(*scene).NumChannelsOut);

		Access::ChooseAudio(*scene, 1);
		Access::RefreshDevicePage(*scene);
		Access::AudioChannels(*scene, 0u, 99.0);
		Access::AudioChannels(*scene, 1u, 99.0);
		EXPECT_EQ(1u, Access::AudioDraft(*scene).NumChannelsIn);
		EXPECT_EQ(2u, Access::AudioDraft(*scene).NumChannelsOut);
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, HaloSelectionIncludesMidiOnlyModelsAndClearsOnDeselect)
{
	auto scene = FreshScene({ Station("Keys") }, {});
	ASSERT_TRUE(scene);
	const auto stations = scene->SnapshotStations();
	ASSERT_EQ(1u, stations.size());
	auto station = stations.front();
	auto take = station->AddTake();
	take->Record({}, station->Name(), { 0u }, { "Keys" });
	scene->CommitChanges();
	ASSERT_EQ(1u, take->GetMidiLoops().size());
	auto model = take->GetMidiLoops().front()->Model();
	ASSERT_TRUE(model);
	station->DeSelect();
	take->DeSelect();
	EXPECT_FALSE(scene->HasSelection());

	// A MIDI model can be selected independently of its aggregate take state.
	model->Select();
	EXPECT_TRUE(scene->HasSelection());
	model->DeSelect();
	EXPECT_FALSE(scene->HasSelection());
	take->Select();
	EXPECT_TRUE(scene->HasSelection());
	take->DeSelect();
	EXPECT_FALSE(scene->HasSelection());
	station->Select();
	EXPECT_TRUE(scene->HasSelection());
	station->DeSelect();
	EXPECT_FALSE(scene->HasSelection());
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ConfiguredMissingMidiSourceCreatesEditableTakeThroughActivateControl)
{
    auto configured = Trigger("record", "Keys");
    configured.CaptureInputs.Midi = { "missing" };
    auto scene = FreshScene({ Station("Keys") }, { configured });
    ASSERT_TRUE(scene);
    // This test simulates both owners; exclude the real structural job consumer.
    auto jobExclusion = engine::SourceLossIntegrationTestAccess::ExcludeJob(*scene);
    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    auto station = scene->SnapshotStations().front();
    station->SetAllowedMidiChannels({ 1 });
    // Confirm the real empty inventory, without manually adding a runtime source.
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, {}, {}, {});
    io::UserConfig cfg;
    cfg.Trigger = { 0u, 0u };
    base::Action action;
	std::uint32_t boundary = 0u;
    auto press = [&](bool down) {
        trigger->QueueExternalControlAction(true, down, action);
		station->BeginCaptureAudioBoundary(boundary, boundary, boundary);
        trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt, 0u, boundary);
        trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
        trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
        scene->CommitChanges();
    };
    press(true); press(false);
    ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    ASSERT_EQ(1u, station->GetLoopTakeSnapshot().size());
    auto take = station->GetLoopTakeSnapshot().front();
    EXPECT_TRUE(take->GetLoops().empty());
    ASSERT_EQ(1u, take->GetMidiLoops().size());
    EXPECT_EQ(std::vector<std::string>{ "missing" }, take->MidiLoopDevices());
    trigger->OnTick(utils::Timer::GetTime(), 256u, cfg, std::nullopt);
	boundary += 256u;
    press(true); press(false);
	station->CompleteMidiIngressPump(boundary + 4410u);
    EXPECT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
    EXPECT_GT(take->GetMidiLoops().front()->CompletedLengthForEditor(), 0u);
    EXPECT_EQ(0u, take->GetMidiLoops().front()->EventCount());
    jobExclusion.unlock();
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SuccessfulMidiConnectionLossCancelsCaptureThroughStoppedRecovery)
{
    auto configured = Trigger("record", "Keys");
    configured.CaptureInputs.Audio = { 0u };
    configured.CaptureInputs.Midi = { "lost" };
    auto scene = FreshScene({ Station("Keys") }, { configured });
    ASSERT_TRUE(scene);
    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    ASSERT_TRUE(trigger);
    // This fixture simulates the job/audio owners; exclude the real job runner.
    auto jobExclusion = engine::SourceLossIntegrationTestAccess::ExcludeJob(*scene);
    // Boundary route setup substitutes actual successfully opened capture names.
    trigger->AddMidiInputDevice("lost");
    trigger->AddInputChannel(0u);
    base::Action action;
    io::UserConfig cfg;
    trigger->QueueExternalControlAction(true, true, action);
    trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
    trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
    trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
    ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    midi::MidiConnectionResult previous;
    previous.Connected.push_back({ 0u, "lost", "lost 0" });
    previous.ConnectedRegistryIds = { "lost" };
    midi::MidiConnectionResult failed;
    failed.Inventory.Error = "transient inventory failure";
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, previous, failed, {});
    EXPECT_FALSE(trigger->HasPendingSourceLoss());
    midi::MidiConnectionResult reopened;
    reopened.Connected = previous.Connected;
    reopened.ConnectedRegistryIds = previous.ConnectedRegistryIds;
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, previous, reopened, { "lost" });
    EXPECT_FALSE(trigger->HasPendingSourceLoss());
    engine::SourceLossIntegrationTestAccess::Confirm(*scene, reopened, {}, {});
    ASSERT_TRUE(trigger->HasPendingSourceLoss());
    for (unsigned int step = 0u; step < 4u; ++step)
        engine::SourceLossIntegrationTestAccess::Recover(*scene);
    EXPECT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
    EXPECT_TRUE(trigger->GetTakes().empty());
    jobExclusion.unlock();
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ResetRetiresSixtyFourRestoredHistoryPinsAndAllowsRecording)
{
    struct SidecarCleanup
    {
        std::filesystem::path Path;
        ~SidecarCleanup() { std::error_code error; std::filesystem::remove(Path, error); }
    } sidecar{ TestPaths::FixtureDirectory() / ("jamma-source-loss-" + utils::GetGuid() + ".jammidi") };
    io::NativeMidiSidecar::Stream contents;
    contents.LogicalLength = 100u;
    contents.Events.push_back({ 0u, 0x90u, 60u, 100u });
    {
        std::ofstream output(sidecar.Path, std::ios::binary);
        ASSERT_TRUE(output);
        ASSERT_TRUE(io::NativeMidiSidecar::ToStream(contents, output));
    }
    auto descriptor = Station("restored-reset");
    auto configured = Trigger("restored-trigger", descriptor.Name);
    configured.Id = "restored-trigger-id";
    io::JamFile::TriggerHistory history;
    history.TriggerId = configured.Id;
    for (unsigned int index = 0u; index < 64u; ++index)
    {
        io::JamFile::LoopTake take{};
        take.Name = "restored-" + std::to_string(index);
        // Station::FromFile uses this persisted name as LoopTakeParams::Id.
        take.MidiPlayLength = 100u;
        take.MidiStreams.push_back({ sidecar.Path.string(), 0u, "healthy", 100u, 0u });
        descriptor.LoopTakes.push_back(take);
        history.Takes.push_back({ static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "", take.Name });
    }
    auto scene = FreshScene({ descriptor }, { configured }, {}, { history });
    ASSERT_TRUE(scene);
    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    auto station = scene->SnapshotStations().front();
    ASSERT_EQ(64u, trigger->GetTakes().size());
    ASSERT_EQ(64u, station->GetLoopTakeSnapshot().size());
    const auto externallyRetainedTarget = station->GetLoopTakeSnapshot().front();
    ASSERT_EQ(history.Takes.front().TargetTakeId, externallyRetainedTarget->Id());
    ASSERT_EQ(station, trigger->GetTakes().front().Receiver);
    station->Reset();
    scene->CommitChanges();
    io::UserConfig cfg;
    for (unsigned int step = 0u; step < 130u; ++step)
        engine::SourceLossIntegrationTestAccess::Recover(*scene);
    EXPECT_TRUE(trigger->GetTakes().empty());
    EXPECT_FALSE(trigger->HasPendingSourceLoss());
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::RecoveryGateIsClosed(*scene));
    EXPECT_TRUE(externallyRetainedTarget->IsTriggerCaptureInactive());
    engine::SourceLossIntegrationTestAccess::StartRecording(*scene, trigger, cfg);
    EXPECT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ShutdownPreventsRecoveryFromReopeningAdmission)
{
    auto scene = FreshScene({ Station("shutdown") }, {});
    ASSERT_TRUE(scene);
    engine::SourceLossIntegrationTestAccess::CloseRecoveryGate(*scene);
    scene->Shutdown();
    engine::SourceLossIntegrationTestAccess::Recover(*scene);
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::RecoveryGateIsClosed(*scene));
}

TEST_F(SceneRoutingIntegrationTest, TakeClicksSelectAndToggleMuteWithoutChangingSibling)
{
	auto scene = FreshScene({ Station("Keys") }, {});
	ASSERT_TRUE(scene);
	const auto station = scene->SnapshotStations().front();
	const auto take = station->AddTake();
	const auto sibling = station->AddTake();
	scene->CommitChanges();
	actions::GuiAction view;
	view.ElementType = actions::GuiAction::ACTIONELEMENT_RADIO;
	view.Index = 100u;
	view.Data = actions::GuiAction::GuiInt{ engine::Scene::VIEW_LOOPTAKE };
	scene->OnAction(view);
	std::vector<unsigned char> pickPath;
	for (const auto index : take->GlobalId())
		pickPath.push_back(static_cast<unsigned char>(index + 1u));
	scene->SetHover3d(pickPath, base::Action::MODIFIER_NONE);

	actions::TouchAction click;
	click.Touch = actions::TouchAction::TOUCH_MOUSE;
	click.Position = { -100, -100 }; // Avoid the 2D controls; use the picked take.
	click.Modifiers = base::Action::MODIFIER_NONE;
	click.Index = 0;
	click.State = actions::TouchAction::TOUCH_DOWN;
	scene->OnAction(click);
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_TRUE(take->IsSelected());
	EXPECT_FALSE(sibling->IsSelected());

	click.Index = 1;
	for (const bool muted : { true, false })
	{
		click.State = actions::TouchAction::TOUCH_DOWN;
		scene->OnAction(click);
		click.State = actions::TouchAction::TOUCH_UP;
		scene->OnAction(click);
		EXPECT_EQ(muted, take->IsMuted());
		EXPECT_FALSE(sibling->IsMuted());
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ExpandedRackSuppressesOtherStationsAndRestoresOnLeavingView)
{
	auto scene = FreshScene({ Station("Keys"), Station("Bass") }, {});
	ASSERT_TRUE(scene);
	scene->SetSize({ 640, 480 });
	const auto stations = scene->SnapshotStations();
	const auto rack = stations[0]->GetGuiRack();
	const auto otherRack = stations[1]->GetGuiRack();
	rack->SetPosition({ 100, 100 });
	otherRack->SetPosition({ 300, 100 });
	rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
	scene->AdvanceUiAnimations();
	ASSERT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());
	EXPECT_TRUE(rack->GetMasterSlider()->Parent()->IsVisible());
	EXPECT_FALSE(otherRack->GetMasterSlider()->Parent()->IsVisible());

	// Closing just the router still leaves an expanded channel rack.
	rack->SetRackState(gui::GuiRackParams::RACK_ROUTER, true);
	rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
	scene->AdvanceUiAnimations();
	EXPECT_FALSE(otherRack->GetMasterSlider()->Parent()->IsVisible());
	rack->SetRackState(gui::GuiRackParams::RACK_MASTER, true);
	scene->AdvanceUiAnimations();
	EXPECT_TRUE(otherRack->GetMasterSlider()->Parent()->IsVisible());

	rack->SetRackState(gui::GuiRackParams::RACK_ROUTER, true);
	rack->SetPosition({ -10000, 100 });
	scene->AdvanceUiAnimations();
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	EXPECT_TRUE(otherRack->GetMasterSlider()->Parent()->IsVisible());
	rack->SetPosition({ 100, 100 });
	scene->AdvanceUiAnimations();
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SelectingAnotherTakeCollapsesRackButHoverAndRackInputPreserveIt)
{
	auto scene = FreshScene({ Station("Keys") }, {});
	ASSERT_TRUE(scene);
	scene->SetSize({ 1280, 900 });
	const auto station = scene->SnapshotStations().front();
	const auto take = station->AddTake();
	const auto sibling = station->AddTake();
	scene->CommitChanges();
	actions::GuiAction view;
	view.ElementType = actions::GuiAction::ACTIONELEMENT_RADIO;
	view.Index = 100u;
	view.Data = actions::GuiAction::GuiInt{ engine::Scene::VIEW_LOOPTAKE };
	scene->OnAction(view);
	const auto rack = take->GetGuiRack();
	rack->SetPosition({ 500, 400 });
	rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
	scene->AdvanceUiAnimations();
	ASSERT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());

	std::vector<unsigned char> pickPath;
	for (const auto index : sibling->GlobalId())
		pickPath.push_back(static_cast<unsigned char>(index + 1u));
	scene->SetHover3d(pickPath, base::Action::MODIFIER_NONE);
	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());

	// Exercise the scene's actual press/release capture path for the master.
	const auto slider = rack->GetMasterSlider();
	const auto sliderPosition = slider->GlobalPosition();
	actions::TouchAction click;
	click.Touch = actions::TouchAction::TOUCH_MOUSE;
	click.Position = { sliderPosition.X + 10, sliderPosition.Y + 10 };
	click.Index = 0;
	click.State = actions::TouchAction::TOUCH_DOWN;
	const auto rackPress = scene->OnAction(click);
	ASSERT_TRUE(rackPress.IsEaten);
	EXPECT_EQ(slider, rackPress.ActiveElement.lock());
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_EQ(gui::GuiRackParams::RACK_CHANNELS, rack->GetRackState());

	actions::TouchMoveAction move;
	move.Touch = actions::TouchAction::TOUCH_MOUSE;
	move.Position = { -100, -100 };
	scene->OnAction(move);
	scene->SetHover3d(pickPath, base::Action::MODIFIER_NONE);
	click.Position = { -100, -100 };
	click.State = actions::TouchAction::TOUCH_DOWN;
	scene->OnAction(click);
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_TRUE(sibling->IsSelected());
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	EXPECT_TRUE(sibling->GetGuiRack()->GetMasterSlider()->Parent()->IsVisible());

	// A background click clears selection and restores every master control.
	rack->SetRackState(gui::GuiRackParams::RACK_ROUTER, true);
	scene->AdvanceUiAnimations();
	scene->SetHover3d({}, base::Action::MODIFIER_NONE);
	click.State = actions::TouchAction::TOUCH_DOWN;
	scene->OnAction(click);
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_FALSE(scene->HasSelection());
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	EXPECT_TRUE(sibling->GetGuiRack()->GetMasterSlider()->Parent()->IsVisible());
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FreshScenesResolveReorderedAndAdditionalStationsByName)
{
	auto reordered = FreshScene({ Station("Bass"), Station("Drums") },
		{ Trigger("drums", "Drums"), Trigger("bass", "Bass") });
	ASSERT_TRUE(reordered);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Bass", 1u }, { "Drums", 1u } }),
		Memberships(reordered));
	reordered->Shutdown();

	auto additional = FreshScene({ Station("Drums"), Station("Unused"), Station("Bass") },
		{ Trigger("bass", "Bass") });
	ASSERT_TRUE(additional);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{
		{ "Drums", 0u }, { "Unused", 0u }, { "Bass", 1u } }), Memberships(additional));
	additional->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, RestoresDistinctHistoriesToTriggersWithSharedStation)
{
	// Restoration must pin real saved material; nonexistent IDs are correctly
	// retired by the independently running source-loss owner.
	struct SidecarCleanup
	{
		std::filesystem::path Path;
		~SidecarCleanup() { std::error_code error; std::filesystem::remove(Path, error); }
	} sidecar{ std::filesystem::temp_directory_path() / ("jamma-history-" + utils::GetGuid() + ".jammidi") };
	io::NativeMidiSidecar::Stream contents;
	contents.LogicalLength = 100u;
	contents.Events.push_back({ 0u, 0x90u, 60u, 100u });
	{
		std::ofstream output(sidecar.Path, std::ios::binary);
		ASSERT_TRUE(output);
		ASSERT_TRUE(io::NativeMidiSidecar::ToStream(contents, output));
	}
	const auto addSavedTakes = [&sidecar](io::JamFile::Station& descriptor, std::initializer_list<const char*> names)
	{
		for (const auto name : names)
		{
			io::JamFile::LoopTake take{};
			take.Name = name;
			take.MidiPlayLength = 100u;
			take.MidiStreams.push_back({ sidecar.Path.string(), 0u, "healthy", 100u, 0u });
			descriptor.LoopTakes.push_back(std::move(take));
		}
	};
	auto bass = Station("Bass");
	auto drums = Station("Drums");
	addSavedTakes(bass, { "bass-source", "bass-target", "other-source", "other-target" });
	addSavedTakes(drums, { "drums-source", "drums-target" });
	auto drumTrigger = Trigger("drums", "Drums"); drumTrigger.Id = "drums-id";
	auto firstBassTrigger = Trigger("bass-first", "Bass"); firstBassTrigger.Id = "bass-first-id";
	auto secondBassTrigger = Trigger("bass-second", "Bass"); secondBassTrigger.Id = "bass-second-id";
	// Station order differs from trigger order, and two triggers share Bass.
	auto scene = FreshScene({ bass, drums }, { drumTrigger, firstBassTrigger, secondBassTrigger }, {}, {
		{ "drums-id", { { static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "drums-source", "drums-target" } } },
		{ "bass-first-id", { { static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "bass-source", "bass-target" } } },
		{ "bass-second-id", { { static_cast<unsigned int>(engine::TriggerTake::SOURCE_ADC), "other-source", "other-target" } } } });
	ASSERT_TRUE(scene);
	const auto restoredStations = scene->SnapshotStations();
	ASSERT_EQ(2u, restoredStations.size());
	ASSERT_EQ(4u, restoredStations[0]->GetLoopTakeSnapshot().size());
	ASSERT_EQ(2u, restoredStations[1]->GetLoopTakeSnapshot().size());
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Bass", 2u }, { "Drums", 1u } }),
		Memberships(scene));
	const auto rig = scene->AcceptedRigSnapshot();
	ASSERT_TRUE(rig);
	ASSERT_EQ(3u, rig->Triggers.size());
	ASSERT_TRUE(rig->Triggers[0].Instance);
	ASSERT_TRUE(rig->Triggers[1].Instance);
	ASSERT_TRUE(rig->Triggers[2].Instance);
	ASSERT_EQ(1u, rig->Triggers[0].Instance->GetTakes().size());
	EXPECT_EQ("drums-source", rig->Triggers[0].Instance->GetTakes()[0].SourceTakeId);
	ASSERT_EQ(1u, rig->Triggers[1].Instance->GetTakes().size());
	EXPECT_EQ("bass-source", rig->Triggers[1].Instance->GetTakes()[0].SourceTakeId);
	EXPECT_EQ("bass-target", rig->Triggers[1].Instance->GetTakes()[0].TargetTakeId);
	ASSERT_EQ(1u, rig->Triggers[2].Instance->GetTakes().size());
	EXPECT_EQ("other-source", rig->Triggers[2].Instance->GetTakes()[0].SourceTakeId);
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FreshScenesLeaveRenamedMissingAmbiguousAndFewerTargetsUnbound)
{
	auto renamed = FreshScene({ Station("Renamed") }, { Trigger("old", "Original") });
	ASSERT_TRUE(renamed);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Renamed", 0u } }), Memberships(renamed));
	renamed->Shutdown();

	auto missing = FreshScene({ Station("Present") }, { Trigger("missing", "Absent") });
	ASSERT_TRUE(missing);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Present", 0u } }), Memberships(missing));
	missing->Shutdown();

	auto ambiguous = FreshScene({ Station("Dup"), Station("Dup") }, { Trigger("ambiguous", "Dup") });
	ASSERT_TRUE(ambiguous);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Dup", 0u }, { "Dup", 0u } }),
		Memberships(ambiguous));
	ambiguous->Shutdown();

	auto fewer = FreshScene({ Station("Only") },
		{ Trigger("only", "Only"), Trigger("removed", "Removed") });
	ASSERT_TRUE(fewer);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Only", 1u } }), Memberships(fewer));
	fewer->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, InitialNullTargetRemainsUnboundWithoutSaving)
{
	auto legacy = Trigger("legacy", "");
	legacy.StationTarget.reset();
	unsigned int saves = 0u;
	auto scene = FreshScene({ Station("Station") }, { legacy }, [&saves](const io::RigFile&)
	{
		++saves;
		return false;
	});
	ASSERT_TRUE(scene);
	EXPECT_EQ(0u, saves);
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Station", 0u } }), Memberships(scene));
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, CloseAudioAndShutdownAreRepeatable)
{
	auto scene = FreshScene({ Station("Station") }, { Trigger("trigger", "Station") });
	ASSERT_TRUE(scene);
	EXPECT_NO_THROW(scene->Controls().CloseAudio());
	EXPECT_NO_THROW(scene->Controls().CloseAudio());
	EXPECT_NO_THROW(scene->Shutdown());
	EXPECT_NO_THROW(scene->Shutdown());
	EXPECT_EQ(engine::RigCoordinator::EditResult::EditsDisabled,
		scene->RequestRigEdit(io::RigFile{}));
}

TEST_F(SceneRoutingIntegrationTest, RoutingEditIsRejectedWhenAudioCallbackIsInactive)
{
	auto scene = FreshScene({ Station("Station") }, { Trigger("trigger", "Station") });
	ASSERT_TRUE(scene);
	EXPECT_EQ(engine::RigCoordinator::EditResult::AudioCallbackInactive,
		scene->RequestRigEdit(io::RigFile{}));
	EXPECT_EQ((std::vector<std::pair<std::string, std::size_t>>{ { "Station", 1u } }),
		Memberships(scene));
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, RoutingEditLivenessExpiresAndRecoversOnNewHeartbeat)
{
    auto now = std::chrono::steady_clock::time_point{std::chrono::seconds(1)};
    auto scene = FreshScene({ Station("Station") }, { Trigger("trigger", "Station") },
        [](const io::RigFile&) { return true; }, {}, [&now] { return now; });
    ASSERT_TRUE(scene);
    using Availability = gui::RoutingEditAvailability;
    using Access = engine::SourceLossIntegrationTestAccess;
    EXPECT_EQ(Availability::AudioCallbackInactive, Access::RoutingAvailability(*scene));
    Access::AudioHeartbeat(*scene);
    EXPECT_EQ(Availability::Ready, Access::RoutingAvailability(*scene));
    now += std::chrono::milliseconds(500);
    EXPECT_EQ(Availability::Ready, Access::RoutingAvailability(*scene));
    now += std::chrono::milliseconds(1);
    EXPECT_EQ(engine::RigCoordinator::EditResult::AudioCallbackInactive,
        scene->RequestRigEdit(io::RigFile{}));
    Access::AudioHeartbeat(*scene);
    EXPECT_EQ(Availability::Ready, Access::RoutingAvailability(*scene));
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, StoppedStreamMembershipPreservesSelectionAndRouteIdentity)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "B")});
 ASSERT_TRUE(scene);
 auto before = scene->SnapshotStations();
 auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
 before[1]->Select();
 before[0]->SetAllowedMidiChannels({9});
 before[1]->AddTake(); scene->CommitChanges();
 engine::SourceLossIntegrationTestAccess::RemoteGrid(*scene, {88200u, 4u, 0u, 120.0f, 1u}, 1234);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, true);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 auto added = scene->SnapshotStations();
 ASSERT_EQ(3u, added.size());
 EXPECT_EQ(before[0], added[1]); EXPECT_EQ(before[1], added[2]);
 EXPECT_TRUE(before[1]->IsSelected()); EXPECT_FALSE(added[0]->IsSelected());
 EXPECT_EQ(trigger, scene->AcceptedRigSnapshot()->Triggers.front().Instance);
 EXPECT_EQ(before[1], scene->AcceptedRigSnapshot()->Triggers.front().BoundReceiver);
 EXPECT_EQ((std::vector<int>{9}), before[0]->AllowedMidiChannels());
 auto newTake = added[0]->AddTake(); added[0]->CommitChanges();
 const auto grid = newTake->ResolvedMidiQuantisation();
 EXPECT_TRUE(grid.HasRemoteGrid()); EXPECT_EQ(88200u, grid.RemoteIntervalSamps); EXPECT_EQ(1234, grid.RemoteOriginSamps);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, added[0]);
 engine::SourceLossIntegrationTestAccess::ConfirmRemoval(*scene);
 const auto deleteDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deleteDeadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 EXPECT_EQ(2u, scene->SnapshotStations().size());
 EXPECT_TRUE(before[1]->IsSelected());
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ActiveStationDeletionCancelsAndUnbindsWithoutSelectingNeighbour)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 auto station = scene->SnapshotStations().front();
 auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
 io::UserConfig cfg{}; cfg.Audio.NumChannelsIn = 1u;
 engine::SourceLossIntegrationTestAccess::StartRecording(*scene, trigger, cfg);
 ASSERT_EQ(1u, station->NumTakes());
 station->Select();
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, station);
 EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::Busy(*scene)); // Consent has not stopped recording.
 EXPECT_FALSE(station->MembershipRemoving());
 engine::SourceLossIntegrationTestAccess::ConfirmRemoval(*scene);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 EXPECT_EQ(1u, scene->SnapshotStations().size());
 EXPECT_FALSE(scene->HasSelection());
 EXPECT_FALSE(scene->AcceptedRigSnapshot()->Triggers.front().BoundReceiver);
 EXPECT_FALSE(scene->AcceptedRigSnapshot()->Rig.Triggers.front().StationTarget);
 EXPECT_TRUE(trigger->GetTakes().empty());
 std::weak_ptr<engine::Station> retired = station;
 station.reset();
 const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (!retired.expired() && std::chrono::steady_clock::now() < retireDeadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 EXPECT_TRUE(retired.expired()) << "Removal must retire without another edit";
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SavedJamAndExplicitRigReproduceAddedMembershipAndBindings)
{
 auto scene = FreshScene({Station("A")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, true);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
 while (engine::SourceLossIntegrationTestAccess::Busy(*scene) && std::chrono::steady_clock::now() < deadline)
 { scene->CommitChanges(); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
 ASSERT_EQ(2u, scene->SnapshotStations().size());
 const auto rig = scene->CaptureCommittedRig(); ASSERT_TRUE(rig);
 std::stringstream rigJson; ASSERT_TRUE(io::RigFile::ToJsonStream(*rig, rigJson));
 auto savedRig = io::RigFile::FromStream(std::move(rigJson)); ASSERT_TRUE(savedRig);
 const auto directory = std::filesystem::temp_directory_path() / ("jamma-station-save-" + std::to_string(utils::Timer::GetTime().time_since_epoch().count()));
 std::filesystem::create_directories(directory);
 ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::Export(*scene, directory.wstring()));
 std::ifstream file(directory / "session.jam"); std::stringstream jamJson; jamJson << file.rdbuf(); file.close();
 auto savedJam = io::JamFile::FromStream(std::move(jamJson)); ASSERT_TRUE(savedJam);
 EXPECT_EQ(2u, savedJam->Stations.size());
 const std::vector<int> allChannels{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
 EXPECT_EQ(allChannels, savedJam->Stations.front().AllowedMidiChannels);
 for (const auto& route : savedJam->Stations.front().MidiChannelRoutes) EXPECT_TRUE(route.PluginIds.empty());
 // The loaded original retains its saved disabled channels, independently of creation defaults.
 EXPECT_TRUE(savedJam->Stations.back().AllowedMidiChannels.empty());
 engine::SceneParams params({ "" }, {}, {640u, 480u});
 auto reloaded = engine::Scene::FromFile(params, *savedJam, *savedRig, directory.wstring()); ASSERT_TRUE(reloaded);
 EXPECT_EQ(scene->SnapshotStations()[0]->Name(), reloaded.value()->SnapshotStations()[0]->Name());
 EXPECT_EQ(allChannels, reloaded.value()->SnapshotStations()[0]->AllowedMidiChannels());
 EXPECT_TRUE(reloaded.value()->SnapshotStations()[1]->AllowedMidiChannels().empty());
 for (const auto& route : reloaded.value()->SnapshotStations()[0]->MidiChannelRoutes()) EXPECT_TRUE(route.PluginIds.empty());
 EXPECT_EQ("A", reloaded.value()->AcceptedRigSnapshot()->Rig.Triggers.front().StationTarget); // frozen target lives in Rig
 reloaded.value()->Shutdown(); scene->Shutdown();
 std::filesystem::remove_all(directory);
}

TEST_F(SceneRoutingIntegrationTest, DeletedStationOrphanReconnectsThroughLockedSceneHudDispatch)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, scene->SnapshotStations()[0]);
 Settle(scene);
 ASSERT_EQ(1u, scene->SnapshotStations().size());
 ASSERT_FALSE(scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false);
 Settle(scene);
 ASSERT_EQ(2u, scene->SnapshotStations().size());
 const auto replacement = scene->SnapshotStations()[1];
 scene->SetSize({1200, 800});
 auto hud = engine::SourceLossIntegrationTestAccess::PrepareHud(*scene);
 LayoutHud(hud);
 std::vector<gui::CableInteraction::Endpoint> endpoints;
 std::vector<gui::CableInteraction::Cable> cables;
 hud->BuildInteractionGeometry(endpoints, cables);
 const auto source = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& e) { return e.Kind == gui::CableInteraction::EndpointKind::TriggerOutput; });
 const auto target = std::find_if(endpoints.begin(), endpoints.end(), [](const auto& e) { return e.Kind == gui::CableInteraction::EndpointKind::Station && e.StationIndex == 1u; });
 ASSERT_NE(endpoints.end(), source); ASSERT_NE(endpoints.end(), target);
 actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0;
 touch.Position = source->Position; touch.State = actions::TouchAction::TOUCH_DOWN;
 EXPECT_TRUE(scene->OnAction(touch).IsEaten);
 ASSERT_TRUE(hud->HasCableDrag());
 actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE;
 move.Position = target->Position; move.MouseButtonsDown = 1u;
 scene->OnAction(move);
 touch.Position = target->Position; touch.State = actions::TouchAction::TOUCH_UP;
 EXPECT_NO_THROW(scene->OnAction(touch));
 EXPECT_FALSE(hud->HasCableDrag());
 Settle(scene);
 EXPECT_EQ(replacement, scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 EXPECT_EQ(replacement->Name(), scene->AcceptedRigSnapshot()->Rig.Triggers[0].StationTarget);
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 EXPECT_FALSE(hud->HasCableDrag());
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, PublicRoutingSubmissionAndFocusLossRemainSafe)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "A")});
 ASSERT_TRUE(scene);
 auto hud = engine::SourceLossIntegrationTestAccess::PrepareHud(*scene);
 auto candidate = scene->AcceptedRigSnapshot()->Rig;
 candidate.Triggers[0].StationTarget = "B";
 auto result = scene->RequestRigEdit(candidate);
 const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
 while (result == engine::RigCoordinator::EditResult::EditsDisabled && std::chrono::steady_clock::now() < deadline)
 { std::this_thread::sleep_for(std::chrono::milliseconds(1)); result = scene->RequestRigEdit(candidate); }
 ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, result);
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 Settle(scene);
 EXPECT_EQ(scene->SnapshotStations()[1], scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FocusLossPreservesMidiChannelToggleSelection)
{
 auto scene = FreshScene({Station("A")}, {});
 ASSERT_TRUE(scene);
 actions::GuiAction action;
 action.ElementType = actions::GuiAction::ACTIONELEMENT_RADIO; action.Index = 7001u;
 action.Data = actions::GuiAction::GuiInt(7);
 scene->OnAction(action);
 EXPECT_EQ(7u, engine::SourceLossIntegrationTestAccess::ForcedChannel(*scene));
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 EXPECT_EQ(7u, engine::SourceLossIntegrationTestAccess::ForcedChannel(*scene));
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FocusLossCommitsPhaseOffsetSettingOutsideSceneLock)
{
 auto scene = FreshScene({Station("A")}, {});
 ASSERT_TRUE(scene);
 auto field = engine::SourceLossIntegrationTestAccess::FocusSetting(*scene, "0.125");
 ASSERT_TRUE(field->HasFocus());
 EXPECT_DOUBLE_EQ(0.0, scene->SnapshotStations()[0]->TransportOffsetLoopFrac());
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 EXPECT_DOUBLE_EQ(0.125, scene->SnapshotStations()[0]->TransportOffsetLoopFrac());
 EXPECT_DOUBLE_EQ(0.125, field->Value());
 EXPECT_FALSE(field->HasFocus());
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, StationMotionPreservesFinalTopologyAndAppendLeavesSurvivorsStill)
{
 auto scene = FreshScene({Station("A"), Station("B")}, {Trigger("record", "B")});
 ASSERT_TRUE(scene);
 const auto original = scene->SnapshotStations();
 engine::SourceLossIntegrationTestAccess::Manage(*scene, true);
 Settle(scene);
 auto prepended = scene->SnapshotStations(); ASSERT_EQ(3u, prepended.size());
 EXPECT_FLOAT_EQ(-50.0f, prepended[0]->ModelPosition().X);
 EXPECT_FLOAT_EQ(-650.0f, prepended[0]->PresentationModelPosition().X);
 EXPECT_FLOAT_EQ(-50.0f, original[0]->PresentationModelPosition().X);
 EXPECT_FLOAT_EQ(550.0f, original[0]->ModelPosition().X);
 EXPECT_EQ(original[1], scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ControlsAvailable(*scene));
 ASSERT_TRUE(scene->CaptureCommittedRig()); // Saved identities are final during motion.
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false);
 Settle(scene);
 auto appended = scene->SnapshotStations(); ASSERT_EQ(4u, appended.size());
 for (size_t i = 0; i < 3; ++i) EXPECT_FALSE(appended[i]->IsPresentationMoving());
 EXPECT_FLOAT_EQ(appended[3]->ModelPosition().X + 600.0f, appended[3]->PresentationModelPosition().X);
 engine::SourceLossIntegrationTestAccess::Manage(*scene, false, appended[0]);
 Settle(scene);
 auto deleted = scene->SnapshotStations(); ASSERT_EQ(3u, deleted.size());
 EXPECT_EQ(original[0], deleted[0]);
 EXPECT_FLOAT_EQ(-50.0f, deleted[0]->ModelPosition().X);
 EXPECT_FLOAT_EQ(550.0f, deleted[0]->PresentationModelPosition().X);
 EXPECT_EQ(original[1], scene->AcceptedRigSnapshot()->Triggers[0].BoundReceiver);
 engine::SourceLossIntegrationTestAccess::FinishMotion(*scene);
 EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ControlsAvailable(*scene));
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, UiFrameRefreshesCameraProjectionWithoutGraphicsResources)
{
 auto scene = FreshScene({Station("A")}, {});
 ASSERT_TRUE(scene);
 scene->SetSize({1200, 800});
 // Represent the previous frame's projection before camera/frame preparation.
 engine::SourceLossIntegrationTestAccess::StaleProjection(*scene);
 EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::HasCurrentProjection(*scene));
 scene->AdvanceUiAnimations();
 EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::HasCurrentProjection(*scene));
 scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, DeviceGuiDispatchIsIsolatedFromStationCommands)
{
    auto scene = FreshScene({Station("A"), Station("B")}, {});
    ASSERT_TRUE(scene);
    auto jobExclusion = engine::SourceLossIntegrationTestAccess::ExcludeJob(*scene);
    audio::AsioInventory inventory;
    inventory.Devices = {{0u, "Device A", true, "", 18u, 20u, {44100u}, 44100u},
                         {1u, "Device B", true, "", 18u, 20u, {44100u}, 44100u}};
    const audio::AudioStreamParams actual{"Device A", 44100u, 2u, 128u, 0u, 0u, 2u, 2u};
    engine::SourceLossIntegrationTestAccess::StartedAudioSelection(*scene, inventory, actual);
    auto target = scene->SnapshotStations().back();
    target->AddTake(); scene->CommitChanges();
    engine::SourceLossIntegrationTestAccess::Manage(*scene, false, target);
    ASSERT_EQ(target, engine::SourceLossIntegrationTestAccess::RemovalTarget(*scene));
    // Exercise registered widgets/bindings, including device IDs 7200/1/2.
    engine::SourceLossIntegrationTestAccess::ChooseAudio(*scene, 1);
    engine::SourceLossIntegrationTestAccess::AudioChannels(*scene, 0u, 7.0);
    engine::SourceLossIntegrationTestAccess::AudioChannels(*scene, 1u, 8.0);
    EXPECT_EQ("Device B", engine::SourceLossIntegrationTestAccess::AudioDraft(*scene).Name);
    EXPECT_EQ(7u, engine::SourceLossIntegrationTestAccess::AudioDraft(*scene).NumChannelsIn);
    EXPECT_EQ(8u, engine::SourceLossIntegrationTestAccess::AudioDraft(*scene).NumChannelsOut);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::RigSaveRequested(*scene));
    EXPECT_EQ(target, engine::SourceLossIntegrationTestAccess::RemovalTarget(*scene));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::Busy(*scene));
    const auto click = [](const std::shared_ptr<gui::GuiButton>& button) {
        actions::TouchAction action; action.Position = {2,2}; action.Index = 0;
        action.State = actions::TouchAction::TOUCH_DOWN; button->OnAction(action);
        action.State = actions::TouchAction::TOUCH_UP; button->OnAction(action);
    };
    auto cancel = engine::SourceLossIntegrationTestAccess::RemovalButton(*scene, 0u);
    ASSERT_TRUE(cancel); click(cancel);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::RemovalTarget(*scene));
    auto save = engine::SourceLossIntegrationTestAccess::RigSaveButton(*scene);
    ASSERT_TRUE(save); click(save);
    EXPECT_TRUE(scene->ConsumeRigSaveRequest());
    EXPECT_EQ(engine::Scene::RigSaveMode::StartupDefault,scene->Controls().RequestedRigSaveMode());
    auto loadRig = engine::SourceLossIntegrationTestAccess::RigSaveChoice(*scene, 0u);
    auto saveAsRig = engine::SourceLossIntegrationTestAccess::RigSaveChoice(*scene, 1u);
    ASSERT_NE(save->Parent(), loadRig->Parent());
    ASSERT_EQ(loadRig->Parent(), saveAsRig->Parent());
    click(loadRig);
    EXPECT_TRUE(scene->ConsumePanelRigLoadRequest());
    EXPECT_FALSE(scene->ConsumeRigSaveRequest());
    click(saveAsRig);
    EXPECT_TRUE(scene->ConsumeRigSaveRequest());
    EXPECT_EQ(engine::Scene::RigSaveMode::SaveAs, scene->Controls().RequestedRigSaveMode());
    engine::SourceLossIntegrationTestAccess::Manage(*scene, false, target);
    cancel = engine::SourceLossIntegrationTestAccess::RemovalButton(*scene, 0u);
    auto remove = engine::SourceLossIntegrationTestAccess::RemovalButton(*scene, 1u);
    ASSERT_TRUE(cancel); ASSERT_TRUE(remove);
    jobExclusion.unlock();
    engine::SourceLossIntegrationTestAccess::BeginDeviceWriter(*scene);
    click(cancel); click(remove); click(save);
    EXPECT_EQ(target, engine::SourceLossIntegrationTestAccess::RemovalTarget(*scene));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::Busy(*scene));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::RigSaveRequested(*scene));
    engine::SourceLossIntegrationTestAccess::EndDeviceWriter(*scene);
    click(remove);
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::Busy(*scene));
    EXPECT_EQ("Device B", engine::SourceLossIntegrationTestAccess::AudioDraft(*scene).Name);
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, PluginFocusLossDefersSceneEntryUntilStateCallUnlocks)
{
    auto scene = FreshScene({ Station("Keys") }, {});
    ASSERT_TRUE(scene);
    actions::KeyAction heldShortcut; heldShortcut.KeyChar = VK_INSERT; heldShortcut.KeyActionType = actions::KeyAction::KEY_DOWN;
    scene->OnAction(heldShortcut); ASSERT_TRUE(midi::MidiRouter::IsAutomationRecordHeld());
    resources::ResourceLib resources;
    graphics::Window window(*scene, resources);
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(host, nullptr);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    actions::KeyAction control;
    control.KeyChar = 17u;
    control.KeyActionType = actions::KeyAction::KEY_DOWN;
    window.OnAction(control);
    ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ControlHeld(*scene));
    engine::SourceLossIntegrationTestAccess::OpenKeyboardModal(*scene);
    actions::KeyAction modalKey; modalKey.KeyChar = 76u; modalKey.KeyActionType = actions::KeyAction::KEY_DOWN;
    window.OnAction(modalKey);
    engine::SourceLossIntegrationTestAccess::CloseKeyboardModal(*scene);
    modalKey.KeyActionType = actions::KeyAction::KEY_UP;
    window.OnAction(modalKey); EXPECT_FALSE(window.ConsumeJamLoadRequest());
    engine::SourceLossIntegrationTestAccess::OpenKeyboardModal(*scene);
    modalKey.KeyChar = 192u; modalKey.KeyActionType = actions::KeyAction::KEY_DOWN;
    window.OnAction(modalKey);
    engine::SourceLossIntegrationTestAccess::CloseKeyboardModal(*scene);
    modalKey.KeyActionType = actions::KeyAction::KEY_UP;
    window.OnAction(modalKey); EXPECT_FALSE(window.ConsumeConsoleToggleRequest());
    engine::SourceLossIntegrationTestAccess::OpenKeyboardModal(*scene);
    modalKey.KeyChar = VK_INSERT; window.OnAction(modalKey); EXPECT_FALSE(midi::MidiRouter::IsAutomationRecordHeld());
    engine::SourceLossIntegrationTestAccess::CloseKeyboardModal(*scene);
    scene->OnAction(heldShortcut); ASSERT_TRUE(midi::MidiRouter::IsAutomationRecordHeld());
    actions::TouchAction press;
    press.Touch = actions::TouchAction::TOUCH_MOUSE;
    press.State = actions::TouchAction::TOUCH_DOWN;
    press.Index = 0;
    press.Position = { 320, 240 };
    window.OnAction(press);
    // Mirror Kontakt's synchronous native notification inside the Scene-locked
    // group state pump. Any Scene/capture/modifier dispatch here re-locks Scene.
    engine::SourceLossIntegrationTestAccess::WhileSceneLocked(*scene, [&] {
        graphics::Window::WindowProcedure(host, WM_KILLFOCUS, 0, 0);
        graphics::Window::WindowProcedure(host, WM_CAPTURECHANGED, 0, 0);
        graphics::Window::WindowProcedure(host, WM_CANCELMODE, 0, 0);
        graphics::Window::WindowProcedure(host, WM_ACTIVATEAPP, FALSE, 0);
        graphics::Window::WindowProcedure(host, WM_ACTIVATE, WA_INACTIVE, 0);
        graphics::Window::WindowProcedure(host, WM_SHOWWINDOW, FALSE, 0);
        graphics::Window::WindowProcedure(host, WM_KILLFOCUS, 0, 0);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ControlHeld(*scene));
        EXPECT_EQ(window.Modifiers(), base::Action::MODIFIER_NONE);
    });
    EXPECT_TRUE(window.ProcessDeferredInputLoss());
    EXPECT_FALSE(window.CancelMouseCapture());
    EXPECT_FALSE(midi::MidiRouter::IsAutomationRecordHeld());
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ControlHeld(*scene));
    EXPECT_EQ(window.Modifiers(), base::Action::MODIFIER_NONE);
    EXPECT_FALSE(window.ProcessDeferredInputLoss());
    DestroyWindow(host);
}

TEST_F(SceneRoutingIntegrationTest, GroupFunctionKeyAndFocusLossDoNotReachConfiguredTrigger)
{
    auto triggerConfig = Trigger("Record", "Keys");
    triggerConfig.ControlBindings.Activate.push_back({io::RigFile::BindingSource::Keyboard, "keyboard",
        {112u, 1u}, {112u, 0u}});
    triggerConfig.ControlBindings.Ditch.push_back({io::RigFile::BindingSource::Keyboard, "keyboard",
        {113u, 1u}, {113u, 0u}});
    auto scene = FreshScene({ Station("Keys") }, { triggerConfig });
    ASSERT_TRUE(scene);
    const auto rig = scene->AcceptedRigSnapshot();
    ASSERT_TRUE(rig && rig->Triggers.size() == 1u);
    actions::KeyAction key;
    key.KeyChar = 112u;
    key.KeyActionType = actions::KeyAction::KEY_DOWN;
    EXPECT_TRUE(scene->OnAction(key).IsEaten);
    EXPECT_TRUE(scene->OnAction(key).IsEaten); // Suppressed repeat.
    scene->OnInputFocusLost();
    key.KeyActionType = actions::KeyAction::KEY_UP;
    EXPECT_TRUE(scene->OnAction(key).IsEaten);
    const auto trigger = rig->Triggers[0].Instance;
    ASSERT_TRUE(trigger);
    engine::SourceLossIntegrationTestAccess::WhileSceneLocked(*scene, [&] {
        trigger->OnTick(utils::Timer::GetTime(), 0u, io::UserConfig{},
            scene->GetAudioStreamParams(), rig->Revision);
    });
    EXPECT_EQ(trigger->GetState(), engine::TRIGSTATE_DEFAULT);
    EXPECT_TRUE(trigger->GetTakes().empty());
    EXPECT_TRUE(scene->SnapshotStations()[0]->GetLoopTakeSnapshot().empty());
}

TEST_F(SceneRoutingIntegrationTest, InWindowItemMiddlePaintUsesSelectionThresholdWithoutGlobalCapture)
{
    for (const bool initiallyMuted : { false, true })
    {
        auto scene = FreshScene({ Station("Local") }, {});
        ASSERT_TRUE(scene);
        scene->SetSize({ 800, 600 });
        auto station = scene->SnapshotStations().front();
        auto audioTake = station->AddTake();
        audioTake->AddLoop(0u, station->Id());
        auto midiTake = station->AddTake();
        midiTake->Record({}, station->Name(), { 0u }, { "Keys" });
        auto untouched = station->AddTake();
        untouched->AddLoop(0u, station->Id());
        scene->CommitChanges();
        engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
        auto audioLoop = audioTake->GetLoops().front();
        const auto midiModel = midiTake->GetMidiLoops().front()->Model();
        ASSERT_TRUE(midiModel);
        audioTake->SetGroupMutedAudio(initiallyMuted);
        audioLoop->SetGroupMutedAudio(initiallyMuted);
        midiTake->SetGroupMutedAudio(initiallyMuted);
        untouched->SetGroupMutedAudio(initiallyMuted);
        const auto pickPath = [](const auto& item) {
            std::vector<unsigned char> path;
            for (const auto index : item->GlobalId()) path.push_back(static_cast<unsigned char>(index + 1u));
            return path;
        };
        const auto audioPath = pickPath(audioLoop);
        const auto midiPath = pickPath(midiModel);
        scene->SetHover3d(audioPath, base::Action::MODIFIER_NONE);
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene); // Valid client point; the picker reports this actual loop.
        click.Index = 1;
        click.MouseButtonsDown = 2u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        ASSERT_TRUE(scene->OnAction(click).IsEaten);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        EXPECT_EQ(initiallyMuted, audioTake->IsMuted());
        EXPECT_EQ(initiallyMuted, midiTake->IsMuted());
        EXPECT_TRUE(audioTake->IsPicking3d());
        EXPECT_FALSE(midiTake->IsPicking3d());
        actions::TouchMoveAction move;
        move.Touch = actions::TouchAction::TOUCH_MOUSE;
        move.Position = click.Position;
        move.MouseButtonsDown = 2u;
        scene->OnAction(move);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        move.Position.X += 5;
        scene->OnAction(move);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        EXPECT_EQ(!initiallyMuted, audioTake->IsMuted());
        EXPECT_EQ(initiallyMuted, midiTake->IsMuted());
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        scene->SetHover3d(midiPath, base::Action::MODIFIER_NONE);
        EXPECT_EQ(!initiallyMuted, midiTake->IsMuted());
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        scene->SetHover3d({}, base::Action::MODIFIER_NONE); // Passing through background keeps item paint ownership.
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        scene->SetHover3d(audioPath, base::Action::MODIFIER_NONE);
        EXPECT_EQ(!initiallyMuted, audioTake->IsMuted()); // Revisit does not toggle direction.
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        click.Position = move.Position;
        click.State = actions::TouchAction::TOUCH_UP;
        click.MouseButtonsDown = 0u;
        scene->OnAction(click);
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_EQ(!initiallyMuted, audioTake->IsMuted());
        EXPECT_EQ(!initiallyMuted, midiTake->IsMuted());
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, SettingsPanelClearsSceneHoverAndLeavingRestoresIt)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto station = scene->SnapshotStations().front();
    std::vector<unsigned char> stationPath;
    for (const auto index : station->GlobalId()) stationPath.push_back(static_cast<unsigned char>(index + 1u));
    const auto canvas = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
    scene->SetHover3d(stationPath, base::Action::MODIFIER_NONE, canvas);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CurrentHover(*scene).empty());

    const auto panel = engine::SourceLossIntegrationTestAccess::SettingsPanel(*scene);
    const auto bounds = panel->PresentedFrameBounds();
    ASSERT_FALSE(bounds.IsEmpty());
    const utils::Position2d background{ bounds.Left + 3, bounds.Bottom + 3 };
    ASSERT_TRUE(panel->RouteHitTest(background));
    scene->SetHover3d(stationPath, base::Action::MODIFIER_NONE, background);
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::CurrentHover(*scene).empty());

    scene->SetHover3d(stationPath, base::Action::MODIFIER_NONE, canvas);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CurrentHover(*scene).empty());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, RawStationPickBelowSelectionDepthDoesNotBecomeGlobalMiddleGesture)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto station = scene->SnapshotStations().front();
    auto take = station->AddTake();
    scene->CommitChanges();
        engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
    take->SetGroupMutedAudio(true);
    std::vector<unsigned char> stationPath;
    for (const auto index : station->GlobalId()) stationPath.push_back(static_cast<unsigned char>(index + 1u));
    scene->SetHover3d(stationPath, base::Action::MODIFIER_NONE);
    actions::TouchAction click;
    click.Touch = actions::TouchAction::TOUCH_MOUSE;
    click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
    click.Index = 1;
    click.MouseButtonsDown = 2u;
    click.State = actions::TouchAction::TOUCH_DOWN;
    scene->OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
    click.State = actions::TouchAction::TOUCH_UP;
    click.MouseButtonsDown = 0u;
    scene->OnAction(click);
    ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
    EXPECT_TRUE(take->IsMuted());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, WindowFreshPressPickWinsOverStaleBackgroundAndItemHover)
{
    for (const bool initiallyMuted : { false, true })
    {
        auto scene = FreshScene({ Station("Local") }, {});
        ASSERT_TRUE(scene);
        resources::ResourceLib resources;
        engine::PressPickWindow window(*scene, resources);
        window.Resize({ 800, 600 });
        auto station = scene->SnapshotStations().front();
        auto take = station->AddTake();
        take->AddLoop(0u, station->Id());
        auto untouched = station->AddTake();
        scene->CommitChanges();
        engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
        take->SetGroupMutedAudio(initiallyMuted);
        take->GetLoops().front()->SetGroupMutedAudio(initiallyMuted);
        untouched->SetGroupMutedAudio(initiallyMuted);
        for (const auto index : take->GetLoops().front()->GlobalId())
            window.PressPath.push_back(static_cast<unsigned char>(index + 1u));
        scene->SetHover3d({}, base::Action::MODIFIER_NONE); // Previous rendered hover was background.
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
        click.Index = 1;
        click.State = actions::TouchAction::TOUCH_DOWN;
        EXPECT_TRUE(window.OnAction(click).IsEaten);
        EXPECT_EQ(1u, window.PickCount);
        EXPECT_EQ(click.Position, window.PickPosition);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePressed(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        actions::TouchMoveAction move;
        move.Touch = actions::TouchAction::TOUCH_MOUSE;
        move.Position = click.Position + utils::Position2d{5, 0};
        move.MouseButtonsDown = 2u;
        window.OnAction(move);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::ItemMutePainting(*scene));
        EXPECT_EQ(!initiallyMuted, take->IsMuted());
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        click.State = actions::TouchAction::TOUCH_UP;
        click.Position = move.Position;
        window.OnAction(click);
        // The inverse stale-hover case must acquire a genuine background press.
        window.PressPath.clear();
        click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene) + utils::Position2d{20, 0};
        click.State = actions::TouchAction::TOUCH_DOWN;
        window.OnAction(click);
        EXPECT_EQ(2u, window.PickCount);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        click.State = actions::TouchAction::TOUCH_UP;
        window.OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_FALSE(take->IsMuted());
        EXPECT_FALSE(untouched->IsMuted());
        scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, InWindowBackgroundGesturesPreviewAndApplyOnlyStationaryRelease)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto station = scene->SnapshotStations().front();
    auto take = station->AddTake();
    take->AddLoop(0u, station->Id());
    take->CommitChanges();
    scene->CommitChanges();
    auto loop = take->GetLoops().front();
    for (const bool mute : { false, true })
    {
        for (int movement = 0; movement < 5; ++movement)
        {
            take->SetGroupMutedAudio(!mute);
            loop->SetGroupMutedAudio(!mute);
            scene->SetHover3d({}, base::Action::MODIFIER_NONE);
            const auto pose = engine::SourceLossIntegrationTestAccess::CameraPose(*scene);
            actions::TouchAction click;
            click.Touch = actions::TouchAction::TOUCH_MOUSE;
            click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene); // Empty space inside the pass-through HUD.
            click.Index = 1;
            click.Modifiers = mute ? base::Action::MODIFIER_ALT : base::Action::MODIFIER_NONE;
            click.MouseButtonsDown = 2u;
            click.State = actions::TouchAction::TOUCH_DOWN;
            ASSERT_TRUE(scene->OnAction(click).IsEaten);
            ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
            EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            EXPECT_EQ(!mute, take->IsMuted());
            actions::TouchMoveAction move;
            move.Touch = actions::TouchAction::TOUCH_MOUSE;
            move.Position = click.Position;
            move.MouseButtonsDown = click.MouseButtonsDown;
            scene->OnAction(move); // A duplicate position is not movement.
            EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            if (movement == 1 || movement == 2)
            {
                move.Position.X += 1;
                scene->OnAction(move);
                EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
                EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
                if (movement == 2) // Returning to the origin does not restore the click.
                {
                    move.Position = click.Position;
                    scene->OnAction(move);
                    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
                }
                click.Position = move.Position;
            }
            else if (movement == 4)
            {
                move.IsRelative = true;
                move.RelativeDelta = { 1, 0 };
                scene->OnAction(move);
                EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            }
            else if (movement == 3) click.Position.X += 1; // Missing MOVE event.
            click.State = actions::TouchAction::TOUCH_UP;
            click.Modifiers = base::Action::MODIFIER_NONE; // Command was latched at press.
            click.MouseButtonsDown = 0u;
            EXPECT_TRUE(scene->OnAction(click).IsEaten);
            EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
            EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
            EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraBackgroundActive(*scene));
            const auto after = engine::SourceLossIntegrationTestAccess::CameraPose(*scene);
            EXPECT_EQ(pose.Eye, after.Eye);
            EXPECT_EQ(pose.Forward, after.Forward);
            EXPECT_EQ(pose.Up, after.Up);
            ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
            EXPECT_EQ(movement == 0 ? mute : !mute, take->IsMuted());
            EXPECT_EQ(movement == 0 ? mute : !mute, loop->IsMuted());
        }
    }
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, NativeBackgroundMiddleAndAltMiddleUseClickCommands)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    resources::ResourceLib resources;
    engine::NativeKeyboardWindow window(*scene, resources);
    window.Resize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(host, nullptr);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    for (const bool mute : { false, true })
    {
        take->SetGroupMutedAudio(!mute);
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        window.NativeKeys.set(VK_MENU, mute);
        if (mute) graphics::Window::WindowProcedure(host, WM_SYSKEYDOWN, VK_MENU, 1u << 29u);
        graphics::Window::WindowProcedure(host, WM_MBUTTONDOWN,
            MK_MBUTTON, MAKELPARAM(560, 400));
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
        window.NativeKeys.reset(VK_MENU);
        if (mute) graphics::Window::WindowProcedure(host, WM_SYSKEYUP, VK_MENU, 1u << 29u);
        graphics::Window::WindowProcedure(host, WM_MBUTTONUP, 0, MAKELPARAM(560, 400));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_EQ(mute, take->IsMuted());
    }
    SetWindowLongPtrW(host, GWLP_USERDATA, 0);
    DestroyWindow(host);
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, NativeAltLeftBackgroundPreviewsAndSelectsAllWithoutMuting)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    resources::ResourceLib resources;
    engine::NativeKeyboardWindow window(*scene, resources);
    window.Resize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
    scene->SetHover3d({}, base::Action::MODIFIER_NONE);
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(host, nullptr);
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    window.NativeKeys.set(VK_MENU);
    graphics::Window::WindowProcedure(host, WM_SYSKEYDOWN, VK_MENU, 1u << 29u);
    graphics::Window::WindowProcedure(host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(560, 400));
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::SelectionPreviewActive(*scene));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
    EXPECT_FALSE(take->IsSelected());
    window.NativeKeys.reset(VK_MENU);
    graphics::Window::WindowProcedure(host, WM_SYSKEYUP, VK_MENU, 1u << 29u);
    graphics::Window::WindowProcedure(host, WM_LBUTTONUP, 0, MAKELPARAM(560, 400));
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundPreviewActive(*scene));
    EXPECT_TRUE(take->IsSelected());
    EXPECT_FALSE(take->IsMuted());
    SetWindowLongPtrW(host, GWLP_USERDATA, 0);
    DestroyWindow(host);
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, BackgroundSelectAllUsesCurrentDepthAndPreservesMuteAndRemoteSelection)
{
    for (const auto depth : { engine::Scene::VIEW_STATION, engine::Scene::VIEW_LOOPTAKE, engine::Scene::VIEW_LOOP })
    {
        auto scene = FreshScene({ Station("Local"), Station("Second") }, {});
        ASSERT_TRUE(scene);
        scene->SetSize({ 800, 600 });
        auto station = scene->SnapshotStations().front();
        auto audioTake = station->AddTake();
        audioTake->AddLoop(0u, station->Id());
        auto midiTake = station->AddTake();
        midiTake->Record({}, station->Name(), { 0u }, { "Keys" });
        auto emptyTake = station->AddTake();
        scene->CommitChanges();
        auto audioLoop = audioTake->GetLoops().front();
        const auto midiModel = midiTake->GetMidiLoops().front()->Model();
        ASSERT_TRUE(audioLoop->Model());
        ASSERT_TRUE(midiModel);
        audioTake->SetGroupMutedAudio(true);
        audioLoop->SetGroupMutedAudio(true);
        midiTake->SetGroupMutedAudio(false);
        midiTake->AcquireTriggerSourceMuteAudio();
        engine::StationParams params;
        params.Id = "remote";
        params.Size = { 160u, 320u };
        auto remote = std::make_shared<engine::RemoteSelectionStation>(params,
            engine::Station::GetMixerParams(params.Size, audio::MergeMixBehaviourParams{}));
        auto remoteSelected = remote->AddTake();
        auto remoteUnselected = remote->AddTake();
        remote->CommitChanges();
        remote->Select();
        remoteSelected->Select();
        remoteUnselected->DeSelect();
        remoteSelected->SetGroupMutedAudio(true);
        engine::SourceLossIntegrationTestAccess::AddStation(*scene, remote);
        engine::SourceLossIntegrationTestAccess::SetDepth(*scene, depth);
        const auto rack = station->GetGuiRack();
        rack->SetPosition({ -1000, -1000 });
        rack->SetRackState(gui::GuiRackParams::RACK_CHANNELS, true);
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
        click.Index = 0;
        click.Modifiers = base::Action::MODIFIER_ALT;
        click.MouseButtonsDown = 1u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        ASSERT_TRUE(scene->OnAction(click).IsEaten);
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::SelectionPreviewActive(*scene));
        EXPECT_FALSE(audioTake->IsSelected());
        click.State = actions::TouchAction::TOUCH_UP;
        click.MouseButtonsDown = 0u;
        scene->OnAction(click);
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::SelectionPreviewActive(*scene));
        EXPECT_EQ(depth == engine::Scene::VIEW_STATION, station->IsSelected());
        EXPECT_EQ(depth == engine::Scene::VIEW_STATION, scene->SnapshotStations()[1]->IsSelected());
        EXPECT_TRUE(audioTake->IsSelected());
        EXPECT_TRUE(midiTake->IsSelected());
        EXPECT_TRUE(audioLoop->IsSelected());
        EXPECT_TRUE(midiModel->IsSelected());
        EXPECT_EQ(depth != engine::Scene::VIEW_LOOP, emptyTake->IsSelected());
        EXPECT_TRUE(audioTake->IsMuted());
        EXPECT_TRUE(audioLoop->IsMuted());
        EXPECT_TRUE(midiTake->IsMuted()); // Temporary source suppression survives selection.
        EXPECT_FALSE(midiTake->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
        midiTake->ReleaseTriggerSourceMuteAudio();
        EXPECT_FALSE(midiTake->IsMuted());
        EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
        EXPECT_TRUE(remote->IsSelected());
        EXPECT_TRUE(remoteSelected->IsSelected());
        EXPECT_FALSE(remoteUnselected->IsSelected());
        EXPECT_TRUE(remoteSelected->IsMuted());
        scene->ApplyDeferredHoverUpdates();
        EXPECT_TRUE(audioTake->IsSelected()); // Selection survives committed hover refresh.
        EXPECT_TRUE(midiModel->IsSelected());
        scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, BackgroundSelectAllMovementKeepsPreviewButDoesNotSelectOrPan)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    for (const bool returnToOrigin : { false, true })
    {
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        const auto pose = engine::SourceLossIntegrationTestAccess::CameraPose(*scene);
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
        click.Index = 0;
        click.Modifiers = base::Action::MODIFIER_ALT;
        click.MouseButtonsDown = 1u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        scene->OnAction(click);
        actions::TouchMoveAction move;
        move.Touch = actions::TouchAction::TOUCH_MOUSE;
        move.Position = click.Position + utils::Position2d{1, 0};
        move.MouseButtonsDown = 1u;
        scene->OnAction(move);
        if (returnToOrigin) { move.Position = click.Position; scene->OnAction(move); }
        EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::SelectionPreviewActive(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
        click.Position = move.Position;
        click.State = actions::TouchAction::TOUCH_UP;
        click.MouseButtonsDown = 0u;
        scene->OnAction(click);
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_FALSE(take->IsSelected());
        EXPECT_EQ(pose.Eye, engine::SourceLossIntegrationTestAccess::CameraPose(*scene).Eye);
    }
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, BackgroundSelectAllCancelsOnLossEscapeResetAndChord)
{
    for (int cancellation = 0; cancellation < 5; ++cancellation)
    {
        auto scene = FreshScene({ Station("Local") }, {});
        ASSERT_TRUE(scene);
        scene->SetSize({ 800, 600 });
        auto take = scene->SnapshotStations().front()->AddTake();
        scene->CommitChanges();
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
        click.Index = 0;
        click.Modifiers = base::Action::MODIFIER_ALT;
        click.MouseButtonsDown = 1u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        scene->OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::SelectionPreviewActive(*scene));
        if (cancellation == 0)
        {
            actions::TouchMoveAction lost;
            lost.Touch = actions::TouchAction::TOUCH_MOUSE;
            lost.MouseButtonsDown = 0u;
            scene->OnAction(lost);
        }
        else if (cancellation == 1) scene->OnInputFocusLost();
        else if (cancellation == 2)
        {
            actions::KeyAction escape;
            escape.KeyChar = 27u;
            escape.KeyActionType = actions::KeyAction::KEY_DOWN;
            scene->OnAction(escape);
        }
        else if (cancellation == 3) scene->Reset();
        else
        {
            auto chord = click;
            chord.Index = 1;
            chord.MouseButtonsDown = 3u;
            scene->OnAction(chord);
        }
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::SelectionPreviewActive(*scene));
        click.State = actions::TouchAction::TOUCH_UP;
        click.MouseButtonsDown = 0u;
        scene->OnAction(click);
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_FALSE(take->IsSelected());
        scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, AltLeftItemSelectsOnlyItsPressedTarget)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    resources::ResourceLib resources;
    engine::PressPickWindow window(*scene, resources);
    window.Resize({ 800, 600 });
    auto station = scene->SnapshotStations().front();
    auto take = station->AddTake();
    take->AddLoop(0u, station->Id());
    take->CommitChanges();
    auto other = station->AddTake();
    scene->CommitChanges();
    engine::SourceLossIntegrationTestAccess::SelectTakeDepth(*scene);
    for (const auto index : take->GetLoops().front()->GlobalId())
        window.PressPath.push_back(static_cast<unsigned char>(index + 1u));
    actions::TouchAction click;
    click.Touch = actions::TouchAction::TOUCH_MOUSE;
    click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
    click.Index = 0;
    click.Modifiers = base::Action::MODIFIER_ALT;
    click.State = actions::TouchAction::TOUCH_DOWN;
    window.OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
    click.State = actions::TouchAction::TOUCH_UP;
    window.OnAction(click);
    EXPECT_TRUE(take->IsSelected());
    EXPECT_FALSE(other->IsSelected());
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, BackgroundUnmuteCancelsOnCaptureLossFocusLossEscapeResetAndChord)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    for (int cancellation = 0; cancellation < 5; ++cancellation)
    {
        take->SetGroupMutedAudio(true);
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        actions::TouchAction click;
        click.Touch = actions::TouchAction::TOUCH_MOUSE;
        click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
        click.Index = 1;
        click.MouseButtonsDown = 2u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        scene->OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        if (cancellation == 0)
        {
            actions::TouchMoveAction lost;
            lost.Touch = actions::TouchAction::TOUCH_MOUSE;
            lost.MouseButtonsDown = 0u;
            scene->OnAction(lost);
        }
        else if (cancellation == 1) scene->OnInputFocusLost();
        else if (cancellation == 2)
        {
            actions::KeyAction escape;
            escape.KeyChar = 27u;
            escape.KeyActionType = actions::KeyAction::KEY_DOWN;
            scene->OnAction(escape);
        }
        else if (cancellation == 3) scene->Reset();
        else
        {
            auto chord = click;
            chord.Index = 0;
            chord.MouseButtonsDown = 3u;
            scene->OnAction(chord);
        }
        click.State = actions::TouchAction::TOUCH_UP;
        click.MouseButtonsDown = 0u;
        scene->OnAction(click);
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_TRUE(take->IsMuted()) << cancellation;
        scene->AdvanceUiAnimations();
    }
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, MiddlePressOnItemAndHudDoesNotAcquireGlobalUnmute)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    auto sibling = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    sibling->SetGroupMutedAudio(true);
    std::vector<unsigned char> itemPath;
    for (auto index : take->GlobalId()) itemPath.push_back(static_cast<unsigned char>(index + 1u));
    scene->SetHover3d(itemPath, base::Action::MODIFIER_NONE);
    actions::TouchAction click;
    click.Touch = actions::TouchAction::TOUCH_MOUSE;
    click.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
    click.Index = 1;
    click.MouseButtonsDown = 2u;
    click.State = actions::TouchAction::TOUCH_DOWN;
    scene->OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
    click.State = actions::TouchAction::TOUCH_UP;
    click.MouseButtonsDown = 0u;
    scene->OnAction(click);
    ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
    EXPECT_TRUE(sibling->IsMuted());
    scene->SetHover3d({}, base::Action::MODIFIER_NONE);
    click.Position = { 10, 10 };
    click.State = actions::TouchAction::TOUCH_DOWN;
    click.MouseButtonsDown = 2u;
    scene->OnAction(click);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
    scene->OnInputFocusLost();
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, BackgroundMuteStopsUnownedCameraCoastButPreservesHeldCameraChord)
{
    auto scene = FreshScene({ Station("Local") }, {});
    ASSERT_TRUE(scene);
    scene->SetSize({ 800, 600 });
    auto take = scene->SnapshotStations().front()->AddTake();
    scene->CommitChanges();
    for (const bool mute : { false, true })
    {
        take->SetGroupMutedAudio(!mute);
        scene->SetHover3d({}, base::Action::MODIFIER_NONE);
        actions::TouchAction right;
        right.Touch = actions::TouchAction::TOUCH_MOUSE;
        right.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
        right.Index = 2;
        right.MouseButtonsDown = 4u;
        right.State = actions::TouchAction::TOUCH_DOWN;
        scene->OnAction(right);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
        actions::TouchMoveAction move;
        move.Touch = actions::TouchAction::TOUCH_MOUSE;
        move.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene, {50, 20});
        move.MouseButtonsDown = 4u;
        scene->OnAction(move);
        right.Position = move.Position;
        right.MouseButtonsDown = 0u;
        right.Value = 4;
        right.State = actions::TouchAction::TOUCH_UP;
        scene->OnAction(right);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::CameraBackgroundActive(*scene));
        ASSERT_FALSE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
        auto click = right;
        click.Index = 1;
        click.Modifiers = mute ? base::Action::MODIFIER_ALT : base::Action::MODIFIER_NONE;
        click.MouseButtonsDown = 2u;
        click.State = actions::TouchAction::TOUCH_DOWN;
        scene->OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
        EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::CameraBackgroundActive(*scene));
        click.MouseButtonsDown = 0u;
        click.State = actions::TouchAction::TOUCH_UP;
        scene->OnAction(click);
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_EQ(mute, take->IsMuted());
    }
    actions::TouchAction right;
    right.Touch = actions::TouchAction::TOUCH_MOUSE;
    right.Position = engine::SourceLossIntegrationTestAccess::CanvasPoint(*scene);
    right.Index = 2;
    right.MouseButtonsDown = 4u;
    right.State = actions::TouchAction::TOUCH_DOWN;
    scene->OnAction(right);
    auto chord = right;
    chord.Index = 1;
    chord.MouseButtonsDown = 6u;
    scene->OnAction(chord);
    EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
    EXPECT_TRUE(engine::SourceLossIntegrationTestAccess::CameraPointerHeld(*scene));
    chord.State = actions::TouchAction::TOUCH_UP;
    chord.MouseButtonsDown = 4u;
    chord.Value = 2;
    scene->OnAction(chord);
    right.State = actions::TouchAction::TOUCH_UP;
    right.MouseButtonsDown = 0u;
    right.Value = 4;
    scene->OnAction(right);
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, MembershipConfirmationRejectsChangedAcceptedRevision)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	for (const bool ditchTake : { false, true })
	{
		auto scene = FreshScene({ Station("A"), Station("B") }, {});
		ASSERT_TRUE(scene);
		auto station = scene->SnapshotStations().front();
		auto take = station->AddTake();
		take->AddLoop(0u, station->Id());
		scene->CommitChanges();
		{
			auto excludeJob = Access::ExcludeJob(*scene);
			if (ditchTake) Access::RequestTakeDitch(*scene, station, take);
			else Access::Manage(*scene, false, station);
			ASSERT_TRUE(Access::ConfirmationOpen(*scene));
			Access::AdvanceAcceptedRevision(*scene);
			if (ditchTake)
			{
				actions::GuiAction confirm;
				confirm.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON;
				confirm.Index = gui::GuiStationControls::ConfirmTakeDitchControlIndex;
				scene->OnAction(confirm);
			}
			else Access::ConfirmRemoval(*scene);
			EXPECT_EQ(2u, scene->SnapshotStations().size());
			EXPECT_EQ(1u, station->NumTakes());
			EXPECT_EQ(take, station->GetLoopTakeSnapshot().front());
			EXPECT_FALSE(Access::Busy(*scene));
			EXPECT_FALSE(station->MembershipRemoving());
		}
		scene->Shutdown();
	}
}

TEST_F(SceneRoutingIntegrationTest, StoppedStreamInteractionUsesConfiguredSampleRate)
{
	io::JamFile jam{};
	jam.Stations = { Station("Local") };
	auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson)).value();
	rig.Triggers.clear();
	rig.Name = "Stopped stream";
	rig.User.Audio.SampleRate = 48000u;
	engine::SceneParams params({ "" }, {}, { 640u, 480u });
	auto created = engine::Scene::FromFile(params, jam, rig, L"");
	ASSERT_TRUE(created.has_value());
	auto scene = *created;
	EXPECT_EQ(0u, scene->GetAudioStreamParams().SampleRate);
	EXPECT_EQ(48000u, engine::SourceLossIntegrationTestAccess::InteractionSampleRate(*scene));
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ResizingRehitsStationaryPointer)
{
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	scene->SetSize({ 800u, 600u });
	const auto handle = engine::SourceLossIntegrationTestAccess::SelectionHandle(*scene);
	ASSERT_TRUE(handle);
	const auto size = handle->GetSize();
	const auto point = handle->GlobalPosition() + utils::Position2d{ static_cast<int>(size.Width / 2u), static_cast<int>(size.Height / 2u) };
	scene->SetSize({ 800u, 900u });
	actions::TouchMoveAction move;
	move.Touch = actions::TouchAction::TOUCH_MOUSE;
	move.Position = point;
	move.MouseButtonsDown = 0u;
	scene->OnAction(move);
	scene->ApplyDeferredHoverUpdates();
	ASSERT_FALSE(handle->RouteHitTest(handle->GlobalToLocal(point)));
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, handle->GetState());
	scene->SetSize({ 800u, 600u });
	ASSERT_TRUE(handle->RouteHitTest(handle->GlobalToLocal(point)));
	scene->ApplyDeferredHoverUpdates(); // No further pointer event: resize must refresh the target.
	EXPECT_EQ(base::GuiElement::STATE_OVER, handle->GetState());
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, AudioRefreshCoalescesNotificationsAndDefersWhileBusy)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		scene->Controls().RequestAudioRefresh();
		scene->Controls().RequestAudioRefresh();
		EXPECT_FALSE(Access::ScheduleAudioRefresh(*scene, {}, true));
		EXPECT_TRUE(Access::AudioRefreshPending(*scene));
		EXPECT_FALSE(Access::ScheduleAudioRefresh(*scene, {true, false, false}));
		EXPECT_TRUE(Access::AudioRefreshPending(*scene));
		EXPECT_TRUE(Access::ScheduleAudioRefresh(*scene));
		EXPECT_TRUE(Access::AudioRefreshOperation(*scene));
		EXPECT_FALSE(Access::AudioRefreshPending(*scene));
		Access::FinishTestAudioRefresh(*scene);
		EXPECT_FALSE(Access::ScheduleAudioRefresh(*scene));
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, AudioRefreshRetriesConfiguredDeviceWithoutApplyingDraftOrPolling)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		const auto configured = Access::SubmittedAudio(*scene);
		auto draft = configured;
		draft.Name = "Uncommitted different hardware";
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		scene->Controls().RequestAudioRefresh();
		// Empty startup inventory must not reject recovery before fresh discovery.
		EXPECT_TRUE(Access::ScheduleAudioRefresh(*scene));
		EXPECT_EQ(configured, Access::FailAudioRefreshWithoutHardware(*scene));
		EXPECT_EQ(draft, Access::SubmittedAudio(*scene));
		Access::FinishTestAudioRefresh(*scene);
		EXPECT_FALSE(Access::ScheduleAudioRefresh(*scene));
		// A late-ready driver or failed open receives one new attempt on rescan.
		actions::GuiAction rescan;
		rescan.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON;
		rescan.Index = 7207u;
		scene->OnAction(rescan);
		EXPECT_TRUE(Access::ScheduleAudioRefresh(*scene));
		EXPECT_EQ(configured, Access::FailAudioRefreshWithoutHardware(*scene));
		Access::FinishTestAudioRefresh(*scene);
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, OpenBackupStreamRequiresManualRescanAndKeepsItsActualGeometry)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		const auto configured = Access::ConfiguredAudio(*scene);
		auto draft = configured;
		draft.Name = "Unsaved different card";
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		scene->Controls().RequestAudioRefresh();
		// Explicit lifecycle observation represents a real open backup stream;
		// no physical ASIO device is constructed by this policy test.
		EXPECT_FALSE(Access::ScheduleRefreshWithOpenStream(*scene));
		EXPECT_TRUE(Access::ControlIdle(*scene));
		EXPECT_FALSE(Access::AudioRefreshPending(*scene));
		EXPECT_NE(std::string::npos, Access::DevicePresentation(*scene).DeviceStatus.find("Rescan audio devices"));
		EXPECT_FALSE(Access::ScheduleAudioRefresh(*scene)); // No periodic retry.

		Access::RequestManualAudioRescan(*scene);
		EXPECT_TRUE(Access::ScheduleRefreshWithOpenStream(*scene));
		EXPECT_TRUE(Access::ManualAudioRescanOperation(*scene)); // Manual intent survives admission.
		const audio::AudioStreamParams backup{"Current backup", 48000u, 3u, 256u, 11u, 22u, 1u, 2u};
		const auto requested = Access::FailAudioRefreshWithoutHardware(*scene, backup);
		EXPECT_EQ(backup.Name, requested.Name);
		EXPECT_EQ(backup.SampleRate, requested.SampleRate);
		EXPECT_EQ(backup.BufSize, requested.BufSize);
		EXPECT_EQ(backup.NumBuffers, requested.NumBuffers);
		EXPECT_EQ(backup.NumInputChannels, requested.NumChannelsIn);
		EXPECT_EQ(backup.NumOutputChannels, requested.NumChannelsOut);
		EXPECT_EQ(configured, Access::ConfiguredAudio(*scene));
		EXPECT_EQ(configured, scene->AcceptedRigSnapshot()->Rig.User.Audio);
		EXPECT_EQ(draft, Access::SubmittedAudio(*scene));
		Access::FinishTestAudioRefresh(*scene);
		EXPECT_FALSE(Access::ScheduleAudioRefresh(*scene));
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SuccessfulRescanDisplaysRestoredFallbackWithoutChangingSavedPreference)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		const auto configured = Access::ConfiguredAudio(*scene);
		audio::AsioInventory inventory;
		inventory.Devices = {{0u, "Primary ASIO device", true, "", 18u, 20u, {44100u}, 44100u},
			{1u, "Fallback ASIO device", true, "", 8u, 4u, {44100u}, 44100u}};
		Access::StartedAudioSelection(*scene, inventory, {"Primary ASIO device", 44100u, 2u, 64u, 0u, 0u, 8u, 4u});
		auto restored = configured;
		restored.Name = "Fallback ASIO device";
		restored.SampleRate = 44100u;
		restored.BufSize = 1024u;
		restored.NumChannelsIn = 8u;
		restored.NumChannelsOut = 4u;
		Access::AcceptRescannedAudioDraft(*scene, restored);
		EXPECT_EQ(restored, Access::SubmittedAudio(*scene));
		EXPECT_EQ(restored, Access::AudioDraft(*scene));
		EXPECT_EQ("Fallback ASIO device", Access::AudioDraft(*scene).Name);
		EXPECT_EQ(configured, Access::ConfiguredAudio(*scene));
		EXPECT_EQ(configured, scene->AcceptedRigSnapshot()->Rig.User.Audio);
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, FailedAudioRequestStillPublishesHostInventoryForDevicePage)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		audio::AsioInventory inventory;
		inventory.Devices = {{0u, "Newly powered preferred", true, "", 8u, 8u, {44100u}, 44100u}};
		// Inject the host's discovery result to exercise the completion/publication
		// path independently of physical ASIO discovery and open failure.
		Access::SetHostInventory(*scene, inventory);
		Access::RequestManualAudioRescan(*scene);
		ASSERT_TRUE(Access::ScheduleRefreshWithOpenStream(*scene));
		Access::FailAudioRefreshWithoutHardware(*scene, {"Current backup", 44100u, 2u, 128u, 0u, 0u, 2u, 2u});
		const auto presentation = Access::DevicePresentation(*scene);
		ASSERT_TRUE(presentation.PagesChanged);
		ASSERT_EQ(1u, presentation.Inventory.Devices.size());
		EXPECT_EQ("Newly powered preferred", presentation.Inventory.Devices.front().Name);
		EXPECT_TRUE(presentation.Inventory.Devices.front().Probed);
		EXPECT_NE(std::string::npos, presentation.DeviceStatus.find("Backend teardown"));
		Access::FinishTestAudioRefresh(*scene);
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, UnavailableDropdownSelectionCanReachFreshDiscoveryWithValidGeometry)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		audio::AsioInventory inventory;
		inventory.Devices = {{0u, "Current backup", true, "", 2u, 2u, {44100u}, 44100u},
			{1u, "Preferred", false, "Powered off at startup"}};
		Access::StartedAudioSelection(*scene, inventory, {"Current backup", 44100u, 2u, 128u, 0u, 0u, 2u, 2u});
		Access::ChooseAudio(*scene, 1);
		EXPECT_EQ("Preferred", Access::SubmittedAudio(*scene).Name);
		std::string error;
		EXPECT_TRUE(Access::ValidateAudioDraft(*scene, false, error));
		auto draft = Access::SubmittedAudio(*scene);
		draft.Name = "Not in stale inventory";
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		EXPECT_TRUE(Access::ValidateAudioDraft(*scene, false, error));
		draft.NumChannelsIn = draft.NumChannelsOut = 0u;
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		EXPECT_FALSE(Access::ValidateAudioDraft(*scene, false, error));
		draft.NumChannelsOut = 2u;
		draft.BufSize = 0u;
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		EXPECT_FALSE(Access::ValidateAudioDraft(*scene, false, error));
		draft.BufSize = 128u;
		draft.SampleRate = Access::ConfiguredAudio(*scene).SampleRate == 48000u ? 44100u : 48000u;
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		EXPECT_FALSE(Access::ValidateAudioDraft(*scene, true, error));
		EXPECT_EQ("Sample rate cannot change while any takes exist", error);
		draft.Name = "Current backup";
		draft.SampleRate = 44100u;
		draft.NumChannelsOut = 3u;
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		EXPECT_FALSE(Access::ValidateAudioDraft(*scene, false, error));
		draft.NumChannelsOut = 2u;
		draft.SampleRate = 48000u;
		Access::SetUnsubmittedAudioDraft(*scene, draft);
		EXPECT_FALSE(Access::ValidateAudioDraft(*scene, false, error));
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, DeviceCompletionPreservesNewerDraftAndRejectsIndependentStaleIdentities)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene); // No physical MIDI enumeration/reconciliation in this test.
		Access::PrepareMidiDraft(*scene);
		Access::ToggleMidi(*scene, false);
		scene->PumpDeviceAndFileControls();
		const auto admitted = Access::TakeMidiRequest(*scene);
		ASSERT_FALSE(admitted.Desired.at(0u).Enabled);
		Access::ToggleMidi(*scene, true); // A newer real widget edit while the first request is in flight.
		for (unsigned int mismatch : {1u, 2u, 3u})
		{
			Access::CompleteMidi(*scene, admitted, mismatch);
			scene->PumpDeviceAndFileControls();
			EXPECT_TRUE(Access::MidiOperation(*scene));
			EXPECT_TRUE(Access::MidiEnabled(*scene));
		}
		Access::CompleteMidi(*scene, admitted);
		scene->PumpDeviceAndFileControls();
		EXPECT_FALSE(Access::MidiOperation(*scene));
		EXPECT_TRUE(Access::MidiEnabled(*scene));
		scene->PumpDeviceAndFileControls();
		const auto latest = Access::TakeMidiRequest(*scene);
		EXPECT_GT(latest.OperationId, admitted.OperationId);
		EXPECT_GT(latest.Revision, admitted.Revision);
		EXPECT_TRUE(latest.Desired.at(0u).Enabled);
		Access::CompleteMidi(*scene, latest);
		scene->PumpDeviceAndFileControls();
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, DevicePageCompletionCommitsPendingTextBeforeCheckingDraftRevision)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, {});
	ASSERT_TRUE(scene);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		audio::AsioInventory inventory;
		inventory.Devices = {{0u, "Device A", true, "", 18u, 20u, {44100u, 48000u}, 44100u}};
		const audio::AudioStreamParams actual{"Device A", 44100u, 2u, 128u, 0u, 0u, 2u, 2u};
		Access::StartedAudioSelection(*scene, inventory, actual);
		const auto revision = Access::AudioRevision(*scene);
		auto accepted = Access::AudioDraft(*scene);
		accepted.NumChannelsOut = 1u; accepted.SampleRate = 48000u;
		Access::FocusAudioText(*scene, "7"); // Still uncommitted: acceptance initially has the same revision.
		Access::ApplyPresentation(*scene, Access::AudioCompletion(*scene, accepted, revision));
		const auto draft = Access::AudioDraft(*scene);
		EXPECT_GT(Access::AudioRevision(*scene), revision);
		EXPECT_EQ(7u, draft.NumChannelsIn);
		EXPECT_EQ(2u, draft.NumChannelsOut);
		EXPECT_EQ(44100u, draft.SampleRate);
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ReplacedSceneRejectsOwnedControlPresentation)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto original = FreshScene({ Station("Original") }, {});
	ASSERT_TRUE(original);
	auto staleSettings = Access::AudioDraft(*original);
	staleSettings.Name = "Outgoing device";
	auto completion = Access::AudioCompletion(*original, staleSettings, Access::AudioRevision(*original));
	original->Shutdown(); original.reset();
	auto replacement = FreshScene({ Station("Replacement") }, {});
	ASSERT_TRUE(replacement);
	const auto before = Access::AudioDraft(*replacement);
	ASSERT_NE(replacement->Identity(), completion.SceneId);
	Access::ApplyPresentation(*replacement, std::move(completion));
	EXPECT_EQ(before, Access::AudioDraft(*replacement));
	replacement->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, QueuedOwnedRigSaveDrainsExactlyOnceAfterImmediateShutdown)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto rig = io::RigFile::FromStream(std::stringstream(io::RigFile::DefaultJson)).value();
	rig.Triggers.clear(); rig.User.Midi.Devices.clear();
	io::JamFile jam{}; jam.Stations = { Station("Final save") };
	std::atomic<bool> releaseStartup{false}, sawQuit{false};
	engine::SceneParams params({ "" }, {}, {640u, 480u});
	params.OnJobThreadStart = [&] {
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!releaseStartup.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
	};
	auto created = engine::Scene::FromFile(params, jam, rig, L"");
	ASSERT_TRUE(created);
	auto scene = *created;
	const auto capture = scene->BeginExplicitRigSave();
	ASSERT_TRUE(capture);
	std::atomic<unsigned int> saves{0u};
	std::uint64_t savedScene = 0u, savedRevision = 0u;
	std::string savedName;
	scene->QueueExplicitRigSave([owned = *capture, &saves, &savedScene, &savedRevision, &savedName] {
		savedScene = owned.SceneId; savedRevision = owned.Revision; savedName = owned.Rig.Name;
		saves.fetch_add(1u, std::memory_order_release);
	});
	std::thread releaseAfterQuit([&] {
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!Access::Quitting(*scene) && std::chrono::steady_clock::now() < limit) std::this_thread::yield();
		sawQuit.store(Access::Quitting(*scene), std::memory_order_release);
		releaseStartup.store(true, std::memory_order_release);
	});
	scene->Shutdown();
	releaseAfterQuit.join();
	EXPECT_TRUE(sawQuit.load(std::memory_order_acquire));
	EXPECT_EQ(1u, saves.load(std::memory_order_acquire));
	EXPECT_EQ(capture->SceneId, savedScene); EXPECT_EQ(capture->Revision, savedRevision); EXPECT_EQ(capture->Rig.Name, savedName);
	scene->Shutdown();
	EXPECT_EQ(1u, saves.load(std::memory_order_acquire));
}

TEST_F(SceneRoutingIntegrationTest, CaptureBecomingBusyBeforeAdmissionRejectsSessionLoadAndRestoresInput)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({ Station("Local") }, { Trigger("Record", "Local") });
	ASSERT_TRUE(scene);
	const auto station = scene->SnapshotStations().front();
	const auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
	ASSERT_TRUE(trigger);
	{
		auto excludeJob = Access::ExcludeJob(*scene);
		ASSERT_TRUE(Access::InputGateOpen(*scene));
		scene->Controls().RequestSessionLoad(L"test-owned-unopened-path.jam");
		scene->PumpDeviceAndFileControls(); // Request closes producers; admission still awaits job acknowledgement.
		EXPECT_FALSE(Access::InputGateOpen(*scene));
		io::UserConfig config{}; config.Audio.NumChannelsIn = 1u;
		Access::StartRecording(*scene, trigger, config); // An already-admitted capture wins before the writer drains.
		ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
		ASSERT_EQ(1u, station->NumTakes());
		excludeJob.unlock(); // The real producer acknowledges closure; the window may now acquire its writer.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!Access::ControlIdle(*scene) && std::chrono::steady_clock::now() < deadline)
		{
			scene->PumpDeviceAndFileControls();
			std::this_thread::yield();
		}
		EXPECT_TRUE(scene->Controls().TakeReadySessionLoad().empty());
		EXPECT_TRUE(Access::ControlIdle(*scene));
		EXPECT_TRUE(Access::InputGateOpen(*scene));
		EXPECT_EQ(station, scene->SnapshotStations().front());
		EXPECT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
		EXPECT_EQ(1u, station->NumTakes());
	}
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SaveControlsRemainEnabledWhileLoadKeepsItsBusyGate)
{
    using Access = engine::SourceLossIntegrationTestAccess;
    auto scene = FreshScene({Station("Empty")}, {}); ASSERT_TRUE(scene);
    auto exclusion = Access::ExcludeJob(*scene);
    Access::SetFileControls(*scene, false);
    EXPECT_TRUE(Access::SessionButton(*scene, 7210u)->IsEnabled());
    EXPECT_TRUE(Access::SessionButton(*scene, 7213u)->IsEnabled());
    EXPECT_FALSE(Access::SessionButton(*scene, 7211u)->IsEnabled());
    EXPECT_TRUE(Access::OrdinaryRigSave(*scene)->IsEnabled());
    EXPECT_TRUE(Access::RigSaveChoice(*scene, 1u)->IsEnabled());
    EXPECT_FALSE(Access::RigSaveChoice(*scene, 0u)->IsEnabled());
    actions::GuiAction command; command.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON;
    command.Index = gui::GuiMainPanel::SaveRigControlIndex; scene->OnAction(command);
    EXPECT_TRUE(scene->ConsumeRigSaveRequest());
    EXPECT_EQ(engine::Scene::RigSaveMode::Save, scene->Controls().RequestedRigSaveMode());
    exclusion.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, EmptyJamSaveDestinationFallbackCancellationFailureAndRepeatedAdmission)
{
    using Access = engine::SourceLossIntegrationTestAccess;
    auto scene = FreshScene({Station("Empty")}, {}); ASSERT_TRUE(scene);
    auto exclusion = Access::ExcludeJob(*scene);
    unsigned int picks = 0u; std::wstring chosen;
    Access::PickDirectory(*scene, [&](const auto&) { ++picks; return chosen; });
    Access::Save(*scene); EXPECT_EQ(1u, picks); EXPECT_TRUE(Access::ControlIdle(*scene));
    EXPECT_TRUE(scene->Controls().CurrentJamPath().empty());
    chosen = L"first-folder"; Access::Save(*scene); EXPECT_EQ(2u, picks);
    EXPECT_EQ(chosen, Access::PendingFolder(*scene));
    Access::Save(*scene); EXPECT_EQ(2u, picks); EXPECT_NE(std::string::npos, Access::FileStatus(*scene).find("busy"));
    Access::FinishPickedSave(*scene, true, chosen, L"first-folder/001.jam");
    Access::Save(*scene); EXPECT_EQ(2u, picks); EXPECT_EQ(chosen, Access::PendingFolder(*scene));
    Access::FinishPickedSave(*scene, false, L"failed-folder", L"failed.jam");
    EXPECT_EQ(L"first-folder/001.jam", scene->Controls().CurrentJamPath());
    EXPECT_EQ(chosen, Access::SaveFolder(*scene));
    chosen.clear(); Access::Save(*scene, true); EXPECT_EQ(3u, picks); EXPECT_TRUE(Access::ControlIdle(*scene));
    EXPECT_EQ(L"first-folder/001.jam", scene->Controls().CurrentJamPath());
    scene->Controls().SetCurrentJamPath(L"loaded/005.jam"); scene->Controls().SetSessionSaveFolder(L"loaded");
    Access::Save(*scene); EXPECT_EQ(3u, picks); EXPECT_EQ(L"loaded", Access::PendingFolder(*scene));
    Access::FinishPickedSave(*scene, true, L"loaded", L"loaded/006.jam");
    chosen = L"explicit"; Access::Save(*scene, true); EXPECT_EQ(4u, picks); EXPECT_EQ(chosen, Access::PendingFolder(*scene));
    Access::FinishPickedSave(*scene, true, chosen, L"explicit/001.jam");
    exclusion.unlock(); scene->Shutdown();
}

class ResetPreservationPlugin final : public vst::IVstPlugin
{
public:
    float Parameter = 0.73f;
    unsigned int Unloads = 0u;
    bool PreInit(const std::wstring&) override { return true; }
    bool Load(const std::wstring&, float, unsigned int, unsigned int, vst::HostedLayoutMode) override { return true; }
    void Unload() override { ++Unloads; }
    void ProcessBlock(float*, int32_t) noexcept override {}
    void ProcessBlockStereo(float*, float*, int32_t) noexcept override {}
    void ProcessBlockMulti(float* const*, int32_t, int32_t) noexcept override {}
    void SetParameter(unsigned int, float value) noexcept override { Parameter = value; }
    float GetParameter(unsigned int) const noexcept override { return Parameter; }
    void BeginMidiBlock(std::uint32_t, std::uint32_t) noexcept override {}
    void SendMidiEvent(const midi::MidiEvent&, bool) noexcept override {}
    bool OpenEditor(HWND) override { return false; }
    void CloseEditor() override {}
    utils::Size2d GetEditorSize() const noexcept override { return {}; }
    bool IsLoaded() const noexcept override { return true; }
    const std::string& Name() const noexcept override { return StateId(); }
    vst::RenderRole Role() const noexcept override { return vst::RenderRole::Effect; }
    vst::MidiInputCapabilities MidiInput() const noexcept override { return {true, true, true}; }
    void SetBypassed(bool) noexcept override {}
    bool IsBypassed() const noexcept override { return false; }
    std::vector<std::uint8_t> GetState() const override { return {7u, 3u}; }
    void SetState(const std::vector<std::uint8_t>&) override {}
};

TEST_F(SceneRoutingIntegrationTest, ResetAllConfirmationCancellationAndSerializedRetirementPreserveRigAndRouting)
{
    using Access = engine::SourceLossIntegrationTestAccess;
    auto scene = FreshScene({Station("Keys"), Station("Drums")}, {Trigger("Keys trigger", "Keys")}); ASSERT_TRUE(scene);
    auto exclusion = Access::ExcludeJob(*scene);
    auto stations = scene->SnapshotStations(); auto station = stations.front();
    auto take = station->AddTake(); auto other = stations.back()->AddTake(); scene->CommitChanges();
    station->SetAllowedMidiChannels({2,5,11});
    ASSERT_TRUE(station->RestoreMixerLevels(0.25, std::vector<double>(station->NumBusChannels(), 0.5)));
    auto plugin = std::make_shared<ResetPreservationPlugin>();
    actions::JobAction load; load.JobActionType = actions::JobAction::JOB_LOADVST;
    load.VstPath = L"test-owned-reset.vst3"; load.PreInitPlugin = plugin;
    station->OnAction(load); station->CommitChanges(); ASSERT_EQ(plugin, station->GetVstPlugin(0u));
    auto routed = station->MidiChannelRoutes(); routed[1].PluginIds = {plugin->StateId()};
    ASSERT_TRUE(station->SetMidiChannelRoutes(routed, station->RackRevision()));
    const auto routes = station->MidiChannelRoutes(); const auto rack = station->GetGuiRack();
    rack->AddRoute(0u, 0u); const auto cables = rack->Routes();
    const auto engineRoutes = station->SnapshotAudioRoutesForExport();
    const auto revision = scene->AcceptedRigSnapshot()->Revision;
    Access::SetClock(*scene);
    Access::SetResetQuantisationState(*scene);
    station->SetStationPhaseOffsetSamps(-123);
    stations.back()->SetStationPhaseOffsetSamps(456);
    scene->Controls().SetCurrentJamPath(L"previous/001.jam"); scene->Controls().SetSessionSaveFolder(L"previous");
    actions::GuiAction reset; reset.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON; reset.Index = 7214u;
    scene->OnAction(reset); const auto decision = Access::ResetDecision(*scene); ASSERT_TRUE(decision);
    scene->OnAction(reset); EXPECT_EQ(decision, Access::ResetDecision(*scene));
    Access::ResolveReset(*scene, false);
    EXPECT_EQ(1u, station->GetLoopTakeSnapshot().size()); EXPECT_DOUBLE_EQ(0.25, station->MasterLevelForExport());
    EXPECT_EQ(96000ul, Access::ClockLength(*scene)); EXPECT_EQ(L"previous/001.jam", scene->Controls().CurrentJamPath());
    EXPECT_EQ(321, Access::GlobalPhase(*scene));
    EXPECT_EQ(-123, station->StationPhaseOffsetSamps());
    EXPECT_EQ(456, stations.back()->StationPhaseOffsetSamps());
    for (const auto& retained : stations) EXPECT_EQ(321, retained->GlobalPhaseOffsetSamps());

    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    io::UserConfig captureConfig{}; captureConfig.Audio.NumChannelsIn = 1u;
    Access::StartRecording(*scene, trigger, captureConfig);
    ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    const auto beforeResetCount = station->NumTakes();
    scene->OnAction(reset); Access::ResolveReset(*scene, true);
    scene->PumpDeviceAndFileControls(); // Closes input; it cannot mutate before job acknowledgement.
    EXPECT_EQ(beforeResetCount, station->NumTakes());
    exclusion.unlock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!Access::ControlIdle(*scene) && std::chrono::steady_clock::now() < deadline)
    { scene->PumpDeviceAndFileControls(); std::this_thread::yield(); }
    ASSERT_TRUE(Access::ControlIdle(*scene));
    EXPECT_TRUE(station->GetLoopTakeSnapshot().empty()); EXPECT_TRUE(stations.back()->GetLoopTakeSnapshot().empty());
    EXPECT_EQ(stations, scene->SnapshotStations()); EXPECT_EQ(revision, scene->AcceptedRigSnapshot()->Revision);
    EXPECT_EQ((std::vector<int>{2,5,11}), station->AllowedMidiChannels());
    EXPECT_EQ(routes, station->MidiChannelRoutes()); EXPECT_EQ(rack, station->GetGuiRack());
    EXPECT_EQ(cables, rack->Routes()); EXPECT_EQ(engineRoutes, station->SnapshotAudioRoutesForExport());
    EXPECT_EQ(plugin, station->GetVstPlugin(0u)); EXPECT_FLOAT_EQ(0.73f, plugin->GetParameter(0u));
    EXPECT_EQ((std::vector<std::uint8_t>{7u,3u}), plugin->GetState()); EXPECT_EQ(0u, plugin->Unloads);
    EXPECT_DOUBLE_EQ(1.0, station->MasterLevelForExport());
    for (auto level : station->BusLevelsForExport()) EXPECT_DOUBLE_EQ(1.0, level);
    EXPECT_EQ(0ul, Access::ClockLength(*scene)); EXPECT_TRUE(scene->Controls().CurrentJamPath().empty());
    EXPECT_TRUE(Access::SaveFolder(*scene).empty()); EXPECT_TRUE(take->IsTriggerCaptureInactive());
    EXPECT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState()); EXPECT_TRUE(trigger->GetTakes().empty());
    scene->CommitChanges(); // The running job thread owns its input-gate producer.
    EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
    unsigned int picks = 0u;
    { auto job = Access::ExcludeJob(*scene); Access::PickDirectory(*scene, [&](const auto&) { ++picks; return std::wstring{}; }); Access::Save(*scene); }
    EXPECT_EQ(1u, picks);
    {
        auto job = Access::ExcludeJob(*scene);
        EXPECT_EQ(0, Access::GlobalPhase(*scene));
        EXPECT_EQ(midi::MidiQuantisationFraction::Sixth, Access::DefaultFraction(*scene));
        EXPECT_EQ(io::JamFile::GlobalMidiQuantState::All, Access::GlobalQuantisationState(*scene));
        for (const auto& retained : stations)
        {
            EXPECT_EQ(0, retained->GlobalPhaseOffsetSamps());
            EXPECT_EQ(0, retained->StationPhaseOffsetSamps());
            const auto freshTake = retained->AddTake();
            EXPECT_EQ(0, freshTake->ResolvedMidiQuantisation().PhaseOffsetSamps);
            EXPECT_EQ(midi::MidiQuantisationFraction::Sixth, freshTake->MidiQuantisation().Fraction);
        }
    }
    scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ReclockButtonArmsOnceAndFormerKeyboardChordDoesNotArm)
{
    using Access = engine::SourceLossIntegrationTestAccess;
    auto scene = FreshScene({Station("Local")}, {}); ASSERT_TRUE(scene);
    auto exclusion = Access::ExcludeJob(*scene);
    actions::KeyAction key; key.KeyChar = 82u; key.KeyActionType = actions::KeyAction::KEY_UP;
    key.Modifiers = static_cast<base::Action::Modifiers>(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT);
    scene->OnAction(key); EXPECT_FALSE(Access::ReclockArmed(*scene));
    actions::GuiAction button; button.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON; button.Index = 7006u;
    Access::RemoteGrid(*scene, {88200u, 4u, 0u, 120.0f, 1u}, 1234);
    scene->OnAction(button); EXPECT_FALSE(Access::ReclockArmed(*scene));
    Access::RemoteGrid(*scene, {}, 0);
    scene->OnAction(button); EXPECT_TRUE(Access::ReclockArmed(*scene));
    scene->OnAction(button); EXPECT_TRUE(Access::ReclockArmed(*scene));
    exclusion.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SaveBusyFeedbackDoesNotOpenDialogsOrReplaceDestinations)
{
    using Access = engine::SourceLossIntegrationTestAccess;
    auto scene = FreshScene({Station("Capture")}, {Trigger("Record", "Capture")}); ASSERT_TRUE(scene);
    auto exclusion = Access::ExcludeJob(*scene);
    unsigned int picks = 0u;
    Access::PickDirectory(*scene, [&](const auto&) { ++picks; return std::wstring{}; });
    scene->Controls().SetCurrentJamPath(L"prior/001.jam"); scene->Controls().SetSessionSaveFolder(L"prior");
    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    io::UserConfig config{}; config.Audio.NumChannelsIn = 1u;
    Access::StartRecording(*scene, trigger, config);
    Access::Save(*scene, true); EXPECT_EQ(0u, picks);
    EXPECT_NE(std::string::npos, Access::FileStatus(*scene).find("capture"));
    actions::GuiAction rig; rig.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON; rig.Index = gui::GuiMainPanel::SaveRigControlIndex;
    scene->OnAction(rig); EXPECT_FALSE(scene->ConsumeRigSaveRequest());
    EXPECT_NE(std::string::npos, Access::FileStatus(*scene).find("busy"));
    EXPECT_FALSE(scene->BeginExplicitRigSave());
    EXPECT_EQ(L"prior/001.jam", scene->Controls().CurrentJamPath());
    exclusion.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SeamPolicyRigChangePreservesStationPlaybackFormat)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({Station("Local"), Station("Other")}, {});
	ASSERT_TRUE(scene);
	Access::PrepareHud(*scene);
	auto station = scene->SnapshotStations().front();
	std::shared_ptr<engine::LoopTake> take;
	Access::WhileSceneLocked(*scene, [&] {
		// A policy edit must retain each station's format, even when it differs
		// from the scene device rate (as a newly joined remote station can).
		station->SetSampleRate(32000.0f);
		take = station->AddTake();
		take->Record({}, "Local", {0u});
		ASSERT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), 0u));
		take->Play(0u, 10000u, 0u);
	});
	EXPECT_NE(32000u, Access::InteractionSampleRate(*scene));
	auto candidate = scene->AcceptedRigSnapshot()->Rig;
	candidate.User.Loop.MidiSeamMergeWindowMs = 500u;
	auto result = scene->RequestRigEdit(candidate);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (result == engine::RigCoordinator::EditResult::EditsDisabled
		&& std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		result = scene->RequestRigEdit(candidate);
	}
	ASSERT_EQ(engine::RigCoordinator::EditResult::Pending, result);
	Settle(scene);
	EXPECT_EQ(500u, scene->AcceptedRigSnapshot()->Rig.User.Loop.MidiSeamMergeWindowMs);
	Access::WhileSceneLocked(*scene, [&] {
		EXPECT_EQ(32000.0f, station->GetSampleRate());
		EXPECT_EQ(32000.0f, take->GetSampleRate());
		midi::MidiLoop::EditState edit;
		ASSERT_TRUE(take->GetMidiLoops().front()->SnapshotForEdit(edit));
		EXPECT_EQ(16000u, edit.SeamMergeWindowSamps);
	});
	scene->Shutdown();
}
