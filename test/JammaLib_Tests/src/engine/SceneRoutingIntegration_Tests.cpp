#include "../gui/WorkspaceTestAccess.h"
#include "../gui/LoopGridEditorTestAccess.h"
#include "gtest/gtest.h"
#include "../../TestPaths.h"
#include <algorithm>
#include <bitset>
#include <filesystem>
#include <fstream>
#include <tuple>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include "engine/Scene.h"
#include "gui/GuiScrollPanel.h"
#include "gui/GuiVstRack.h"
#include "gui/SceneSelector.h"
#include "graphics/Window.h"
#include "graphics/MuteMarqueeProjection.h"
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
        bool FailPick = false;
    protected:
        std::optional<unsigned int> _PickPointerPress(utils::Position2d position) override
        {
            PickPosition = position;
            ++PickCount;
            if (FailPick) return std::nullopt;
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
        static gui::Workspace& Workspace(Scene& scene) { return *scene._workspace; }
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
        static std::uint64_t ReclockGeneration(Scene& scene) { return scene._quantisation.ReclockGeneration(); }
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
        static void SetSaveTiming(Scene& scene)
        { scene._quantisation.ApplyTiming({16u,128ul,8u,120.0f,4u},"marquee save fixture"); }
        static std::vector<GroupTargetId> GeometryAt(Scene& scene, utils::Rect2d rectangle, base::SelectDepth depth)
        {
            const std::array visible{utils::Rect2d{0,0,1280,900}};
            const graphics::MuteMarqueeProjection projection(scene._workspace->ViewProjection(),{1280u,900u},rectangle,visible);
            std::vector<GroupTargetId> hits;
            for(const auto& station:scene._stations) station->CollectMuteMarqueeTargets(projection,depth,hits);
            return hits;
        }
        static bool GlobalMuteCommand(Scene& scene, bool muted)
        {
            std::scoped_lock lock(scene._sceneMutex);
            return scene._performanceGroups.SetAllMuted(muted,scene._stations,
                [&](auto image){scene._PublishGroupMuteApplication(std::move(image));});
        }
        static void ConnectMasterTiming(Scene& scene)
        {
            auto options=scene._networkService->TempoJoinOptions(); options.PushLocalTempoOnJoin=false;
            scene._networkService->SetTempoJoinOptions(options);
            scene._networkService->PrepareTempoSyncOnConnect(scene._quantisation.CurrentTempoTiming(scene._CurrentSampleRate()));
        }
        static std::uint64_t MasterTempoSends(const Scene& scene) { return scene._networkService->TimingDiagnostics().TempoRequestsSent; }
        static ninjam::NinjamLocalTempoState MasterFollowState(const Scene& scene) { return scene._networkService->LocalTempoState(); }
        static void PumpMasterJob(Scene& scene)
        {
            // Existing completion runs at the end of the real job owner's idle pump.
            auto completed=std::make_shared<std::atomic_bool>(false);
            scene.CompleteStartupWhenIdle([completed] { completed->store(true,std::memory_order_release); });
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
            while(!completed->load(std::memory_order_acquire) && !scene.JobThreadFailed() && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            ASSERT_FALSE(scene.JobThreadFailed()) << scene.JobThreadFailureDetail();
            ASSERT_TRUE(completed->load(std::memory_order_acquire));
        }
        static bool RequestResetDuringMaster(Scene& scene)
        { scene._operation.RequestReset(scene._ControlEffects()); return scene._operation.Current()==SessionControl::ResetSession; }
        static bool MarqueeActive(const Scene& scene) { return scene._performanceGroups.MuteMarqueeActive(); }
        static void PrepareMasterOnly(Scene& scene)
        {
            std::scoped_lock lock(scene._sceneMutex); const auto clock=scene._quantisation.Clock();
            scene._audioEngine->ApplyLocalMasterTimingAtAudioBoundary(scene._stations,clock.get());
            scene._localTiming.Advance(scene._quantisation,*scene._audioEngine,scene._stations);
        }
        static void ActivateMasterAudioOnly(Scene& scene)
        { std::scoped_lock lock(scene._sceneMutex); scene._audioEngine->ApplyLocalMasterTimingAtAudioBoundary(scene._stations,scene._quantisation.Clock().get()); }
        static std::uint64_t ActivatedMasterVersion(const Scene& scene) { return scene._audioEngine->LastActivatedLocalMasterTimingVersion(); }
        static auto ChosenMaster(const Scene& scene) { return scene._quantisation.ExportChosenMasterReference(scene._stations); }
        static auto AssignMaster(Scene& scene,const std::shared_ptr<LoopTake>& take)
        {
            std::scoped_lock lock(scene._sceneMutex);
            return scene._quantisation.SetMasterFromTarget(take,base::DEPTH_LOOPTAKE,scene._stations,scene._CurrentSampleRate(),scene._userConfig,true);
        }
        static auto MasterClock(const Scene& scene)
        { const auto clock=scene._quantisation.Clock(); return std::tuple{clock->SeedSourceLength(),clock->AbsoluteSamplePos(),clock->QuantiseSamps(),scene._quantisation.GlobalPhaseOffsetSamps()}; }
        static std::uint64_t MasterVersion(const Scene& scene) { return scene._quantisation.PendingLocalMasterTimingVersion(); }
        static bool MasterPending(const Scene& scene) { return scene._quantisation.LocalMasterTimingPending(); }
        static std::string MasterStatus(const Scene& scene) { return scene._quantisation.MasterStatus(); }
        static void ActivateMaster(Scene& scene)
        {
            std::scoped_lock lock(scene._sceneMutex); const auto clock=scene._quantisation.Clock();
            scene._audioEngine->ApplyLocalMasterTimingAtAudioBoundary(scene._stations,clock.get());
            scene._localTiming.Advance(scene._quantisation,*scene._audioEngine,scene._stations);
            scene._audioEngine->ApplyLocalMasterTimingAtAudioBoundary(scene._stations,clock.get());
            scene._localTiming.Advance(scene._quantisation,*scene._audioEngine,scene._stations);
        }
        static utils::Position2d MasterButtonPoint(Scene& scene)
        {
            auto panel=scene._workspace->MainPanel(); panel->SetPage(gui::SettingsPage::Timing); panel->SetExpanded(true);
            for(unsigned int frame=0u;frame<20u;++frame) panel->AdvanceAnimation(0.05f);
            const auto button=scene._workspace->Settings().MasterButton(); const auto size=button->GetSize();
            const auto point=button->GlobalPosition()+utils::Position2d{static_cast<int>(size.Width/2u),static_cast<int>(size.Height/2u)};
            EXPECT_TRUE(panel->RouteHitTest(panel->GlobalToLocal(point))); return point;
        }
        static bool DesiredMute(Scene& scene, const GroupTargetId& target)
        { return scene._performanceGroups.DesiredMuted(target,scene._stations); }
        static utils::Rect2d ExpandSettings(Scene& scene)
        {
            auto panel=scene._workspace->MainPanel(); panel->SetExpanded(true);
            for(unsigned int frame=0u;frame<20u;++frame) panel->AdvanceAnimation(0.05f);
            return panel->PresentedFrameBounds().Translated(panel->GlobalPosition());
        }
        static void CameraCentreAt(Scene& scene, utils::Position2d point)
        {
            auto& camera=gui::WorkspaceTestAccess::Camera(*scene._workspace); auto pose=camera.CurrentPose();
            const auto centre=gui::Workspace::StationCentre(scene._stations);
            const auto projection=camera.Projection(1280.0f/900.0f,centre); const auto distance=std::abs(pose.Eye.Z-centre.Z);
            pose.Eye.X=centre.X+(640.0f-static_cast<float>(point.X))*distance/(640.0f*projection[0][0]);
            pose.Eye.Y=centre.Y+(450.0f-static_cast<float>(point.Y))*distance/(450.0f*projection[1][1]);
            camera.SetViewTarget(camera.CurrentView(),pose);
            for(unsigned int frame=0u;frame<20u;++frame) camera.TickBackgroundDrag(0.05f);
        }
        static void FrontCamera(Scene& scene, const Station& station)
        {
            auto& camera=gui::WorkspaceTestAccess::Camera(*scene._workspace); const auto position=station.PresentationModelPosition();
            camera.SetViewTarget(graphics::Camera::View::Front,{{position.X,position.Y,position.Z+500.0f},{0,0,-1},{0,1,0}});
            for(unsigned int frame=0u;frame<20u;++frame) camera.TickBackgroundDrag(0.05f);
        }
        static utils::Position2d MidiPitchPoint(Scene& scene,const Station& station,const LoopTake& take,const midi::MidiLoop& stream,int pitch)
        {
            const auto model=stream.Model(); const auto scale=static_cast<float>(model->ModelScale());
            const auto fitted=graphics::MidiModel::FitPitch(pitch,static_cast<float>(take.GetSize().Height)*0.94f/scale,{60,67},scale);
            const auto stationPos=station.PresentationModelPosition(), takePos=take.ModelPosition(), modelPos=model->ModelPosition();
            const auto takeScale=static_cast<float>(take.ModelScale()), stationScale=static_cast<float>(station.ModelScale());
            const glm::vec3 local{50.0f*scale+modelPos.X,fitted.first*scale+modelPos.Y,modelPos.Z};
            const glm::vec3 world{stationPos.X+stationScale*(takePos.X+takeScale*local.x),stationPos.Y+stationScale*(takePos.Y+takeScale*local.y),stationPos.Z+stationScale*(takePos.Z+takeScale*local.z)};
            const auto clip=scene._workspace->ViewProjection()*glm::vec4(world,1.0f);
            return {static_cast<int>(std::lround((clip.x/clip.w+1.0f)*640.0f)),static_cast<int>(std::lround((clip.y/clip.w+1.0f)*450.0f))};
        }
        static bool MidiGeometryAt(Scene& scene,const Station& station,const LoopTake& take,const midi::MidiLoop& stream,utils::Rect2d rectangle)
        {
            const std::array visible{utils::Rect2d{0,0,1280,900}};
            const auto sp=station.PresentationModelPosition(),tp=take.ModelPosition();
            const auto matrix=glm::translate(glm::mat4(1),glm::vec3{sp.X,sp.Y,sp.Z})*glm::scale(glm::mat4(1),glm::vec3{static_cast<float>(station.ModelScale())})*
                glm::translate(glm::mat4(1),glm::vec3{tp.X,tp.Y,tp.Z})*glm::scale(glm::mat4(1),glm::vec3{static_cast<float>(take.ModelScale())});
            return stream.Model()->IntersectsMuteMarquee(graphics::MuteMarqueeProjection(scene._workspace->ViewProjection()*matrix,{1280u,900u},rectangle,visible));
        }
        static void MoveCameraAway(Scene& scene)
        {
            auto& camera=gui::WorkspaceTestAccess::Camera(*scene._workspace);
            auto pose=camera.CurrentPose(); pose.Eye.X+=100000.0f;
            camera.SetViewTarget(camera.CurrentView(),pose);
            for(unsigned int frame=0u;frame<20u;++frame) camera.TickBackgroundDrag(0.05f);
        }
        static void PrepareHudPresentation(Scene& scene)
        { std::scoped_lock lock(scene._sceneMutex); scene._workspace->Hud()->PrepareOverdubSourcesLocked({}); }
        static auto BorrowMute(Scene& scene) { return scene._audioEngine->_groupMuteApplication.borrow(); }
        static void AdoptBorrowedMute(Scene& scene, const GroupMuteApplication* image)
        { scene._audioEngine->ApplyBorrowedGroupMuteAtAudioBoundary(image); }
        static std::uint64_t MuteReceipt(const Scene& scene) { return scene._audioEngine->GroupMuteReceipt(); }
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
        static io::IoSessionExporter::ExportResult SaveOffline(Scene& scene, const std::wstring& directory)
        {
            auto effects=scene._ControlEffects(); const auto capture=effects.CaptureSession;
            io::IoSessionExporter::ExportResult result;
            effects.CaptureSession=[&](const auto& folder,const auto& actual,const auto& restore,const auto& eligible) {
                result=capture(folder,actual,restore,eligible); return result;
            };
            scene._operation._pendingSaveFolder=directory;
            scene._operation._CaptureSession(scene._audioEngine->GetStreamParams(),{scene._sceneMutex,scene._stations},effects);
            return result;
        }
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
		static void ExpectUncoveredRackPoint(Scene& scene, utils::Position2d point)
		{
			for (const auto& panel : {scene._workspace->MainPanel(), scene._workspace->SelectionPanel()})
				EXPECT_FALSE(panel->RouteHitTest(panel->GlobalToLocal(point)));
			if (const auto hud = scene._workspace->Hud())
				EXPECT_FALSE(hud->CapturesPointer(hud->GlobalToLocal(point)));
		}
		static void CollapseCapturedRack(Scene& scene)
		{
			std::scoped_lock lock(scene._sceneMutex);
			scene._workspace->CollapseRacksLocked(scene._stations);
			EXPECT_FALSE(gui::WorkspaceTestAccess::HasCapture(*scene._workspace));
		}
		static midi::LoopGridEditor& Editor(Scene& scene) { return scene._workspace->Editor(); }
		static void AdvanceEditor(Scene& scene)
		{
			for (auto i = 0u; i < 20u; ++i)
			{ gui::WorkspaceTestAccess::Camera(*scene._workspace).TickBackgroundDrag(0.05f); scene._workspace->Editor().Tick(0.05f); }
		}
		static std::size_t EditorSelectionHistory(const Scene& scene)
		{ return midi::LoopGridEditorTestAccess::SelectionHistorySize(scene._workspace->Editor()); }
		static std::shared_ptr<gui::GuiSlider> PhaseSetting(Scene& scene)
		{
			// FreshScene's compact window is not a dependable settings input viewport.
			scene.SetSize({1200u, 900u});
			auto panel = scene._workspace->MainPanel();
			panel->SetPage(gui::SettingsPage::Timing); panel->SetExpanded(true);
			// AdvanceAnimation bounds each frame to .05s, even for a large argument.
			for (unsigned int frame = 0u; frame < 16u && panel->TransitionValue() < 1.0f; ++frame)
				panel->AdvanceAnimation(0.05f);
			EXPECT_FLOAT_EQ(1.0f, panel->TransitionValue());
			return scene._workspace->Settings().Offset();
		}
		static utils::Position2d PhasePoint(Scene& scene)
		{
			const auto slider = PhaseSetting(scene);
			const auto point = slider->GlobalPosition() + utils::Position2d{static_cast<int>(slider->GetSize().Width / 2u), 10};
			const auto panel = scene._workspace->MainPanel();
			EXPECT_TRUE(panel->RouteHitTest(panel->GlobalToLocal(point)));
			return point;
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
            std::vector<gui::GuiHud::StationAnchor> anchors;
            for(std::size_t index=0u;index<scene._stations.size();++index)
                if(const auto& station=scene._stations[index]) anchors.push_back({index,station->Name(),glm::dvec2{600.0+200.0*static_cast<double>(index),350.0},glm::vec4{1.0f}});
            scene._workspace->Hud()->SetStationAnchors(std::move(anchors));
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
    static void ExecuteReset(const std::shared_ptr<engine::Scene>& scene,bool removeVsts,std::unique_lock<std::mutex>& job)
    {
        using Access=engine::SourceLossIntegrationTestAccess;
        actions::GuiAction reset; reset.ElementType=actions::GuiAction::ACTIONELEMENT_BUTTON; reset.Index=removeVsts ? 7215u : 7214u;
        scene->OnAction(reset); ASSERT_TRUE(Access::ResetDecision(*scene)); Access::ResolveReset(*scene,true);
        scene->PumpDeviceAndFileControls(); job.unlock();
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!Access::ControlIdle(*scene) && std::chrono::steady_clock::now()<deadline)
        { scene->PumpDeviceAndFileControls(); std::this_thread::yield(); }
        ASSERT_TRUE(Access::ControlIdle(*scene)); job=Access::ExcludeJob(*scene); scene->CommitChanges();
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

    class ScopedMuteDirectory
    {
    public:
        explicit ScopedMuteDirectory(const std::string& prefix) : Path(TestPaths::FixtureDirectory() /
            (prefix+std::to_string(utils::Timer::GetTime().time_since_epoch().count())))
        { if (!std::filesystem::create_directory(Path)) throw std::runtime_error("Mute fixture directory already exists"); }
        ~ScopedMuteDirectory() { std::error_code error; std::filesystem::remove_all(Path,error); }
        const std::filesystem::path Path;
    };

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
		const auto before=take->IsMuted();
		click.State = actions::TouchAction::TOUCH_DOWN;
		scene->OnAction(click);
		click.State = actions::TouchAction::TOUCH_UP;
		scene->OnAction(click);
        EXPECT_EQ(muted,engine::SourceLossIntegrationTestAccess::DesiredMute(*scene,{engine::GroupTargetKind::Take,station->Id(),take->Id(),{}}));
        EXPECT_EQ(before,take->IsMuted());
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
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
	const auto stationRack = station->GetGuiRack();
	const auto stationGrid = stationRack->VstRack(); ASSERT_TRUE(stationGrid);
	const auto takeGrid = rack->VstRack(); ASSERT_TRUE(takeGrid);
	stationGrid->SetChannelsCollapsed(false); takeGrid->SetChannelsCollapsed(false);
	const auto allowed = station->AllowedMidiChannels();
	const auto midiRoutes = station->MidiChannelRoutes();
	const auto gain = stationRack->GetMasterSlider()->Value();
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
	EXPECT_FALSE(stationGrid->ChannelsCollapsed());
	EXPECT_FALSE(takeGrid->ChannelsCollapsed());

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
	EXPECT_TRUE(stationGrid->ChannelsCollapsed()); EXPECT_TRUE(takeGrid->ChannelsCollapsed());
	EXPECT_EQ(allowed, station->AllowedMidiChannels()); EXPECT_EQ(midiRoutes, station->MidiChannelRoutes());
	EXPECT_DOUBLE_EQ(gain, stationRack->GetMasterSlider()->Value());

	// A background click clears selection and restores every master control.
	rack->SetRackState(gui::GuiRackParams::RACK_ROUTER, true);
	// The station faders are already MASTER; its grid can still be independently open.
	stationGrid->SetChannelsCollapsed(false); takeGrid->SetChannelsCollapsed(false);
	scene->AdvanceUiAnimations();
	scene->SetHover3d({}, base::Action::MODIFIER_NONE);
	click.State = actions::TouchAction::TOUCH_DOWN;
	scene->OnAction(click);
	click.State = actions::TouchAction::TOUCH_UP;
	scene->OnAction(click);
	EXPECT_FALSE(scene->HasSelection());
	EXPECT_TRUE(stationGrid->ChannelsCollapsed()); EXPECT_TRUE(takeGrid->ChannelsCollapsed());
	EXPECT_TRUE(stationRack->GetMasterSlider()->Parent()->IsVisible());
	EXPECT_EQ(gui::GuiRackParams::RACK_MASTER, rack->GetRackState());
	EXPECT_TRUE(sibling->GetGuiRack()->GetMasterSlider()->Parent()->IsVisible());
	stationGrid->SetChannelsCollapsed(false);
	EXPECT_FALSE(stationGrid->ChannelsCollapsed()); // Explicit collapse does not disable reopening.
	scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SharedRackCollapseCancelsChannelOwnerCableAndFooterCapture)
{
    using Access = engine::SourceLossIntegrationTestAccess;
    for (const auto target : {0u, 1u, 2u, 3u})
    {
        auto scene = FreshScene({Station("Keys")}, {}); ASSERT_TRUE(scene);
        auto exclusion = Access::ExcludeJob(*scene);
        scene->SetSize({1280u, 900u});
        auto station = scene->SnapshotStations().front(); station->Select();
        station->SetAllowedMidiChannels({2, 5});
        const auto rack = station->GetGuiRack(); const auto grid = rack->VstRack(); ASSERT_TRUE(grid);
        rack->SetPosition({800, 300}); // Grid extends left of its fader; keep the full grid in free client space.
        gui::GuiVstRack::Model model; model.Editable = true; model.Revision = 1u;
        grid->Update(model, [](auto) {}); grid->SetChannelsCollapsed(false);
        const auto channels = grid->ChannelsPanel();
        // Footer identities are semantic command indices after GuiRack::Init.
        const auto controlIndex = target < 2u ? 0u : target == 2u ? gui::GuiRack::MidiDeselectAllIndex : gui::GuiRack::MidiSelectAllIndex;
        const auto control = channels->TryGetChild(static_cast<unsigned char>(controlIndex));
        ASSERT_TRUE(control); EXPECT_EQ(channels, control->Parent());
        if (target >= 2u)
        {
            const auto footer = std::dynamic_pointer_cast<gui::GuiButton>(control); ASSERT_TRUE(footer);
            EXPECT_EQ(target == 2u ? "None" : "All", footer->Text());
        }
        else ASSERT_TRUE(std::dynamic_pointer_cast<gui::GuiToggle>(control));
        actions::TouchAction down; down.State = actions::TouchAction::TOUCH_DOWN;
        const auto local = utils::Position2d{10, 10};
        down.Position = control->GlobalPosition() + local;
        SCOPED_TRACE(::testing::Message() << "target " << target << " at " << down.Position.X << "," << down.Position.Y);
        ASSERT_GE(down.Position.X, 0); ASSERT_LT(down.Position.X, 1280);
        ASSERT_GE(down.Position.Y, 0); ASSERT_LT(down.Position.Y, 900);
        Access::ExpectUncoveredRackPoint(*scene, down.Position);
        const auto press = scene->OnAction(down); ASSERT_TRUE(press.IsEaten);
        auto captured = press.ActiveElement.lock(); ASSERT_TRUE(captured);
        if (target < 2u) ASSERT_EQ(grid, captured); // Grid owns channel press/drag.
        else ASSERT_EQ(control, captured); // All/None footer owns its own press.
        if (target == 1u)
        {
            actions::TouchMoveAction move; move.MouseButtonsDown = 1u;
            move.Position = down.Position + utils::Position2d{20, 0}; scene->OnAction(move);
            ASSERT_TRUE(grid->HasCableDrag());
        }
        const auto allowed = station->AllowedMidiChannels(); const auto routes = station->MidiChannelRoutes();
        Access::CollapseCapturedRack(*scene);
        EXPECT_TRUE(grid->ChannelsCollapsed()); EXPECT_FALSE(grid->HasCableDrag());
        EXPECT_FALSE(channels->IsVisible()); EXPECT_EQ(base::GuiElement::STATE_NORMAL, control->GetState());
        actions::TouchAction up = down; up.State = actions::TouchAction::TOUCH_UP;
        up.Position = control->GlobalPosition() + local;
        EXPECT_TRUE(scene->OnAction(up).IsEaten); // Removed capture's delayed release is consumed.
        EXPECT_TRUE(station->IsSelected()); EXPECT_EQ(allowed, station->AllowedMidiChannels());
        EXPECT_EQ(routes, station->MidiChannelRoutes());
        grid->SetChannelsCollapsed(false); EXPECT_FALSE(grid->ChannelsCollapsed());
        exclusion.unlock(); scene->Shutdown();
    }
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
 const auto phase = engine::SourceLossIntegrationTestAccess::PhaseSetting(*scene);
 phase->SetValue(-0.375, false);
 for (const auto& station : scene->SnapshotStations()) EXPECT_DOUBLE_EQ(-0.375, station->TransportOffsetLoopFrac());
 const auto rig = scene->CaptureCommittedRig(); ASSERT_TRUE(rig);
 std::stringstream rigJson; ASSERT_TRUE(io::RigFile::ToJsonStream(*rig, rigJson));
 auto savedRig = io::RigFile::FromStream(std::move(rigJson)); ASSERT_TRUE(savedRig);
 const auto directory = std::filesystem::temp_directory_path() / ("jamma-station-save-" + std::to_string(utils::Timer::GetTime().time_since_epoch().count()));
 std::filesystem::create_directories(directory);
 ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::Export(*scene, directory.wstring()));
 std::ifstream file(directory / "session.jam"); std::stringstream jamJson; jamJson << file.rdbuf(); file.close();
 auto savedJam = io::JamFile::FromStream(std::move(jamJson)); ASSERT_TRUE(savedJam);
 EXPECT_EQ(2u, savedJam->Stations.size());
 EXPECT_DOUBLE_EQ(-0.375, savedJam->TransportOffsetLoopFrac);
 const std::vector<int> allChannels{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
 EXPECT_EQ(allChannels, savedJam->Stations.front().AllowedMidiChannels);
 for (const auto& route : savedJam->Stations.front().MidiChannelRoutes) EXPECT_TRUE(route.PluginIds.empty());
 // The loaded original retains its saved disabled channels, independently of creation defaults.
 EXPECT_TRUE(savedJam->Stations.back().AllowedMidiChannels.empty());
 engine::SceneParams params({ "" }, {}, {640u, 480u});
 auto reloaded = engine::Scene::FromFile(params, *savedJam, *savedRig, directory.wstring()); ASSERT_TRUE(reloaded);
 EXPECT_DOUBLE_EQ(-0.375, engine::SourceLossIntegrationTestAccess::PhaseSetting(*reloaded.value())->Value());
 for (const auto& station : reloaded.value()->SnapshotStations()) EXPECT_DOUBLE_EQ(-0.375, station->TransportOffsetLoopFrac());
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

TEST_F(SceneRoutingIntegrationTest, PhaseSliderSceneCaptureFocusLossRestoresAcceptedBaseline)
{
 using Access = engine::SourceLossIntegrationTestAccess;
 auto scene = FreshScene({Station("A")}, {}); ASSERT_TRUE(scene);
 auto exclusion = Access::ExcludeJob(*scene);
 const auto field = Access::PhaseSetting(*scene);
 field->SetValue(0.125, false);
 auto point = Access::PhasePoint(*scene);
 actions::TouchAction press; press.State = actions::TouchAction::TOUCH_DOWN; press.Position = point;
 const auto captured = scene->OnAction(press);
 ASSERT_EQ(field, captured.ActiveElement.lock());
 actions::TouchMoveAction move; move.MouseButtonsDown = 1u; move.Position = point + utils::Position2d{20, 0};
 scene->OnAction(move);
 EXPECT_GT(field->Value(), 0.125);
 EXPECT_DOUBLE_EQ(field->Value(), scene->SnapshotStations()[0]->TransportOffsetLoopFrac());
 EXPECT_NO_THROW(scene->OnInputFocusLost());
 EXPECT_DOUBLE_EQ(0.125, scene->SnapshotStations()[0]->TransportOffsetLoopFrac());
 EXPECT_DOUBLE_EQ(0.125, field->Value());
 press.State = actions::TouchAction::TOUCH_UP; press.Position = move.Position;
 scene->OnAction(press); // Delayed release must not replay a canceled phase preview.
 EXPECT_DOUBLE_EQ(0.125, field->Value());
 exclusion.unlock(); scene->Shutdown();
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
        EXPECT_EQ(initiallyMuted,audioTake->IsMuted());
        EXPECT_EQ(!initiallyMuted,engine::SourceLossIntegrationTestAccess::DesiredMute(*scene,{engine::GroupTargetKind::Take,station->Id(),audioTake->Id(),{}}));
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
        EXPECT_EQ(!initiallyMuted, audioTake->IsMuted());
        EXPECT_EQ(initiallyMuted, midiTake->IsMuted());
        EXPECT_EQ(initiallyMuted, untouched->IsMuted());
        scene->SetHover3d(midiPath, base::Action::MODIFIER_NONE);
        EXPECT_EQ(initiallyMuted,midiTake->IsMuted());
        EXPECT_EQ(!initiallyMuted,engine::SourceLossIntegrationTestAccess::DesiredMute(*scene,{engine::GroupTargetKind::Take,station->Id(),midiTake->Id(),{}}));
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
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
        EXPECT_EQ(initiallyMuted,take->IsMuted());
        EXPECT_EQ(!initiallyMuted,engine::SourceLossIntegrationTestAccess::DesiredMute(*scene,{engine::GroupTargetKind::Take,station->Id(),take->Id(),{}}));
        ASSERT_TRUE(engine::SourceLossIntegrationTestAccess::ApplyMuteBoundary(*scene));
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

TEST_F(SceneRoutingIntegrationTest, NativeSystemAltEdgeReleaseSnapsCapturedEditorWithoutBackgroundAction)
{
    auto scene = FreshScene({Station("Keys")}, {}); ASSERT_TRUE(scene); scene->SetSize({1280u, 900u});
    resources::ResourceLib resources; engine::NativeKeyboardWindow window(*scene, resources); window.Resize({1280u, 900u});
    auto excludeJob = engine::SourceLossIntegrationTestAccess::ExcludeJob(*scene);
    const auto station = scene->SnapshotStations().front(); station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed); auto take = station->AddTake();
    take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
    take->Record({}, station->Name(), {0u}, {"source"}); take->Play(0u, 100u, 0u);
    auto settings = take->ResolvedMidiQuantisation(); settings.Enabled = true; settings.GrainSamps = 10u;
    settings.Fraction = midi::MidiQuantisationFraction::Whole; take->SetMidiQuantisation(settings);
    const auto loop = take->GetMidiLoops().front();
    const std::array events{midi::MidiEvent::MakeNoteOn(40u, 0u, 60u, 80u), midi::MidiEvent::MakeNoteOff(60u, 0u, 60u)};
    loop->ReplaceRecordedEvents(events.data(), events.size(), 100u);
    ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(), take->MidiQuantisationTransportStartSamps())); scene->CommitChanges();
    ASSERT_TRUE(loop->UpdateModelFromEvents(100u, true)); take->Select();
    auto& editor = engine::SourceLossIntegrationTestAccess::Editor(*scene); ASSERT_TRUE(editor.Open(take, nullptr, loop));
    engine::SourceLossIntegrationTestAccess::AdvanceEditor(*scene); ASSERT_TRUE(editor.IsReady()); loop->Model()->SetEditorPitchRange(48,24);
    const auto edge = midi::LoopGridEditorTestAccess::MidiPoint(editor, 0.60, 60);
    const auto drag = midi::LoopGridEditorTestAccess::MidiPoint(editor, 0.67, 60); ASSERT_TRUE(edge); ASSERT_TRUE(drag);
    ASSERT_GE(edge->X, 0); ASSERT_LT(edge->X, 1280); ASSERT_GE(edge->Y, 0); ASSERT_LT(edge->Y, 900);
    engine::SourceLossIntegrationTestAccess::ExpectUncoveredRackPoint(*scene, *edge);
    ASSERT_EQ(1u, loop->Model()->NoteInstanceCount());
    ASSERT_TRUE(loop->Model()->EditorNoteMatches(0u, midi::MidiNote{40u,20u,0u,60u,80u,0u},100u));
    const auto hit = midi::LoopGridEditorTestAccess::MidiHitPoint(editor, *edge); ASSERT_TRUE(hit);
    ASSERT_EQ(60u, hit->Pitch); ASSERT_GE(hit->Sample,58u); ASSERT_LE(hit->Sample,61u);
    SCOPED_TRACE(testing::Message() << "native pick sample=" << hit->Sample << " U=" << hit->U
        << " precise=" << hit->EdgePosition() << " pixels/sample=" << midi::LoopGridEditorTestAccess::MidiPixelsPerSample(editor));
    ASSERT_TRUE(hit->PickSample.has_value());
    ASSERT_LE(std::abs(hit->EdgePosition()-60.0)*midi::LoopGridEditorTestAccess::MidiPixelsPerSample(editor),6.0);
    ASSERT_TRUE(midi::LoopGridEditorTestAccess::HasMidiEdgeAt(editor,*edge));
    const auto host = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(nullptr, host); SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&window));
    const auto revision = loop->Revision(); window.NativeKeys.set(VK_MENU);
    EXPECT_EQ(0, graphics::Window::WindowProcedure(host, WM_SYSKEYDOWN, VK_MENU, 1u << 29u));
    ASSERT_TRUE(editor.IsReady()); ASSERT_EQ(base::Action::MODIFIER_ALT,window.Modifiers());
    ASSERT_TRUE(midi::LoopGridEditorTestAccess::HasMidiEdgeAt(editor,*edge));
    graphics::Window::WindowProcedure(host, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(edge->X, 900 - edge->Y));
    SCOPED_TRACE(testing::Message() << "edge=" << edge->X << "," << edge->Y
        << " hit=" << hit->Sample << " ready=" << editor.IsReady()
        << " feedback=" << midi::LoopGridEditorTestAccess::Feedback(editor));
    ASSERT_TRUE(editor.OwnsPointer()); EXPECT_FALSE(editor.IsOrbitDragging());
    graphics::Window::WindowProcedure(host, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(drag->X, 900 - drag->Y));
    EXPECT_EQ(revision, loop->Revision()); window.NativeKeys.reset(VK_MENU);
    EXPECT_EQ(0, graphics::Window::WindowProcedure(host, WM_SYSKEYUP, VK_MENU, 1u << 29u));
    EXPECT_TRUE(editor.OwnsPointer()); EXPECT_FALSE(engine::SourceLossIntegrationTestAccess::BackgroundOwned(*scene));
    graphics::Window::WindowProcedure(host, WM_LBUTTONUP, 0, MAKELPARAM(drag->X, 900 - drag->Y));
    EXPECT_FALSE(editor.OwnsPointer()); EXPECT_EQ(revision + 1u, loop->Revision());
    midi::MidiLoop::EditState edited; ASSERT_TRUE(loop->SnapshotForEdit(edited));
    EXPECT_EQ(40u, edited.Events[0].sampleOffset); EXPECT_EQ(70u, edited.Events[1].sampleOffset);
    EXPECT_TRUE(edited.Events[0].HasExactTiming()); EXPECT_TRUE(edited.Events[1].HasExactTiming()); EXPECT_FALSE(take->IsMuted());
    SetWindowLongPtrW(host, GWLP_USERDATA, 0); DestroyWindow(host); excludeJob.unlock(); scene->Shutdown();
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
        const auto grid = rack->VstRack(); ASSERT_TRUE(grid); grid->SetChannelsCollapsed(false);
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
        EXPECT_TRUE(grid->ChannelsCollapsed());
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

