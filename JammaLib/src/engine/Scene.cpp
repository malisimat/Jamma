// Scene wires job/UI concerns and off-callback presentation; it does not reconstruct
// NINJAM timing authority or mutate Timer/loop timing at the audio boundary.
#include "Scene.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "glm/ext.hpp"
#include "../utils/PathUtils.h"
#include "../utils/MathUtils.h"
#include "../midi/MidiTimestampMapper.h"
#include "../io/IoSessionExporter.h"
#include "../vst/Vst3Plugin.h"

using namespace engine;
using namespace base;
using namespace actions;
using namespace audio;
using namespace gui;
using namespace io;
using namespace midi;
using namespace graphics;
using namespace resources;
using namespace utils;
using namespace vst;
using namespace ninjam;
using namespace std::placeholders;

Scene::Scene(SceneParams params,
	UserConfig user) :
	Drawable(params),
	Moveable(params),
	Sizeable(params),
	_isSceneTouching(false),
	_isSceneQuitting(false),
	_isSceneReset(true),
	_viewProj(glm::mat4()),
	_overlayViewProj(glm::mat4()),
	_skyboxViewProj(glm::mat4()),
	_skyboxStarted(false),
	_skyboxStartTime(Timer::GetZero()),
	_label(nullptr),
	_selector(nullptr),
	_modeRadio(nullptr),
	_midiChannelOverrideInput(nullptr),
	_transportOffsetInput(nullptr),
	_ninjamMetronomeToggle(nullptr),
	_globalMidiQuantRadio(nullptr),
	_globalMidiQuantState(io::JamFile::GlobalMidiQuantState::Off),
	_transportOffsetLoopFrac(0.0),
	_mainPanel(nullptr),
	_quantisation(),
	_loggingConfig{},
	_stations(),
	_touchDownElement(std::weak_ptr<GuiElement>()),
	_hoverElement3d(std::weak_ptr<GuiElement>()),
	_hoverPath3d(),
	_hoverPath2d(),
	_hover2dDirty(true),
	_lastLoggedHoverPath(),
	_ctrlHandleOverlay(),
	_quantisationInteraction(_ctrlHandleOverlay, _quantisation, _stations),
	_cursorPos{},
	_camera(CameraParams(
		MoveableParams(
			Position2d{ 0,0 },
			Position3d{ 0, 0, 420 },
			1.0),
		0)),
	_userConfig(user),
	_viewMode(VIEW_STATION),
	_audioEngine(std::make_unique<audio::AudioHost>(user)),
	_inputSubsystem(std::make_unique<io::IoInputSubsystem>(user, io::LoggingConfig{})),
	_windowSubsystem(std::make_unique<vst::VstEditorWindowManager>()),
	_networkService(std::make_unique<ninjam::NinjamNetworkService>()),
	_loopEditor(midi::LoopGridEditor::Host{
		_camera, _undoHistory, _sceneMutex, _stations,
		[this]() { return _hoverElement3d.lock(); },
		[this](float aspect) { return _camera.Projection(aspect, _StationCentre(_stations)) * _camera.ViewMatrix(); },
		[this]() { _OnLoopGridEditorOpened(); },
		[this](int button, utils::Position2d anchor) { return _beginRelativePointer && _beginRelativePointer(button, anchor); },
		[this](int button) { if (_endRelativePointer) _endRelativePointer(button); } },
		params.Size)
{
	_quantisation.SetClock(std::make_shared<Timer>());
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);

	GuiLabelParams labelParams;
	const std::string versionText = "Jamma v" LIB_VERSION;
	labelParams.String = versionText;
	labelParams.Ellipsize = true;
	labelParams.ClipText = true;
	labelParams.VerticalAlign = GuiTextVerticalAlign::Center;
	labelParams.Position = { (int)params.Size.Width - 220, (int)params.Size.Height - 28 };
	labelParams.ModelPosition = { (float)(int)params.Size.Width - 220.0f, (float)(int)params.Size.Height - 28.0f, 0.0f };
	labelParams.Size = { 220, 24 };
	_label = std::make_unique<GuiLabel>(labelParams);


	GuiHudParams hudParams;
	hudParams.Size = params.Size;
	hudParams.MinSize = params.Size;
	hudParams.PopupManager = &_popupManager;
	hudParams.RoutingEditAvailabilityState = [this]() { return _RoutingEditAvailability(); };
	hudParams.AcceptTriggerInput = [this](std::uint64_t revision)
	{
		return _inputSubsystem && _inputSubsystem->TryAcceptUiRigTriggerInput(revision);
	};
	hudParams.SubmitRigEdit = [this](const io::RigFile& candidate)
	{
		return RequestRigEdit(candidate) == RigCoordinator::EditResult::Pending;
	};
	_hudPanel = std::make_shared<GuiHud>(hudParams);
	AddChild(_hudPanel);

	_EnsureRemoteTempoPromptUi();

	GuiSelectorParams selectorParams;
	selectorParams.Position = { 10, 2 };
	selectorParams.Size = params.Size;
	_selector = std::make_unique<SceneSelector>(selectorParams);
	_selector->SetSelectDepth(base::DEPTH_STATION);

	GuiRadioParams modeRadioParams;
	modeRadioParams.Index = 100u;
	modeRadioParams.Size = { 384, 64 };
	std::vector<GuiToggleParams> radioToggleParams;

	for (auto i = 0u; i < 3; i++)
	{
		GuiToggleParams toggleParams;
		toggleParams.Position = { (int)i * 128, 0 };
		toggleParams.Size = { 128, 64 };

		switch (i)
		{
		case 0:
			toggleParams.Texture = "stationmode";
			toggleParams.OverTexture = "stationmode_over";
			toggleParams.DownTexture = "stationmode_down";
			toggleParams.ToggledTexture = "stationmode_toggled";
			toggleParams.ToggledOverTexture = "stationmode_over";
			toggleParams.ToggledDownTexture = "stationmode_down";
			break;
		case 1:
			toggleParams.Texture = "takemode";
			toggleParams.OverTexture = "takemode_over";
			toggleParams.DownTexture = "takemode_down";
			toggleParams.ToggledTexture = "takemode_toggled";
			toggleParams.ToggledOverTexture = "takemode_over";
			toggleParams.ToggledDownTexture = "takemode_down";
			break;
		case 2:
			toggleParams.Texture = "loopmode";
			toggleParams.OverTexture = "loopmode_over";
			toggleParams.DownTexture = "loopmode_down";
			toggleParams.ToggledTexture = "loopmode_toggled";
			toggleParams.ToggledOverTexture = "loopmode_over";
			toggleParams.ToggledDownTexture = "loopmode_down";
			break;
		}

		radioToggleParams.push_back(toggleParams);
	}
	
	modeRadioParams.ToggleParams = radioToggleParams;
	_modeRadio = std::make_shared<GuiRadio>(modeRadioParams);

	GuiNumericInputParams midiChannelOverrideParams = GuiNumericInputParams::PanelInput(72u);
	midiChannelOverrideParams.Index = MidiChannelOverrideControlIndex;
	midiChannelOverrideParams.Size = { 80, GuiNumericInputParams::DefaultHeight };
	midiChannelOverrideParams.Min = 0.0;
	midiChannelOverrideParams.Max = 16.0;
	midiChannelOverrideParams.Step = 0.1;
	midiChannelOverrideParams.Decimals = 0;
	midiChannelOverrideParams.InitValue = static_cast<double>(_inputSubsystem->ForcedChannelOverride());
	_midiChannelOverrideInput = std::make_shared<GuiNumericInput>(midiChannelOverrideParams);


	GuiNumericInputParams transportOffsetParams = GuiNumericInputParams::PanelInput(88u);
	transportOffsetParams.Index = TransportOffsetControlIndex;
	transportOffsetParams.Size = { 96, GuiNumericInputParams::DefaultHeight };
	transportOffsetParams.Min = -1.0;
	transportOffsetParams.Max = 1.0;
	transportOffsetParams.Step = 0.005;
	transportOffsetParams.Decimals = 3;
	transportOffsetParams.InitValue = _transportOffsetLoopFrac;
	_transportOffsetInput = std::make_shared<GuiNumericInput>(transportOffsetParams);


	GuiToggleParams metronomeToggleParams = GuiToggleParams::PanelPrimary();
	metronomeToggleParams.Index = NinjamMetronomeControlIndex;
	metronomeToggleParams.ToggleIndex = NinjamMetronomeControlIndex;
	metronomeToggleParams.Text = "Metronome";
	metronomeToggleParams.Size = { 120, GuiToggleParams::DefaultHeight };
	metronomeToggleParams.MinSize = { 120, GuiToggleParams::DefaultHeight };
	metronomeToggleParams.InitState = GuiToggleParams::TOGGLE_ON;
	_ninjamMetronomeToggle = std::make_shared<GuiToggle>(metronomeToggleParams);


	GuiRadioParams globalMidiQuantRadioParams;
	globalMidiQuantRadioParams.Index = 101u;
	globalMidiQuantRadioParams.InitValue = static_cast<unsigned int>(_globalMidiQuantState);
	globalMidiQuantRadioParams.Size = { 228, 40 };

	std::vector<GuiToggleParams> globalQuantToggleParams;
	for (auto i = 0u; i < 3; ++i)
	{
		GuiToggleParams toggleParams;
		toggleParams.Text = i == 0 ? "Off" : i == 1 ? "Mixed" : "All";
		toggleParams.TextureShader = "texture_tinted";
		toggleParams.Position = { static_cast<int>(i * 76), 0 };
		toggleParams.Size = { 72, 40 };
		toggleParams.Texture = "rounded_but";
		toggleParams.OverTexture = "rounded_but_over";
		toggleParams.DownTexture = "rounded_but_down";
		toggleParams.ToggledTexture = "rounded_but_on";
		toggleParams.ToggledOverTexture = "rounded_but_on_over";
		toggleParams.ToggledDownTexture = "rounded_but_on_down";
		switch (i)
		{
		case 0:
			toggleParams.TintColor = glm::vec3(1.0f, 0.34f, 0.30f);
			break;
		case 1:
			toggleParams.TintColor = glm::vec3(1.0f, 0.75f, 0.20f);
			break;
		case 2:
		default:
			toggleParams.TintColor = glm::vec3(0.33f, 0.92f, 0.45f);
			break;
		}

		globalQuantToggleParams.push_back(toggleParams);
	}
	globalMidiQuantRadioParams.ToggleParams = globalQuantToggleParams;
	_globalMidiQuantRadio = std::make_shared<GuiRadio>(globalMidiQuantRadioParams);

	auto tapParams = GuiButtonParams::PanelButton(180u);
	tapParams.Index = TapTempoControlIndex;
	tapParams.Text = "Tap tempo (Space)";
	auto tapButton = std::make_shared<GuiButton>(tapParams);
	GuiRadioParams subdivisionParams;
	subdivisionParams.Index = MidiSubdivisionControlIndex;
	subdivisionParams.InitValue = midi::MidiQuantisation::FractionDisplayIndex(midi::MidiQuantisationFraction::Quarter);
	subdivisionParams.Size = { 450u, 40u };
	for (int index = 0; index < midi::MidiQuantisationFractionCount; ++index)
	{
		auto toggle = GuiToggleParams::PanelPrimary();
		toggle.Text = midi::MidiQuantisation::FractionLabel(midi::MidiQuantisation::ClampFractionDisplayIndex(index));
		toggle.Position = { index * 50, 0 };
		toggle.Size = { 48u, 40u };
		subdivisionParams.ToggleParams.push_back(toggle);
	}
	_midiSubdivisionRadio = std::make_shared<GuiRadio>(subdivisionParams);

	GuiMainPanelParams mainParams;
	mainParams.Size = params.Size;
	mainParams.PopupManager = &_popupManager;
	mainParams.BeforeHide = [this](const auto& subtree) { _OnSettingsHidden(subtree); };
	mainParams.Settings = {
		{ SettingsPage::Timing, "Global MIDI quantisation", _globalMidiQuantRadio, 101u },
		{ SettingsPage::Timing, "Local phase offset (loops)", _transportOffsetInput, TransportOffsetControlIndex },
		{ SettingsPage::Timing, "Subdivision: local grain / remote beat", _midiSubdivisionRadio, MidiSubdivisionControlIndex },
		{ SettingsPage::Timing, "", tapButton, TapTempoControlIndex },
		{ SettingsPage::Midi, "MIDI channel (0 = unchanged)", _midiChannelOverrideInput, MidiChannelOverrideControlIndex },
		{ SettingsPage::Timing, "", _ninjamMetronomeToggle, NinjamMetronomeControlIndex }
	};
	_mainPanel = std::make_shared<GuiMainPanel>(mainParams);
	AddChild(_mainPanel);
	mainParams.SelectionOnly = true;
	mainParams.Settings = { { SettingsPage::Selection, "", _modeRadio, 100u } };
	_selectionPanel = std::make_shared<GuiMainPanel>(mainParams);
	AddChild(_selectionPanel);

	_PublishAudioStations();

	_jobRunner = std::thread([this, onStart = params.OnJobThreadStart]() {
		if (onStart) onStart();
		this->_JobLoop();
	});
}

void Scene::ConnectNinjam(const std::string& host)
{
	ConnectNinjam(host, _networkService->TempoJoinOptions());
}

void Scene::ConnectNinjam(const std::string& host,
	const ninjam::NinjamTempoJoinOptions& options)
{
	const auto localTiming = _quantisation.CurrentTempoTiming(_CurrentSampleRate());
	++_ninjamJoinGeneration;
	if (options.PushLocalTempoOnJoin)
		++_ninjamTempoRequestId;
	{
		std::scoped_lock lock(_sceneMutex);
		_networkService->SetTempoJoinOptions(options);
		_ApplyNinjamTimingUpdate(_networkService->PrepareTempoSyncOnConnect(localTiming));
		_CloseRemoteTempoPrompt();
	}
	if (_loggingConfig.Event == "verbose")
	{
		std::cout << "[NINJAM][TimingPolicy] changed policy=no-sync reason=reconnect"
			<< " join=" << _ninjamJoinGeneration << '\n';
		std::cout << "[NINJAM][TempoJoin] connect join=" << _ninjamJoinGeneration
			<< " request=" << (options.PushLocalTempoOnJoin ? _ninjamTempoRequestId : 0u)
			<< " pushLocal=" << options.PushLocalTempoOnJoin;
		if (localTiming.has_value())
			std::cout << " bpm=" << localTiming->Bpm << " bpi=" << localTiming->SeedCount
				<< " interval=" << localTiming->MasterLoopSamps << " grain=" << localTiming->SeedSamps;
		std::cout << '\n';
	}
	_lastLoggedTempoRequestState = ninjam::TempoRequestState::Idle;
	_LogNinjamTempoJoinState();
	_networkService->Connect(host);
}

void Scene::DisconnectNinjam()
{
	{
		std::scoped_lock lock(_sceneMutex);
		_CloseRemoteTempoPrompt();
		_ApplyNinjamTimingUpdate(_networkService->ResetTempoSyncOnDisconnect());
	}
	if (_loggingConfig.Event == "verbose")
		std::cout << "[NINJAM][TimingPolicy] changed policy=no-sync reason=disconnect\n";
	_networkService->Disconnect();
}

void Scene::_EnsureRemoteTempoPromptUi()
{
	if (_remoteTempoDialog)
		return;

	_remoteTempoDialog = std::make_shared<GuiPopup>(GuiPopupParams::PanelDefault());
	_remoteTempoDialog->SetTitle("Current server tempo");
	_remoteTempoDialog->ConfigureButtons({ {
		{ "Cancel", NinjamRemoteTempoRejectControlIndex },
		{ "Follow server", NinjamRemoteTempoAcceptControlIndex }
	} });
	_remoteTempoDialog->Init();
}

void Scene::_HandleRemoteTempoSnapshot(const ninjam::NinjamRemoteSnapshot& snapshot,
	const std::optional<engine::QuantisationTiming>& localTiming,
	bool hasLocalContent)
{
	auto previous = _networkService->PendingRemoteTempoPrompt();
	const auto liveTiming = _audioEngine->LatestNinjamTiming();
	NinjamTiming timing = liveTiming.value_or(ToDeviceTiming(snapshot.Timing, true,
		_CurrentSampleRate(), 0u, 0ul, 0u, 0u, 0u));
	if (auto clock = _quantisation.Clock())
		_ApplyNinjamTimingUpdate(_networkService->ObserveTiming(timing,
			localTiming, hasLocalContent, _userConfig, *clock));
	auto current = _networkService->PendingRemoteTempoPrompt();

	if (_remoteTempoDialogOpen
		&& ((!current.has_value())
			|| !previous.has_value()
			|| !current->HasSameProposalIdentity(previous.value())))
	{
		_CloseRemoteTempoPrompt();
	}
}

void Scene::_OpenRemoteTempoPromptIfNeeded()
{
	const auto pendingChange = _networkService->PendingRemoteTempoPrompt();
	if (_remoteTempoDialogOpen || !pendingChange.has_value())
		return;

	_EnsureRemoteTempoPromptUi();
	if (!_remoteTempoDialog)
		return;

	const auto& change = pendingChange.value();
	std::ostringstream bpmStream;
	bpmStream.setf(std::ios::fixed, std::ios::floatfield);
	bpmStream << std::setprecision(1) << change.Bpm;

	_remoteTempoDialog->SetBodyLines({
		"Tempo: " + bpmStream.str() + " BPM, " + std::to_string(change.Bpi) + " BPI",
		"Remote master interval: " + std::to_string(change.RemoteMasterIntervalLengthSamps) + " samples",
		"Remote grid step: " + std::to_string(change.RemoteGridStepSamps) + " samples. Apply locally?"
	});
	_remoteTempoDialog->FitToViewport(_sizeParams.Size);

	_popupManager.Open(_remoteTempoDialog);
	_remoteTempoDialogOpen = true;
}

void Scene::_HandleRemoteTempoPromptDecision(bool accept)
{
	std::scoped_lock lock(_sceneMutex);
	if (auto clock = _quantisation.Clock())
		_ApplyNinjamTimingUpdate(_networkService->ResolveRemoteTempoPromptDecision(accept,
			_quantisation.CurrentTempoTiming(_CurrentSampleRate()), *clock));
	_CloseRemoteTempoPrompt();
}

void Scene::_ApplyNinjamTimingUpdate(const ninjam::NinjamTimingUpdate& update)
{
	if (update.DesiredTransport.has_value())
	{
		const auto& desired = update.DesiredTransport.value();
		if (_loggingConfig.Event == "verbose" && desired.HasRemoteTiming)
		{
			std::cout << "[NINJAM][TimingPolicy] determined policy="
				<< ninjam::NinjamTimingCoordinator::FollowPolicyName(desired.LocalFollowPolicy)
				<< " remoteBpm=" << desired.TempoBpm
				<< " generation=" << desired.Generation
				<< " remotePhaseDeviceSample=" << desired.RemotePhaseDeviceSample << '\n';
		}
		if (update.RemoteGrid.has_value())
			_quantisation.SetRemoteMidiGrid(update.RemoteGrid->Geometry,
				update.RemoteGrid->OriginSamps, _stations);
		else if (desired.Intent == ninjam::NinjamDesiredTimingIntent::NoSync)
			_quantisation.SetRemoteMidiGrid({}, 0, _stations);
		if (_audioEngine)
			_audioEngine->PublishDesiredTiming(desired);
		if (_loggingConfig.Event == "verbose"
			&& desired.Intent == ninjam::NinjamDesiredTimingIntent::NoSync
			&& update.NoSyncReason == ninjam::NinjamNoSyncReason::StayLocal)
		{
			std::cout << "[NINJAM][TimingPolicy] determined policy=no-sync reason=stay-local\n";
		}
	}

	if (update.TempoRequest.has_value())
		_networkService->SendTempoRequest(update.TempoRequest.value());
	_LogNinjamTempoJoinState();
}

void Scene::_LogNinjamTempoJoinState()
{
	if (_loggingConfig.Event != "verbose")
		return;
	const auto state = _networkService->TempoJoinRequestState();
	if (state == _lastLoggedTempoRequestState)
		return;

	_lastLoggedTempoRequestState = state;
	const char* name = "idle";
	switch (state)
	{
	case ninjam::TempoRequestState::Queued: name = "queued"; break;
	case ninjam::TempoRequestState::SentAwaitingOutcome: name = "awaiting-server-observation"; break;
	case ninjam::TempoRequestState::Acknowledged: name = "acknowledged"; break;
	case ninjam::TempoRequestState::Expired: name = "expired-unknown"; break;
	default: break;
	}
	const auto diagnostics = _networkService->TimingDiagnostics();
	std::cout << "[NINJAM][TempoJoin] state join=" << _ninjamJoinGeneration
		<< " request=" << _ninjamTempoRequestId
		<< " value=" << name
		<< " sent=" << diagnostics.TempoRequestsSent
		<< " retries=" << diagnostics.TempoRequestRetries
		<< " acknowledged=" << diagnostics.TempoAcknowledged
		<< " expired=" << diagnostics.TempoRequestsExpired << '\n';
}

void Scene::_LogNinjamTimingDiagnostics(const ninjam::NinjamTimingDiagnostics& diagnostics)
{
	if (_loggingConfig.Event != "verbose")
		return;

	const auto present = [this](const ninjam::NinjamTimingDiagnosticEvent& event)
	{
		std::cout << "[NINJAM][TimingDiagnostic] reason="
			<< ninjam::NinjamTimingCoordinator::DiagnosticReasonName(event.Reason)
			<< " sessionEpoch=" << event.SessionEpoch
			<< " appliedSessionEpoch=" << event.AppliedSessionEpoch
			<< " desiredVersion=" << event.DesiredVersion
			<< " appliedVersion=" << event.AppliedVersion
			<< " generation=" << event.Generation
			<< " value=" << event.ValueSamps
			<< " limit=" << event.LimitSamps
			<< " occurrences=" << event.OccurrenceCount
			<< " cumulativeSuppressed=" << event.CumulativeSuppressedCount << '\n';
		_lastPresentedNinjamDiagnosticSequence = event.Sequence;
	};

	for (auto eventIndex = 0u; eventIndex < diagnostics.CapturedEventCount; ++eventIndex)
	{
		const auto& event = diagnostics.Events[eventIndex];
		if (event.Sequence > _lastPresentedNinjamDiagnosticSequence)
			present(event);
	}
	if (diagnostics.LatestEvent.Sequence > _lastPresentedNinjamDiagnosticSequence)
		present(diagnostics.LatestEvent);
	if (diagnostics.EventOverflowSummaryCount > _lastPresentedNinjamDiagnosticOverflowCount)
	{
		std::cout << "[NINJAM][TimingDiagnostic] overflowSummary="
			<< diagnostics.EventOverflowSummaryCount
			<< " overflowTotal=" << diagnostics.EventOverflowCount << '\n';
		_lastPresentedNinjamDiagnosticOverflowCount = diagnostics.EventOverflowSummaryCount;
	}
}

void Scene::_CloseRemoteTempoPrompt()
{
	if (_remoteTempoDialogOpen)
	{
		if (_popupManager.Top() == _remoteTempoDialog)
			_popupManager.Close();
		_remoteTempoDialogOpen = false;
	}
}

std::optional<std::shared_ptr<Scene>> Scene::FromFile(SceneParams sceneParams,
	io::JamFile jamStruct,
	io::RigFile rigStruct,
	std::wstring dir,
	std::function<bool(const io::RigFile&)> saveRig)
{
	std::cout << "[LOAD] Constructing scene for JAM '" << jamStruct.Name << "' with "
		<< jamStruct.Stations.size() << " station descriptor(s) and " << rigStruct.Triggers.size()
		<< " rig trigger(s)." << std::endl;
	auto scene = std::make_shared<Scene>(sceneParams, rigStruct.User);
	scene->_saveRig = std::move(saveRig);

	unsigned int hudAudioInputCount = std::max(1u, rigStruct.User.Audio.NumChannelsIn);
	for (const auto& triggerCfg : rigStruct.Triggers)
	{
		for (const auto channel : triggerCfg.InputChannels)
			hudAudioInputCount = std::max(hudAudioInputCount, channel + 1u);
	}

	std::vector<std::string> hudMidiInputs;
	hudMidiInputs.reserve(rigStruct.User.Midi.Devices.size());
	for (const auto& device : rigStruct.User.Midi.Devices)
	{
		if (device.Enabled && !device.Name.empty())
			hudMidiInputs.push_back(device.Name);
	}

	TriggerParams trigParams;
	trigParams.DebounceMs = rigStruct.User.Trigger.DebounceSamps;

	StationParams stationParams;
	stationParams.Index = 0;
	stationParams.Position = { 20, 20 };
	stationParams.ModelPosition = { -50, -20 };
	stationParams.Size = { 136, 300 };
	stationParams.FadeSamps = rigStruct.User.Loop.FadeSamps > constants::MaxLoopFadeSamps ?
		constants::MaxLoopFadeSamps :
		rigStruct.User.Loop.FadeSamps;

	MergeMixBehaviourParams mergeParams;
	AudioMixerParams mixerParams = Station::GetMixerParams(stationParams.Size, mergeParams);
	std::vector<std::shared_ptr<Station>> initialStations;
	initialStations.reserve(jamStruct.Stations.size());
	std::vector<io::JamFile::Station> initialStationDescriptors;
	initialStationDescriptors.reserve(jamStruct.Stations.size());

	size_t stationDescriptorIndex = 0u;
	for (auto& stationStruct : jamStruct.Stations)
	{
		auto station = Station::FromFile(stationParams, mixerParams, stationStruct, dir);
		if (station.has_value())
		{
			if (stationStruct.AllowedMidiChannels.empty())
			{
				const auto defaultChannel = static_cast<int>((stationParams.Index % 16u) + 1u);
				station.value()->SetAllowedMidiChannels({ defaultChannel });
			}

			initialStations.push_back(station.value());
			initialStationDescriptors.push_back(stationStruct);
			stationParams.Index++;
			stationParams.Position += { 600, 0 };
			stationParams.ModelPosition += { 600, 0 };
		}
		else
			std::cout << "[LOAD] Station descriptor " << stationDescriptorIndex << " '" << stationStruct.Name
				<< "' was not constructed; continuing with the remaining stations." << std::endl;
		stationDescriptorIndex++;
	}
	std::cout << "[LOAD] Constructed " << initialStations.size() << " of " << jamStruct.Stations.size()
		<< " station descriptor(s)." << std::endl;
	if (!scene->_rigCoordinator.BuildInitial(rigStruct,
		initialStationDescriptors,
		initialStations,
		rigStruct.User.Audio.NumChannelsIn,
		hudMidiInputs,
		trigParams,
		scene->_saveRig))
	{
		std::cout << "[LOAD] Rig construction failed after station reconstruction." << std::endl;
		return std::nullopt;
	}
	const auto acceptedRig = scene->_rigCoordinator.Accepted();
	if (!acceptedRig)
		return std::nullopt;
	for (const auto& trigger : acceptedRig->Graph.Triggers)
	{
		const auto target = trigger.TargetName.value_or("<legacy target>");
		if (trigger.Reason == io::RigFileRouting::Warning::TargetMissing)
			std::cout << "[LOAD] Warning: trigger '" << trigger.TriggerName << "' target '" << target
				<< "' was not found; leaving it unconnected." << std::endl;
		else if (trigger.Reason == io::RigFileRouting::Warning::TargetAmbiguous)
			std::cout << "[LOAD] Warning: trigger '" << trigger.TriggerName << "' target '" << target
				<< "' is ambiguous; leaving it unconnected." << std::endl;
	}
	if (initialStations.empty())
	{
		std::cout << "Load: no constructible stations" << std::endl;
		return std::nullopt;
	}
	for (const auto& runtime : acceptedRig->Triggers)
	{
		if (!runtime.Instance)
			continue;
		auto saved = std::find_if(jamStruct.TriggerHistories.begin(), jamStruct.TriggerHistories.end(),
			[&runtime](const io::JamFile::TriggerHistory& history) { return history.TriggerId == runtime.Id; });
		if (saved == jamStruct.TriggerHistories.end())
			continue;
		std::vector<TriggerTake> takes;
		takes.reserve(saved->Takes.size());
		for (const auto& take : saved->Takes)
			takes.push_back({ static_cast<decltype(TriggerTake{}.SourceType)>(take.SourceType), take.SourceTakeId, take.TargetTakeId });
		runtime.Instance->RestoreTakes(std::move(takes));
	}
	for (auto& station : initialStations)
		scene->_AddStation(std::move(station), false);
	scene->_PublishAudioStations();
	scene->_audioEngine->PublishPendingRigSnapshot(acceptedRig);
	scene->_inputSubsystem->PublishRigInputDispatch(acceptedRig);

	if (scene->_hudPanel)
		scene->_hudPanel->SetRoutingConfig(hudAudioInputCount, std::move(hudMidiInputs), *acceptedRig);
	scene->_inputSubsystem->OpenRigTriggerInput(acceptedRig->Revision);

	if (!jamStruct.TransportInitialised)
	{
		// No local geometry was saved. The first completed recording seeds the
		// clock from its physical length under the active user timing policy.
		scene->_quantisation.Clear(false);
	}
	else
	{
		scene->_SetQuantisation(jamStruct.QuantiseSamps, jamStruct.Quantisation);
		if (jamStruct.Version == io::JamFile::VERSION_V)
		{
			auto clock = scene->_quantisation.Clock();
			if (!clock || jamStruct.MasterLengthSamps == 0ul)
			{
				std::cout << "Load: invalid local transport state" << std::endl;
				return std::nullopt;
			}
			clock->SetSeedSourceLength(jamStruct.MasterLengthSamps);
			if (!clock->InitialiseAbsoluteSamplePos(jamStruct.AbsoluteSamplePos))
			{
				std::cout << "Load: invalid local transport state" << std::endl;
				return std::nullopt;
			}
		}
	}
	scene->_quantisation.SetGlobalPhaseOffsetSamps(jamStruct.GlobalPhaseOffsetSamps, scene->_stations);
	scene->_SetGlobalMidiQuantState(jamStruct.GlobalMidiQuantStateValue, true);
	scene->_SetTransportOffsetLoopFrac(jamStruct.TransportOffsetLoopFrac);
	// Saved sessions always start locally.  NINJAM config/anchors are live
	// connection state and are intentionally never restored from a .jam.
	scene->InitReceivers();

	return scene;
}

void Scene::Draw(DrawContext& ctx)
{
	_loopEditor.UpdateUi(_viewProj);
	std::scoped_lock lock(_sceneMutex);
	const bool midiEditorEngaged = _loopEditor.IsEngaged() && _loopEditor.TargetMidiLoop();
	_UpdateRackVisibilityLocked();
	if (_hudPanel)
	{
		_hudPanel->SetLoopEditorMode(midiEditorEngaged);
		_hudPanel->SetQuantisationFeedback(_quantisationFeedback);
		_hudPanel->SetEditModeAlpha(_quantisationInteraction.PanelAlpha());
	}

	glDisable(GL_DEPTH_TEST);

	// Draw overlays
	auto &glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glCtx.ClearMvp();
	glCtx.PushMvp(_overlayViewProj);

	if (_midiChannelOverrideInput && !_midiChannelOverrideInput->HasFocus())
	{
		const auto forcedChannel = static_cast<double>(_inputSubsystem->ForcedChannelOverride());
		if (_midiChannelOverrideInput->Value() != forcedChannel)
			_midiChannelOverrideInput->SetValue(forcedChannel, false);
	}



	if (_hudPanel)
	{
		const auto streamParams = _audioEngine->GetStreamParams();
		const auto numSamps = std::max(1u, streamParams.BufSize);
		for (auto channel = 0u; channel < streamParams.NumInputChannels; ++channel)
			_hudPanel->SetAudioInputPeak(channel, _audioEngine->GetAdcPeak(channel), numSamps);

		unsigned int midiInput = 0u;
		for (const auto& deviceName : _midiState.load(std::memory_order_acquire)->ConnectedNames)
		{
			_hudPanel->SetMidiInputPeak(midiInput++,
				_inputSubsystem->ConsumeMidiInputPeak(deviceName),
				numSamps);
		}
	}

	if (!midiEditorEngaged)
	{
		auto opacity = ctx.WithOpacity(1.0f - _quantisationInteraction.PanelAlpha());
		for (auto& station : _stations)
			station->Draw(ctx);
	}

	_selector->Draw(ctx);
	if (!midiEditorEngaged)
		_ctrlHandleOverlay.Draw(ctx);


	{
		auto opacity = ctx.WithOpacity(1.0f - _quantisationInteraction.PanelAlpha());
		for (auto& child : _guiChildren)
			if (child && child != _mainPanel && child != _selectionPanel && child != _hudPanel)
				child->Draw(ctx);
	}
	if (_hudPanel) _hudPanel->Draw(ctx);
	const int statusWidth = GuiStyle::StatusBarWidth(static_cast<int>(_sizeParams.Size.Width));
	const int versionWidth = GuiStyle::VersionColumnWidth(statusWidth);
	const int statusHeight = std::min(GuiStyle::StatusBarHeight, static_cast<int>(_sizeParams.Size.Height));
	_label->SetPosition({ static_cast<int>(_sizeParams.Size.Width) - versionWidth, std::max(0, (statusHeight - 22) / 2) });
	_label->SetSize({ static_cast<unsigned int>(versionWidth), 22u });
	if (versionWidth > 0 && statusHeight >= 22) _label->Draw(ctx);
	if (!_popupManager.IsOpen()) _loopEditor.Draw(ctx);
	{
		auto opacity = ctx.WithOpacity(1.0f - _quantisationInteraction.PanelAlpha());
		_mainPanel->Draw(ctx);
		_selectionPanel->Draw(ctx);
	}
	_popupManager.Draw(ctx);

	glCtx.PopMvp();
}