class ResetPinnedTakeUndo final : public base::ActionUndo
{
public:
    ResetPinnedTakeUndo(std::shared_ptr<engine::LoopTake> take,std::shared_ptr<unsigned int> calls):ActionUndo(std::weak_ptr<base::ActionSender>{}),_take(std::move(take)),_calls(std::move(calls)) {}
    bool Undo() override { ++*_calls; return true; }
    bool Redo() override { ++*_calls; return true; }
private:
    std::shared_ptr<engine::LoopTake> _take;
    std::shared_ptr<unsigned int> _calls;
};

TEST_F(SceneRoutingIntegrationTest, ResetAllConfirmationCancellationAndSerializedRetirementPreserveRigAndRouting)
{
    for(const bool removeVsts:{false,true})
    {
    SCOPED_TRACE(removeVsts);
    using Access = engine::SourceLossIntegrationTestAccess;
    auto scene = FreshScene({Station("Keys"), Station("Drums")}, {Trigger("Keys trigger", "Keys")}); ASSERT_TRUE(scene);
    auto exclusion = Access::ExcludeJob(*scene);
    auto stations = scene->SnapshotStations(); auto station = stations.front();
    auto take = station->AddTake(); auto other = stations.back()->AddTake();
    take->Record({},station->Name(),{0u},{""}); take->Play(0u,128u,0u); take->BeginCaptureAudioBoundary(0u,0u,0u);
    scene->CommitChanges();
    station->SetAllowedMidiChannels({2,5,11});
    ASSERT_TRUE(station->RestoreMixerLevels(0.25, std::vector<double>(station->NumBusChannels(), 0.5)));
    auto plugin = std::make_shared<ResetPreservationPlugin>();
    actions::JobAction load; load.JobActionType = actions::JobAction::JOB_LOADVST;
    load.VstPath = L"test-owned-reset.vst3"; load.PreInitPlugin = plugin;
    if(!load.VstManaged) EXPECT_TRUE(station->StageVstJob(load)); station->OnAction(load); station->CommitChanges(); ASSERT_EQ(plugin, station->GetVstPlugin(0u));
    auto routed = station->MidiChannelRoutes(); routed[1].PluginIds = {plugin->StateId()};
    ASSERT_TRUE(station->SetMidiChannelRoutes(routed, station->RackRevision()));
    auto pendingPlugin=std::make_shared<ResetPreservationPlugin>();
    actions::JobAction pendingJob; pendingJob.JobActionType=actions::JobAction::JOB_LOADVST; pendingJob.VstPath=L"pending-reset.vst3"; pendingJob.PreInitPlugin=pendingPlugin;
    ASSERT_TRUE(station->StageVstJob(pendingJob)); const auto pendingIdentity=pendingJob.VstRequestId;
    auto undoCalls=std::make_shared<unsigned int>(0u);
    auto firstUndo=std::make_shared<ResetPinnedTakeUndo>(take,undoCalls),secondUndo=std::make_shared<ResetPinnedTakeUndo>(take,undoCalls);
    std::weak_ptr<ResetPinnedTakeUndo> weakFirst=firstUndo,weakSecond=secondUndo;
    auto& history=Access::Workspace(*scene).Undo(); history.Add(firstUndo); history.Add(secondUndo); firstUndo.reset(); secondUndo.reset(); ASSERT_TRUE(history.Undo()); ASSERT_EQ(1u,*undoCalls);
    const auto routes = station->MidiChannelRoutes(); const auto rack = station->GetGuiRack();
    rack->AddRoute(0u, 0u); const auto cables = rack->Routes();
    const auto engineRoutes = station->SnapshotAudioRoutesForExport();
    const auto revision = scene->AcceptedRigSnapshot()->Revision;
    Access::SetClock(*scene);
    Access::SetResetQuantisationState(*scene);
    const auto phaseSlider = Access::PhaseSetting(*scene); phaseSlider->SetValue(0.375, false);
    Access::SetDepth(*scene, engine::Scene::VIEW_LOOPTAKE);
    std::vector<unsigned char> selectedTakePath;
    for (const auto index : take->GlobalId()) selectedTakePath.push_back(static_cast<unsigned char>(index + 1u));
    scene->SetHover3d(selectedTakePath, base::Action::MODIFIER_NONE);
    actions::TouchAction selectTake; selectTake.Position = Access::CanvasPoint(*scene);
    selectTake.State = actions::TouchAction::TOUCH_DOWN; scene->OnAction(selectTake);
    selectTake.State = actions::TouchAction::TOUCH_UP; scene->OnAction(selectTake);
    ASSERT_TRUE(take->IsSelected()); ASSERT_GT(Access::EditorSelectionHistory(*scene), 0u);
    auto& editor=Access::Workspace(*scene).Editor();
    ASSERT_TRUE(editor.Open(take,{},take->GetMidiLoops().front()));
    ASSERT_GT(midi::LoopGridEditorTestAccess::RevisionCursorCount(editor),0u);
    editor.Close(); Access::AdvanceEditor(*scene);
    ASSERT_FALSE(editor.IsEngaged()); // Reset must clear cached cursors even after ordinary close.
    station->SetRetainedMuted(true);
    station->SetStationPhaseOffsetSamps(-123);
    stations.back()->SetStationPhaseOffsetSamps(456);
    scene->Controls().SetCurrentJamPath(L"previous/001.jam"); scene->Controls().SetSessionSaveFolder(L"previous");
    actions::GuiAction reset; reset.ElementType = actions::GuiAction::ACTIONELEMENT_BUTTON; reset.Index = removeVsts ? 7215u : 7214u;
    scene->OnAction(reset); const auto decision = Access::ResetDecision(*scene); ASSERT_TRUE(decision);
    auto otherMode=reset; otherMode.Index=removeVsts ? 7214u : 7215u;
    scene->OnAction(otherMode); EXPECT_EQ(decision, Access::ResetDecision(*scene));
    Access::ResolveReset(*scene, false);
    EXPECT_EQ(1u, station->GetLoopTakeSnapshot().size()); EXPECT_DOUBLE_EQ(0.25, station->MasterLevelForExport());
    EXPECT_EQ(96000ul, Access::ClockLength(*scene)); EXPECT_EQ(L"previous/001.jam", scene->Controls().CurrentJamPath());
    EXPECT_EQ(321, Access::GlobalPhase(*scene));
    EXPECT_DOUBLE_EQ(0.375, phaseSlider->Value());
    EXPECT_GT(Access::EditorSelectionHistory(*scene), 0u);
    EXPECT_EQ(-123, station->StationPhaseOffsetSamps());
    EXPECT_EQ(456, stations.back()->StationPhaseOffsetSamps());
    for (const auto& retained : stations) EXPECT_EQ(321, retained->GlobalPhaseOffsetSamps());

    auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
    io::UserConfig captureConfig{}; captureConfig.Audio.NumChannelsIn = 1u;
    Access::StartRecording(*scene, trigger, captureConfig);
    ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
    // Leave a real Scene/Workspace phase capture active across accepted reset.
    const auto phasePoint = Access::PhasePoint(*scene);
    actions::TouchAction phasePress; phasePress.State = actions::TouchAction::TOUCH_DOWN; phasePress.Position = phasePoint;
    const auto phaseCapture = scene->OnAction(phasePress);
    ASSERT_EQ(phaseSlider, phaseCapture.ActiveElement.lock());
    actions::TouchMoveAction phaseMove; phaseMove.MouseButtonsDown = 1u;
    phaseMove.Position = phasePoint + utils::Position2d{20, 0}; scene->OnAction(phaseMove);
    ASSERT_GT(phaseSlider->Value(), 0.375);
    EXPECT_DOUBLE_EQ(phaseSlider->Value(), station->TransportOffsetLoopFrac());
    const auto beforeResetCount = station->NumTakes();
    scene->OnAction(reset); Access::ResolveReset(*scene, true);
    scene->PumpDeviceAndFileControls(); // Closes input; it cannot mutate before job acknowledgement.
    EXPECT_EQ(beforeResetCount, station->NumTakes());
    exclusion.unlock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!Access::ControlIdle(*scene) && std::chrono::steady_clock::now() < deadline)
    { scene->PumpDeviceAndFileControls(); std::this_thread::yield(); }
    ASSERT_TRUE(Access::ControlIdle(*scene));
    EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
    EXPECT_DOUBLE_EQ(0.0, phaseSlider->Value());
    EXPECT_EQ(0u, Access::EditorSelectionHistory(*scene));
    EXPECT_EQ(0u,midi::LoopGridEditorTestAccess::RevisionCursorCount(editor));
    EXPECT_EQ(nullptr,editor.Take()); EXPECT_EQ(nullptr,editor.AudioLoop());
    EXPECT_FALSE(station->RetainedMuted());
    EXPECT_TRUE(weakFirst.expired()); EXPECT_TRUE(weakSecond.expired());
    EXPECT_FALSE(history.Undo()); EXPECT_FALSE(history.Redo()); EXPECT_EQ(1u,*undoCalls);
    EXPECT_DOUBLE_EQ(0.0, station->TransportOffsetLoopFrac());
    phasePress.State = actions::TouchAction::TOUCH_UP; phasePress.Position = phaseMove.Position;
    scene->OnAction(phasePress); // Old captured release cannot restore the pre-reset target.
    EXPECT_DOUBLE_EQ(0.0, phaseSlider->Value());
    for (const auto& retained : stations) EXPECT_DOUBLE_EQ(0.0, retained->TransportOffsetLoopFrac());
    EXPECT_TRUE(stations.back()->GetLoopTakeSnapshot().empty());
    EXPECT_EQ(stations, scene->SnapshotStations()); EXPECT_EQ(revision, scene->AcceptedRigSnapshot()->Revision);
    EXPECT_EQ((std::vector<int>{2,5,11}), station->AllowedMidiChannels());
    if(removeVsts) for(const auto& route:station->MidiChannelRoutes()) EXPECT_TRUE(route.PluginIds.empty());
    else EXPECT_EQ(routes,station->MidiChannelRoutes());
    EXPECT_EQ(rack,station->GetGuiRack());
    EXPECT_EQ(cables, rack->Routes()); EXPECT_EQ(engineRoutes, station->SnapshotAudioRoutesForExport());
    if(removeVsts) { EXPECT_EQ(0u,station->RackSlotCount()); EXPECT_EQ(nullptr,station->GetVstPlugin(0u)); }
    else EXPECT_EQ(plugin,station->GetVstPlugin(0u));
    EXPECT_FLOAT_EQ(0.73f,plugin->GetParameter(0u));
    EXPECT_EQ((std::vector<std::uint8_t>{7u,3u}), plugin->GetState()); EXPECT_EQ(0u, plugin->Unloads);
    EXPECT_DOUBLE_EQ(1.0, station->MasterLevelForExport());
    for (auto level : station->BusLevelsForExport()) EXPECT_DOUBLE_EQ(1.0, level);
    EXPECT_EQ(0ul, Access::ClockLength(*scene)); EXPECT_TRUE(scene->Controls().CurrentJamPath().empty());
    EXPECT_TRUE(Access::SaveFolder(*scene).empty()); EXPECT_TRUE(take->IsTriggerCaptureInactive());
    EXPECT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState()); EXPECT_TRUE(trigger->GetTakes().empty());
    {
        auto job=Access::ExcludeJob(*scene);
        EXPECT_EQ(pendingIdentity,pendingJob.VstRequestId);
        EXPECT_EQ(!removeVsts,station->OnAction(pendingJob).IsEaten); station->CommitChanges();
        EXPECT_EQ(removeVsts ? 0u : 2u,station->RackSlotCount());
        if(!removeVsts) EXPECT_EQ(pendingPlugin,station->GetVstPlugin(1u));
    }
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
            EXPECT_FALSE(freshTake->IsMuted());
            EXPECT_EQ(0, freshTake->ResolvedMidiQuantisation().PhaseOffsetSamps);
            EXPECT_EQ(midi::MidiQuantisationFraction::Sixth, freshTake->MidiQuantisation().Fraction);
        }
    }
    scene->Shutdown();
    }
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
    scene->OnAction(button); EXPECT_TRUE(Access::ReclockArmed(*scene));
    const auto generation = Access::ReclockGeneration(*scene);
    scene->OnAction(button); EXPECT_TRUE(Access::ReclockArmed(*scene));
    EXPECT_EQ(generation, Access::ReclockGeneration(*scene));
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