void Scene::DrawBackground(DrawContext& ctx)
{
	std::scoped_lock lock(_sceneMutex);
	const auto ar = _sizeParams.Size.Height > 0
		? static_cast<float>(_sizeParams.Size.Width) / _sizeParams.Size.Height : 1.0f;
	const auto view = _camera.ViewMatrix();
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	glEnable(GL_DEPTH_TEST);
	if (!_skyboxStarted)
	{
		_skyboxStartTime = Timer::GetTime();
		_skyboxStarted = true;
	}
	{
		auto t = (float)Timer::GetElapsedSeconds(_skyboxStartTime, Timer::GetTime());
		auto yaw   = glm::radians(2.0f * std::sin(0.047f * t) + 1.5f * std::sin(0.031f * t + 1.1f));
		auto pitch = glm::radians(1.5f * std::sin(0.053f * t + 2.3f) + 1.0f * std::sin(0.019f * t + 0.7f));
		auto roll  = glm::radians(0.8f * std::sin(0.037f * t + 1.8f));
		auto R = glm::rotate(glm::mat4(1.0f), yaw,   glm::vec3(0.0f, 1.0f, 0.0f));
		R = glm::rotate(R, pitch, glm::vec3(1.0f, 0.0f, 0.0f));
		R = glm::rotate(R, roll,  glm::vec3(0.0f, 0.0f, 1.0f));
		auto skyboxProjection = _camera.SkyboxProjection(ar);
		_skyboxViewProj = skyboxProjection * glm::mat4(glm::mat3(view)) * R;
	}

	glCtx.ClearMvp();
	glCtx.PushMvp(_skyboxViewProj);
	_skybox.Draw(glCtx);
	glCtx.PopMvp();

}

void Scene::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	std::scoped_lock lock(_sceneMutex);

	auto ar = _sizeParams.Size.Height > 0 ?
		(float)_sizeParams.Size.Width / (float)_sizeParams.Size.Height :
		1.0f;
	auto projection = _camera.Projection(ar, _StationCentre(_stations));
	auto view = _camera.ViewMatrix();
	_viewProj = projection * view;
	_UpdateHudStationAnchors();

	if (PASS_SCENE == pass)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);

	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);


	glCtx.ClearMvp();
	glCtx.PushMvp(projection);
	glCtx.PushMvp(view);
	if (PASS_PICKER == pass && _loopEditor.IsEngaged())
	{
		glCtx.PopMvp();
		glCtx.PopMvp();
		return;
	}

	if (PASS_SCENE == pass)
	{
		const auto now = Timer::GetTime();
		_ApplyQuantisationOverlayAlpha(_QuantisationOverlayAlpha(now));
		_quantisationInteraction.Tick(now);
	}

	glCtx.SetUniform("SelectionActive", PASS_SCENE == pass && _HasSelection() ? 1.0f : 0.0f);
	glCtx.SetUniform("SceneDim", _loopEditor.SurroundingDim());
	glCtx.SetUniform("EditorMorph", 0.0f);
	glCtx.SetUniform("EditorTime", _skyboxStarted
		? static_cast<float>(Timer::GetElapsedSeconds(_skyboxStartTime, Timer::GetTime())) : 0.0f);
	const auto probeId = PASS_SCENE == pass && _loopEditor.IsEngaged()
		? _skybox.CubemapId() : 0u;
	// Active cubemap and material samplers must use distinct units even when
	// editor lighting is disabled; otherwise GL rejects the draw's sampler types.
	glCtx.SetUniform("ProbeSampler", 3);
	glCtx.SetUniform("ProbeStrength", probeId != 0u ? 1.0f : 0.0f);
	if (probeId != 0u)
	{
		glCtx.SetUniform("EditorProbeEye", _loopEditor.ProbeEyeLocal());
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_CUBE_MAP, probeId);
		glActiveTexture(GL_TEXTURE0);
	}
	_loopEditor.ApplyToModels();
	for (auto& station : _stations)
	{
		station->Draw3d(ctx, 1, pass);
	}
	glCtx.SetUniform("SelectionActive", 0.0f);
	glCtx.SetUniform("SceneDim", 1.0f);
	if (probeId != 0u)
	{
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
		glActiveTexture(GL_TEXTURE0);
	}

	glCtx.PopMvp();
	glCtx.PopMvp();
}

void Scene::UpdateCamera()
{
	// Keep camera state on the window thread; audio never reads or advances it.
	const auto now = Timer::GetTime();
	const auto deltaSeconds = _lastCameraUpdateTime
		? std::clamp(static_cast<float>(Timer::GetElapsedSeconds(*_lastCameraUpdateTime, now)), 0.0f, 0.05f)
		: 0.0f;
	_lastCameraUpdateTime = now;

	{
		std::scoped_lock lock(_sceneMutex);
		for (size_t index = 0u; index < _stations.size(); ++index)
			_camera.ObserveStation(index, _stations[index], _stations[index]->LoopTakeRevision(), _stations[index]->ModelPosition());
		_camera.CompleteStationObservation(_stations.size(), _stations.empty()
			? std::optional<Position3d>{}
			: std::optional<Position3d>{ _stations.front()->ModelPosition() });
	}

	if (_camera.IsBackgroundDragging() || _camera.IsTransitioning())
		_camera.TickBackgroundDrag(deltaSeconds);
	_loopEditor.Tick(deltaSeconds);
	if (!_loopEditor.IsEngaged())
		_ApplyCameraSelectDepthChange(_camera.PendingSelectDepthChange());
	if (_isSceneTouching && !_camera.IsBackgroundDragging())
		_EndBackgroundDrag();
}

void Scene::AdvanceUiAnimations()
{
	if (_quantisationInputResetRequested.exchange(false, std::memory_order_acq_rel))
		_ReleaseQuantisationInput();
	// Window calls once per UI frame before deferred hover and drawing.
	// This clock is independent of the transport/audio callback.
	const auto now = Timer::GetTime();
	const float elapsed = _lastPanelAnimationTime
		? static_cast<float>(Timer::GetElapsedSeconds(*_lastPanelAnimationTime, now)) : 0.0f;
	_lastPanelAnimationTime = now;
	const bool mainChanged = _mainPanel && _mainPanel->AdvanceAnimation(elapsed);
	const bool selectionChanged = _selectionPanel && _selectionPanel->AdvanceAnimation(elapsed);
	if (mainChanged || selectionChanged) _InvalidateHover2d();
	std::scoped_lock lock(_sceneMutex);
	_UpdateRackVisibilityLocked();
}

void Scene::_InitResources(ResourceLib& resourceLib, bool forceInit)
{
	std::scoped_lock lock(_sceneMutex);

	_skybox.InitResources(resourceLib, forceInit);
	_label->InitResources(resourceLib, forceInit);
	_loopEditor.InitResources(resourceLib, forceInit);
	_selector->InitResources(resourceLib, forceInit);
	for (auto& child : _guiChildren)
		if (child)
			child->InitResources(resourceLib, forceInit);
	if (_remoteTempoDialog)
		_remoteTempoDialog->InitResources(resourceLib, forceInit);
	_ctrlHandleOverlay.InitResources(resourceLib, forceInit);

	for (auto& station : _stations)
		station->InitResources(resourceLib, forceInit);

	_InitSize();

	ResourceUser::_InitResources(resourceLib, forceInit);
}

void Scene::_ReleaseResources()
{
	_skybox.ReleaseResources();
	_label->ReleaseResources();
	_loopEditor.ReleaseResources();
	_selector->ReleaseResources();
	for (auto& child : _guiChildren)
		if (child)
			child->ReleaseResources();
	if (_remoteTempoDialog)
		_remoteTempoDialog->ReleaseResources();
	_ctrlHandleOverlay.ReleaseResources();

	for (auto& station : SnapshotStations())
		station->ReleaseResources();

	Drawable::_ReleaseResources();
}

void Scene::_OnSettingsHidden(const std::shared_ptr<GuiElement>& subtree)
{
	auto belongs = [&subtree](std::shared_ptr<GuiElement> element)
	{
		for (; element; element = element->Parent())
			if (element == subtree) return true;
		return false;
	};
	if (belongs(_focusManager.CurrentFocus())) _focusManager.ClearFocus();
	if (belongs(_touchDownElement.lock()))
	{
		_touchDownElement.reset();
		_touchDownIsHud = false;
		_touchDownIsSettings = false;
		_consumeSettingsRelease = true;
	}
	_InvalidateHover2d();
}

std::optional<ActionResult> Scene::_RouteSettingsTouch(TouchAction action)
{
	if (action.State == TouchAction::TOUCH_UP && _consumeSettingsRelease)
	{
		_consumeSettingsRelease = false;
		return ActionResult{ true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_touchDownIsSettings)
	{
		auto active = _touchDownElement.lock();
		auto result = active ? active->OnAction(active->GlobalToLocal(action)) : ActionResult::NoAction();
		if (action.State == TouchAction::TOUCH_UP)
		{
			_touchDownElement.reset();
			_touchDownIsSettings = false;
			_consumeSettingsRelease = false;
		}
		result.IsEaten = true;
		return result;
	}
	// A gesture already owned elsewhere keeps its release and move stream.
	if (_touchDownElement.lock() || _camera.IsBackgroundDragging() || _isSceneTouching ||
		_loopEditor.OwnsPointer() || _loopEditor.IsOrbitDragging() || _quantisationInteraction.OwnsPointer())
		return std::nullopt;
	for (const auto& panel : { _selectionPanel, _mainPanel })
	{
		if (!panel || !panel->RouteHitTest(panel->GlobalToLocal(action.Position))) continue;
		auto result = panel->OnAction(panel->GlobalToLocal(action));
		result.IsEaten = true;
		if (action.State == TouchAction::TOUCH_DOWN &&
			!(action.Touch == TouchAction::TOUCH_MOUSE && action.Index == 4))
		{
			auto active = result.ActiveElement.lock();
			if (!active) active = panel;
			_touchDownElement = active;
			_touchDownIsSettings = true;
			_touchDownIsHud = false;
			auto focused = _focusManager.CurrentFocus();
			if (focused && focused != active) focused->FinalizeEdits();
			if (active->WantsFocusOnPress()) _focusManager.RequestFocus(active);
			else _focusManager.ClearFocus();
		}
		return result;
	}
	return std::nullopt;
}

ActionResult Scene::OnAction(TouchAction action)
{
	ActionResult res;
	// Wheel events are encoded as DOWN/index 4 and have no matching UP. They
	// must not become pointer captures or replace keyboard focus.
	const bool pointerPress = action.State == TouchAction::TOUCH_DOWN &&
		!(action.Touch == TouchAction::TOUCH_MOUSE && action.Index == 4);
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	_cursorPos = action.Position;
	_InvalidateHover2d();
	if (_popupManager.IsOpen())
	{
		_loopEditor.CancelInput();
		auto popupRes = _popupManager.OnAction(action);
		if (_remoteTempoDialogOpen && !_popupManager.IsOpen())
			_HandleRemoteTempoPromptDecision(false);
		if (TouchAction::TOUCH_UP == action.State)
		{
			_touchDownElement.reset();
			_touchDownIsHud = false;
			_touchDownIsSettings = false;
			_consumeSettingsRelease = false;
		}
		return popupRes;
	}
	if (_quantisationInteraction.OwnsPointer() || (_EditControlsSuppressed() && !_loopEditor.IsEngaged()))
	{
		if (auto overlay = _RouteQuantisationTouch(action))
			return *overlay;
		return ActionResult::NoAction();
	}
	if (!_EditControlsSuppressed())
		if (auto settingsResult = _RouteSettingsTouch(action)) return *settingsResult;
	if (!_EditControlsSuppressed() && _loopEditor.IsEngaged() && !_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() && _hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		auto hudResult = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		if (hudResult.IsEaten) return hudResult;
	}
	if (auto editorRes = _loopEditor.OnAction(action))
		return *editorRes;
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
	{
		// Clear the old hover now so only the capture target appears pressed.
		std::vector<std::weak_ptr<GuiElement>> none;
		_ApplyHoverPath2d(none);
		_hoverPath2d.clear();
	}

	std::cout << "Touch action " << action.Touch << " [State " << action.State << "] Index " << action.Index << "(Modifiers " << action.Modifiers << ")" << std::endl;

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State)
		&& (0 == action.Index)
		&& (Action::MODIFIER_SHIFT & action.Modifiers)
		&& !(Action::MODIFIER_CTRL & action.Modifiers)
		&& _TrySetMasterFromHover(true))
	{
		res.IsEaten = true;
		res.ResultType = ACTIONRESULT_DEFAULT;
		return res;
	}

	if (auto overlayRes = _RouteQuantisationTouch(action);
		overlayRes.has_value())
	{
		return overlayRes.value();
	}

	if (TouchAction::TouchState::TOUCH_UP == action.State)
	{
		auto activeElement = _touchDownElement.lock();

		if (activeElement)
		{
			if (_touchDownIsHud)
			{
				std::scoped_lock lock(_sceneMutex);
				res = activeElement->OnAction(activeElement->GlobalToLocal(action));
			}
			else
				res = activeElement->OnAction(activeElement->GlobalToLocal(action));

			if (res.IsEaten)
			{
				if (nullptr != res.Undo)
					_undoHistory.Add(res.Undo);
			}
		}
		else if (_camera.IsBackgroundDragging())
		{
			auto wasDragged = _camera.BackgroundDragWasDragged();
			_camera.HandleBackgroundDrag(action);
			_EndBackgroundDrag();

			// Clear selection after an unhandled background drag.
			if (!wasDragged && !_camera.IsBackgroundDragging())
				_UpdateSelection(ACTIONRESULT_CLEARSELECT);
		}

		// Update selector and then react to result (selecting, deselecting, muting, unmuting)
		res = _selector->OnAction(_selector->ParentToLocal(action));

		_UpdateSelection(res.ResultType);

		_touchDownElement.reset();
		_touchDownIsHud = false;

		return ActionResult::NoAction();
	}

	for (auto it = _guiChildren.rbegin(); it != _guiChildren.rend(); ++it)
	{
		if (!*it)
			continue;

		const auto& child = *it;
		const auto isHudChild = (child == _hudPanel);
		if (isHudChild)
		{
			std::scoped_lock lock(_sceneMutex);
			res = child->OnAction(child->ParentToLocal(action));
		}
		else
			res = child->OnAction(child->ParentToLocal(action));
		if (res.IsEaten)
		{
			if (nullptr != res.Undo)
				_undoHistory.Add(res.Undo);

			if (pointerPress && !_touchDownElement.lock())
			{
				_touchDownElement = res.ActiveElement;
				_touchDownIsHud = isHudChild;
			}

			// Focus follows the pressed control when it wants the keyboard.
			if (pointerPress)
			{
				auto active = res.ActiveElement.lock();
				if (active && active->WantsFocusOnPress())
					_focusManager.RequestFocus(active);
				else
					_focusManager.ClearFocus();
			}

			return res;
		}
	}

	for (auto& station : SnapshotStations())
	{
		res = static_cast<std::shared_ptr<base::GuiElement>>(station)->OnAction(station->ParentToLocal(action));

		if (res.IsEaten)
		{
			if (nullptr != res.Undo)
				_undoHistory.Add(res.Undo);

			if (pointerPress && !_touchDownElement.lock())
			{
				_touchDownElement = res.ActiveElement;
				_touchDownIsHud = false;
			}

			return res;
		}
	}

	res = _selector->OnAction(_selector->ParentToLocal(action));

	_UpdateSelection(res.ResultType);

	if (res.IsEaten)
	{
		if (nullptr != res.Undo)
			_undoHistory.Add(res.Undo);

		return res;
	}

	if ((TouchAction::TouchState::TOUCH_DOWN == action.State) && (4 == action.Index))
		return _camera.HandleWheel(action.Value,
			action.Position,
			_sizeParams.Size.Width,
			_sizeParams.Size.Height,
			_StationCentre(SnapshotStations()));

	// Pressing empty background clears keyboard focus.
	if (TouchAction::TouchState::TOUCH_DOWN == action.State)
		_focusManager.ClearFocus();

	return _BeginBackgroundDrag(action);
}