static std::shared_ptr<engine::LoopTake> AddCompletedSourceChoiceTake(const std::shared_ptr<engine::Station>& station, bool includeMidi = false, bool includeAudio = true, bool siblingMidi = false)
{
	auto take = station->AddTake();
	take->Record(includeAudio ? std::vector<unsigned int>{0u} : std::vector<unsigned int>{}, station->Name(), includeMidi ? (siblingMidi ? std::vector<unsigned int>{0u,1u} : std::vector<unsigned int>{0u}) : std::vector<unsigned int>{},
		includeMidi ? std::vector<std::string>{"Keys"} : std::vector<std::string>{});
	take->CommitChanges();
	std::vector<float> data(constants::MaxLoopFadeSamps + 128u, 0.25f);
	base::AudioWriteRequest request; request.samples = data.data(); request.numSamps = static_cast<unsigned int>(data.size()); request.fadeNew = 1.0f;
	if (includeAudio) { take->GetLoops().front()->OnBlockWrite(request, 0); take->GetLoops().front()->EndWrite(request.numSamps, true); }
	if (includeMidi)
	{
		take->EndMultiWrite(128u, true, base::Audible::AUDIOSOURCE_ADC);
		EXPECT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(10u, 0u, 60u, 100u), "Keys", 128u));
		EXPECT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(90u, 0u, 60u), "Keys", 128u));
        if (siblingMidi)
        {
            EXPECT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOn(20u,1u,67u,100u),"Keys",128u));
            EXPECT_TRUE(take->RecordMidiEvent(midi::MidiEvent::MakeNoteOff(80u,1u,67u),"Keys",128u));
        }
	}
	take->Play(includeAudio ? constants::MaxLoopFadeSamps : 0u, 128u, 0u);
	take->BeginCaptureAudioBoundary(0u, 0u, 0u);
	station->CommitChanges();
	return take;
}