ActionResult Scene::OnAction(TouchMoveAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	_cursorPos = action.Position;
	_InvalidateHover2d();
	if (_quantisationInteraction.OwnsPointer())
	{
		if (action.MouseButtonsDown == 0u)
			_quantisationInteraction.CancelInteraction();
		else if (auto overlay = _RouteQuantisationMove(action))
			return *overlay;
		return ActionResult::NoAction();
	}
	if (_EditControlsSuppressed() && !_loopEditor.IsEngaged())
		return ActionResult::NoAction();
	if (_popupManager.IsOpen())
	{
		_loopEditor.CancelInput();
		return _popupManager.OnAction(action);
	}
	if (_consumeSettingsRelease && action.MouseButtonsDown == 0u)
	{
		_consumeSettingsRelease = false;
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (_touchDownIsSettings)
	{
		if (auto active = _touchDownElement.lock())
		{
			if (action.MouseButtonsDown == 0u) active->ClearPointerState();
			else active->OnAction(active->GlobalToLocal(action));
		}
		if (action.MouseButtonsDown == 0u)
		{
			_touchDownElement.reset();
			_touchDownIsSettings = false;
			_consumeSettingsRelease = false;
			_InvalidateHover2d();
		}
		return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
	}
	if (!_EditControlsSuppressed() && !_touchDownElement.lock() && !_camera.IsBackgroundDragging() && !_isSceneTouching &&
		!_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() && !_quantisationInteraction.OwnsPointer())
	{
		for (const auto& panel : { _selectionPanel, _mainPanel })
			if (panel && panel->RouteHitTest(panel->GlobalToLocal(action.Position)))
			{
				panel->OnAction(panel->GlobalToLocal(action));
				return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
			}
	}
	if (!_EditControlsSuppressed() && _loopEditor.IsEngaged() && !_loopEditor.OwnsPointer() && !_loopEditor.IsOrbitDragging() && _hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		auto hudResult = _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
		if (hudResult.IsEaten) return hudResult;
	}
	if (auto editorRes = _loopEditor.OnAction(action))
		return *editorRes;

	if (auto overlayRes = _RouteQuantisationMove(action);
		overlayRes.has_value())
	{
		return overlayRes.value();
	}

	if (_camera.IsBackgroundDragging() && (0u != action.MouseButtonsDown))
		return _UpdateBackgroundDrag(action);

	auto activeElement = _touchDownElement.lock();

	if (activeElement)
	{
		if (action.Touch == TouchAction::TOUCH_MOUSE && action.MouseButtonsDown == 0u)
		{
			// Native capture loss is reported as a zero-button move. Clear the
			// captured widget and owner together, including HUD captures.
			if (_touchDownIsHud)
			{
				std::scoped_lock lock(_sceneMutex);
				activeElement->ClearPointerState();
			}
			else activeElement->ClearPointerState();
			_touchDownElement.reset();
			_touchDownIsHud = false;
			return { true, {}, {}, ACTIONRESULT_DEFAULT, nullptr, {} };
		}
		if (_touchDownIsHud)
		{
			std::scoped_lock lock(_sceneMutex);
			return activeElement->OnAction(activeElement->GlobalToLocal(action));
		}
		return activeElement->OnAction(activeElement->GlobalToLocal(action));
	}

	if (_isSceneTouching)
		return _UpdateBackgroundDrag(action);
	{
		auto selectionMove = _selector->OnAction(_selector->ParentToLocal(action));
		if (selectionMove.IsEaten)
		{
			_UpdateSelection(selectionMove.ResultType);
			return selectionMove;
		}
	}
	if (_hudPanel)
	{
		// Lock the HUD tree while the job thread rebuilds it.
		std::scoped_lock lock(_sceneMutex);
		return _hudPanel->OnAction(_hudPanel->GlobalToLocal(action));
	}

	return ActionResult::NoAction();
}

ActionResult Scene::OnAction(KeyAction action)
{
	action.SetActionTime(Timer::GetTime());
	action.SetUserConfig(_userConfig);
	action.SetAudioParams(_audioEngine->GetStreamParams());

	// Physical modifier transitions must survive contextual editor consumption.
	if (17u == action.KeyChar)
	{
		_ctrlHeld = actions::KeyAction::KEY_DOWN == action.KeyActionType;
		const bool contextualText = _focusManager.IsEditingText() || _loopEditor.IsEditingText();
		{
			std::scoped_lock lock(_sceneMutex);
			_quantisationInteraction.OnCtrlModifierChanged(_ctrlHeld && !contextualText, action.GetActionTime(),
				_InteractionContextLocked(), [this](const auto& path) { return _ChildFromPathLocked(path); });
		}
		if (_ctrlHeld && !contextualText && !_loopEditor.IsEngaged())
		{
			_focusManager.ClearFocus();
			if (auto active = _touchDownElement.lock()) active->ClearPointerState();
			_touchDownElement.reset();
			_touchDownIsHud = _touchDownIsSettings = false;
			_ApplyHoverPath2d({});
			_InvalidateHover2d();
		}
	}
	if (32u == action.KeyChar)
	{
		if (actions::KeyAction::KEY_UP == action.KeyActionType)
		{
			_spaceHeld = false;
			_SetQuantisationOverlayHeld(false);
		}
		else if (!_spaceHeld)
		{
			_spaceHeld = true;
			if (!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !_loopEditor.IsEditingText())
			{
				_SetQuantisationOverlayHeld(true);
				_HandleTapTempo(action.GetActionTime());
			}
		}
		// A text consumer still receives Space, but the physical release above
		// always unwinds a previous hold, even after focus changes.
		if (!_popupManager.IsOpen() && !_focusManager.IsEditingText() && !_loopEditor.IsEditingText())
			return ActionResult::NoAction();
	}
	if (27u == action.KeyChar && _quantisationInteraction.OwnsPointer())
	{
		_quantisationInteraction.CancelInteraction();
		return ActionResult::NoAction();
	}

	std::cout << "Key action " << action.KeyActionType << " [" << action.KeyChar << "] IsSytem:" << action.IsSystem << ", Modifiers:" << action.Modifiers << "]" << std::endl;
	if (_popupManager.IsOpen())
	{
		_loopEditor.CancelInput();
		auto popupRes = _popupManager.OnAction(action);
		if (_remoteTempoDialogOpen && !_popupManager.IsOpen())
			_HandleRemoteTempoPromptDecision(false);
		if (_loopEditor.IsEngaged())
			popupRes.IsEaten = true;
		if (popupRes.IsEaten)
			return popupRes;
	}

	if ((192u == action.KeyChar) || (96u == action.KeyChar))
	{
		if (_hudPanel)
		{
			std::scoped_lock lock(_sceneMutex);
			_hudPanel->SetCableRevealHeld(actions::KeyAction::KEY_DOWN == action.KeyActionType);
		}
		return ActionResult::NoAction();
	}
	if (_hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		if (_hudPanel->HasCableDrag())
		{
			auto hudResult = _hudPanel->OnAction(action);
			if (hudResult.IsEaten)
				return hudResult;
		}
	}

	if (auto editorRes = _loopEditor.OnAction(action))
		return *editorRes;

	if (auto overrideRes = _inputSubsystem->HandleChannelOverrideKey(action, SnapshotStations());
		overrideRes.IsEaten)
	{
		if (_midiChannelOverrideInput)
		{
			_midiChannelOverrideInput->SetValue(
				static_cast<double>(_inputSubsystem->ForcedChannelOverride()),
				false);
		}
		return overrideRes;
	}

	// 2. The focused control gets first refusal. While it is editing text we
	//    swallow the key entirely so global shortcuts don't fire mid-edit.
	if (auto focus = _focusManager.CurrentFocus())
	{
		auto focusRes = focus->OnAction(action);
		if (focusRes.IsEaten)
			return focusRes;
		if (_focusManager.IsEditingText())
			return ActionResult::NoAction();
	}
	if (auto editorRes = _loopEditor.TryOpenFromKey(action))
		return *editorRes;

	if ((9u == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_NONE == action.Modifiers))
	{
		_CycleCameraView();
		ActionResult result;
		result.IsEaten = true;
		return result;
	}

	if ((82 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		_HandleReclockArm();
		return ActionResult::NoAction();
	}

	if ((90 == action.KeyChar) && (actions::KeyAction::KEY_UP == action.KeyActionType) && (Action::MODIFIER_CTRL & action.Modifiers))
	{
		return _HandleUndo();
	}

	// Ctrl+Shift+V - insert a VST on the hovered station/take/loop.
	if ((86 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		const auto pluginPath = utils::PickFile(L"Choose VST plugin");
		if (pluginPath.empty())
			return ActionResult::NoAction();

		auto hovering = _ChildFromPath(_selector->CurrentHover());
		return _windowSubsystem->HandleVstInsert(pluginPath,
			_selector->CurrentSelectDepth(),
			hovering,
			[this]() { CommitChanges(); });
	}

	// Ctrl+Shift+E - open the first plugin editor for the hovered station/take/loop.
	if ((69 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers)
		&& (Action::MODIFIER_SHIFT & action.Modifiers))
	{
		auto hovering = _ChildFromPath(_selector->CurrentHover());
		return _windowSubsystem->HandleVstEditorOpen(hovering,
			_selector->CurrentSelectDepth(),
			SnapshotStations());
	}

	// Ctrl+S - export session to directory.
	if ((83 == action.KeyChar)
		&& (actions::KeyAction::KEY_UP == action.KeyActionType)
		&& (Action::MODIFIER_CTRL & action.Modifiers))
	{
		return io::IoSessionExporter::ExportSession(_stations,
			AcceptedRigSnapshot(),
			_quantisation,
			_globalMidiQuantState,
			_transportOffsetLoopFrac,
			_userConfig,
			_audioEngine->GetStreamParams(),
			_audioEngine->GetDevice(),
			_sceneMutex,
			_networkService->GetController());
	}

	// Insert + Ctrl+Shift+L/W/X/[/] - MIDI automation record, learn, wire, delete, lane cycle.
	{
		auto hovered = _ChildFromPath(_selector->CurrentHover());
		auto hoveredTake = std::dynamic_pointer_cast<LoopTake>(hovered);
		auto automationRes = _inputSubsystem->HandleAutomationKey(action,
			SnapshotStations(),
			_selector->CurrentHover(),
			hoveredTake);
		if (automationRes.IsEaten)
			return automationRes;
	}

	bool checkReset = false;
	auto result = ActionResult::NoAction();

	const auto acceptedRig = _rigCoordinator.Accepted();
	if (acceptedRig && _inputSubsystem->TryAcceptUiRigTriggerInput(acceptedRig->Revision))
	{
		static const std::string EmptyDevice;
		const auto keyState = action.KeyActionType == KeyAction::KEY_DOWN ? 1u : 0u;
		const auto& keyboardTriggers = acceptedRig->InputDispatch.KeyboardTriggers;
		for (const auto& trigger : keyboardTriggers)
		{
			if (!trigger)
				continue;
			auto res = trigger->QueueInputEvent(TRIGGER_INPUT_UI,
				acceptedRig->Revision, TriggerSource::TRIGGER_KEY,
				action.KeyChar, keyState, action, EmptyDevice);

			if (!res.IsEaten)
				continue;

		std::cout << "KeyAction eaten: " << res.SourceId << ", " << res.TargetId << ", " << res.ResultType << std::endl;
		switch (res.ResultType)
		{
		case ACTIONRESULT_ACTIVATE:
			_isSceneReset.store(false, std::memory_order_relaxed);
			checkReset = true;
			// Propagate any grain the clock just acquired (e.g. from first-loop seed).
			// _SetQuantisation is not called by Station's TrySeedClockFromFirstLoop, so
			// do it here after every activation so all LoopTakes see the current grain.
			if (auto clock = _quantisation.Clock())
			{
				std::scoped_lock lock(_sceneMutex);
				_SetMidiQuantisationGrain(clock->QuantiseSamps(), "loop activated");
			}
			break;
		case ACTIONRESULT_DITCH:
			checkReset = true;
			break;
		default:
			break;
		}

			if (!result.IsEaten || (res.ResultType != ACTIONRESULT_DEFAULT))
				result = res;
		}
	}

	if (checkReset)
	{
		std::scoped_lock lock(_sceneMutex);
		_ResetIfEmpty();
	}

	if (result.IsEaten)
		return result;

	auto res = _selector->OnAction(action);

	if (res.IsEaten)
	{
		std::cout << "KeyAction eaten by selector: " << res.ResultType << std::endl;
		_UpdateSelection(res.ResultType);
		return res;
	}

	return ActionResult::NoAction();
}

void Scene::_HandleReclockArm()
{
	std::scoped_lock lock(_sceneMutex);
	if (_quantisation.ActiveGrid().Source == QuantisationGridSource::Remote)
	{
		std::cout << "[quantisation] reclock rejected: remote authority; choose Stay local first" << std::endl;
		return;
	}
	std::cout << ">> Reclock armed (Ctrl+Shift+R) <<" << std::endl;
	_quantisation.ArmReclock(_stations);
	_quantisation.SetRemoteMidiGrid({}, 0, _stations);
	_quantisation.SetMidiGrain(0u, "reclock arm", _stations);
	for (const auto& station : _stations)
		if (station && !station->IsRemote())
			for (const auto& take : station->GetLoopTakes()) take->SetMidiBaseGrid(0u, 0u);
}

ActionResult Scene::_HandleUndo()
{
	std::cout << ">> Undo <<" << std::endl;
	auto res = _undoHistory.Undo();
	return { res };
}

ActionResult Scene::OnAction(GuiAction action)
{
	if (GuiAction::ACTIONELEMENT_BUTTON == action.ElementType && action.Index == TapTempoControlIndex)
	{
		_PulseQuantisationOverlay();
		_HandleTapTempo(Timer::GetTime());
		return ActionResult::NoAction();
	}
	if (GuiAction::ACTIONELEMENT_RADIO == action.ElementType && action.Index == MidiSubdivisionControlIndex)
	{
		if (auto value = std::get_if<GuiAction::GuiInt>(&action.Data))
		{
			const auto fraction = midi::MidiQuantisation::ClampFractionDisplayIndex(value->Value);
			for (const auto& station : SnapshotStations())
				if (station && !station->IsRemote())
					for (const auto& take : station->GetLoopTakes())
					{
						auto settings = take->MidiQuantisation();
						settings.Fraction = fraction;
						take->SetMidiQuantisation(settings);
					}
		}
		return ActionResult::NoAction();
	}
	if ((GuiAction::ACTIONELEMENT_BUTTON == action.ElementType)
		&& (action.Index == NinjamRemoteTempoAcceptControlIndex))
	{
		_HandleRemoteTempoPromptDecision(true);
		return ActionResult::NoAction();
	}

	if ((GuiAction::ACTIONELEMENT_BUTTON == action.ElementType)
		&& (action.Index == NinjamRemoteTempoRejectControlIndex))
	{
		_HandleRemoteTempoPromptDecision(false);
		return ActionResult::NoAction();
	}

	if (GuiAction::ACTIONELEMENT_MIDIQUANTISATION == action.ElementType)
	{
		_ForceGlobalMidiQuantStateMixedOnLocalEdit();
		return ActionResult::NoAction();
	}

	if (GuiAction::ACTIONELEMENT_RADIO == action.ElementType)
	{
		if (auto i = std::get_if<GuiAction::GuiInt>(&action.Data))
		{
			if (action.Index == 100u)
			{
				auto viewMode = static_cast<unsigned int>(std::clamp(i->Value,
					static_cast<int>(VIEW_STATION), static_cast<int>(VIEW_LOOP)));
				if (_camera.SelectDepthChanged(VIEW_STATION == viewMode))
				{
					viewMode = VIEW_LOOPTAKE;
					_modeRadio->SetCurrentValue(viewMode, true);
				}
				_viewMode = static_cast<ViewMode>(viewMode);
				_UpdateSelectDepth((unsigned int)_viewMode);
			}
			else if (action.Index == 101u)
			{
				auto stateValue = i->Value;
				if (stateValue < static_cast<int>(io::JamFile::GlobalMidiQuantState::Off)
					|| stateValue > static_cast<int>(io::JamFile::GlobalMidiQuantState::All))
				{
					stateValue = static_cast<int>(io::JamFile::GlobalMidiQuantState::Mixed);
				}

				_SetGlobalMidiQuantState(static_cast<io::JamFile::GlobalMidiQuantState>(stateValue));
			}
		}
	}

	if ((GuiAction::ACTIONELEMENT_TOGGLE == action.ElementType)
		&& (action.Index == NinjamMetronomeControlIndex))
	{
		if (auto value = std::get_if<GuiAction::GuiInt>(&action.Data))
			_audioEngine->SetNinjamMetronomeEnabled(value->Value == GuiToggleParams::TOGGLE_ON);
	}
	else if ((GuiAction::ACTIONELEMENT_RACK == action.ElementType)
		&& (action.Index == MidiChannelOverrideControlIndex)
		&& _midiChannelOverrideInput)
	{
		int clamped = 0;
		if (auto str = std::get_if<GuiAction::GuiString>(&action.Data))
		{
			try
			{
				size_t consumed = 0;
				const int parsed = std::stoi(str->Value, &consumed);
				clamped = consumed == str->Value.size() ? std::clamp(parsed, 0, 16)
					: static_cast<int>(_inputSubsystem->ForcedChannelOverride());
			}
			catch (...)
			{
				clamped = static_cast<int>(_inputSubsystem->ForcedChannelOverride());
			}
		}
		else if (auto value = std::get_if<GuiAction::GuiDouble>(&action.Data))
		{
			clamped = std::clamp(static_cast<int>(value->Value + 0.5), 0, 16);
		}
		else
		{
			return ActionResult::NoAction();
		}

		_inputSubsystem->SetForcedChannelOverride(static_cast<std::uint8_t>(clamped), SnapshotStations());
		_midiChannelOverrideInput->SynchronizeValueFromOwner(static_cast<double>(clamped));
	}
	else if ((GuiAction::ACTIONELEMENT_RACK == action.ElementType)
		&& (action.Index == TransportOffsetControlIndex)
		&& _transportOffsetInput)
	{
		double value = _transportOffsetLoopFrac;
		if (auto str = std::get_if<GuiAction::GuiString>(&action.Data))
		{
			try
			{
				size_t consumed = 0;
				const double parsed = std::stod(str->Value, &consumed);
				if (consumed == str->Value.size() && std::isfinite(parsed)) value = parsed;
			}
			catch (...)
			{
				value = _transportOffsetLoopFrac;
			}
		}
		else if (auto numeric = std::get_if<GuiAction::GuiDouble>(&action.Data))
		{
			value = numeric->Value;
		}
		else
		{
			return ActionResult::NoAction();
		}

		_SetTransportOffsetLoopFrac(value, false);
		_transportOffsetInput->SynchronizeValueFromOwner(_transportOffsetLoopFrac);
	}

	return ActionResult::NoAction();
}

void Scene::OnTick(Time curTime,
	unsigned int samps,
	const std::optional<io::UserConfig>& cfg,
	const std::optional<audio::AudioStreamParams>& params)
{
	if (auto clock = _quantisation.Clock())
	{
		clock->Tick(samps, 0u);
	}

	const auto stationsSnapshot = _audioEngine->GetStationsSnapshot();
	static const std::vector<std::shared_ptr<Station>> emptyStations;
	const auto& stations = stationsSnapshot ? *stationsSnapshot : emptyStations;

	const auto streamParams = _audioEngine->GetStreamParams();
	for (auto& station : stations)
	{
		station->OnTick(curTime,
			samps,
			_userConfig,
			streamParams);
	}
}

void Scene::OnJobTick(Time curTime)
{
	if (!_isSceneQuitting.load(std::memory_order_acquire))
		_inputSubsystem->AcknowledgeRigTriggerInputCloseFromJob();
	_RefreshMidiIfNeeded();
	_PumpTriggerStructuralActions();
	_AdvanceRigPublication();
	_PumpMidi();
	_PumpSerial();
	_ConsumeTriggerOutcomes();

	auto pumpResult = _networkService->GetController()->Pump();
	{
		// Keep the station clock synced when local content seeds it without NINJAM.
		std::scoped_lock lock(_sceneMutex);
		if (auto clock = _quantisation.Clock(); clock && clock->QuantiseSamps() != _quantisation.EffectiveSamps())
			_SetMidiQuantisationGrain(clock->QuantiseSamps(), "clock geometry changed");
		const auto localTiming = _quantisation.CurrentTempoTiming(_CurrentSampleRate());
		bool hasLocalContent = false;
		for (const auto& station : _stations)
		{
			if (!station || station->IsRemote())
				continue;
			hasLocalContent = !station->GetLoopTakeSnapshot().empty();
			if (hasLocalContent)
				break;
		}
		if (pumpResult.TimingStatus.Changed)
		{
			_ApplyNinjamTimingUpdate(_networkService->ObserveSessionStatus(
				pumpResult.TimingStatus, localTiming));
			if (!pumpResult.TimingStatus.IsAvailable)
				_UpdateRemoteStationsFromSnapshot({});
		}
		if (pumpResult.Snapshot.has_value())
			_HandleRemoteTempoSnapshot(pumpResult.Snapshot.value(), localTiming, hasLocalContent);
		else if (auto clock = _quantisation.Clock())
			_ApplyNinjamTimingUpdate(_networkService->TickTiming(
				localTiming, hasLocalContent, *clock));
		if (_loggingConfig.Event == "verbose" && _audioEngine)
		{
			_LogNinjamTimingDiagnostics(_networkService->ObserveAppliedTimingReceipt(
				_audioEngine->LastAppliedDesiredTiming()));
		}
		_HandleAudioLocalContentState(hasLocalContent);
	}

	actions::JobAction job;
	job.SetActionTime(Timer::GetTime());
	job.SetUserConfig(_userConfig);
	job.SetAudioParams(_audioEngine->GetStreamParams());

	{
		std::scoped_lock lock(_jobMutex);

		if (_jobList.empty())
			return;

		job = _jobList.front();
		_jobList.pop_front();

		// Run only the newest job,
		// removing previous identical jobs
		for (auto j = _jobList.begin(); j != _jobList.end();)
		{
			if (job == *j)
				j = _jobList.erase(j);
			else
				++j;
		}
	}

	auto receiver = job.Receiver.lock();
	if (receiver)
		receiver->OnAction(job);

	// Destroy PreInit'd VST plugins on the UI thread that initialized them.
	if (job.PreInitPlugin)
		vst::QueueForUiThreadDestroy(std::move(job.PreInitPlugin));
}

void Scene::_RefreshMidiIfNeeded()
{
	if (!_midiActive.load(std::memory_order_acquire) ||
		_isSceneQuitting.load(std::memory_order_acquire))
		return;
	const auto now = std::chrono::steady_clock::now();
	const bool requested = _midiRefreshRequested.exchange(false, std::memory_order_acq_rel);
	if (!requested && now - _lastMidiInventoryCheck < std::chrono::seconds(1))
		return;
	_lastMidiInventoryCheck = now;
	const auto inventory = midi::MidiDevice::InventoryInputDevices();
	if (!inventory.Error.empty())
		return; // A transient inventory error must not tear down working ports.
	const auto previous = _midiState.load(std::memory_order_acquire);
	const auto& oldPorts = previous->Connection.Inventory.Devices;
	const bool sameInventory = oldPorts.size() == inventory.Devices.size() &&
		std::equal(oldPorts.begin(), oldPorts.end(), inventory.Devices.begin(),
			[](const auto& a, const auto& b) {
				return a.DeviceId == b.DeviceId && a.PortName == b.PortName;
			});
	if (sameInventory)
	{
		const auto retryable = std::any_of(previous->Connection.Attempts.begin(),
			previous->Connection.Attempts.end(), [](const auto& attempt) {
				return attempt.Status == midi::MidiConnectionStatus::Missing ||
					attempt.Status == midi::MidiConnectionStatus::Failed;
			});
		if (!requested || !retryable)
			return;
	}
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire) ||
		_isSceneQuitting.load(std::memory_order_acquire))
		return;
	auto result = _inputSubsystem->RefreshMidi(_audioEngine->GetMidiClockAnchor_Ref(),
		_audioEngine->GetStreamParams().SampleRate, inventory);
	std::vector<std::string> connectedNames;
	for (const auto& endpoint : result.Connected)
		if (std::find(connectedNames.begin(), connectedNames.end(), endpoint.Name) == connectedNames.end())
			connectedNames.push_back(endpoint.Name);
	_midiState.store(std::make_shared<const MidiState>(MidiState{ std::move(result), connectedNames }),
		std::memory_order_release);
	const auto inputs = _audioEngine->GetStreamParams().NumInputChannels;
	if (const auto runtime = _rigCoordinator.RefreshRuntimeAvailability(inputs, connectedNames))
	{
		_audioEngine->PublishPendingRigSnapshot(runtime);
		_inputSubsystem->PublishRigInputDispatch(runtime);
		_inputSubsystem->OpenRigTriggerInput(runtime->Revision);
		if (_hudPanel)
		{
			std::scoped_lock sceneLock(_sceneMutex);
			_hudPanel->SetRoutingConfig(inputs, connectedNames, *runtime);
		}
	}
}

void Scene::_PumpMidi()
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire)) return;
	auto stations = SnapshotStations();
	_inputSubsystem->PumpMidi(stations, _audioEngine->GetAudioSampleCounter(),
		_audioEngine->GetStreamParams(), _sceneMutex);
}

void Scene::_PumpTriggerStructuralActions()
{
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted)
		return;
	const auto streamParams = _audioEngine->GetStreamParams();
	std::scoped_lock lock(_sceneMutex);
	for (const auto& runtime : accepted->Triggers)
		if (runtime.Instance)
			runtime.Instance->ProcessStructuralActionsOnJob(_userConfig, streamParams);
}

void Scene::_AdvanceRigPublication()
{
	const auto staged = _rigCoordinator.Staged();
	if (staged)
	{
		const auto revision = staged->Revision;
		if (_rigTransitionRequestedRevision != revision)
		{
			const auto accepted = _rigCoordinator.Accepted();
			if (!accepted || !_inputSubsystem->RigTriggerInputReadyForAudioBoundary(accepted->Revision))
				return;
			_audioEngine->RequestRigTriggerTransition(revision, accepted, staged);
			_rigTransitionRequestedRevision = revision;
		}
		if (_audioEngine->RejectedRigRevision() == revision)
		{
			_rigCoordinator.CompleteTransition(revision, false, _saveRig);
			_audioEngine->ClearRigTriggerTransition();
			_rigTransitionRequestedRevision = 0u;
			if (const auto accepted = _rigCoordinator.Accepted())
				_inputSubsystem->OpenRigTriggerInput(accepted->Revision);
			return;
		}
		if (_audioEngine->TransitionReadyRigRevision() != revision)
			return;
		const auto result = _rigCoordinator.CompleteTransition(revision, true, _saveRig);
		_audioEngine->ClearRigTriggerTransition();
		_rigTransitionRequestedRevision = 0u;
		if (result != RigCoordinator::EditResult::Pending)
		{
			if (const auto accepted = _rigCoordinator.Accepted())
				_inputSubsystem->OpenRigTriggerInput(accepted->Revision);
			return;
		}
		_audioEngine->PublishPendingRigSnapshot(_rigCoordinator.Pending());
		return;
	}

	const auto pending = _rigCoordinator.Pending();
	if (!pending)
		return;
	const auto audioRevision = _audioEngine->AppliedRigRevision();
	if (audioRevision != pending->Revision ||
		!_rigCoordinator.ObserveAudioAcknowledgement(audioRevision))
		return;
	if (_rigCoordinator.InputAcknowledgement() != pending->Revision)
	{
		_inputSubsystem->PublishRigInputDispatch(pending);
		if (!_rigCoordinator.AcknowledgeInput(pending->Revision))
			return;
	}
	if (!_rigCoordinator.PromoteAcknowledged())
		return;
	_audioEngine->ReleaseRigSnapshotsBefore(pending->Revision);
	_rigCoordinator.ReleaseRetired();
	if (_hudPanel)
	{
		// Protect the HUD tree while routing rebuilds replace widgets used by rendering.
		std::scoped_lock lock(_sceneMutex);
		unsigned int audioInputs = _audioEngine->GetStreamParams().NumInputChannels;
		const auto midiState = _midiState.load(std::memory_order_acquire);
		_hudPanel->SetRoutingConfig(audioInputs, midiState->ConnectedNames, *pending);
	}
	_inputSubsystem->OpenRigTriggerInput(pending->Revision);
}

RigCoordinator::EditResult Scene::RequestRigEdit(const io::RigFile& candidateRig)
{
	switch (_RoutingEditAvailability())
	{
	case gui::RoutingEditAvailability::Applying: return RigCoordinator::EditResult::EditsDisabled;
	case gui::RoutingEditAvailability::AudioCallbackInactive: return RigCoordinator::EditResult::AudioCallbackInactive;
	case gui::RoutingEditAvailability::TriggerBusy: return RigCoordinator::EditResult::TriggerBusy;
	case gui::RoutingEditAvailability::Ready: break;
	}
	const auto accepted = _rigCoordinator.Accepted();
	const auto result = _rigCoordinator.SubmitCandidate(candidateRig);
	if (result == RigCoordinator::EditResult::Pending)
	{
		const auto staged = _rigCoordinator.Staged();
		if (!accepted || !staged || _audioEngine->AppliedRigRevision() != accepted->Revision)
		{
			if (staged)
				_rigCoordinator.CompleteTransition(staged->Revision, false, _saveRig);
			return RigCoordinator::EditResult::TransitionRejected;
		}
		if (!_inputSubsystem->RequestCloseRigTriggerInputFromUi(accepted->Revision))
		{
			_rigCoordinator.CompleteTransition(staged->Revision, false, _saveRig);
			return RigCoordinator::EditResult::TransitionRejected;
		}
		_rigTransitionRequestedRevision = 0u;
	}
	return result;
}

gui::RoutingEditAvailability Scene::_RoutingEditAvailability()
{
	if (!_rigCoordinator.EditsEnabled())
		return gui::RoutingEditAvailability::Applying;
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted || !_inputSubsystem->TryAcceptUiRigTriggerInput(accepted->Revision))
		return gui::RoutingEditAvailability::Applying;

	const auto heartbeat = _audioEngine->AudioCallbackHeartbeat();
	const auto now = std::chrono::steady_clock::now();
	if (heartbeat != _lastAudioCallbackHeartbeat)
	{
		_lastAudioCallbackHeartbeat = heartbeat;
		_lastAudioCallbackHeartbeatAt = now;
	}
	constexpr auto heartbeatTimeout = std::chrono::milliseconds(500);
	if (heartbeat == 0u || _lastAudioCallbackHeartbeatAt == std::chrono::steady_clock::time_point{} ||
		now - _lastAudioCallbackHeartbeatAt > heartbeatTimeout)
		return gui::RoutingEditAvailability::AudioCallbackInactive;
	return gui::RoutingEditAvailability::Ready;
}

void Scene::_PumpSerial()
{
	std::scoped_lock midiLock(_midiLifecycleMutex);
	if (!_midiActive.load(std::memory_order_acquire)) return;
	_inputSubsystem->PumpSerial(_stations, _audioEngine->GetStreamParams(), _sceneMutex);
}

void Scene::_ConsumeTriggerOutcomes()
{
	const auto accepted = _rigCoordinator.Accepted();
	if (!accepted)
		return;
	for (auto observed = _triggerOutcomeCounts.begin(); observed != _triggerOutcomeCounts.end();)
	{
		const auto stillPublished = std::any_of(accepted->Triggers.begin(), accepted->Triggers.end(),
			[instance = observed->first](const RigSnapshotTrigger& runtime)
			{
				return runtime.Instance.get() == instance;
			});
		if (!stillPublished)
			observed = _triggerOutcomeCounts.erase(observed);
		else
			++observed;
	}
	for (const auto& runtime : accepted->Triggers)
	{
		if (!runtime.Instance)
			continue;
		const auto activation = runtime.Instance->ActivationOutcomeCount();
		const auto ditch = runtime.Instance->DitchOutcomeCount();
		auto& observed = _triggerOutcomeCounts[runtime.Instance.get()];
		if (activation > observed.first)
		{
			_isSceneReset.store(false, std::memory_order_relaxed);
			if (auto clock = _quantisation.Clock())
				_SetMidiQuantisationGrain(clock->QuantiseSamps(), "loop activated");
		}
		if (ditch > observed.second)
			_ResetIfEmpty();
		observed = { activation, ditch };
	}
}

void Scene::InitReceivers()
{
	_selector->SetReceiver(ActionReceiver::shared_from_this());
	_quantisationInteraction.SetFeedbackSink([this](const std::string& text) {
		// UI-owned text; apply to the HUD during the next locked draw.
		_quantisationFeedback = text;
	});
	_mainPanel->SetCommandOwner(ActionReceiver::shared_from_this());
	_selectionPanel->SetCommandOwner(ActionReceiver::shared_from_this());
	if (_remoteTempoDialog)
		_remoteTempoDialog->SetButtonReceiver(ActionReceiver::shared_from_this());
}

void Scene::AddChild(std::shared_ptr<base::GuiElement> child)
{
	if (!child)
		return;

	auto it = std::find(_guiChildren.begin(), _guiChildren.end(), child);
	if (it == _guiChildren.end())
	{
		_guiChildren.push_back(child);
		child->Init();
		_InvalidateHover2d();
	}
}