static std::vector<unsigned char> SourceChoicePickPath(const std::shared_ptr<engine::LoopTake>& take)
{
	std::vector<unsigned char> path;
	const auto id = take->GetLoops().empty() ? take->GetMidiLoops().front()->Model()->GlobalId() : take->GetLoops().front()->GlobalId();
	for (const auto index : id) path.push_back(static_cast<unsigned char>(index + 1u));
	return path;
}

static std::optional<utils::Position2d> SourceChoiceOutputPoint(const std::shared_ptr<gui::GuiHud>& hud)
{
	std::vector<gui::CableInteraction::Endpoint> endpoints; std::vector<gui::CableInteraction::Cable> cables;
	hud->BuildInteractionGeometry(endpoints, cables);
	for (const auto& endpoint : endpoints)
		if (endpoint.Kind == gui::CableInteraction::EndpointKind::TriggerOutput && endpoint.TriggerIndex == 0u)
			return endpoint.Position + hud->GlobalPosition();
	return std::nullopt;
}

TEST_F(SceneRoutingIntegrationTest, ShiftOutputDragUsesFreshReleasePickAndDimsOnlyOtherAudioMidiWithoutDomainEdits)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({Station("Keys"), Station("Other")}, {Trigger("source", "Keys")}); ASSERT_TRUE(scene);
	resources::ResourceLib resources; engine::PressPickWindow window(*scene, resources); window.Resize({1280u, 900u});
	Settle(scene); auto exclusion = Access::ExcludeJob(*scene);
	const auto stations = scene->SnapshotStations();
	auto a = AddCompletedSourceChoiceTake(stations[0]); auto b = AddCompletedSourceChoiceTake(stations[0], true, false);
	auto other = AddCompletedSourceChoiceTake(stations[1]); b->Mute(); scene->CommitChanges();
	auto hud = Access::PrepareHud(*scene); LayoutHud(hud);
	const auto output = SourceChoiceOutputPoint(hud); ASSERT_TRUE(output);
	auto& workspace = Access::Workspace(*scene);
	const auto rig = scene->AcceptedRigSnapshot(); const auto trigger = rig->Triggers.front().Instance;
	const auto master = Access::ClockLength(*scene); const auto aId = a->GetLoops().front()->Model()->GlobalId();
	const auto midiId = b->GetMidiLoops().front()->Model()->GlobalId();
	actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0;
	touch.State = actions::TouchAction::TOUCH_DOWN; touch.Position = *output; touch.Modifiers = base::Action::MODIFIER_SHIFT;
	const auto down = window.OnAction(touch); EXPECT_EQ(hud, down.ActiveElement.lock()); ASSERT_TRUE(workspace.OverdubSourceDragActive());
	EXPECT_TRUE(gui::WorkspaceTestAccess::HasCapture(workspace)); EXPECT_FALSE(hud->HasCableDrag());
    const auto dragFeedback=hud->OverdubSourceFeedback(); ASSERT_FALSE(dragFeedback.empty());
    hud->ApplyExclusiveHoverPoint(hud->GlobalToLocal(*output)); EXPECT_EQ(dragFeedback,hud->OverdubSourceFeedback());
	const auto point = Access::CanvasPoint(*scene);
	actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE; move.Position = point; move.MouseButtonsDown = 1u;
	EXPECT_TRUE(window.OnAction(move).IsEaten);
	scene->SetHover3d(SourceChoicePickPath(b), base::Action::MODIFIER_SHIFT, point);
	EXPECT_EQ(b, gui::WorkspaceTestAccess::OverdubHover(workspace));
	const auto preview = gui::WorkspaceTestAccess::OverdubPreview(workspace, stations);
	ASSERT_TRUE(preview.contains(aId)); ASSERT_TRUE(preview.contains(midiId));
	EXPECT_FALSE(preview.at(aId)); EXPECT_TRUE(preview.at(midiId));
	EXPECT_EQ(midiId, b->GetMidiLoops().front()->Model()->GlobalId()); EXPECT_TRUE(b->IsMuted()); EXPECT_FALSE(a->IsMuted());
	// Deliberately stale asynchronous hover A: matching UP must pick B now.
	scene->SetHover3d(SourceChoicePickPath(a), base::Action::MODIFIER_NONE, point);
	window.PressPath = SourceChoicePickPath(b);
	actions::KeyAction shiftUp; shiftUp.KeyChar = VK_SHIFT; shiftUp.KeyActionType = actions::KeyAction::KEY_UP;
	scene->OnAction(shiftUp); EXPECT_TRUE(workspace.OverdubSourceDragActive());
	touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point; touch.Modifiers = base::Action::MODIFIER_NONE;
	EXPECT_TRUE(window.OnAction(touch).IsEaten); EXPECT_EQ(2u, window.PickCount);
	EXPECT_FALSE(workspace.OverdubSourceDragActive()); EXPECT_FALSE(gui::WorkspaceTestAccess::HasCapture(workspace));
	Access::WhileSceneLocked(*scene, [&] { EXPECT_EQ(b, trigger->NextOverdubSource()); });
	EXPECT_EQ(rig, scene->AcceptedRigSnapshot()); EXPECT_EQ(master, Access::ClockLength(*scene));
	EXPECT_TRUE(trigger->GetTakes().empty()); EXPECT_EQ(0u, trigger->ActivationOutcomeCount());
	EXPECT_FALSE(a->IsSelected()); EXPECT_FALSE(b->IsSelected()); EXPECT_FALSE(other->IsSelected());
	EXPECT_TRUE(gui::WorkspaceTestAccess::OverdubPreview(workspace, stations).empty());
	// A failed release pick must discard stale A rather than change the armed MIDI-only B.
	touch.State = actions::TouchAction::TOUCH_DOWN; touch.Position = *output; touch.Modifiers = base::Action::MODIFIER_SHIFT;
	window.PressPath.clear(); ASSERT_TRUE(window.OnAction(touch).IsEaten); ASSERT_TRUE(workspace.OverdubSourceDragActive());
	scene->SetHover3d(SourceChoicePickPath(a), base::Action::MODIFIER_SHIFT, point); window.FailPick = true;
	touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point; touch.Modifiers = base::Action::MODIFIER_NONE;
	EXPECT_TRUE(window.OnAction(touch).IsEaten);
	Access::WhileSceneLocked(*scene, [&] { EXPECT_EQ(b, trigger->NextOverdubSource()); });
	exclusion.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SourceDragCancellationAndForeignReleasesPreserveBindingAndEatOnlyMatchingLateUp)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	for (const auto cancellation : {0u, 1u, 2u, 3u})
	{
		SCOPED_TRACE(cancellation);
		auto scene = FreshScene({Station("Keys"), Station("Other")}, {Trigger("source", "Keys")}); ASSERT_TRUE(scene);
		scene->SetSize({1280u, 900u}); Settle(scene); auto exclusion = Access::ExcludeJob(*scene);
		const auto stations = scene->SnapshotStations(); auto a = AddCompletedSourceChoiceTake(stations[0]);
		auto foreign = AddCompletedSourceChoiceTake(stations[1]); auto recording = stations[0]->AddTake(); recording->Record({0u}, "busy"); scene->CommitChanges();
		auto hud = Access::PrepareHud(*scene); LayoutHud(hud); const auto output = SourceChoiceOutputPoint(hud); ASSERT_TRUE(output);
		auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance; auto& workspace = Access::Workspace(*scene);
		Access::WhileSceneLocked(*scene, [&] { ASSERT_TRUE(trigger->BindNextOverdubSource(stations[0], a)); });
		actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0; touch.Position = *output;
		touch.State = actions::TouchAction::TOUCH_DOWN; touch.Modifiers = base::Action::MODIFIER_SHIFT;
		ASSERT_TRUE(scene->OnAction(touch).IsEaten); ASSERT_TRUE(workspace.OverdubSourceDragActive());
		const auto point = Access::CanvasPoint(*scene);
		scene->SetHover3d(SourceChoicePickPath(foreign), base::Action::MODIFIER_SHIFT, point);
		EXPECT_EQ(nullptr, gui::WorkspaceTestAccess::OverdubHover(workspace));
		scene->SetHover3d(SourceChoicePickPath(recording), base::Action::MODIFIER_SHIFT, point);
		EXPECT_EQ(nullptr, gui::WorkspaceTestAccess::OverdubHover(workspace));
		auto foreignUp = touch; foreignUp.Index = 2; foreignUp.State = actions::TouchAction::TOUCH_UP;
		EXPECT_TRUE(scene->OnAction(foreignUp).IsEaten); EXPECT_TRUE(workspace.OverdubSourceDragActive());
		if (cancellation == 0u) scene->OnInputFocusLost();
		else if (cancellation == 1u)
		{ actions::KeyAction escape; escape.KeyChar = VK_ESCAPE; escape.KeyActionType = actions::KeyAction::KEY_DOWN; EXPECT_TRUE(scene->OnAction(escape).IsEaten); }
		else if (cancellation == 2u)
		{ actions::TouchMoveAction lost; lost.Touch = actions::TouchAction::TOUCH_MOUSE; lost.Position = point; lost.MouseButtonsDown = 0u; EXPECT_TRUE(scene->OnAction(lost).IsEaten); }
		else Access::OpenKeyboardModal(*scene);
		EXPECT_FALSE(workspace.OverdubSourceDragActive());
		foreignUp.Index = 4; scene->OnAction(foreignUp); // Wheel cannot consume the primary tombstone.
		touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point; touch.Modifiers = base::Action::MODIFIER_NONE;
		EXPECT_TRUE(scene->OnAction(touch).IsEaten); EXPECT_FALSE(gui::WorkspaceTestAccess::HasCapture(workspace));
		Access::WhileSceneLocked(*scene, [&] { EXPECT_EQ(a, trigger->NextOverdubSource()); });
		EXPECT_FALSE(hud->HasCableDrag()); EXPECT_TRUE(trigger->GetTakes().empty());
		exclusion.unlock(); scene->Shutdown();
	}
}