void Scene::SetHover3d(std::vector<unsigned char> path, Action::Modifiers modifiers)
{
	if (_loopEditor.IsEngaged())
		return;
	// Resolve GUI ownership before taking the scene lock: station snapshots also lock it.
	std::vector<std::weak_ptr<base::GuiElement>> guiPath;
	_ResolveHoverPath2d(guiPath);
	std::unique_lock lock(_sceneMutex);
	bool isSelected = false;
	auto tweakState = base::Tweakable::TweakState::TWEAKSTATE_NONE;
	std::vector<unsigned char> fullElementPath;
	std::vector<unsigned char> elementPath;

	for (auto segment : path)
	{
		// Picker path uses 0xFF as the terminator for missing path segments.
		if ((0xFF == segment) || (0 == segment))
			break;

		fullElementPath.push_back(segment - 1);
	}

	_hoverPath3d = fullElementPath;
	_hoverElement3d = _ChildFromPathLocked(fullElementPath);
	elementPath = fullElementPath;
	// Keep the picked stream identity; MIDI layers are independent loop targets.

	elementPath = TrimPath(elementPath, _selector->CurrentSelectDepth() + 1);
	// Rack controls own the pointer even when the 3D picker sees a model behind them.
	if (std::any_of(guiPath.begin(), guiPath.end(), [](const auto& element) {
		return nullptr != std::dynamic_pointer_cast<GuiRack>(element.lock());
	}))
		elementPath.clear();

	if (elementPath != _lastLoggedHoverPath)
	{
		if (_loggingConfig.Ui == "verbose")
		{
			std::string pathString = "[";
			for (size_t i = 0; i < elementPath.size(); ++i)
			{
				pathString += std::to_string(static_cast<unsigned int>(elementPath[i]));
				if ((i + 1) < elementPath.size())
					pathString += ",";
			}
			pathString += "]";
			std::cout << "Hover3d resolved: " << pathString << std::endl;
		}

		_lastLoggedHoverPath = elementPath;
	}

	auto hovering = _ChildFromPathLocked(elementPath);
	if (nullptr != hovering)
	{
		isSelected = hovering->IsSelected();

		tweakState = _SelectionTweakStateLocked(hovering);
	}

	_selector->UpdateCurrentHover(elementPath,
		modifiers,
		isSelected,
		tweakState);
	_quantisationInteraction.RefreshOverlay(_InteractionContextLocked(),
		[this](const std::vector<unsigned char>& path) { return _ChildFromPathLocked(path); });

	lock.unlock();
	_UpdateSelection(ACTIONRESULT_DEFAULT);

	auto candidate = (Action::MODIFIER_SHIFT & modifiers) ? _ChildFromPath(elementPath) : nullptr;
	_UpdateStationQuantisation(candidate, _selector->CurrentSelectDepth(), false);
}

void Scene::Reset()
{
	// Reset also runs on the job thread; the controller/editor belong to UI.
	_quantisationInputResetRequested.store(true, std::memory_order_release);
	std::cout << "Reset" << std::endl;
	_ClearTimingState(true);
	_ClearStationQuantisation();
	_quantisation.ClearOverlay();
	_isSceneReset.store(true, std::memory_order_relaxed);
}

void Scene::InitGui()
{
	_selector->Init();
}

void Scene::InitAudio(bool generatedRig, const audio::AsioInventory* inventory)
{
	// Setup audio engine which starts device
	bool started = _audioEngine->Init(_networkService->GetController(), [this](Time streamTime, unsigned int numSamps,
		const std::optional<io::UserConfig>& cfg,
		const std::optional<audio::AudioStreamParams>& params) {
		this->OnTick(Timer::GetTime(), numSamps, cfg, params);
	}, generatedRig, inventory);

	// Share the master transport clock so the audio callback can apply unified
	// NINJAM timing commands to the Timer and local takes at one boundary.
	_audioEngine->SetTimingClock(_quantisation.Clock());

	if (started) {
		InitMidi(generatedRig);
		InitSerial();
	}
	else
	{
		_midiInputInventory.reset();
		CloseMidi();
		_midiState.store( std::make_shared<const MidiState>(), std::memory_order_release);
	}
	const auto midiState = _midiState.load(std::memory_order_acquire);
	const auto actualInputs = started ? _audioEngine->GetStreamParams().NumInputChannels : 0u;
	if (const auto runtime = _rigCoordinator.RefreshRuntimeAvailability(actualInputs, midiState->ConnectedNames))
	{
		_audioEngine->PublishPendingRigSnapshot(runtime);
		_inputSubsystem->PublishRigInputDispatch(runtime);
		_inputSubsystem->OpenRigTriggerInput(runtime->Revision);
		if (_hudPanel)
		{
			std::scoped_lock lock(_sceneMutex);
			_hudPanel->SetRoutingConfig(actualInputs, midiState->ConnectedNames, *runtime);
		}
	}

	CommitChanges();
}

void Scene::SetLogging(io::LoggingConfig config) noexcept
{
	_loggingConfig = config;
	if (_networkService)
		_networkService->SetTimingDiagnosticsEnabled(_loggingConfig.Event == "verbose");
	if (_inputSubsystem)
		_inputSubsystem->SetLogging(_loggingConfig);
	if (_windowSubsystem)
		_windowSubsystem->SetLogging(_loggingConfig);
	for (auto& station : _stations)
	{
		if (station)
			station->SetLogging(_loggingConfig);
	}
}

void Scene::CloseAudio()
{
	CloseSerial();
	CloseMidi();
	_audioEngine->Close();
}

bool Scene::PauseAudio()
{
	auto* device = _audioEngine ? _audioEngine->GetDevice() : nullptr;
	return device && device->Pause();
}

bool Scene::ResumeAudio()
{
	auto* device = _audioEngine ? _audioEngine->GetDevice() : nullptr;
	return device && device->Resume();
}

bool Scene::InitGlobalKeyCapture()
{
	return _inputSubsystem->InitGlobalKeyCapture();
}

void Scene::CloseGlobalKeyCapture()
{
	_inputSubsystem->CloseGlobalKeyCapture();
}

bool Scene::PumpGlobalKeyCapture(actions::KeyAction& action) noexcept
{
	return _inputSubsystem->PumpGlobalKeyCapture(action);
}

void Scene::Shutdown()
{
	_rigCoordinator.Shutdown();
	_audioEngine->ClearRigTriggerTransition();
	_inputSubsystem->CloseRigTriggerInputForever();
	_isSceneQuitting.store(true, std::memory_order_release);
	// RtAudio::Stop() waits for an in-flight callback to return.  Do this before
	// closing an editor or releasing a plugin: the callback can be dispatching
	// VST processing, MIDI, or recorded parameter automation.
	CloseAudio();
	CloseAllVstEditorWindows();

	if (_jobRunner.joinable())
		_jobRunner.join();
	assert(_inputSubsystem->RigTriggerInputReadyForShutdown());
	_inputSubsystem->PublishEmptyRigInputDispatch();
	CloseGlobalKeyCapture();
	CloseSerial();
	CloseMidi();

	// No work from the outgoing session may survive a session replacement.
	// In particular, queued VST loads retain UI-thread-created plugin objects;
	// hand those objects back to the UI destroy queue before dropping the jobs.
	std::list<actions::JobAction> abandonedJobs;
	// The consumer thread is joined and Shutdown is called by the UI owner, so
	// there can be no concurrent queue reader or producer at this point.
	abandonedJobs.swap(_jobList);
	for (auto& job : abandonedJobs)
	{
		if (job.PreInitPlugin)
			vst::QueueForUiThreadDestroy(std::move(job.PreInitPlugin));
	}

	ForceUnloadAllVstPlugins();
	_audioEngine->Close();
	_rigCoordinator.ReleaseAfterReadersStopped();
}

void Scene::ForceUnloadAllVstPlugins()
{
	// Shutdown() stops the audio device before reaching here.  Releasing a VST
	// while its callback may call SetParameter/ProcessBlock is not safe.
	std::scoped_lock lock(_sceneMutex);
	for (auto& station : _stations)
	{
		if (station)
			station->ForceUnloadAllVstPlugins();
	}
}

void Scene::CommitChanges()
{
	std::vector<JobAction> syncJobs = {};
	std::vector<JobAction> jobList = {};
	bool hoverChanged = false;

	{
		// Non-blocking: avoid holding up the audio thread. If the lock is
		// contended, pending snapshots remain unconsumed until next frame.
		if (!_sceneMutex.try_lock())
			return;

		std::lock_guard<std::mutex> lock(_sceneMutex, std::adopt_lock);

		std::optional<ninjam::NinjamRemoteSnapshot> pendingRemoteSnapshot = _networkService->GetController()->TakePendingSnapshot();
		hoverChanged = pendingRemoteSnapshot.has_value();

		if (pendingRemoteSnapshot.has_value())
			_UpdateRemoteStationsFromSnapshot(pendingRemoteSnapshot.value());

		for (auto& station : _stations)
		{
			station->ReleaseRetiredAudioStates();
			auto jobs = station->CommitChanges();
			if (!jobs.empty())
			{
				hoverChanged = true;
				for (auto& job : jobs)
				{
					switch (job.JobActionType)
					{
					case JobAction::JOB_UPDATELOOPS:
					case JobAction::JOB_ENDRECORDING:
						syncJobs.push_back(std::move(job));
						break;
					default:
						jobList.push_back(std::move(job));
						break;
					}
				}
			}
		}

		_OpenRemoteTempoPromptIfNeeded();

		// Completion/update payloads share structural ownership with start/end/ditch.
		// Keep validation and mutation under the same off-callback scene lock.
		for (auto& job : syncJobs)
		{
			auto receiver = job.Receiver.lock();
			if (receiver)
				receiver->OnAction(job);
		}
	}

	// Initialize VSTs on the UI thread after releasing _sceneMutex.
	for (auto& job : jobList)
	{
		if (job.JobActionType == JobAction::JOB_LOADVST)
		{
			auto plugin = vst::MakePluginForPath(job.VstPath);
			if (plugin->PreInit(job.VstPath))
				job.PreInitPlugin = std::move(plugin);
			// If PreInit fails, fall back to a fresh plugin on the job thread.
		}
	}

	if (!jobList.empty())
	{
		std::scoped_lock lock(_jobMutex);
		_jobList.insert(_jobList.end(), jobList.begin(), jobList.end());
	}

	if (hoverChanged)
		_InvalidateHover2d();
}

void Scene::ApplyDeferredHoverUpdates()
{
	// Keep active hover in sync with both deferred 2D hit-testing and latest 3D picker result.
	if (!_hover2dDirty)
		return;

	if (_popupManager.IsOpen())
	{
		if (!_hoverPath2d.empty())
		{
			std::vector<std::weak_ptr<base::GuiElement>> none;
			_ApplyHoverPath2d(none);
			_hoverPath2d.clear();
		}

		_hover2dDirty = false;
		return;
	}

	if (_touchDownElement.lock() || _isSceneTouching)
		return;

	_hoverPath2dScratch.clear();
	_ResolveHoverPath2d(_hoverPath2dScratch);
	auto& nextPath = _hoverPath2dScratch;
	_ApplyHoverPath2d(nextPath);

	bool stationHoverPromotedFrom2d = false;
	const bool rackOwnsHover = std::any_of(nextPath.begin(), nextPath.end(), [](const auto& element) {
		return nullptr != std::dynamic_pointer_cast<GuiRack>(element.lock());
	});

	if (!nextPath.empty() && !rackOwnsHover)
	{
		_hoverPath2dNextSharedScratch.clear();
		_LockHoverPath(nextPath, _hoverPath2dNextSharedScratch);
		auto& nextShared = _hoverPath2dNextSharedScratch;
		auto stationIt = std::find_if(nextShared.begin(), nextShared.end(), [](const std::shared_ptr<base::GuiElement>& element) {
			return nullptr != std::dynamic_pointer_cast<Station>(element);
		});

		if (stationIt != nextShared.end())
		{
			auto stationGlobalId = (*stationIt)->GlobalId();
			std::vector<unsigned char> stationPathRaw;
			stationPathRaw.reserve(stationGlobalId.size());
			for (auto part : stationGlobalId)
				stationPathRaw.push_back(static_cast<unsigned char>(part & 0xFFu));

			auto stationPath = TrimPath(stationPathRaw, _selector->CurrentSelectDepth() + 1u);
			bool isSelected = false;
			auto tweakState = base::Tweakable::TweakState::TWEAKSTATE_NONE;
			auto hovering = _ChildFromPath(stationPath);
			if (hovering)
			{
				isSelected = hovering->IsSelected();
				if (auto tweakable = std::dynamic_pointer_cast<Tweakable>(hovering))
					tweakState = tweakable->IsMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
			}

			_selector->UpdateCurrentHover(stationPath,
				Action::MODIFIER_NONE,
				isSelected,
				tweakState);
			_UpdateSelection(ACTIONRESULT_DEFAULT);
			stationHoverPromotedFrom2d = true;
		}
	}

	if (!stationHoverPromotedFrom2d && (rackOwnsHover || _hoverPath3d.empty()) && !_selector->CurrentHover().empty())
	{
		_selector->UpdateCurrentHover({ },
			Action::MODIFIER_NONE,
			false,
			base::Tweakable::TweakState::TWEAKSTATE_NONE);
		_UpdateSelection(ACTIONRESULT_DEFAULT);
	}
	else if (!stationHoverPromotedFrom2d && !rackOwnsHover && !_hoverPath3d.empty())
	{
		// The picker may keep the same model ID while the pointer leaves a control.
		auto pickPath = _hoverPath3d;
		for (auto& segment : pickPath)
			++segment;
		SetHover3d(std::move(pickPath), Action::MODIFIER_NONE);
	}

	_hoverPath2d = std::move(nextPath);
	_hover2dDirty = false;
}

void Scene::_InvalidateHover2d()
{
	_hover2dDirty = true;
}

void Scene::_ResolveHoverPath2d(std::vector<std::weak_ptr<base::GuiElement>>& outPath)
{
	if (_EditControlsSuppressed() && !_loopEditor.IsEngaged())
	{
		outPath.clear();
		return;
	}
	outPath.clear();

	auto resolveTop = [this](const std::shared_ptr<base::GuiElement>& root) {
		if (!root)
			return std::shared_ptr<base::GuiElement>();

		return root->FindTopmostDescendant(root->ParentToLocal(_cursorPos));
	};

	auto leaf = std::shared_ptr<base::GuiElement>();
	for (auto it = _guiChildren.rbegin(); it != _guiChildren.rend(); ++it)
	{
		leaf = resolveTop(*it);
		if (leaf)
			break;
	}

	if (!leaf)
	{
		for (auto& station : SnapshotStations())
		{
			leaf = resolveTop(std::static_pointer_cast<base::GuiElement>(station));
			if (leaf)
				break;
		}
	}

	while (leaf)
	{
		outPath.push_back(leaf);
		leaf = leaf->Parent();
	}

	std::reverse(outPath.begin(), outPath.end());
}

void Scene::_ApplyHoverPath2d(const std::vector<std::weak_ptr<base::GuiElement>>& nextPath)
{
	_hoverPath2dPrevSharedScratch.clear();
	_hoverPath2dNextSharedScratch.clear();
	_LockHoverPath(_hoverPath2d, _hoverPath2dPrevSharedScratch);
	_LockHoverPath(nextPath, _hoverPath2dNextSharedScratch);

	// Only the path leaf is the pointer target; ancestors are for ownership lookup.
	for (const auto& element : _hoverPath2dPrevSharedScratch)
		element->ApplyHoverState(false);

	if (!_hoverPath2dNextSharedScratch.empty())
	{
		auto& leaf = _hoverPath2dNextSharedScratch.back();
		leaf->ApplyHoverPoint(leaf->GlobalToLocal(_cursorPos));
	}
}

void Scene::_LockHoverPath(const std::vector<std::weak_ptr<base::GuiElement>>& path,
	std::vector<std::shared_ptr<base::GuiElement>>& outPath) const
{
	outPath.clear();
	outPath.reserve(path.size());

	for (const auto& element : path)
	{
		auto locked = element.lock();
		if (!locked)
			break;

		outPath.push_back(std::move(locked));
	}
}

std::shared_ptr<StationRemote> Scene::FindRemoteStation(const std::vector<std::shared_ptr<Station>>& stations,
	const std::string& userName)
{
	for (const auto& station : stations)
	{
		auto remote = std::dynamic_pointer_cast<StationRemote>(station);
		if (remote && remote->RemoteUserName() == userName)
			return remote;
	}

	return nullptr;
}

std::vector<unsigned char> Scene::TrimPath(std::vector<unsigned char> path, unsigned int depth)
{
	unsigned int pathLength = depth <= path.size() ?
		depth :
		(unsigned int)path.size();

	std::vector<unsigned char> curPath(path.begin(), path.begin() + pathLength);

	return curPath;
}





bool Scene::_OnUndo(std::shared_ptr<base::ActionUndo> undo)
{
	switch (undo->UndoType())
	{
	case UNDO_DOUBLE:
		auto doubleUndo = std::dynamic_pointer_cast<actions::GuiActionUndo>(undo);
		if (doubleUndo)
		{
			doubleUndo->Value();
			return true;
		}		
	}

	return false;
}