TEST_F(SceneRoutingIntegrationTest, ShiftRightOutputClearsInvalidUnwiredChoiceOnlyOnMatchedRelease)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({Station("Keys"), Station("Other")}, {Trigger("source", "Keys")}); ASSERT_TRUE(scene);
	scene->SetSize({1280u, 900u}); Settle(scene); auto exclusion = Access::ExcludeJob(*scene);
	const auto stations = scene->SnapshotStations(); auto a = AddCompletedSourceChoiceTake(stations[0]); scene->CommitChanges();
	auto hud = Access::PrepareHud(*scene); LayoutHud(hud); const auto output = SourceChoiceOutputPoint(hud); ASSERT_TRUE(output);
	auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance;
	Access::WhileSceneLocked(*scene, [&] { ASSERT_TRUE(trigger->BindNextOverdubSource(stations[0], a)); });
	// Simulate the existing accepted route boundary, not a UI rig candidate.
	std::shared_ptr<base::ActionReceiver> route; std::vector<unsigned int> channels; std::vector<std::string> devices;
	std::shared_ptr<audio::AudioMixer> mixer; std::shared_ptr<base::BounceWriter> writer;
	trigger->ApplyCaptureRouting(route, channels, devices, mixer, writer);
	Access::WhileSceneLocked(*scene, [&] { ASSERT_TRUE(trigger->NextOverdubSourceInvalid()); });
	actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 2;
	touch.State = actions::TouchAction::TOUCH_DOWN; touch.Position = *output; touch.Modifiers = base::Action::MODIFIER_SHIFT;
	EXPECT_TRUE(scene->OnAction(touch).IsEaten); EXPECT_TRUE(trigger->HasExplicitNextOverdubSource());
	scene->OnInputFocusLost(); touch.State = actions::TouchAction::TOUCH_UP; touch.Modifiers = base::Action::MODIFIER_NONE;
	EXPECT_TRUE(scene->OnAction(touch).IsEaten); EXPECT_TRUE(trigger->HasExplicitNextOverdubSource());
	touch.State = actions::TouchAction::TOUCH_DOWN; touch.Modifiers = base::Action::MODIFIER_SHIFT;
	EXPECT_TRUE(scene->OnAction(touch).IsEaten); EXPECT_TRUE(trigger->HasExplicitNextOverdubSource());
	touch.State = actions::TouchAction::TOUCH_UP; touch.Modifiers = base::Action::MODIFIER_NONE;
	EXPECT_TRUE(scene->OnAction(touch).IsEaten); EXPECT_FALSE(trigger->HasExplicitNextOverdubSource());
	EXPECT_FALSE(hud->HasCableDrag()); EXPECT_EQ(0u, trigger->ActivationOutcomeCount());
	exclusion.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SourceChoiceAndClearRemainAvailableDuringAcceptedCaptureWithoutRetargetingItsPins)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({Station("Keys"), Station("Other")}, {Trigger("source", "Keys")}); ASSERT_TRUE(scene);
	scene->SetSize({1280u, 900u}); Settle(scene); auto exclusion = Access::ExcludeJob(*scene);
	const auto stations = scene->SnapshotStations(); auto a = AddCompletedSourceChoiceTake(stations[0]); auto b = AddCompletedSourceChoiceTake(stations[0]); scene->CommitChanges();
	auto hud = Access::PrepareHud(*scene); LayoutHud(hud); const auto output = SourceChoiceOutputPoint(hud); ASSERT_TRUE(output);
	auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance; auto& workspace = Access::Workspace(*scene);
	Access::WhileSceneLocked(*scene, [&] { ASSERT_TRUE(trigger->BindNextOverdubSource(stations[0], a)); });
	base::Action input; io::UserConfig cfg;
	auto control = [&](bool activate, bool down)
	{
		ASSERT_TRUE(trigger->QueueExternalControlAction(activate, down, input).IsEaten);
		trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
		Access::WhileSceneLocked(*scene, [&] { trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt); });
		trigger->OnTick(utils::Timer::GetTime(), 0u, cfg, std::nullopt);
	};
	control(false, true); control(true, true);
	ASSERT_EQ(engine::TRIGSTATE_OVERDUBBING, trigger->GetState()); ASSERT_FALSE(trigger->CanEditRouting());
	ASSERT_FALSE(trigger->GetTakes().empty()); EXPECT_EQ(a->Id(), trigger->GetTakes().back().SourceTakeId);
	actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0; touch.Position = *output;
	touch.State = actions::TouchAction::TOUCH_DOWN; touch.Modifiers = base::Action::MODIFIER_SHIFT;
	ASSERT_TRUE(scene->OnAction(touch).IsEaten); ASSERT_TRUE(workspace.OverdubSourceDragActive()); EXPECT_FALSE(hud->HasCableDrag());
	const auto point = Access::CanvasPoint(*scene); scene->SetHover3d(SourceChoicePickPath(b), base::Action::MODIFIER_SHIFT, point);
	ASSERT_EQ(b, gui::WorkspaceTestAccess::OverdubHover(workspace));
	touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point; touch.Modifiers = base::Action::MODIFIER_NONE;
	EXPECT_TRUE(scene->OnAction(touch).IsEaten);
	Access::WhileSceneLocked(*scene, [&] { EXPECT_EQ(b, trigger->NextOverdubSource()); });
	EXPECT_EQ(a->Id(), trigger->GetTakes().back().SourceTakeId); EXPECT_EQ(engine::TRIGSTATE_OVERDUBBING, trigger->GetState());
	touch.Index = 2; touch.Position = *output; touch.State = actions::TouchAction::TOUCH_DOWN; touch.Modifiers = base::Action::MODIFIER_SHIFT;
	ASSERT_TRUE(scene->OnAction(touch).IsEaten); EXPECT_TRUE(trigger->HasExplicitNextOverdubSource());
	touch.State = actions::TouchAction::TOUCH_UP; touch.Modifiers = base::Action::MODIFIER_NONE;
	EXPECT_TRUE(scene->OnAction(touch).IsEaten); EXPECT_FALSE(trigger->HasExplicitNextOverdubSource());
	EXPECT_EQ(a->Id(), trigger->GetTakes().back().SourceTakeId); EXPECT_EQ(1u, trigger->ActivationOutcomeCount());
	touch.Index = 0; touch.State = actions::TouchAction::TOUCH_DOWN; touch.Modifiers = base::Action::MODIFIER_NONE;
	scene->OnAction(touch); EXPECT_FALSE(hud->HasCableDrag()); EXPECT_FALSE(workspace.OverdubSourceDragActive());
	touch.State = actions::TouchAction::TOUCH_UP; scene->OnAction(touch);
	EXPECT_EQ(1u, trigger->ActivationOutcomeCount()); EXPECT_EQ(a->Id(), trigger->GetTakes().back().SourceTakeId);
	exclusion.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, SourceDragRejectsForeignDropAndChangedRigWhileCtrlAltKeepExistingInputPath)
{
	using Access = engine::SourceLossIntegrationTestAccess;
	auto scene = FreshScene({Station("Keys"), Station("Other")}, {Trigger("source", "Keys")}); ASSERT_TRUE(scene);
	scene->SetSize({1280u, 900u}); Settle(scene); auto exclusion = Access::ExcludeJob(*scene);
	const auto stations = scene->SnapshotStations(); auto a = AddCompletedSourceChoiceTake(stations[0]); auto foreign = AddCompletedSourceChoiceTake(stations[1]); scene->CommitChanges();
	auto hud = Access::PrepareHud(*scene); LayoutHud(hud); const auto output = SourceChoiceOutputPoint(hud); ASSERT_TRUE(output);
	auto trigger = scene->AcceptedRigSnapshot()->Triggers.front().Instance; auto& workspace = Access::Workspace(*scene);
	Access::WhileSceneLocked(*scene, [&] { ASSERT_TRUE(trigger->BindNextOverdubSource(stations[0], a)); });
	actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE; touch.Index = 0; touch.Position = *output;
	for (const auto modifiers : {base::Action::MODIFIER_SHIFT | base::Action::MODIFIER_CTRL, base::Action::MODIFIER_SHIFT | base::Action::MODIFIER_ALT})
	{
		touch.State = actions::TouchAction::TOUCH_DOWN; touch.Modifiers = static_cast<base::Action::Modifiers>(modifiers);
		scene->OnAction(touch); EXPECT_FALSE(workspace.OverdubSourceDragActive());
		scene->OnInputFocusLost(); touch.State = actions::TouchAction::TOUCH_UP; scene->OnAction(touch);
	}
	touch.State = actions::TouchAction::TOUCH_DOWN; touch.Modifiers = base::Action::MODIFIER_SHIFT; ASSERT_TRUE(scene->OnAction(touch).IsEaten);
	const auto point = Access::CanvasPoint(*scene); scene->SetHover3d(SourceChoicePickPath(foreign), base::Action::MODIFIER_SHIFT, point);
	EXPECT_EQ(nullptr, gui::WorkspaceTestAccess::OverdubHover(workspace));
	touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point; EXPECT_TRUE(scene->OnAction(touch).IsEaten);
	Access::WhileSceneLocked(*scene, [&] { EXPECT_EQ(a, trigger->NextOverdubSource()); });
	touch.State = actions::TouchAction::TOUCH_DOWN; touch.Position = *output; ASSERT_TRUE(scene->OnAction(touch).IsEaten);
	ASSERT_TRUE(workspace.OverdubSourceDragActive());
	auto replacement = *scene->AcceptedRigSnapshot(); replacement.Revision += 1u;
	hud->SetRoutingConfig(2u, {}, replacement); LayoutHud(hud);
	actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE; move.MouseButtonsDown = 1u; move.Position = point;
	EXPECT_TRUE(scene->OnAction(move).IsEaten); EXPECT_FALSE(workspace.OverdubSourceDragActive());
	touch.State = actions::TouchAction::TOUCH_UP; touch.Position = point; EXPECT_TRUE(scene->OnAction(touch).IsEaten);
	Access::WhileSceneLocked(*scene, [&] { EXPECT_EQ(a, trigger->NextOverdubSource()); });
	EXPECT_FALSE(hud->HasCableDrag()); EXPECT_TRUE(trigger->GetTakes().empty());
	exclusion.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, MiddleMarqueeActualGeometryRestoresOnShrinkAndConsumesCanceledRelease)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Marquee")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene);
    auto station=scene->SnapshotStations().front(); auto take=AddCompletedSourceChoiceTake(station);
    scene->CommitChanges(); Access::SetDepth(*scene,engine::Scene::VIEW_STATION);
    scene->AdvanceUiAnimations();
    scene->SetHover3d({},base::Action::MODIFIER_NONE);
    actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1;
    press.Position={1050,40}; press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u;
    Access::ExpectUncoveredRackPoint(*scene,press.Position);
    ASSERT_TRUE(Access::GeometryAt(*scene,{1050,40,1051,41},base::DEPTH_STATION).empty());
    scene->OnAction(press);
    ASSERT_TRUE(Access::BackgroundOwned(*scene)); EXPECT_FALSE(station->RetainedMuted());
    actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.MouseButtonsDown=2u; move.Position={400,800};
    scene->OnAction(move); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene));
    EXPECT_TRUE(station->RetainedMuted()); EXPECT_TRUE(take->IsMuted());
    move.Position=press.Position+utils::Position2d{1,1}; scene->OnAction(move); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene));
    EXPECT_FALSE(station->RetainedMuted()); EXPECT_FALSE(take->IsMuted());
    move.Position={400,800}; scene->OnAction(move); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene));
    ASSERT_TRUE(station->RetainedMuted());
    Access::MoveCameraAway(*scene); scene->AdvanceUiAnimations();
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_FALSE(station->RetainedMuted());
    scene->OnInputFocusLost();
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_FALSE(station->RetainedMuted());
    // A delayed middle release must not become global unmute after cancellation.
    take->SetGroupMutedAudio(true); press.State=actions::TouchAction::TOUCH_UP; press.MouseButtonsDown=0u;
    press.Position=move.Position; scene->OnAction(press); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_TRUE(take->IsMuted());
    job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, OutputHintLeavesWithNormalHudHoverAndFocus)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Keys"),Station("Other")},{Trigger("source","Keys")}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); scene->AdvanceUiAnimations();
    auto hud=Access::PrepareHud(*scene); Access::PrepareHudPresentation(*scene); const auto output=SourceChoiceOutputPoint(hud); ASSERT_TRUE(output);
    ASSERT_TRUE(hud->OverdubOutputAt(hud->GlobalToLocal(*output)));
    actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.Position=*output;
    scene->OnAction(move); EXPECT_FALSE(hud->OverdubSourceFeedback().empty());
    const auto shownHint=hud->OverdubSourceFeedback();
    hud->ApplyExclusiveHoverPoint(hud->GlobalToLocal(*output)); EXPECT_EQ(shownHint,hud->OverdubSourceFeedback());
    move.Position=Access::CanvasPoint(*scene); scene->OnAction(move); EXPECT_TRUE(hud->OverdubSourceFeedback().empty());
    move.Position=*output; scene->OnAction(move); EXPECT_FALSE(hud->OverdubSourceFeedback().empty());
    scene->OnInputFocusLost(); EXPECT_TRUE(hud->OverdubSourceFeedback().empty()); job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ImmediateOfflineSaveAdoptsPendingManualIntentBeforeJamCapture)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Save mute")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); Access::SetSaveTiming(*scene);
    auto station=scene->SnapshotStations().front(); auto take=AddCompletedSourceChoiceTake(station); scene->CommitChanges();
    Access::SetDepth(*scene,engine::Scene::VIEW_LOOPTAKE);
    const auto point=Access::CanvasPoint(*scene);
    scene->SetHover3d(SourceChoicePickPath(take),base::Action::MODIFIER_NONE,point);
    actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1;
    press.Position=point; press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u;
    scene->OnAction(press); press.State=actions::TouchAction::TOUCH_UP; press.MouseButtonsDown=0u; scene->OnAction(press);
    ASSERT_TRUE(Access::DesiredMute(*scene,{engine::GroupTargetKind::Take,station->Id(),take->Id(),{}}));
    auto admitted=Access::BorrowMute(*scene); ASSERT_TRUE(admitted); ASSERT_EQ(1u,admitted->Takes.size()); EXPECT_EQ(take,admitted->Takes[0].Take); EXPECT_TRUE(admitted->Takes[0].Muted); admitted.reset();
    EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); // No callback has consumed the admitted intent.
    const ScopedMuteDirectory ownedDirectory("jamma-pending-mute-save-"); const auto& directory=ownedDirectory.Path;
    const auto saved=Access::SaveOffline(*scene,directory.wstring()); ASSERT_TRUE(saved.Success) << saved.Error;
    std::ifstream file(std::filesystem::path(saved.Path)); std::stringstream json; json<<file.rdbuf();
    auto jam=io::JamFile::FromStream(std::move(json)); ASSERT_TRUE(jam); ASSERT_EQ(1u,jam->Stations.size());
    ASSERT_EQ(1u,jam->Stations[0].LoopTakes.size());
    ASSERT_EQ(1u,jam->Stations[0].LoopTakes[0].Loops.size()); EXPECT_TRUE(jam->Stations[0].LoopTakes[0].Loops[0].Muted);
    job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, ManualUiSupersedesAlreadyBorrowedMuteAtActualHostAdoption)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Borrowed mute")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front();
    auto take=AddCompletedSourceChoiceTake(station); scene->CommitChanges(); Access::SetDepth(*scene,engine::Scene::VIEW_LOOPTAKE);
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene));
    const auto point=Access::CanvasPoint(*scene);
    const auto click=[&] {
        scene->SetHover3d(SourceChoicePickPath(take),base::Action::MODIFIER_NONE,point);
        actions::TouchAction action; action.Touch=actions::TouchAction::TOUCH_MOUSE; action.Index=1;
        action.Position=point; action.State=actions::TouchAction::TOUCH_DOWN; action.MouseButtonsDown=2u; scene->OnAction(action);
        action.State=actions::TouchAction::TOUCH_UP; action.MouseButtonsDown=0u; scene->OnAction(action);
    };
    click(); auto borrowed=Access::BorrowMute(*scene); ASSERT_TRUE(borrowed);
    EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    click(); // Latest desired true toggles false even though raw is still false.
    auto newest=Access::BorrowMute(*scene); ASSERT_TRUE(newest); ASSERT_GT(newest->Version,borrowed->Version);
    EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    Access::AdoptBorrowedMute(*scene,borrowed.get()); EXPECT_TRUE(take->IsMuted());
    EXPECT_EQ(borrowed->Version,Access::MuteReceipt(*scene));
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_FALSE(take->IsMuted());
    EXPECT_EQ(newest->Version,Access::MuteReceipt(*scene));
    Access::AdoptBorrowedMute(*scene,borrowed.get()); EXPECT_FALSE(take->IsMuted());
    EXPECT_EQ(newest->Version,Access::MuteReceipt(*scene));
    borrowed.reset(); newest.reset(); job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, MarqueeAllDepthsNormalizesMidiSiblingsAndPreservesRawBaseline)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    for (const auto depth : {engine::Scene::VIEW_STATION,engine::Scene::VIEW_LOOPTAKE,engine::Scene::VIEW_LOOP})
    {
        SCOPED_TRACE(depth);
        auto scene=FreshScene({Station("Depth marquee")},{}); ASSERT_TRUE(scene);
        resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
        auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front();
        auto take=AddCompletedSourceChoiceTake(station,true,true,true); scene->CommitChanges();
        ASSERT_EQ(2u,take->GetMidiLoops().size()); Access::SetDepth(*scene,depth); scene->AdvanceUiAnimations();
        scene->SetHover3d({},base::Action::MODIFIER_NONE);
        actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1; press.Position={1050,40};
        press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u; scene->OnAction(press); ASSERT_TRUE(Access::BackgroundOwned(*scene));
        actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.Position={400,800}; move.MouseButtonsDown=2u; scene->OnAction(move);
        ASSERT_TRUE(Access::ApplyMuteBoundary(*scene));
        EXPECT_TRUE(take->IsMuted());
        if(depth==engine::Scene::VIEW_STATION) EXPECT_TRUE(station->RetainedMuted());
        else EXPECT_FALSE(station->RetainedMuted());
        // Even leaf-depth intersection with either channel mutes the owning take, not an individual MIDI stream.
        EXPECT_TRUE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
        scene->OnInputFocusLost(); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene));
        EXPECT_FALSE(take->IsMuted()); EXPECT_FALSE(station->RetainedMuted());
        job.unlock(); scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, OfflineSaveCommitsReleasedMarqueeButCancelsActivePreview)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    for(const bool commit : {false,true})
    {
        SCOPED_TRACE(commit);
        auto scene=FreshScene({Station("Marquee save")},{}); ASSERT_TRUE(scene);
        resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
        auto job=Access::ExcludeJob(*scene); Access::SetSaveTiming(*scene);
        auto station=scene->SnapshotStations().front(); auto take=AddCompletedSourceChoiceTake(station); scene->CommitChanges();
        Access::SetDepth(*scene,engine::Scene::VIEW_STATION); scene->AdvanceUiAnimations(); scene->SetHover3d({},base::Action::MODIFIER_NONE);
        actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1; press.Position={1050,40};
        press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u; scene->OnAction(press); ASSERT_TRUE(Access::BackgroundOwned(*scene));
        actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.Position={400,800}; move.MouseButtonsDown=2u; scene->OnAction(move);
        ASSERT_TRUE(Access::DesiredMute(*scene,{engine::GroupTargetKind::Station,station->Id(),{},{}}));
        if(commit) { press.State=actions::TouchAction::TOUCH_UP; press.Position=move.Position; press.MouseButtonsDown=0u; scene->OnAction(press); }
        else { ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); ASSERT_TRUE(station->RetainedMuted()); }
        const ScopedMuteDirectory ownedDirectory("jamma-marquee-save-"); const auto& directory=ownedDirectory.Path;
        const auto saved=Access::SaveOffline(*scene,directory.wstring()); ASSERT_TRUE(saved.Success) << saved.Error;
        std::ifstream file(std::filesystem::path(saved.Path)); std::stringstream json; json<<file.rdbuf();
        auto jam=io::JamFile::FromStream(std::move(json)); ASSERT_TRUE(jam); ASSERT_EQ(1u,jam->Stations.size());
        EXPECT_EQ(commit,jam->Stations[0].Muted); EXPECT_EQ(commit,station->RetainedMuted());
        ASSERT_EQ(1u,jam->Stations[0].LoopTakes.size()); ASSERT_EQ(1u,jam->Stations[0].LoopTakes[0].Loops.size());
        EXPECT_EQ(commit,jam->Stations[0].LoopTakes[0].Loops[0].Muted);
        file.close(); job.unlock(); scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, MarqueeIncludesDisplayedRemoteAndEmptyStationWithoutSelectionMutation)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Empty marquee")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); auto empty=scene->SnapshotStations().front(); ASSERT_TRUE(empty->GetLoopTakeSnapshot().empty());
    engine::StationParams params; params.Id="remote-marquee"; params.Size={160u,320u};
    auto remote=std::make_shared<engine::RemoteSelectionStation>(params,engine::Station::GetMixerParams(params.Size,audio::MergeMixBehaviourParams{}));
    auto remoteTake=AddCompletedSourceChoiceTake(remote); Access::AddStation(*scene,remote); scene->CommitChanges();
    Access::SetDepth(*scene,engine::Scene::VIEW_STATION); scene->AdvanceUiAnimations(); scene->SetHover3d({},base::Action::MODIFIER_NONE);
    actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1; press.Position={1050,40};
    press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u; scene->OnAction(press); ASSERT_TRUE(Access::BackgroundOwned(*scene));
    actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.Position={400,800}; move.MouseButtonsDown=2u; scene->OnAction(move);
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_TRUE(empty->RetainedMuted()); EXPECT_TRUE(remote->RetainedMuted()); EXPECT_TRUE(remoteTake->IsMuted());
    EXPECT_FALSE(empty->IsSelected()); EXPECT_FALSE(remote->IsSelected()); EXPECT_FALSE(remoteTake->IsSelected());
    scene->OnInputFocusLost(); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_FALSE(empty->RetainedMuted()); EXPECT_FALSE(remote->RetainedMuted());
    job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, LeafMarqueeKeepsMidiOwnerMutedWhileOneSiblingLeavesAndOtherRemains)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Sibling geometry")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front();
    auto take=AddCompletedSourceChoiceTake(station,true,true,true); scene->CommitChanges(); ASSERT_EQ(2u,take->GetMidiLoops().size());
    Access::SetDepth(*scene,engine::Scene::VIEW_LOOP); Access::FrontCamera(*scene,*station); scene->AdvanceUiAnimations();
    Access::GeometryAt(*scene,{0,0,1280,900},base::DEPTH_LOOP); // Prepare current published model geometry.
    const auto a=take->GetMidiLoops()[0],b=take->GetMidiLoops()[1];
    const auto low=Access::MidiPitchPoint(*scene,*station,*take,*a,60), high=Access::MidiPitchPoint(*scene,*station,*take,*b,67);
    ASSERT_LT(low.Y,high.Y);
    const utils::Position2d anchor{(std::min)(low.X,high.X)-20,high.Y+30};
    const utils::Position2d both{(std::max)(low.X,high.X)+20,low.Y-30},onlyB{both.X,high.Y-15};
    const utils::Rect2d bRectangle{anchor.X,onlyB.Y,onlyB.X,anchor.Y};
    ASSERT_FALSE(Access::MidiGeometryAt(*scene,*station,*take,*a,bRectangle));
    ASSERT_TRUE(Access::MidiGeometryAt(*scene,*station,*take,*b,bRectangle));
    Access::ExpectUncoveredRackPoint(*scene,anchor); scene->SetHover3d({},base::Action::MODIFIER_NONE);
    actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1; press.Position=anchor;
    press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u; scene->OnAction(press); ASSERT_TRUE(Access::BackgroundOwned(*scene));
    actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.Position=both; move.MouseButtonsDown=2u;
    scene->OnAction(move); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); ASSERT_TRUE(take->IsMuted());
    move.Position=onlyB; scene->OnAction(move); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_TRUE(take->IsMuted());
    EXPECT_TRUE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    move.Position=anchor+utils::Position2d{1,1}; scene->OnAction(move); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_FALSE(take->IsMuted());
    scene->OnInputFocusLost(); job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, LeafMarqueeExcludesActualSettingsOcclusionButIncludesVisiblePartialSurface)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Occluded audio")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front();
    auto take=AddCompletedSourceChoiceTake(station); scene->CommitChanges(); ASSERT_EQ(1u,take->GetLoops().size()); const auto loop=take->GetLoops()[0];
    Access::SetDepth(*scene,engine::Scene::VIEW_LOOP); Access::FrontCamera(*scene,*station); scene->AdvanceUiAnimations();
    const auto frame=Access::ExpandSettings(*scene); ASSERT_GT(frame.Right-frame.Left,260); ASSERT_GT(frame.Top-frame.Bottom,300);
    const auto middleY=(frame.Bottom+frame.Top)/2;
    Access::CameraCentreAt(*scene,{(frame.Left+frame.Right)/2,middleY}); scene->AdvanceUiAnimations();
    const utils::Position2d anchor{1050,40},end{frame.Left+1,frame.Top-1};
    const utils::Rect2d rectangle{end.X,anchor.Y,anchor.X,end.Y};
    const auto unclipped=Access::GeometryAt(*scene,rectangle,base::DEPTH_LOOP);
    ASSERT_TRUE(std::any_of(unclipped.begin(),unclipped.end(),[&](const auto& target){return target.Kind==engine::GroupTargetKind::AudioLoop && target.EntityId==loop->Id();}));
    Access::ExpectUncoveredRackPoint(*scene,anchor); ASSERT_TRUE(Access::GeometryAt(*scene,{1050,40,1051,41},base::DEPTH_LOOP).empty());
    scene->SetHover3d({},base::Action::MODIFIER_NONE);
    actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1; press.Position=anchor; press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u;
    scene->OnAction(press); ASSERT_TRUE(Access::BackgroundOwned(*scene));
    actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.Position=end; move.MouseButtonsDown=2u;
    scene->OnAction(move); ASSERT_TRUE(Access::ApplyMuteBoundary(*scene));
    EXPECT_FALSE(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); // Geometry exists but is wholly hidden by actual Settings scissor.
    Access::CameraCentreAt(*scene,{frame.Right-20,middleY}); scene->AdvanceUiAnimations();
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_TRUE(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED); EXPECT_FALSE(station->RetainedMuted());
    // Keeping the same capture/rectangle, cover the surface again: restore only that raw leaf baseline.
    Access::CameraCentreAt(*scene,{(frame.Left+frame.Right)/2,middleY}); scene->AdvanceUiAnimations();
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_FALSE(loop->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    scene->OnInputFocusLost(); job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, StationaryMarqueeLateReleaseCannotOverwriteCompetingMuteBeforePresentation)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Competing mute")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front(); auto take=AddCompletedSourceChoiceTake(station); scene->CommitChanges();
    Access::SetDepth(*scene,engine::Scene::VIEW_STATION); scene->AdvanceUiAnimations();
    const utils::Position2d point{1050,40}; Access::ExpectUncoveredRackPoint(*scene,point);
    ASSERT_TRUE(Access::GeometryAt(*scene,{1050,40,1051,41},base::DEPTH_STATION).empty()); scene->SetHover3d({},base::Action::MODIFIER_NONE);
    actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1; press.Position=point;
    press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u; scene->OnAction(press); ASSERT_TRUE(Access::BackgroundOwned(*scene));
    ASSERT_TRUE(Access::GlobalMuteCommand(*scene,true)); auto newer=Access::BorrowMute(*scene); ASSERT_TRUE(newer);
    ASSERT_TRUE(Access::DesiredMute(*scene,{engine::GroupTargetKind::Take,station->Id(),take->Id(),{}}));
    EXPECT_FALSE(take->GetTweakState() & base::Tweakable::TWEAKSTATE_MUTED);
    // No presentation frame and no audio adoption occur between newer command and the old matching release.
    press.State=actions::TouchAction::TOUCH_UP; press.MouseButtonsDown=0u; EXPECT_TRUE(scene->OnAction(press).IsEaten);
    auto afterRelease=Access::BorrowMute(*scene); ASSERT_TRUE(afterRelease); EXPECT_EQ(newer->Version,afterRelease->Version);
    ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_TRUE(take->IsMuted()); EXPECT_EQ(newer->Version,Access::MuteReceipt(*scene));
    EXPECT_FALSE(Access::BackgroundOwned(*scene)); newer.reset(); afterRelease.reset(); job.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, UseMasterButtonAndShiftClickShareActualAdmissionAndApplyingIdentity)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto scene=FreshScene({Station("Master command")},{}); ASSERT_TRUE(scene);
    resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
    auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front();
    auto take=station->AddTake(); take->Record({0u},station->Name()); take->CommitChanges();
    const auto loop=take->GetLoops().front(); std::vector<float> samples(constants::MaxLoopFadeSamps+48000u,0.25f);
    base::AudioWriteRequest write; write.samples=samples.data(); write.numSamps=static_cast<unsigned int>(samples.size()); write.fadeNew=1.0f;
    loop->OnBlockWrite(write,0u); loop->EndWrite(write.numSamps,true); take->Play(constants::MaxLoopFadeSamps,48000u,0u); take->BeginCaptureAudioBoundary(0u,0u,0u);
    station->CommitChanges(); scene->CommitChanges(); ASSERT_TRUE(loop->CanEditAudioSource());
    Access::SetDepth(*scene,engine::Scene::VIEW_LOOPTAKE); const auto canvas=Access::CanvasPoint(*scene);
    scene->SetHover3d(SourceChoicePickPath(take),base::Action::MODIFIER_NONE,canvas);
    actions::TouchAction touch; touch.Touch=actions::TouchAction::TOUCH_MOUSE; touch.Index=0; touch.Position=canvas;
    touch.State=actions::TouchAction::TOUCH_DOWN; touch.MouseButtonsDown=1u; scene->OnAction(touch);
    touch.State=actions::TouchAction::TOUCH_UP; touch.MouseButtonsDown=0u; scene->OnAction(touch); ASSERT_TRUE(take->IsSelected());
    const auto button=Access::MasterButtonPoint(*scene);
    actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.Position=button; scene->OnAction(move);
    touch.Position=button; touch.State=actions::TouchAction::TOUCH_DOWN; touch.MouseButtonsDown=1u; scene->OnAction(touch);
    touch.State=actions::TouchAction::TOUCH_UP; touch.MouseButtonsDown=0u; scene->OnAction(touch);
    ASSERT_TRUE(Access::MasterPending(*scene)); const auto version=Access::MasterVersion(*scene); ASSERT_GT(version,0u);
    unsigned int savePickers=0u; Access::PickDirectory(*scene,[&](const auto&) { ++savePickers; return std::wstring{}; });
    Access::Save(*scene); EXPECT_EQ(0u,savePickers); EXPECT_TRUE(Access::MasterPending(*scene));
    EXPECT_NE(std::string::npos,Access::MasterStatus(*scene).find("Applying"));
    // Same pending source through retained Shift-click does not publish another request.
    touch.Position=canvas; touch.Modifiers=base::Action::MODIFIER_SHIFT; touch.State=actions::TouchAction::TOUCH_DOWN; touch.MouseButtonsDown=1u;
    scene->SetHover3d(SourceChoicePickPath(take),base::Action::MODIFIER_SHIFT,canvas); EXPECT_TRUE(scene->OnAction(touch).IsEaten);
    EXPECT_EQ(version,Access::MasterVersion(*scene));
    touch.State=actions::TouchAction::TOUCH_UP; touch.MouseButtonsDown=0u; scene->OnAction(touch);
    Access::ActivateMaster(*scene); EXPECT_FALSE(Access::MasterPending(*scene));
    EXPECT_NE(std::string::npos,Access::MasterStatus(*scene).find("Current master"));
    touch.State=actions::TouchAction::TOUCH_DOWN; touch.MouseButtonsDown=1u; scene->OnAction(touch);
    EXPECT_EQ(version,Access::MasterVersion(*scene)); EXPECT_NE(std::string::npos,Access::MasterStatus(*scene).find("Already master"));
    touch.State=actions::TouchAction::TOUCH_UP; touch.MouseButtonsDown=0u; scene->OnAction(touch);
    // A failed new choice with an already accepted master must roll back before
    // generic activated-master discovery can send that old tempo as the new request.
    Access::ConnectMasterTiming(*scene); const auto sends=Access::MasterTempoSends(*scene);
    auto other=station->AddTake(); other->Record({},station->Name(),{0u},{""}); other->Play(0u,48000u,0u); other->BeginCaptureAudioBoundary(0u,0u,0u);
    station->CommitChanges(); scene->CommitChanges();
    scene->SetHover3d(SourceChoicePickPath(other),base::Action::MODIFIER_SHIFT,canvas);
    touch.State=actions::TouchAction::TOUCH_DOWN; touch.MouseButtonsDown=1u; ASSERT_TRUE(scene->OnAction(touch).IsEaten);
    ASSERT_TRUE(Access::MasterPending(*scene));
    touch.State=actions::TouchAction::TOUCH_UP; touch.MouseButtonsDown=0u; scene->OnAction(touch);
    other->CancelCapture();
    job.unlock(); Access::PumpMasterJob(*scene);
    EXPECT_FALSE(Access::MasterPending(*scene)); EXPECT_EQ(sends,Access::MasterTempoSends(*scene));
    EXPECT_EQ(ninjam::NinjamLocalTempoState::Following,Access::MasterFollowState(*scene));
    auto finalJob=Access::ExcludeJob(*scene);
    Access::SetDepth(*scene,engine::Scene::VIEW_LOOP);
    scene->SetHover3d(SourceChoicePickPath(take),base::Action::MODIFIER_SHIFT,canvas);
    touch.State=actions::TouchAction::TOUCH_DOWN; touch.MouseButtonsDown=1u; scene->OnAction(touch);
    ASSERT_TRUE(Access::MasterPending(*scene)); EXPECT_TRUE(Access::RequestResetDuringMaster(*scene));
    finalJob.unlock(); scene->Shutdown();
}

TEST_F(SceneRoutingIntegrationTest, RestoredChosenMasterAndOtherCompletedSourceRemainEligibleWithoutInferringInvalidReference)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    auto source=FreshScene({Station("Saved masters")},{}); ASSERT_TRUE(source); Settle(source);
    auto sourceJob=Access::ExcludeJob(*source); auto station=source->SnapshotStations().front();
    auto chosen=AddCompletedSourceChoiceTake(station); auto other=AddCompletedSourceChoiceTake(station); source->CommitChanges();
    ASSERT_EQ(engine::Quantiser::MasterOutcome::Applying,Access::AssignMaster(*source,chosen).Outcome); Access::ActivateMaster(*source);
    ASSERT_TRUE(Access::ChosenMaster(*source));
    const ScopedMuteDirectory directory("jamma-chosen-master-scene-");
    const auto saved=Access::SaveOffline(*source,directory.Path.wstring()); ASSERT_TRUE(saved.Success) << saved.Error;
    std::ifstream file(std::filesystem::path(saved.Path)); std::stringstream bytes; bytes<<file.rdbuf();
    const auto jam=io::JamFile::FromStream(std::move(bytes)); ASSERT_TRUE(jam); ASSERT_TRUE(jam->ChosenMaster); file.close();
    const auto rig=source->AcceptedRigSnapshot()->Rig; sourceJob.unlock(); source->Shutdown();
    for(const bool valid:{true,false})
    {
        SCOPED_TRACE(valid);
        auto manifest=*jam; if(!valid) manifest.ChosenMaster->TakeId="removed-take";
        engine::SceneParams params({""},{},{640u,480u});
        auto created=engine::Scene::FromFile(params,manifest,rig,directory.Path.wstring(),[](const io::RigFile&) { return true; }); ASSERT_TRUE(created);
        auto restored=*created;
        // Let the real job owner reconcile imported membership before any UI child commit.
        const auto before=Access::MasterClock(*restored); Access::PumpMasterJob(*restored);
        auto job=Access::ExcludeJob(*restored); const auto stations=restored->SnapshotStations(); ASSERT_EQ(1u,stations.size());
        const auto takes=stations[0]->GetLoopTakeSnapshot(); ASSERT_EQ(2u,takes.size());
        ASSERT_TRUE(takes[0]->GetLoops().empty()); ASSERT_TRUE(takes[0]->TimingMaterialForRestore().first);
        EXPECT_EQ(valid,Access::ChosenMaster(*restored).has_value());
        for(const auto& take:takes) take->CommitChanges();
        stations[0]->CommitChanges(); restored->CommitChanges();
        ASSERT_FALSE(takes[0]->GetLoops().empty());
        ASSERT_EQ(engine::LoopTake::STATE_INACTIVE,takes[0]->TakeState()); ASSERT_TRUE(takes[0]->HasCompletedTimingMaterial());
        EXPECT_EQ(before,Access::MasterClock(*restored));
        EXPECT_EQ(valid,Access::ChosenMaster(*restored).has_value());
        if(valid)
        {
            const auto version=Access::MasterVersion(*restored);
            EXPECT_EQ(engine::Quantiser::MasterOutcome::Unchanged,Access::AssignMaster(*restored,takes[0]).Outcome);
            EXPECT_EQ(version,Access::MasterVersion(*restored)); EXPECT_EQ(before,Access::MasterClock(*restored));
            ASSERT_EQ(engine::Quantiser::MasterOutcome::Applying,Access::AssignMaster(*restored,takes[1]).Outcome);
            Access::ActivateMaster(*restored); EXPECT_FALSE(Access::MasterPending(*restored));
            const auto accepted=Access::ChosenMaster(*restored); ASSERT_TRUE(accepted); EXPECT_EQ(takes[1]->Id(),accepted->TakeId);
        }
        else
        {
            EXPECT_FALSE(Access::ChosenMaster(*restored)); EXPECT_EQ(before,Access::MasterClock(*restored));
            const auto resaved=Access::SaveOffline(*restored,directory.Path.wstring()); ASSERT_TRUE(resaved.Success) << resaved.Error;
            std::ifstream again(std::filesystem::path(resaved.Path)); std::stringstream contents; contents<<again.rdbuf();
            const auto unknown=io::JamFile::FromStream(std::move(contents)); ASSERT_TRUE(unknown); EXPECT_FALSE(unknown->ChosenMaster); EXPECT_TRUE(unknown->MasterAudioLoopId.empty());
        }
        job.unlock(); restored->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, BothResetModesCancelActualShiftSourceCaptureAndConsumeLateReleaseWithoutRebinding)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    for(const bool removeVsts:{false,true})
    {
        SCOPED_TRACE(removeVsts);
        auto scene=FreshScene({Station("Keys")},{Trigger("source","Keys")}); ASSERT_TRUE(scene);
        resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
        auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front();
        auto a=AddCompletedSourceChoiceTake(station),b=AddCompletedSourceChoiceTake(station,true,false); scene->CommitChanges();
        auto trigger=scene->AcceptedRigSnapshot()->Triggers.front().Instance;
        Access::WhileSceneLocked(*scene,[&] { ASSERT_TRUE(trigger->BindNextOverdubSource(station,a)); });
        auto hud=Access::PrepareHud(*scene); LayoutHud(hud); const auto output=SourceChoiceOutputPoint(hud); ASSERT_TRUE(output);
        auto& workspace=Access::Workspace(*scene);
        actions::TouchAction touch; touch.Touch=actions::TouchAction::TOUCH_MOUSE; touch.Index=0; touch.Position=*output;
        touch.Modifiers=base::Action::MODIFIER_SHIFT; touch.MouseButtonsDown=1u; touch.State=actions::TouchAction::TOUCH_DOWN;
        ASSERT_TRUE(window.OnAction(touch).IsEaten); ASSERT_TRUE(workspace.OverdubSourceDragActive());
        const auto point=Access::CanvasPoint(*scene); scene->SetHover3d(SourceChoicePickPath(b),base::Action::MODIFIER_SHIFT,point);
        ASSERT_EQ(b,gui::WorkspaceTestAccess::OverdubHover(workspace)); ASSERT_FALSE(gui::WorkspaceTestAccess::OverdubPreview(workspace,{station}).empty());
        ExecuteReset(scene,removeVsts,job); ASSERT_TRUE(Access::ControlIdle(*scene));
        EXPECT_FALSE(workspace.OverdubSourceDragActive()); EXPECT_FALSE(hud->HasCableDrag()); EXPECT_FALSE(gui::WorkspaceTestAccess::HasCapture(workspace));
        EXPECT_TRUE(gui::WorkspaceTestAccess::OverdubPreview(workspace,{station}).empty()); EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
        Access::WhileSceneLocked(*scene,[&] { EXPECT_FALSE(trigger->HasExplicitNextOverdubSource()); EXPECT_EQ(nullptr,trigger->NextOverdubSource()); });
        auto fresh=AddCompletedSourceChoiceTake(station,true,false); scene->CommitChanges();
        touch.Position=point; touch.Modifiers=base::Action::MODIFIER_NONE; touch.MouseButtonsDown=0u; touch.State=actions::TouchAction::TOUCH_UP;
        window.PressPath=SourceChoicePickPath(fresh); EXPECT_TRUE(window.OnAction(touch).IsEaten);
        EXPECT_FALSE(trigger->HasExplicitNextOverdubSource()); EXPECT_TRUE(trigger->GetTakes().empty()); EXPECT_EQ(0ul,Access::ClockLength(*scene));
        scene->AdvanceUiAnimations(); // Normal frame consumes the reset input-reset request before a fresh gesture.
        touch.Position=*output; touch.Modifiers=base::Action::MODIFIER_SHIFT; touch.MouseButtonsDown=1u; touch.State=actions::TouchAction::TOUCH_DOWN;
        ASSERT_TRUE(window.OnAction(touch).IsEaten); ASSERT_TRUE(workspace.OverdubSourceDragActive()); scene->OnInputFocusLost();
        job.unlock(); scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, BothResetModesAbandonAppliedMarqueeAndQueuedRestoreWithoutConsumingFutureMute)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    for(const bool removeVsts:{false,true})
    {
        SCOPED_TRACE(removeVsts);
        auto scene=FreshScene({Station("Marquee reset")},{}); ASSERT_TRUE(scene);
        resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
        auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front(); auto discarded=AddCompletedSourceChoiceTake(station); scene->CommitChanges();
        Access::SetDepth(*scene,engine::Scene::VIEW_STATION); scene->AdvanceUiAnimations(); scene->SetHover3d({},base::Action::MODIFIER_NONE);
        actions::TouchAction press; press.Touch=actions::TouchAction::TOUCH_MOUSE; press.Index=1; press.Position={1050,40}; press.MouseButtonsDown=2u; press.State=actions::TouchAction::TOUCH_DOWN;
        Access::ExpectUncoveredRackPoint(*scene,press.Position); ASSERT_TRUE(Access::GeometryAt(*scene,{1050,40,1051,41},base::DEPTH_STATION).empty());
        scene->OnAction(press); ASSERT_TRUE(Access::BackgroundOwned(*scene));
        actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.MouseButtonsDown=2u; move.Position={400,800}; scene->OnAction(move);
        ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); ASSERT_TRUE(station->RetainedMuted()); auto applied=Access::BorrowMute(*scene); ASSERT_TRUE(applied);
        move.Position=press.Position+utils::Position2d{1,1}; scene->OnAction(move); auto queuedRestore=Access::BorrowMute(*scene); ASSERT_TRUE(queuedRestore); ASSERT_GT(queuedRestore->Version,applied->Version);
        ASSERT_TRUE(Access::MarqueeActive(*scene)); ASSERT_TRUE(station->RetainedMuted()); // Restore is queued, not yet audible.
        ExecuteReset(scene,removeVsts,job); EXPECT_FALSE(Access::MarqueeActive(*scene)); EXPECT_FALSE(Access::BackgroundOwned(*scene));
        EXPECT_FALSE(station->RetainedMuted()); EXPECT_FALSE(Access::BorrowMute(*scene)); EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
        auto fresh=AddCompletedSourceChoiceTake(station); scene->CommitChanges(); ASSERT_TRUE(Access::GlobalMuteCommand(*scene,true));
        auto newer=Access::BorrowMute(*scene); ASSERT_TRUE(newer); ASSERT_GT(newer->Version,queuedRestore->Version);
        press.State=actions::TouchAction::TOUCH_UP; press.MouseButtonsDown=0u; EXPECT_TRUE(scene->OnAction(press).IsEaten);
        ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_TRUE(fresh->IsMuted()); EXPECT_EQ(newer->Version,Access::MuteReceipt(*scene)); EXPECT_FALSE(station->RetainedMuted());
        scene->OnInputFocusLost(); EXPECT_FALSE(Access::MarqueeActive(*scene)); EXPECT_TRUE(fresh->IsMuted());
        applied.reset(); queuedRestore.reset(); newer.reset();
        scene->AdvanceUiAnimations(); // After old-UP checks, resume fresh input through normal reset presentation.
        press.State=actions::TouchAction::TOUCH_DOWN; press.MouseButtonsDown=2u; scene->SetHover3d({},base::Action::MODIFIER_NONE); scene->OnAction(press);
        ASSERT_TRUE(Access::BackgroundOwned(*scene)); ASSERT_TRUE(Access::MarqueeActive(*scene));
        press.State=actions::TouchAction::TOUCH_UP; press.MouseButtonsDown=0u; scene->OnAction(press);
        ASSERT_TRUE(Access::ApplyMuteBoundary(*scene)); EXPECT_FALSE(fresh->IsMuted());
        job.unlock(); scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, BothResetModesInvalidatePreparedAndActivatedBeforeReceiptMasterWithArmedSource)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    for(const bool removeVsts:{false,true}) for(const bool activated:{false,true})
    {
        SCOPED_TRACE(removeVsts);
        SCOPED_TRACE(activated);
        auto scene=FreshScene({Station("Master reset")},{Trigger("source","Master reset")}); ASSERT_TRUE(scene);
        resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
        auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front(); auto target=AddCompletedSourceChoiceTake(station,true,false); scene->CommitChanges();
        auto trigger=scene->AcceptedRigSnapshot()->Triggers.front().Instance;
        Access::WhileSceneLocked(*scene,[&] { ASSERT_TRUE(trigger->BindNextOverdubSource(station,target)); });
        Access::SetDepth(*scene,engine::Scene::VIEW_LOOPTAKE); const auto point=Access::CanvasPoint(*scene);
        scene->SetHover3d(SourceChoicePickPath(target),base::Action::MODIFIER_SHIFT,point);
        actions::TouchAction touch; touch.Touch=actions::TouchAction::TOUCH_MOUSE; touch.Index=0; touch.Position=point;
        touch.Modifiers=base::Action::MODIFIER_SHIFT; touch.MouseButtonsDown=1u; touch.State=actions::TouchAction::TOUCH_DOWN;
        ASSERT_TRUE(scene->OnAction(touch).IsEaten); ASSERT_TRUE(Access::MasterPending(*scene));
        const auto version=Access::MasterVersion(*scene); ASSERT_GT(version,0u); Access::PrepareMasterOnly(*scene);
        if(activated) { Access::ActivateMasterAudioOnly(*scene); ASSERT_EQ(version,Access::ActivatedMasterVersion(*scene)); }
        else ASSERT_LT(Access::ActivatedMasterVersion(*scene),version);
        ASSERT_TRUE(Access::MasterPending(*scene)); // Owner receipt has not run, even when audio activation won.
        ExecuteReset(scene,removeVsts,job); ASSERT_TRUE(Access::ControlIdle(*scene));
        EXPECT_FALSE(Access::MasterPending(*scene)); EXPECT_FALSE(Access::ChosenMaster(*scene)); EXPECT_EQ(0ul,Access::ClockLength(*scene));
        EXPECT_TRUE(station->GetLoopTakeSnapshot().empty()); EXPECT_FALSE(trigger->HasExplicitNextOverdubSource()); EXPECT_TRUE(trigger->GetTakes().empty());
        Access::ActivateMasterAudioOnly(*scene); // Actual next boundary cannot adopt the old prepared target after drain.
        EXPECT_EQ(0ul,Access::ClockLength(*scene)); EXPECT_FALSE(Access::ChosenMaster(*scene));
        touch.State=actions::TouchAction::TOUCH_UP; touch.MouseButtonsDown=0u; touch.Modifiers=base::Action::MODIFIER_NONE; scene->OnAction(touch);
        EXPECT_EQ(0ul,Access::ClockLength(*scene));
        EXPECT_EQ(engine::Quantiser::MasterOutcome::Rejected,Access::AssignMaster(*scene,target).Outcome); // Externally pinned discarded source is no member.
        auto fresh=AddCompletedSourceChoiceTake(station,true,false); scene->CommitChanges();
        ASSERT_EQ(engine::Quantiser::MasterOutcome::Applying,Access::AssignMaster(*scene,fresh).Outcome); EXPECT_GT(Access::MasterVersion(*scene),version);
        Access::ActivateMaster(*scene); ASSERT_TRUE(Access::ChosenMaster(*scene)); EXPECT_EQ(fresh->Id(),Access::ChosenMaster(*scene)->TakeId);
        job.unlock(); scene->Shutdown();
    }
}