void Scene::_InitSize()
{
	_loopEditor.SetSize(_sizeParams.Size);
	if (_remoteTempoDialog) _remoteTempoDialog->FitToViewport(_sizeParams.Size);
	if (_mainPanel) _mainPanel->SetViewportSize(_sizeParams.Size);
	if (_selectionPanel) _selectionPanel->SetViewportSize(_sizeParams.Size);
	auto ar = _sizeParams.Size.Height > 0 ?
		(float)_sizeParams.Size.Width / (float)_sizeParams.Size.Height :
		1.0f;
	auto projection = _camera.Projection(ar, _StationCentre(_stations));
	_viewProj = projection * _camera.ViewMatrix();
	// _skyboxViewProj is updated in DrawBackground(); do not reset it here

	auto hScale = _sizeParams.Size.Width > 0 ? 2.0f / (float)_sizeParams.Size.Width : 1.0f;
	auto vScale = _sizeParams.Size.Height > 0 ? 2.0f / (float)_sizeParams.Size.Height : 1.0f;
	_overlayViewProj = glm::mat4(1.0);
	_overlayViewProj = glm::translate(_overlayViewProj, glm::vec3(-1.0f, -1.0f, -1.0f));
	_overlayViewProj = glm::scale(_overlayViewProj, glm::vec3(hScale, vScale, 1.0f));

	if (_hudPanel)
		_hudPanel->SetSize(_sizeParams.Size);

	_UpdateHudStationAnchors();
}

void Scene::_OnLoopGridEditorOpened()
{
	_ReleaseQuantisationInput();
	if (_hudPanel)
	{
		std::scoped_lock lock(_sceneMutex);
		_hudPanel->SetLoopEditorMode(static_cast<bool>(_loopEditor.TargetMidiLoop()));
	}
	_focusManager.ClearFocus();
	_touchDownElement.reset();
	_touchDownIsHud = false;
	_touchDownIsSettings = false;
	_consumeSettingsRelease = false;
	_EndBackgroundDrag();
	_quantisationInteraction.OnCtrlModifierChanged(false, Timer::GetTime(),
		_InteractionContext(), [this](const std::vector<unsigned char>& path) { return _ChildFromPath(path); });
}

void Scene::_UpdateHudStationAnchors()
{
	if (!_hudPanel || (_sizeParams.Size.Width == 0) || (_sizeParams.Size.Height == 0))
		return;

	std::vector<gui::GuiHud::StationAnchor> anchors;
	anchors.reserve(_stations.size());

	for (size_t stationIndex = 0u; stationIndex < _stations.size(); ++stationIndex)
	{
		const auto& station = _stations[stationIndex];
		const auto modelPos = station->TopCapModelPosition();
		auto clip = _viewProj * glm::vec4(modelPos.X, modelPos.Y, 0.0f, 1.0f);
		anchors.push_back({ stationIndex, station->Name(),
			gui::CableInteraction::ProjectAnchor(clip, _sizeParams.Size), glm::vec4(0.85f, 0.90f, 0.95f, 0.45f) });
	}

	_hudPanel->SetStationAnchors(std::move(anchors));
}

base::Tweakable::TweakState Scene::_SelectionTweakStateLocked(const std::shared_ptr<GuiElement>& target) const
{
	if (auto station = std::dynamic_pointer_cast<Station>(target))
		return station->AllTakesMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
	if (auto tweakable = std::dynamic_pointer_cast<Tweakable>(target))
		return tweakable->IsMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
	if (target)
		if (auto take = std::dynamic_pointer_cast<LoopTake>(target->Parent()))
			return take->IsMuted() ? Tweakable::TWEAKSTATE_MUTED : Tweakable::TWEAKSTATE_NONE;
	return Tweakable::TWEAKSTATE_NONE;
}

void Scene::_SetSelectionMutedLocked(const std::vector<unsigned char>& path, bool muted)
{
	// Caller holds the scene lock across target resolution and membership access.
	// The existing mute flags are atomic for audio readers.
	auto target = _ChildFromPathLocked(path);
	const auto setMuted = [muted](const std::shared_ptr<Tweakable>& tweakable) {
		if (!tweakable) return;
		if (muted) tweakable->Mute(); else tweakable->UnMute();
	};
	if (auto station = std::dynamic_pointer_cast<Station>(target))
	{
		for (const auto& take : station->GetLoopTakes()) setMuted(take);
	}
	else if (auto tweakable = std::dynamic_pointer_cast<Tweakable>(target))
		setMuted(tweakable);
	else if (target)
		// MIDI streams share their owning take's mute and backing ring.
		setMuted(std::dynamic_pointer_cast<LoopTake>(target->Parent()));
}

void Scene::_UpdateRackVisibilityLocked()
{
	// Presentation stays on the UI thread under the existing scene lock.
	const auto forEachRack = [this](auto&& visit) {
		for (const auto& station : _stations)
		{
			const auto position = station->ModelPosition();
			const auto clip = _viewProj * glm::vec4(position.X, position.Y, position.Z, 1.0f);
			const bool stationInView = station->IsVisible() && !_loopEditor.IsEngaged() &&
				clip.w > 0.0f && clip.z >= -clip.w && clip.z <= clip.w;
			visit(station->GetGuiRack(), stationInView);
			for (const auto& take : station->GetLoopTakes())
				visit(take->GetGuiRack(), stationInView && take->IsVisible());
		}
	};

	std::shared_ptr<GuiRack> expanded;
	forEachRack([&](const std::shared_ptr<GuiRack>& rack, bool ownerInView) {
		if (!rack || rack->GetRackState() == GuiRackParams::RACK_MASTER) return;
		if (!ownerInView || !rack->IsInView(_sizeParams.Size) || expanded)
		{
			rack->SetRackState(GuiRackParams::RACK_MASTER, true);
			_InvalidateHover2d();
		}
		else
			expanded = rack;
	});
	forEachRack([&](const std::shared_ptr<GuiRack>& rack, bool) {
		if (!rack) return;
		const bool visible = !expanded || rack == expanded;
		if (rack->GetMasterSlider()->Parent()->IsVisible() != visible)
		{
			rack->SetMasterControlsVisible(visible);
			_InvalidateHover2d();
		}
	});
}

void Scene::_CollapseRacksLocked()
{
	const auto collapse = [](const std::shared_ptr<GuiRack>& rack) {
		if (!rack) return;
		rack->SetRackState(GuiRackParams::RACK_MASTER, true);
		rack->SetMasterControlsVisible(true);
	};
	for (const auto& station : _stations)
	{
		collapse(station->GetGuiRack());
		for (const auto& take : station->GetLoopTakes()) collapse(take->GetGuiRack());
	}
	_InvalidateHover2d();
}

void Scene::_UpdateSelection(ActionResultType res)
{
	// Called when touch up + down, and when hover updated
	std::scoped_lock lock(_sceneMutex);
	const auto& stations = _stations;
	auto currentMode = _selector->CurrentMode();
	std::shared_ptr<GuiElement> hovering = nullptr;
	const auto applySelection = [this](const std::vector<unsigned char>& path, bool selected) {
		auto target = _ChildFromPathLocked(path);
		if (!target)
			return;
		// Only committed scene selection reaches here. Rack controls consume
		// their input before the selector, so editing a rack cannot dismiss it.
		_CollapseRacksLocked();
		if (_selector->CurrentSelectDepth() == base::DEPTH_STATION)
		{
			if (auto station = std::dynamic_pointer_cast<Station>(target))
			{
				if (selected) station->Select(); else station->DeSelect();
				for (const auto& take : station->GetLoopTakes())
					if (selected) take->Select(); else take->DeSelect();
				return;
			}
		}
		if (_selector->CurrentSelectDepth() == base::DEPTH_LOOP)
		{
			if (auto take = std::dynamic_pointer_cast<LoopTake>(target->Parent()))
			{
				// A take keeps the aggregate selected state used by the editor, while
				// individual loop models carry the loop-depth visual selection.
				if (selected && !take->IsSelected())
				{
					take->Select();
					for (const auto& loop : take->GetLoops())
						if (loop != target) loop->DeSelect();
					for (const auto& midiLoop : take->GetMidiLoops())
						if (auto model = midiLoop->Model(); model && model != target)
							model->DeSelect();
				}
				if (selected) target->Select(); else target->DeSelect();
				if (!selected && take->IsSelected())
				{
					bool anySelected = false;
					for (const auto& loop : take->GetLoops()) anySelected |= loop->IsSelected();
					for (const auto& midiLoop : take->GetMidiLoops())
						if (auto model = midiLoop->Model()) anySelected |= model->IsSelected();
					if (!anySelected) take->DeSelect();
				}
				return;
			}
		}
		if (selected) target->Select();
		else target->DeSelect();
	};
	const auto clearSelection = [this, &stations]() {
		_CollapseRacksLocked();
		for (const auto& station : stations)
		{
			station->DeSelect();
			for (const auto& take : station->GetLoopTakes())
			{
				take->DeSelect();
				for (const auto& loop : take->GetLoops()) loop->DeSelect();
				for (const auto& midiLoop : take->GetMidiLoops())
					if (auto model = midiLoop->Model()) model->DeSelect();
			}
		}
	};
	switch (res)
	{
	case ACTIONRESULT_DEFAULT:
		// Only called when hover changed or touch down
		switch (currentMode)
		{
		case SceneSelector::SELECT_NONE:
			for (auto& station : stations)
				station->SetPicking3d(false);

			hovering = _ChildFromPathLocked(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		case SceneSelector::SELECT_NONEADD:
			for (auto& station : stations)
				station->SetPicking3d(false);

			hovering = _ChildFromPathLocked(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		case SceneSelector::SELECT_SELECT:
			for (auto& station : stations)
				station->SetPicking3d(false);
			hovering = _ChildFromPathLocked(_selector->CurrentHover());
			if (nullptr != hovering)
				hovering->SetPicking3d(true);

			break;
		case SceneSelector::SELECT_SELECTADD:
			for (auto& station : stations)
				station->SetPicking3d(false);
			applySelection(_selector->CurrentHover(), true);

			break;
		case SceneSelector::SELECT_SELECTREMOVE:
			for (auto& station : stations)
				station->SetPicking3d(false);
			applySelection(_selector->CurrentHover(), false);

			break;
		case SceneSelector::SELECT_MUTE:
		case SceneSelector::SELECT_UNMUTE:
			for (auto& station : stations)
				station->SetPicking3d(false);
			if (_selector->IsPaintingMute())
				_SetSelectionMutedLocked(_selector->CurrentHover(), currentMode == SceneSelector::SELECT_MUTE);
			else if (auto target = _ChildFromPathLocked(_selector->CurrentHover()))
				target->SetPicking3d(true);

			break;
		}
		break;
	case ACTIONRESULT_SELECT:
		// A click replaces the selection with the item pressed at touch down.
		clearSelection();
		applySelection(_selector->PaintedPathForTest(), true);
		for (auto& station : stations)
		{
			station->SetPicking3d(false);
		}
		hovering = _ChildFromPathLocked(_selector->CurrentHover());
		if (hovering) hovering->SetPicking3d(true);

		break;
	case ACTIONRESULT_MUTE:
	case ACTIONRESULT_UNMUTE:
		// A click changes only its pressed target; a paint stroke already applied.
		_SetSelectionMutedLocked(_selector->PaintedPathForTest(), res == ACTIONRESULT_MUTE);
		for (auto& station : stations) station->SetPicking3d(false);
		if (auto target = _ChildFromPathLocked(_selector->CurrentHover())) target->SetPicking3d(true);
		break;
	case ACTIONRESULT_INITSELECT:
		for (auto& station : stations)
			station->SetPicking3d(false);

		hovering = _ChildFromPathLocked(_selector->CurrentHover());
		if (currentMode == SceneSelector::SELECT_SELECTADD ||
			currentMode == SceneSelector::SELECT_SELECTREMOVE)
		{
			const bool select = currentMode == SceneSelector::SELECT_SELECTADD;
			applySelection(_selector->PaintedPathForTest(), select);
			applySelection(_selector->CurrentHover(), select);
		}
		else if (_selector->IsPaintingMute())
		{
			const bool muted = currentMode == SceneSelector::SELECT_MUTE;
			_SetSelectionMutedLocked(_selector->PaintedPathForTest(), muted);
			_SetSelectionMutedLocked(_selector->CurrentHover(), muted);
		}
		// Paint strokes show committed selection/mute states without hover or press shading.
		if (currentMode != SceneSelector::SELECT_SELECTADD
			&& currentMode != SceneSelector::SELECT_SELECTREMOVE && !_selector->IsPaintingMute() && hovering)
			hovering->SetPicking3d(true);

		break;
	case ACTIONRESULT_CLEARSELECT:
		clearSelection();
		for (auto& station : stations)
			station->SetPicking3d(false);

		break;
	}
	const bool paintingSelection = currentMode == SceneSelector::SELECT_SELECTADD
		|| currentMode == SceneSelector::SELECT_SELECTREMOVE || _selector->IsPaintingMute();
	if (!paintingSelection && _selector->CurrentSelectDepth() == base::DEPTH_LOOP)
	{
		if (auto hovered = _ChildFromPathLocked(_selector->CurrentHover()))
		{
			if (auto take = std::dynamic_pointer_cast<LoopTake>(hovered->Parent()))
			{
				for (const auto& midiLoop : take->GetMidiLoops())
					if (midiLoop->Model() == hovered)
					{
						for (const auto& sibling : take->GetMidiLoops())
							if (auto model = sibling->Model()) model->SetPicking3d(true);
						break;
					}
			}
		}
	}
	// A click has a short-lived pressed appearance. Keep it separate from
	// persistent selection and hover, and clear it when the gesture becomes paint.
	for (const auto& station : stations)
	{
		station->SetClickPressed(false);
		for (const auto& take : station->GetLoopTakes())
		{
			for (const auto& loop : take->GetLoops())
				if (auto model = loop->Model()) model->SetClickPressed(false);
			for (const auto& midiLoop : take->GetMidiLoops())
				if (auto model = midiLoop->Model()) model->SetClickPressed(false);
		}
	}
	if (_selector->IsClickPressed()
		&& _selector->CurrentHover() == _selector->PaintedPathForTest())
	{
		if (auto pressed = _ChildFromPathLocked(_selector->CurrentHover()))
		{
			const bool mutePressed = _selector->IsMutePressed();
			const auto pressTakeLoops = [mutePressed](const std::shared_ptr<LoopTake>& take) {
				for (const auto& loop : take->GetLoops())
					if (auto model = loop->Model()) model->SetClickPressed(true, mutePressed);
				for (const auto& midiLoop : take->GetMidiLoops())
					if (auto model = midiLoop->Model()) model->SetClickPressed(true, mutePressed);
			};
			if (auto station = std::dynamic_pointer_cast<Station>(pressed))
			{
				station->SetClickPressed(true, mutePressed);
				for (const auto& take : station->GetLoopTakes())
					pressTakeLoops(take);
			}
			else if (auto take = std::dynamic_pointer_cast<LoopTake>(pressed))
				pressTakeLoops(take);
			else if (auto loop = std::dynamic_pointer_cast<Loop>(pressed))
			{
				if (auto model = loop->Model()) model->SetClickPressed(true, mutePressed);
			}
			else if (auto take = std::dynamic_pointer_cast<LoopTake>(pressed->Parent()))
			{
				for (const auto& midiLoop : take->GetMidiLoops())
					if (auto model = midiLoop->Model()) model->SetClickPressed(true, mutePressed);
			}
		}
	}
	// The picker may keep the same ID after a click or paint stroke, so update
	// the selector's cached starting state without waiting for another pick.
	if (auto hovered = _ChildFromPathLocked(_selector->CurrentHover()))
	{
		const auto tweakState = _SelectionTweakStateLocked(hovered);
		_selector->UpdateCurrentHover(_selector->CurrentHover(),
			Action::MODIFIER_NONE, hovered->IsSelected(), tweakState);
	}

	_quantisationInteraction.RefreshOverlay(_InteractionContextLocked(),
		[this](const std::vector<unsigned char>& path) { return _ChildFromPathLocked(path); });
}

void Scene::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	ResourceUser::InitResources(resourceLib, forceInit);
	// Initialize controls added by rig edits on the render thread before drawing them.
	{
		std::scoped_lock lock(_sceneMutex);
		if (_hudPanel)
			_hudPanel->InitResources(resourceLib, false);
	}

	// Stations can be added after scene resources are initialised.
	auto stations = SnapshotStations();
	for (auto& station : stations)
	{
		if (station)
			station->InitResources(resourceLib, forceInit);
	}
}

Position3d Scene::_StationCentre(const std::vector<std::shared_ptr<Station>>& stations)
{
	Position3d stationCentre{};
	if (!stations.empty())
	{
		for (const auto& station : stations)
			stationCentre += station->ModelPosition();
		const auto inverseCount = 1.0f / static_cast<float>(stations.size());
		stationCentre.X *= inverseCount;
		stationCentre.Y *= inverseCount;
		stationCentre.Z *= inverseCount;
	}
	return stationCentre;
}

void Scene::_ApplyCameraSelectDepthChange(graphics::Camera::SelectDepthChange change)
{
	if (graphics::Camera::SelectDepthChange::None == change)
		return;
	_viewMode = (graphics::Camera::SelectDepthChange::Station == change) ? VIEW_STATION : VIEW_LOOPTAKE;
	_modeRadio->SetCurrentValue(static_cast<unsigned int>(_viewMode), true);
	_UpdateSelectDepth(static_cast<unsigned int>(_viewMode));
}

void Scene::_CycleCameraView()
{
	const auto stations = SnapshotStations();
	std::optional<Position3d> hoveredStation;
	std::shared_ptr<Station> hoveredIdentity;
	const auto hoverPath = _selector->CurrentHover();
	if (!hoverPath.empty() && (hoverPath.front() < stations.size()))
	{
		hoveredStation = stations[hoverPath.front()]->ModelPosition();
		hoveredIdentity = stations[hoverPath.front()];
	}
	const auto firstStation = stations.empty()
		? std::optional<Position3d>{}
		: std::optional<Position3d>{ stations.front()->ModelPosition() };
	_ApplyCameraSelectDepthChange(_camera.CycleView(_StationCentre(stations), hoveredStation,
		firstStation, hoveredIdentity, stations.empty() ? nullptr : stations.front(),
		VIEW_STATION == _viewMode));
}

std::vector<std::shared_ptr<Station>> Scene::SnapshotStations() const
{
	std::lock_guard<std::mutex> lock(_sceneMutex);
	return _stations;
}

void Scene::_AddStation(std::shared_ptr<Station> station, bool publishAudioStations)
{
	station->SetReceiver(ActionReceiver::shared_from_this());
	station->SetLogging(_loggingConfig);
	station->SetClock(_quantisation.Clock());
	station->SetTransportOffsetLoopFrac(_transportOffsetLoopFrac);
	station->SetupBuffers(ChannelMixer::DefaultBufferSize);
	station->SetNumAdcChannels(_audioEngine->GetChannelMixer()->Source()->NumOutputChannels(Audible::AUDIOSOURCE_ADC));
	station->SetNumDacChannels(_audioEngine->GetChannelMixer()->Sink()->NumInputChannels(Audible::AUDIOSOURCE_LOOPS));
	station->Init();

	auto selectDepth = (unsigned int)_viewMode;
	if (selectDepth > (unsigned int)DEPTH_LOOP)
		selectDepth = (unsigned int)DEPTH_LOOP;

	station->SetSelectDepth((SelectDepth)selectDepth);
	station->SetGlobalMidiQuantState(_globalMidiQuantState);

	{
		std::lock_guard<std::mutex> lock(_sceneMutex);
		_stations.push_back(station);
		_camera.RegisterStation(_stations.size() - 1u, station, station->LoopTakeRevision());
		if (publishAudioStations)
			_PublishAudioStations();
	}
}

void Scene::_SetQuantisation(unsigned int quantiseSamps, Timer::QuantisationType quantisation)
{
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);
	_quantisation.Set(quantiseSamps, quantisation);
	_quantisation.SetMidiGrain(quantiseSamps, "scene quantisation set", _stations);
}

void Scene::_SetMidiQuantisationGrain(unsigned int grainSamps, const char* source)
{
	_quantisation.SetMidiGrain(grainSamps, source, _stations);
}

void Scene::_SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState state, bool fromLocalEdit)
{
	if ((_globalMidiQuantState == state) && !fromLocalEdit)
		return;

	_globalMidiQuantState = state;
	if (_globalMidiQuantRadio)
		_globalMidiQuantRadio->SetCurrentValue(static_cast<unsigned int>(state), true);

	_ApplyGlobalMidiQuantStateToAllLoopTakes();
}

void Scene::_SetTransportOffsetLoopFrac(double loopFrac, bool updateInput)
{
	loopFrac = std::isfinite(loopFrac) ? std::clamp(loopFrac, -1.0, 1.0) : 0.0;
	const auto previousLoopFrac = _transportOffsetLoopFrac;
	_transportOffsetLoopFrac = loopFrac;

	for (auto& station : SnapshotStations())
	{
		if (station)
			station->SetTransportOffsetLoopFrac(loopFrac);
	}
	if (_audioEngine && previousLoopFrac != loopFrac)
		_audioEngine->PublishLocalTransportOffsetLoopFrac(loopFrac);

	if (updateInput && _transportOffsetInput)
		_transportOffsetInput->SetValue(loopFrac, false);
}

void Scene::_ApplyGlobalMidiQuantStateToAllLoopTakes()
{
	for (auto& station : SnapshotStations())
	{
		if (station)
			station->SetGlobalMidiQuantState(_globalMidiQuantState);
	}
}

void Scene::_ForceGlobalMidiQuantStateMixedOnLocalEdit()
{
	_SetGlobalMidiQuantState(io::JamFile::GlobalMidiQuantState::Mixed, true);
}

void Scene::_UpdateRemoteStationsFromSnapshot(const NinjamRemoteSnapshot& snapshot)
{
	if (_networkService->UpdateRemoteStationsFromSnapshot(snapshot, _stations))
		_PublishAudioStations();
}

bool Scene::_IsMidiPhaseDragModifier(base::Action::Modifiers modifiers) const noexcept
{
	return (Action::MODIFIER_CTRL & modifiers);
}

QuantisationInteractionContext Scene::_InteractionContext() const
{
	std::scoped_lock lock(_sceneMutex);
	return _InteractionContextLocked();
}

QuantisationInteractionContext Scene::_InteractionContextLocked() const
{
	QuantisationInteractionContext context;
	context.CursorPos = _cursorPos;
	context.ViewportSize = _sizeParams.Size;
	context.SelectDepth = _selector->CurrentSelectDepth();
	context.HoverPath = _selector->CurrentHover();
	context.HoverPath3d = _hoverPath3d;
	context.SampleRate = _CurrentSampleRate();
	if (context.SelectDepth == base::DEPTH_LOOP)
	{
		bool anySelected = false;
		auto hovered = _ChildFromPathLocked(context.HoverPath);
		if (!hovered) hovered = _ChildFromPathLocked(context.HoverPath3d);
		for (const auto& station : _stations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
				{
					for (const auto& loop : take->GetLoops())
						if (loop)
						{
							anySelected |= loop->IsSelected();
							if (loop->IsSelected() || loop == hovered) context.LoopDepthHasAudioTarget = true;
						}
					for (const auto& loop : take->GetMidiLoops())
						if (auto model = loop->Model())
						{
							if (model->IsSelected()) { context.SelectedMidiLoops.push_back(loop); anySelected = true; }
							if (model == hovered) context.HoveredMidiLoop = loop;
						}
				}
		if (anySelected) context.HoveredMidiLoop.reset();
	}
	return context;
}

ActionResult Scene::_BeginBackgroundDrag(actions::TouchAction action)
{
	auto res = _camera.HandleBackgroundDrag(action);
	_isSceneTouching = _camera.IsBackgroundDragging();
	return res;
}

ActionResult Scene::_UpdateBackgroundDrag(actions::TouchMoveAction action)
{
	auto res = _camera.UpdateBackgroundDrag(action);
	if (!_camera.IsBackgroundDragging())
		_EndBackgroundDrag();

	return res;
}

void Scene::_EndBackgroundDrag()
{
	_isSceneTouching = false;
}

void Scene::_ClearTimingState(bool clearTapTempo)
{
	const auto hasConnectedTiming = _networkService->HasConnectedTiming();
	_quantisation.Clear(clearTapTempo, hasConnectedTiming);
	if (!hasConnectedTiming)
		_quantisation.SetMidiGrain(0u, "timing clear", _stations);
}

void Scene::_HandleAudioLocalContentState(bool hasLocalContent)
{
	if (hasLocalContent)
		return;

	// A connected empty scene still follows the accepted remote transport. Keep
	// the edge armed so a later physical-loss visit can clear local timing once.
	if (_networkService->HasConnectedTiming())
	{
		_isSceneReset.store(false, std::memory_order_relaxed);
		return;
	}

	if (_isSceneReset.exchange(true, std::memory_order_relaxed))
		return;

	// No local takes remain, so there is no station hierarchy to update. Keep
	// the destructive Quantiser cleanup on this job-owned edge and off OnTick.
	if (auto clock = _quantisation.Clock())
		std::cout << "MIDI timing reset: reason=empty-local clockLength="
			<< clock->SeedSourceLength() << " scene=" << clock->SceneSamplePos() << '\n';
	_quantisation.Clear(false);
}

void Scene::_ResetIfEmpty()
{
	if (_isSceneReset.load(std::memory_order_relaxed))
		return;
	for (const auto& station : _stations)
	{
		if (station && !station->IsRemote()
			&& !station->GetLoopTakeSnapshot().empty())
		{
			return;
		}
	}

	if (_networkService->HasConnectedTiming())
	{
		_isSceneReset.store(false, std::memory_order_relaxed);
		return;
	}
	Reset();
}

void Scene::_JobLoop()
{
	while (!_isSceneQuitting.load(std::memory_order_acquire))
	{
		OnJobTick(Timer::GetTime());
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	// Final job-producer barrier for permanent ingress closure.
	_inputSubsystem->AcknowledgeRigTriggerInputCloseFromJob();
}

void Scene::_PublishAudioStations()
{
	auto stations = std::make_shared<const std::vector<std::shared_ptr<Station>>>(_stations.begin(), _stations.end());
	_audioEngine->SetStations(stations);
}

std::shared_ptr<GuiElement> Scene::_ChildFromPath(std::vector<unsigned char> path)
{
	std::scoped_lock lock(_sceneMutex);
	return _ChildFromPathLocked(path);
}

std::shared_ptr<GuiElement> Scene::_ChildFromPathLocked(const std::vector<unsigned char>& path) const
{
	if (path.size() < 1)
		return nullptr;

	const auto& stations = _stations;
	std::shared_ptr<GuiElement> curChild;
	std::vector<unsigned char> curPath(path);

	auto stationIndex = path[0];

	if (stationIndex < stations.size())
	{
		curChild = stations[stationIndex];

		std::vector<unsigned char> curPath(path);

		while (nullptr != curChild)
		{
			curPath.erase(curPath.begin());

			if (curPath.empty() || (0xFF == curPath[0]))
				return curChild;

			curChild = curChild->TryGetChild(curPath[0]);
		}
	}

	return nullptr;
}

void Scene::_UpdateSelectDepth(unsigned int depth)
{
	if (depth > (unsigned int)DEPTH_LOOP)
		depth = (unsigned int)DEPTH_LOOP;

	auto selectDepth = (SelectDepth)depth;
	_selector->SetSelectDepth(selectDepth);

	for (auto& station : SnapshotStations())
		station->SetSelectDepth(selectDepth);

	_UpdateSelection(ACTIONRESULT_DEFAULT);
}

QuantisationPolicy Scene::_QuantisationPolicy() const
{
	QuantisationPolicy policy;
	policy.SeedGrainMinMs = _userConfig.Loop.SeedGrainMinMs;
	policy.SeedGrainTargetMaxMs = _userConfig.Loop.SeedGrainTargetMaxMs;
	policy.SeedBpmMin = _userConfig.Loop.SeedBpmMin;
	policy.SeedUsesPowers = _userConfig.Loop.SeedUsesPowers;
	return policy;
}

unsigned int Scene::_CurrentSampleRate() const
{
	if (_audioEngine->GetDevice())
	{
		const auto streamRate = _audioEngine->GetStreamParams().SampleRate;
		if (streamRate > 0u)
			return streamRate;
	}

	if (_userConfig.Audio.SampleRate > 0u)
		return _userConfig.Audio.SampleRate;

	return constants::DefaultSampleRate;
}

std::uint64_t Scene::_EstimatedAudioSampleAt(Time actionTime) const
{
	const auto sampleRate = _CurrentSampleRate();
	const auto anchor = midi::ReadMidiClockAnchor(_audioEngine->GetMidiClockAnchor(), {});
	const auto actionMicros = std::chrono::duration_cast<std::chrono::microseconds>(
		actionTime.time_since_epoch()).count();

	return MapMidiTimestampToAudioSample(sampleRate,
		anchor.Sample,
		anchor.SteadyMicros,
		actionMicros);
}

void Scene::_ApplyQuantisationTiming(const QuantisationTiming& timing, const char* source)
{
	_quantisation.SetSeedUsesPowers(_userConfig.Loop.SeedUsesPowers);
	_quantisation.ApplyTiming(timing, source);
	_quantisation.SetMidiGrain(timing.SeedSamps, source, _stations);
}

bool Scene::_HandleTapTempo(Time actionTime)
{
	std::scoped_lock lock(_sceneMutex);
	const auto handled = _quantisation.HandleTapTempo(_EstimatedAudioSampleAt(actionTime),
		_CurrentSampleRate(),
		_stations,
		_userConfig);
	if (handled)
	{
		_quantisation.UpdateStationHints(nullptr, _selector->CurrentSelectDepth(), false, _stations);
		for (const auto& station : _stations)
			if (station && !station->IsRemote())
				for (const auto& take : station->GetLoopTakes())
					if (take && _midiSubdivisionRadio)
					{
						_midiSubdivisionRadio->SetCurrentValue(
							midi::MidiQuantisation::FractionDisplayIndex(take->MidiQuantisation().Fraction), true);
						return handled;
					}
	}
	return handled;
}

void Scene::_PulseQuantisationOverlay()
{
	_quantisation.PulseOverlay();
}

void Scene::_SetQuantisationOverlayHeld(bool held)
{
	_quantisation.SetOverlayHeld(held);
}

float Scene::_QuantisationOverlayAlpha(Time now) const
{
	return _quantisation.OverlayAlpha(now);
}

void Scene::_ApplyQuantisationOverlayAlpha(float alpha)
{
	_quantisation.ApplyOverlayAlpha(alpha, _stations);
}

bool Scene::HasSelection() const
{
	std::scoped_lock lock(_sceneMutex);
	return _HasSelection();
}

bool Scene::_HasSelection() const
{
	for (const auto& station : _stations)
	{
		if (!station)
			continue;

		if (station->IsSelected())
			return true;

		for (const auto& take : station->GetLoopTakes())
		{
			if (!take)
				continue;

			if (take->IsSelected())
				return true;

			for (const auto& loop : take->GetLoops())
			{
				if (loop && loop->IsSelected())
					return true;
			}
			for (const auto& loop : take->GetMidiLoops())
			{
				if (loop && loop->Model() && loop->Model()->IsSelected())
					return true;
			}
		}
	}

	return false;
}

bool Scene::_HasQuantisationHover() const
{
	return !_selector->CurrentHover().empty() || !_hoverPath3d.empty();
}

bool Scene::_TrySetMasterFromHover(bool confirm)
{
	return _quantisation.TrySetMasterFromHover(_ChildFromPath(_selector->CurrentHover()),
		_selector->CurrentSelectDepth(),
		SnapshotStations(),
		_CurrentSampleRate(),
		_userConfig,
		confirm);
}

void Scene::_UpdateStationQuantisation(std::shared_ptr<base::GuiElement> candidate,
	base::SelectDepth depth,
	bool confirmCandidate)
{
	_quantisation.UpdateStationHints(candidate, depth, confirmCandidate, SnapshotStations());
}

void Scene::_ClearStationQuantisation()
{
	_quantisation.ClearStationHints(_stations);
}

void Scene::SetRelativePointerHost(std::function<bool(int, utils::Position2d)> begin,
	std::function<void(int)> end)
{
	_ReleaseQuantisationInput();
	_loopEditor.CancelInput();
	_beginRelativePointer = std::move(begin);
	_endRelativePointer = std::move(end);
}

bool Scene::_EditControlsSuppressed() const
{
	return _quantisationInteraction.EditModeActive() || _quantisationInteraction.PanelAlpha() > 0.001f;
}

std::optional<ActionResult> Scene::_RouteQuantisationTouch(TouchAction action)
{
	std::scoped_lock lock(_sceneMutex);
	return _quantisationInteraction.TryHandleTouchAction(action, _CurrentSampleRate(),
		_ctrlHeld, _InteractionContextLocked(), [this](const auto& path) { return _ChildFromPathLocked(path); });
}

std::optional<ActionResult> Scene::_RouteQuantisationMove(TouchMoveAction action)
{
	std::scoped_lock lock(_sceneMutex);
	return _quantisationInteraction.TryHandleTouchMove(action, _CurrentSampleRate());
}

void Scene::_ReleaseQuantisationInput()
{
	_spaceHeld = _ctrlHeld = false;
	_quantisation.SetOverlayHeld(false);
	_quantisationInteraction.CancelInteraction(true);
	_quantisationInteraction.OnCtrlModifierChanged(false, Timer::GetTime(),
		_InteractionContext(), [this](const auto& path) { return _ChildFromPath(path); });
}

void Scene::OnInputFocusLost()
{
	_ReleaseQuantisationInput();
	_loopEditor.CancelInput();
	_focusManager.ClearFocus();
	_ApplyHoverPath2d({});
	_InvalidateHover2d();
}