TEST_F(SceneRoutingIntegrationTest, BothResetModesCancelActualAltEdgePreviewAndDropUndoPinsBeforeLateInput)
{
    using Access=engine::SourceLossIntegrationTestAccess;
    for(const bool removeVsts:{false,true})
    {
        SCOPED_TRACE(removeVsts);
        auto scene=FreshScene({Station("Edge reset")},{}); ASSERT_TRUE(scene);
        resources::ResourceLib resources; engine::PressPickWindow window(*scene,resources); window.Resize({1280u,900u}); Settle(scene);
        auto job=Access::ExcludeJob(*scene); auto station=scene->SnapshotStations().front(); station->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed);
        auto take=station->AddTake(); take->SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed); take->Record({},station->Name(),{0u},{"source"}); take->Play(0u,100u,0u);
        auto grid=take->ResolvedMidiQuantisation(); grid.Enabled=true; grid.GrainSamps=10u; grid.Fraction=midi::MidiQuantisationFraction::Whole; take->SetMidiQuantisation(grid);
        auto loop=take->GetMidiLoops().front(); const std::array events{midi::MidiEvent::MakeNoteOn(40u,0u,60u,80u),midi::MidiEvent::MakeNoteOff(60u,0u,60u)};
        loop->ReplaceRecordedEvents(events.data(),events.size(),100u); ASSERT_TRUE(loop->SetQuantisation(take->ResolvedMidiQuantisation(),take->MidiQuantisationTransportStartSamps()));
        scene->CommitChanges(); ASSERT_TRUE(loop->UpdateModelFromEvents(100u,true)); take->Select();
        auto& editor=Access::Editor(*scene); ASSERT_TRUE(editor.Open(take,nullptr,loop)); Access::AdvanceEditor(*scene); ASSERT_TRUE(editor.IsReady()); loop->Model()->SetEditorPitchRange(48,24);
        const auto edge=midi::LoopGridEditorTestAccess::MidiPoint(editor,0.60,60),drag=midi::LoopGridEditorTestAccess::MidiPoint(editor,0.67,60); ASSERT_TRUE(edge); ASSERT_TRUE(drag);
        ASSERT_TRUE(midi::LoopGridEditorTestAccess::HasMidiEdgeAt(editor,*edge)); Access::ExpectUncoveredRackPoint(*scene,*edge);
        auto calls=std::make_shared<unsigned int>(0u); auto undo=std::make_shared<ResetPinnedTakeUndo>(take,calls); std::weak_ptr<ResetPinnedTakeUndo> weakUndo=undo;
        Access::Workspace(*scene).Undo().Add(undo); undo.reset(); const auto revision=loop->Revision();
        actions::KeyAction alt; alt.KeyChar=VK_MENU; alt.KeyActionType=actions::KeyAction::KEY_DOWN; alt.Modifiers=base::Action::MODIFIER_ALT; scene->OnAction(alt);
        actions::TouchAction touch; touch.Touch=actions::TouchAction::TOUCH_MOUSE; touch.Index=0; touch.Position=*edge; touch.Modifiers=base::Action::MODIFIER_ALT;
        touch.MouseButtonsDown=1u; touch.State=actions::TouchAction::TOUCH_DOWN; ASSERT_TRUE(scene->OnAction(touch).IsEaten); ASSERT_TRUE(editor.OwnsPointer());
        actions::TouchMoveAction move; move.Touch=actions::TouchAction::TOUCH_MOUSE; move.MouseButtonsDown=1u; move.Modifiers=base::Action::MODIFIER_ALT; move.Position=*drag; scene->OnAction(move); editor.Tick(0.05f);
        ASSERT_TRUE(editor.OwnsPointer()); EXPECT_EQ(revision,loop->Revision());
        ExecuteReset(scene,removeVsts,job); ASSERT_TRUE(Access::ControlIdle(*scene));
        EXPECT_FALSE(editor.IsEngaged()); EXPECT_FALSE(editor.OwnsPointer()); EXPECT_EQ(0u,midi::LoopGridEditorTestAccess::RevisionCursorCount(editor)); EXPECT_TRUE(weakUndo.expired());
        EXPECT_TRUE(station->GetLoopTakeSnapshot().empty()); EXPECT_FALSE(Access::Workspace(*scene).Undo().Undo()); EXPECT_FALSE(Access::Workspace(*scene).Undo().Redo()); EXPECT_EQ(0u,*calls);
        touch.Position=*drag; touch.State=actions::TouchAction::TOUCH_UP; touch.MouseButtonsDown=0u; scene->OnAction(touch);
        alt.KeyActionType=actions::KeyAction::KEY_UP; alt.Modifiers=base::Action::MODIFIER_NONE; scene->OnAction(alt);
        EXPECT_EQ(revision,loop->Revision()); midi::MidiLoop::EditState accepted; ASSERT_TRUE(loop->SnapshotForEdit(accepted));
        ASSERT_EQ(2u,accepted.EventCount); EXPECT_EQ(40u,accepted.Events[0].sampleOffset); EXPECT_EQ(60u,accepted.Events[1].sampleOffset);
        auto fresh=AddCompletedSourceChoiceTake(station,true,false); fresh->Select(); scene->CommitChanges();
        ASSERT_TRUE(editor.Open(fresh,nullptr,fresh->GetMidiLoops().front())); EXPECT_EQ(1u,midi::LoopGridEditorTestAccess::RevisionCursorCount(editor));
        editor.Close(); job.unlock(); scene->Shutdown();
    }
}
