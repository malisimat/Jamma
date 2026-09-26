
#include "gtest/gtest.h"
#include <sstream>
#include <stdexcept>
#include "resources/ResourceLib.h"
#include "midi/MidiEvent.h"
#include "engine/LoopTake.h"
#include "engine/Scene.h"
#include "engine/Station.h"
#include "engine/Trigger.h"
#include "gui/GuiButton.h"
#include "io/UserConfig.h"
#include "io/Json.h"
#include "io/RigFile.h"

using base::ActionSender;
using base::ActionReceiver;
using engine::LoopTake;
using engine::LoopTakeParams;
using engine::Scene;
using engine::SceneParams;
using engine::Station;
using engine::StationParams;
using engine::Trigger;
using engine::TriggerParams;
using utils::Timer;
using actions::TriggerAction;
using actions::KeyAction;
using actions::TouchAction;
using actions::TouchMoveAction;
using audio::MergeMixBehaviourParams;

const unsigned int ActivateChar = 49;
const unsigned int DitchChar = 50;
const unsigned int LeftMouseButtonMask = 1u << 0;
const unsigned int RightMouseButtonMask = 1u << 2;

Time GetTime()
{
	return std::chrono::steady_clock::now();
}

Time OffsetTime(const Time t, unsigned int ms)
{
	return t + std::chrono::milliseconds(ms);
}

template <typename TriggerPointer>
static void CompleteQueuedStructuralAction(const TriggerPointer& trigger,
	const std::optional<io::UserConfig>& cfg = std::nullopt,
	std::uint64_t revision = 0u)
{
	trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);
	trigger->OnTick(GetTime(), 0u, cfg, std::nullopt, revision);
}

template <typename TriggerPointer>
static void TickAndComplete(const TriggerPointer& trigger,
	unsigned int samps = 0u,
	const std::optional<io::UserConfig>& cfg = std::nullopt,
	std::uint64_t revision = 0u)
{
	trigger->OnTick(GetTime(), samps, cfg, std::nullopt, revision);
	CompleteQueuedStructuralAction(trigger, cfg, revision);
}

static void SendMidiEvent(const std::shared_ptr<Trigger>& trigger, const midi::MidiEvent& event, Time t)
{
	base::Action midiAction;
	midiAction.SetActionTime(t);
	trigger->OnEvent(event, midiAction);
}

class MockedTriggerReceiver :
	public ActionReceiver
{
public:
	MockedTriggerReceiver() :
		ActionReceiver(),
		_expected(TriggerAction::TRIGGER_REC_START),
		_lastMatched(false),
		_numTimesCalled(0) {}
	MockedTriggerReceiver(TriggerAction::TriggerActionType expected) :
		ActionReceiver(),
		_expected(expected),
		_lastMatched(false),
		_numTimesCalled(0) {}
public:
	virtual actions::ActionResult OnAction(actions::TriggerAction action)
	{
		_numTimesCalled++;
		_lastMatched = action.ActionType == _expected;

		return { _lastMatched, "", "", actions::ACTIONRESULT_DEFAULT, nullptr, std::weak_ptr<base::GuiElement>() };
	};
	void SetExpected(TriggerAction::TriggerActionType expected) { _expected = expected; }
	bool GetLastMatched() const { return _lastMatched; }
	int GetNumTimesCalled() const { return _numTimesCalled; }

private:
	TriggerAction::TriggerActionType _expected;
	bool _lastMatched;
	int _numTimesCalled;
};

class SequenceTriggerReceiver :
	public ActionReceiver
{
public:
	SequenceTriggerReceiver() = default;
	SequenceTriggerReceiver(std::shared_ptr<base::TriggerPunchTarget> sourceTake,
		std::shared_ptr<base::TriggerPunchTarget> targetTake) :
		_sourceTake(std::move(sourceTake)),
		_targetTake(std::move(targetTake)) {}

	virtual actions::ActionResult OnAction(actions::TriggerAction action)
	{
		_actions.push_back(action);

		actions::ActionResult result{
			true,
			"source-loop-take",
			"target-loop-take",
			actions::ACTIONRESULT_DEFAULT,
			nullptr,
			std::weak_ptr<base::GuiElement>()
		};
		if (action.ActionType == TriggerAction::TRIGGER_DITCH)
			result.DitchResult = actions::DitchDisposition::Removed;
		if (action.ActionType == TriggerAction::TRIGGER_REC_START ||
			action.ActionType == TriggerAction::TRIGGER_OVERDUB_START)
		{
			result.TriggerSourceTake = _sourceTake;
			result.TriggerTargetTake = _targetTake;
		}
		return result;
	}

	const std::vector<TriggerAction>& Actions() const
	{
		return _actions;
	}

private:
	std::vector<TriggerAction> _actions;
	std::shared_ptr<base::TriggerPunchTarget> _sourceTake;
	std::shared_ptr<base::TriggerPunchTarget> _targetTake;
};

class TestTriggerPunchTarget : public base::TriggerPunchTarget
{
public:
	void SetTriggerSourceMutedAudio(bool muted) noexcept override
	{
		if (muted) ++MuteCount;
		else ++UnmuteCount;
	}
	void TriggerPunchInAudio() noexcept override { ++PunchInCount; }
	void TriggerPunchOutAudio() noexcept override { ++PunchOutCount; }

	unsigned int MuteCount = 0u;
	unsigned int UnmuteCount = 0u;
	unsigned int PunchInCount = 0u;
	unsigned int PunchOutCount = 0u;
};

class RoutingHistoryReceiver : public ActionReceiver
{
public:
	explicit RoutingHistoryReceiver(std::string name) : _name(std::move(name)) {}

	actions::ActionResult OnAction(actions::TriggerAction action) override
	{
		_actions.push_back(action);
		const auto id = action.ActionType == TriggerAction::TRIGGER_REC_START ?
			_name + "-" + std::to_string(++_nextTake) : std::string();
		actions::ActionResult result{ true, "", id, actions::ACTIONRESULT_DEFAULT, nullptr,
			std::weak_ptr<base::GuiElement>() };
		if (action.ActionType == TriggerAction::TRIGGER_DITCH)
			result.DitchResult = actions::DitchDisposition::Removed;
		return result;
	}

	const std::vector<TriggerAction>& Actions() const noexcept { return _actions; }

private:
	std::string _name;
	unsigned int _nextTake = 0u;
	std::vector<TriggerAction> _actions;
};

class ConfigurableTriggerReceiver :
	public ActionReceiver
{
public:
	explicit ConfigurableTriggerReceiver(bool eat = true,
		actions::DitchDisposition ditchResult = actions::DitchDisposition::Removed) :
		_eat(eat),
		_ditchResult(ditchResult)
	{
	}

	virtual actions::ActionResult OnAction(actions::TriggerAction action)
	{
		_actions.push_back(action);
		actions::ActionResult result{
			_eat,
			"source-loop-take",
			"target-loop-take",
			actions::ACTIONRESULT_DEFAULT,
			nullptr,
			std::weak_ptr<base::GuiElement>()
		};
		if (_eat && action.ActionType == TriggerAction::TRIGGER_DITCH)
			result.DitchResult = _ditchResult;
		return result;
	}

	const std::vector<TriggerAction>& Actions() const
	{
		return _actions;
	}

private:
	bool _eat;
	actions::DitchDisposition _ditchResult;
	std::vector<TriggerAction> _actions;
};

class TestLoopTake :
	public LoopTake
{
public:
	TestLoopTake(LoopTakeParams params,
		audio::AudioMixerParams mixerParams) :
		LoopTake(params, mixerParams)
	{
	}

	std::size_t MidiLoopEventCount(std::size_t index = 0u) const
	{
		if (index >= _midiLoops.size() || !_midiLoops[index])
			return 0u;

		return _midiLoops[index]->EventCount();
	}
};

class TestScene :
	public Scene
{
public:
	TestScene(SceneParams params,
		io::UserConfig user) :
		Scene(params, user)
	{
	}

	void AddStationForTest(const std::shared_ptr<Station>& station)
	{
		_AddStation(station);
	}

	void RemoveStationForTest(const std::shared_ptr<Station>& station)
	{
		std::scoped_lock lock(_sceneMutex);
		std::erase(_stations, station);
	}

	bool IsSceneResetForTest() const
	{
		return _isSceneReset.load(std::memory_order_relaxed);
	}

	void StopJobForTest()
	{
		Shutdown();
	}

	void SeedTimingForTest()
	{
		engine::QuantisationTiming timing;
		timing.SeedSamps = 12000u;
		timing.MasterLoopSamps = 48000u;
		timing.SeedCount = 4u;
		timing.Bpm = 120.0f;
		timing.Bpi = 4u;
		_quantisation.ApplyTiming(timing, "B008 test");
	}

	bool HasTimingForTest() const
	{
		return _quantisation.CurrentTempoTiming(48000u).has_value();
	}

	void ObserveTimingAvailabilityForTest(bool available, std::uint64_t epoch)
	{
		ninjam::NinjamSessionTimingStatus status;
		status.IsAvailable = available;
		status.Changed = true;
		status.SessionEpoch = epoch;
		_ApplyNinjamTimingUpdate(_networkService->ObserveSessionStatus(status,
			_quantisation.CurrentTempoTiming(48000u)));
	}

	utils::Position3d CameraPositionForTest() const
	{
		return _camera.ModelPosition();
	}

	graphics::Camera::View CameraViewForTest() const
	{
		return _camera.CurrentView();
	}

	graphics::Camera::Pose CameraPoseForTest() const
	{
		return _camera.CurrentPose();
	}

	float StationInteriorFieldOfViewForTest() const
	{
		return _camera.StationInteriorFieldOfView();
	}

	glm::mat4 CameraProjectionForTest() const
	{
		return _camera.Projection(1.0f, {});
	}

	utils::Position3d CameraFocusAtCursorForTest(utils::Position2d cursorPosition) const
	{
		return _camera.FocusPointAtCursor(cursorPosition,
			_sizeParams.Size.Width,
			_sizeParams.Size.Height,
			{});
	}

	glm::vec2 ProjectWorldPositionForTest(utils::Position3d position) const
	{
		const auto width = static_cast<float>(_sizeParams.Size.Width);
		const auto height = static_cast<float>(_sizeParams.Size.Height);
		const auto aspectRatio = width / height;
		const auto clip = _camera.Projection(aspectRatio, {}) * _camera.ViewMatrix()
			* glm::vec4(position.X, position.Y, position.Z, 1.0f);
		const auto ndc = glm::vec3(clip) / clip.w;
		return { ((ndc.x + 1.0f) * width) / 2.0f, ((ndc.y + 1.0f) * height) / 2.0f };
	}

	ViewMode CameraSelectDepthForTest() const
	{
		return _viewMode;
	}

	void SetCameraSelectDepthForTest(unsigned int value)
	{
		actions::GuiAction action;
		action.ElementType = actions::GuiAction::ACTIONELEMENT_RADIO;
		action.Index = 100u;
		action.Data = actions::GuiAction::GuiInt(static_cast<int>(value));
		OnAction(action);
	}

	bool IsCameraTransitioningForTest() const
	{
		return _camera.IsTransitioning();
	}

	void UpdateCameraStationFollowForTest()
	{
		UpdateCamera();
	}

	void TickCameraForTest(unsigned int samps, unsigned int sampleRate)
	{
		_camera.TickBackgroundDrag(static_cast<float>(samps) / static_cast<float>(sampleRate));
	}

	void SettleCameraForTest()
	{
		for (unsigned int tick = 0u; tick < 8u; ++tick)
			TickCameraForTest(2205u, 44100u);
	}

	bool IsSceneTouchingForTest() const
	{
		return _camera.IsBackgroundDragging();
	}

	bool HasTouchCaptureForTest() const
	{
		return !_touchDownElement.expired();
	}

	void OpenPopupForTest(const std::shared_ptr<base::GuiElement>& popup)
	{
		_popupManager.Open(popup);
	}

	void ApplyHoverForTest()
	{
		ApplyDeferredHoverUpdates();
	}
};

TouchAction MakeSceneTouch(TouchAction::TouchState state,
	utils::Position2d pos,
	int index,
	unsigned int mouseButtonsDown)
{
	TouchAction action;
	action.Touch = TouchAction::TOUCH_MOUSE;
	action.State = state;
	action.Index = index;
	action.Position = pos;
	action.MouseButtonsDown = mouseButtonsDown;
	return action;
}

TouchAction MakeSceneWheel(int value, utils::Position2d pos = { 700, 450 })
{
	TouchAction action;
	action.Touch = TouchAction::TOUCH_MOUSE;
	action.State = TouchAction::TOUCH_DOWN;
	action.Index = 4;
	action.Value = value;
	action.Position = pos;
	return action;
}

TouchMoveAction MakeSceneTouchMove(utils::Position2d pos,
	unsigned int mouseButtonsDown)
{
	TouchMoveAction action;
	action.Touch = TouchAction::TOUCH_MOUSE;
	action.Position = pos;
	action.MouseButtonsDown = mouseButtonsDown;
	return action;
}

gui::GuiButtonParams MakeSceneButtonParams(utils::Position2d position,
	utils::Size2d size)
{
	gui::GuiButtonParams params;
	params.Position = position;
	params.Size = size;
	params.MinSize = size;
	return params;
}

std::shared_ptr<Station> MakeTestStation(const std::string& name = "station")
{
	StationParams params;
	params.Name = name;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = Station::GetMixerParams(params.Size, merge);
	return std::make_shared<Station>(params, mixerParams);
}

std::shared_ptr<TestLoopTake> MakeTestLoopTake(const std::string& id = "take-0")
{
	LoopTakeParams params;
	params.Id = id;
	params.Size = { 100, 100 };
	MergeMixBehaviourParams merge;
	auto mixerParams = LoopTake::GetMixerParams(params.Size, merge);
	return std::make_shared<TestLoopTake>(params, mixerParams);
}

std::unique_ptr<Trigger> MakeDefaultTrigger(std::shared_ptr<ActionReceiver> receiver,
	unsigned int debounceMs)
{
	auto activateBind = engine::DualBinding();
	activateBind.SetDown(engine::TriggerBinding(engine::TRIGGER_KEY, ActivateChar, 1), true);

	auto ditchBind = engine::DualBinding();
	ditchBind.SetRelease(engine::TriggerBinding(engine::TRIGGER_KEY, DitchChar, 0), true);

	TriggerParams trigParams;
	trigParams.Activate = { activateBind };
	TriggerParams ditchParams;
	trigParams.Ditch = { ditchBind };
	trigParams.DebounceMs = debounceMs;
	auto trigger = std::make_unique<Trigger>(trigParams);
	trigger->SetReceiver(receiver);

	return std::move(trigger);
}

std::shared_ptr<Trigger> MakeSharedDefaultTrigger(unsigned int debounceMs = 0u)
{
	auto activateBind = engine::DualBinding();
	activateBind.SetDown(engine::TriggerBinding(engine::TRIGGER_KEY, ActivateChar, 1), true);

	auto ditchBind = engine::DualBinding();
	ditchBind.SetRelease(engine::TriggerBinding(engine::TRIGGER_KEY, DitchChar, 0), true);

	TriggerParams trigParams;
	trigParams.Activate = { activateBind };
	trigParams.Ditch = { ditchBind };
	trigParams.DebounceMs = debounceMs;
	return std::make_shared<Trigger>(trigParams);
}

std::shared_ptr<Trigger> MakeTriggerFromRigJson(const std::string& jsonText,
	unsigned int debounceMs = 0u)
{
	auto testStream = std::stringstream(jsonText);
	auto json = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(testStream)).value());
	auto trigStruct = io::RigFile::Trigger::FromJson(json);
	EXPECT_TRUE(trigStruct.has_value());
	if (!trigStruct.has_value())
		return nullptr;

	TriggerParams trigParams;
	trigParams.DebounceMs = debounceMs;
	auto trigger = Trigger::FromFile(trigParams, trigStruct.value());
	EXPECT_TRUE(trigger.has_value());
	return trigger.has_value() ? trigger.value() : nullptr;
}

TEST(Trigger, DitchesLoop) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	auto trigger = MakeDefaultTrigger(receiver, 0);
	auto action = KeyAction();;
	actions::ActionResult actionRes;

	receiver->SetExpected(TriggerAction::TRIGGER_REC_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());

	receiver->SetExpected(TriggerAction::TRIGGER_DITCH);
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
}

TEST(Trigger, ExternalControlActionsDriveTheExistingStateMachine) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	auto trigger = MakeDefaultTrigger(receiver, 0);
	base::Action action;
	ASSERT_TRUE(trigger->CanEditRouting());

	auto activateDown = trigger->QueueExternalControlAction(true, true, action);
	ASSERT_TRUE(activateDown.IsEaten);
	ASSERT_FALSE(trigger->CanEditRouting());
	ASSERT_EQ(actions::ACTIONRESULT_DEFAULT, activateDown.ResultType);
	ASSERT_TRUE(receiver->Actions().empty());
	TickAndComplete(trigger);
	ASSERT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);
	ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
	ASSERT_TRUE(trigger->IsActivateInputDown());
	ASSERT_TRUE(trigger->QueueExternalControlAction(true, false, action).IsEaten);
	TickAndComplete(trigger);
	ASSERT_FALSE(trigger->IsActivateInputDown());

	auto ditchDown = trigger->QueueExternalControlAction(false, true, action);
	ASSERT_TRUE(ditchDown.IsEaten);
	TickAndComplete(trigger);
	ASSERT_TRUE(trigger->IsDitchDown());
	ASSERT_TRUE(trigger->IsDitchInputDown());

	auto ditchUp = trigger->QueueExternalControlAction(false, false, action);
	ASSERT_TRUE(ditchUp.IsEaten);
	ASSERT_EQ(actions::ACTIONRESULT_DEFAULT, ditchUp.ResultType);
	TickAndComplete(trigger);
	ASSERT_EQ(TriggerAction::TRIGGER_DITCH, receiver->Actions()[1].ActionType);
	ASSERT_EQ(TriggerAction::TRIGGER_DITCH_UNMUTE, receiver->Actions()[2].ActionType);
	ASSERT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
	ASSERT_FALSE(trigger->IsActivateInputDown());
	ASSERT_FALSE(trigger->IsDitchInputDown());
	ASSERT_FALSE(trigger->IsDitchDown());
	ASSERT_TRUE(trigger->CanEditRouting());
}

TEST(Trigger, CaptureEditsAndStationMovesKeepDitchHistoryInRecordingOrder)
{
	auto firstStation = std::make_shared<RoutingHistoryReceiver>("first");
	auto secondStation = std::make_shared<RoutingHistoryReceiver>("second");
	auto trigger = MakeDefaultTrigger(firstStation, 0);
	base::Action action;

	auto applyRoute = [&](std::shared_ptr<base::ActionReceiver> receiver,
		std::vector<unsigned int> channels)
	{
		std::vector<std::string> midiDevices;
		auto midiMode = io::RigFile::Trigger::MidiInputMode::None;
		auto mixer = std::make_shared<audio::AudioMixer>(Trigger::GetOverdubMixerParams(channels));
		auto writer = Trigger::CreateBounceWriter(mixer);
		trigger->ApplyCaptureRouting(receiver, channels, midiDevices, midiMode, mixer, writer);
	};
	auto press = [&](bool activate, bool down)
	{
		ASSERT_TRUE(trigger->QueueExternalControlAction(activate, down, action).IsEaten);
		TickAndComplete(trigger);
	};
	auto record = [&]()
	{
		press(true, true);
		press(true, false);
		trigger->OnTick(GetTime(), 64u, std::nullopt, std::nullopt);
		press(true, true);
		press(true, false);
	};
	auto ditch = [&]() { press(false, true); press(false, false); };

	applyRoute(firstStation, { 0u });
	record();
	applyRoute(firstStation, { 0u, 1u });
	record();
	applyRoute(secondStation, { 0u, 1u });
	record();

	ASSERT_EQ(4u, firstStation->Actions().size());
	EXPECT_EQ((std::vector<unsigned int>{ 0u }), firstStation->Actions()[0].InputChannels);
	EXPECT_EQ((std::vector<unsigned int>{ 0u, 1u }), firstStation->Actions()[2].InputChannels);
	ASSERT_EQ(2u, secondStation->Actions().size());
	EXPECT_EQ((std::vector<unsigned int>{ 0u, 1u }), secondStation->Actions()[0].InputChannels);

	ditch();
	ASSERT_EQ(4u, secondStation->Actions().size());
	EXPECT_EQ("second-1", secondStation->Actions()[2].TargetId);
	ditch();
	ASSERT_EQ(6u, firstStation->Actions().size());
	EXPECT_EQ("first-2", firstStation->Actions()[4].TargetId);
	ditch();
	ASSERT_EQ(8u, firstStation->Actions().size());
	EXPECT_EQ("first-1", firstStation->Actions()[6].TargetId);
	EXPECT_TRUE(trigger->GetTakes().empty());
}

TEST(Trigger, RejectedStartDoesNotEndAnOlderHistoryEntry)
{
	auto acceptedReceiver = std::make_shared<ConfigurableTriggerReceiver>(true);
	auto rejectedReceiver = std::make_shared<ConfigurableTriggerReceiver>(false);
	auto trigger = MakeDefaultTrigger(acceptedReceiver, 0u);
	base::Action action;
	auto press = [&](bool down)
	{
		ASSERT_TRUE(trigger->QueueExternalControlAction(true, down, action).IsEaten);
		TickAndComplete(trigger, down ? 64u : 0u);
	};
	press(true); press(false); press(true); press(false);
	ASSERT_EQ(1u, trigger->GetTakes().size());
	ASSERT_EQ(2u, acceptedReceiver->Actions().size());

	std::shared_ptr<base::ActionReceiver> route = rejectedReceiver;
	std::vector<unsigned int> channels{ 0u };
	std::vector<std::string> midiDevices;
	auto midiMode = io::RigFile::Trigger::MidiInputMode::None;
	auto mixer = std::make_shared<audio::AudioMixer>(Trigger::GetOverdubMixerParams(channels));
	auto writer = Trigger::CreateBounceWriter(mixer);
	trigger->ApplyCaptureRouting(route, channels, midiDevices, midiMode, mixer, writer);
	press(true); press(false); press(true); press(false);

	EXPECT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
	EXPECT_EQ(1u, trigger->GetTakes().size());
	EXPECT_EQ(2u, acceptedReceiver->Actions().size());
	ASSERT_EQ(2u, rejectedReceiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, rejectedReceiver->Actions()[0].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, rejectedReceiver->Actions()[1].ActionType);
}

TEST(Trigger, DitchPopsOnlyRemovedOrAlreadyAbsentHistory)
{
	for (const auto disposition : { actions::DitchDisposition::Removed,
		actions::DitchDisposition::AlreadyAbsent,
		actions::DitchDisposition::Failed })
	{
		auto receiver = std::make_shared<ConfigurableTriggerReceiver>(true, disposition);
		auto trigger = MakeDefaultTrigger(receiver, 0u);
		base::Action action;
		auto press = [&](bool activate, bool down)
		{
			ASSERT_TRUE(trigger->QueueExternalControlAction(activate, down, action).IsEaten);
			TickAndComplete(trigger, down ? 64u : 0u);
		};
		press(true, true); press(true, false); press(true, true); press(true, false);
		ASSERT_EQ(1u, trigger->GetTakes().size());
		press(false, true); press(false, false);
		if (disposition == actions::DitchDisposition::Failed)
			EXPECT_EQ(1u, trigger->GetTakes().size());
		else
			EXPECT_TRUE(trigger->GetTakes().empty());
	}
}

TEST(Trigger, RestoredHistoryDitchesItsMostRecentTake) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	auto trigger = MakeSharedDefaultTrigger();
	trigger->SetReceiver(receiver);
	trigger->RestoreTakes({
		{ engine::TriggerTake::SOURCE_ADC, "source-take", "older-take" },
		{ engine::TriggerTake::SOURCE_LOOPTAKE, "older-take", "latest-take" }
	});

	base::Action action;
	ASSERT_TRUE(trigger->QueueExternalControlAction(false, true, action).IsEaten);
	TickAndComplete(trigger);
	ASSERT_TRUE(trigger->QueueExternalControlAction(false, false, action).IsEaten);
	TickAndComplete(trigger);

	ASSERT_EQ(2u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH, receiver->Actions()[0].ActionType);
	EXPECT_EQ("latest-take", receiver->Actions()[0].TargetId);
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH_UNMUTE, receiver->Actions()[1].ActionType);
	EXPECT_EQ("older-take", receiver->Actions()[1].TargetId);
	ASSERT_EQ(1u, trigger->GetTakes().size());
	EXPECT_EQ("older-take", trigger->GetTakes()[0].TargetTakeId);
}

TEST(Trigger, ResetClearsPublishedDitchState) {
	auto trigger = MakeSharedDefaultTrigger();
	base::Action action;

	ASSERT_TRUE(trigger->QueueExternalControlAction(false, true, action).IsEaten);
	trigger->OnTick(GetTime(), 0u, std::nullopt, std::nullopt);
	ASSERT_TRUE(trigger->IsDitchDown());

	trigger->Reset();

	ASSERT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
	ASSERT_FALSE(trigger->IsActivateInputDown());
	ASSERT_FALSE(trigger->IsDitchInputDown());
	ASSERT_FALSE(trigger->IsDitchDown());
}

TEST(Trigger, RecordsTwoLoops) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	auto trigger = MakeDefaultTrigger(receiver, 0);
	auto action = KeyAction();
	actions::ActionResult actionRes;

	receiver->SetExpected(TriggerAction::TRIGGER_REC_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_REC_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(2, receiver->GetNumTimesCalled());
}

TEST(Trigger, SerialBindingFromRigOnlyMatchesSerialSource) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	engine::TriggerParams trigParams;
	auto rigTrigger = io::RigFile::Trigger();
	rigTrigger.Name = "serial";
	rigTrigger.TriggerPairs.push_back({
		0u,
		0u,
		1u,
		1u,
		io::RigFile::TriggerPair::SOURCE_SERIAL,
		"pedal-a"
	});

	auto triggerOpt = Trigger::FromFile(trigParams, rigTrigger);
	ASSERT_TRUE(triggerOpt.has_value());
	auto trigger = triggerOpt.value();
	trigger->SetReceiver(receiver);

	base::Action action;
	action.SetActionTime(GetTime());

	receiver->SetExpected(TriggerAction::TRIGGER_REC_START);
	auto keyboardRes = trigger->OnEvent(engine::TRIGGER_KEY, 0u, 1u, action);
	EXPECT_FALSE(keyboardRes.IsEaten);
	EXPECT_EQ(0, receiver->GetNumTimesCalled());

	auto wrongDeviceRes = trigger->OnEvent(engine::TRIGGER_SERIAL, 0u, 1u, action, "pedal-b");
	EXPECT_FALSE(wrongDeviceRes.IsEaten);
	EXPECT_EQ(0, receiver->GetNumTimesCalled());

	auto serialRes = trigger->OnEvent(engine::TRIGGER_SERIAL, 0u, 1u, action, "pedal-a");
	EXPECT_TRUE(serialRes.IsEaten);
	EXPECT_TRUE(receiver->GetLastMatched());
	EXPECT_EQ(1, receiver->GetNumTimesCalled());
	EXPECT_EQ(actions::ACTIONRESULT_ACTIVATE, serialRes.ResultType);
}

TEST(Trigger, NoReleaseSkipsAction) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	auto trigger = MakeDefaultTrigger(receiver, 0);
	auto action = KeyAction();
	actions::ActionResult actionRes;

	receiver->SetExpected(TriggerAction::TRIGGER_REC_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(1, receiver->GetNumTimesCalled());
}

TEST(Trigger, OverDubReleasingActivateFirst) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	auto trigger = MakeDefaultTrigger(receiver, 0);
	auto action = KeyAction();
	actions::ActionResult actionRes;

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(0, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_OVERDUB_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_PUNCHIN_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_PUNCHIN_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(3, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(3, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_OVERDUB_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(4, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(4, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(4, receiver->GetNumTimesCalled());
}

TEST(Trigger, OverDubReleasingDitchFirst) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	auto trigger = MakeDefaultTrigger(receiver, 0);
	auto action = KeyAction();
	actions::ActionResult actionRes;

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(0, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_OVERDUB_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_OVERDUB_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(2, receiver->GetNumTimesCalled());
}

TEST(Trigger, OverDubNotReleasingActivateBeforeDub) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	auto trigger = MakeDefaultTrigger(receiver, 0);
	auto action = KeyAction();
	actions::ActionResult actionRes;

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(0, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_OVERDUB_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_OVERDUB_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(2, receiver->GetNumTimesCalled());
}


TEST(Trigger, DebounceSimpleTest) {
	auto receiver = std::make_shared<MockedTriggerReceiver>();
	auto debounceMs = 100;
	auto trigger = MakeDefaultTrigger(receiver, debounceMs);
	auto action = KeyAction();
	actions::ActionResult actionRes;
	auto curTime = GetTime();

	receiver->SetExpected(TriggerAction::TRIGGER_REC_START);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	curTime = OffsetTime(curTime, debounceMs/2);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	curTime = OffsetTime(curTime, debounceMs/2);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	curTime = OffsetTime(curTime, debounceMs/2);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(1, receiver->GetNumTimesCalled());

	curTime = OffsetTime(curTime, debounceMs*2);
	receiver->SetExpected(TriggerAction::TRIGGER_REC_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	curTime = OffsetTime(curTime, debounceMs/2);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	curTime = OffsetTime(curTime, debounceMs/2);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(2, receiver->GetNumTimesCalled());

	curTime = OffsetTime(curTime, debounceMs/2);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	action.SetActionTime(curTime);
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(2, receiver->GetNumTimesCalled());
}

TEST(Trigger, EndOverdubPreservesDelayedPunchActions) {
	auto sourceTake = std::make_shared<TestTriggerPunchTarget>();
	auto targetTake = std::make_shared<TestTriggerPunchTarget>();
	auto receiver = std::make_shared<SequenceTriggerReceiver>(sourceTake, targetTake);
	auto trigger = MakeDefaultTrigger(receiver, 0);

	io::UserConfig cfg;
	cfg.Audio = {
		"",
		48000,
		256,
		1000000,
		0,
		2,
		2,
		2
	};
	cfg.Loop = { 0 };
	cfg.Trigger = { 64, 0 };

	auto action = KeyAction();
	action.SetUserConfig(cfg);

	// Start overdub (ditch down + activate down).
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	// Punch in and out: source mute/unmute happens immediately, target-state changes are delayed.
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	trigger->OnAction(action);

	// End overdub (Ditch down + Activate down simultaneously ends overdub).
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	auto actionsBeforeTick = receiver->Actions();
	ASSERT_EQ(4u, actionsBeforeTick.size());
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_START, actionsBeforeTick[0].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_START, actionsBeforeTick[1].ActionType);
	EXPECT_FALSE(actionsBeforeTick[1].ApplyToTargetTake);
	EXPECT_FALSE(actionsBeforeTick[1].ApplyToSourceTake);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_END, actionsBeforeTick[2].ActionType);
	EXPECT_FALSE(actionsBeforeTick[2].ApplyToTargetTake);
	EXPECT_FALSE(actionsBeforeTick[2].ApplyToSourceTake);
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_END, actionsBeforeTick[3].ActionType);
	EXPECT_EQ(1u, sourceTake->MuteCount);
	EXPECT_EQ(1u, sourceTake->UnmuteCount);
	EXPECT_EQ(0u, targetTake->PunchInCount);
	EXPECT_EQ(0u, targetTake->PunchOutCount);

	// Flush delayed audio work; it remains valid after the job-side overdub end.
	trigger->OnTick(GetTime(), cfg.Trigger.PreDelay + constants::MaxLoopFadeSamps, cfg, std::nullopt);
	trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);

	auto actionsAfterTick = receiver->Actions();
	ASSERT_EQ(4u, actionsAfterTick.size());
	EXPECT_EQ(1u, targetTake->PunchInCount);
	EXPECT_EQ(1u, targetTake->PunchOutCount);
}

TEST(Trigger, MixedAudioMidiPunchDelaysAudioTargetButNotMidiTarget) {
	auto sourceTake = std::make_shared<TestTriggerPunchTarget>();
	auto targetTake = std::make_shared<TestTriggerPunchTarget>();
	auto receiver = std::make_shared<SequenceTriggerReceiver>(sourceTake, targetTake);
	auto trigger = MakeDefaultTrigger(receiver, 0);
	trigger->AddInputChannel(0u);
	trigger->AddMidiInputDevice("Keys");

	io::UserConfig cfg;
	cfg.Audio = {
		"",
		48000,
		256,
		1000000,
		0,
		2,
		2,
		2
	};
	cfg.Loop = { 0 };
	cfg.Trigger = { 64, 0 };

	auto action = KeyAction();
	action.SetUserConfig(cfg);

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	trigger->OnAction(action);

	auto actionsBeforeTick = receiver->Actions();
	ASSERT_EQ(3u, actionsBeforeTick.size());
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_START, actionsBeforeTick[0].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_START, actionsBeforeTick[1].ActionType);
	EXPECT_TRUE(actionsBeforeTick[1].ApplyToTargetTake);
	EXPECT_FALSE(actionsBeforeTick[1].ApplyToSourceTake);
	EXPECT_FALSE(actionsBeforeTick[1].ApplyToTargetAudio);
	EXPECT_TRUE(actionsBeforeTick[1].ApplyToTargetMidi);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_END, actionsBeforeTick[2].ActionType);
	EXPECT_TRUE(actionsBeforeTick[2].ApplyToTargetTake);
	EXPECT_FALSE(actionsBeforeTick[2].ApplyToSourceTake);
	EXPECT_FALSE(actionsBeforeTick[2].ApplyToTargetAudio);
	EXPECT_TRUE(actionsBeforeTick[2].ApplyToTargetMidi);
	EXPECT_EQ(1u, sourceTake->MuteCount);
	EXPECT_EQ(1u, sourceTake->UnmuteCount);
	EXPECT_EQ(0u, targetTake->PunchInCount);
	EXPECT_EQ(0u, targetTake->PunchOutCount);

	trigger->OnTick(GetTime(), cfg.Trigger.PreDelay + constants::MaxLoopFadeSamps, cfg, std::nullopt);
	trigger->ProcessStructuralActionsOnJob(cfg, std::nullopt);

	auto actionsAfterTick = receiver->Actions();
	ASSERT_EQ(3u, actionsAfterTick.size());
	EXPECT_EQ(1u, targetTake->PunchInCount);
	EXPECT_EQ(1u, targetTake->PunchOutCount);
}

TEST(Trigger, MidiBindingsDriveRecordAndDitchActions) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"TriggerPad\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(testStream)).value());
	auto trigStruct = io::RigFile::Trigger::FromJson(json);
	ASSERT_TRUE(trigStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto trigger = Trigger::FromFile(trigParams, trigStruct.value());
	ASSERT_TRUE(trigger.has_value());
	trigger.value()->SetReceiver(receiver);

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOff(0u, 0u, 60u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	midi::MidiEvent ccDown{ 0u, 0xB0u, 64u, 127u, 0u };
	midi::MidiEvent ccUp{ 0u, 0xB0u, 64u, 0u, 0u };
	SendMidiEvent(trigger.value(), ccDown, GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	SendMidiEvent(trigger.value(), ccUp, GetTime());
	ASSERT_EQ(3u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH, receiver->Actions()[1].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH_UNMUTE, receiver->Actions()[2].ActionType);
}

TEST(Trigger, NoteOffMidiActivateBindingStartsAndEndsRecordingOnRelease) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"TriggerPad\",\"activate\":{\"kind\":\"noteoff\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(testStream)).value());
	auto trigStruct = io::RigFile::Trigger::FromJson(json);
	ASSERT_TRUE(trigStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto trigger = Trigger::FromFile(trigParams, trigStruct.value());
	ASSERT_TRUE(trigger.has_value());
	trigger.value()->SetReceiver(receiver);

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	EXPECT_TRUE(receiver->Actions().empty());

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOff(0u, 0u, 60u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOff(0u, 0u, 60u), GetTime());
	ASSERT_EQ(2u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_END, receiver->Actions()[1].ActionType);
}

TEST(Trigger, NoteOffMidiDitchBindingCompletesOnNextNoteOn) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"TriggerPad\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"noteoff\",\"channel\":1,\"id\":61}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(testStream)).value());
	auto trigStruct = io::RigFile::Trigger::FromJson(json);
	ASSERT_TRUE(trigStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto trigger = Trigger::FromFile(trigParams, trigStruct.value());
	ASSERT_TRUE(trigger.has_value());
	trigger.value()->SetReceiver(receiver);

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOn(0u, 0u, 61u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOff(0u, 0u, 61u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	SendMidiEvent(trigger.value(), midi::MidiEvent::MakeNoteOn(0u, 0u, 61u, 100u), GetTime());
	ASSERT_EQ(3u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH, receiver->Actions()[1].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH_UNMUTE, receiver->Actions()[2].ActionType);
}


TEST(Trigger, SceneWithoutPublishedRigDoesNotUseStationTriggerFallback) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto firstStation = MakeTestStation("station-a");
	scene.AddStationForTest(firstStation);

	auto secondStation = MakeTestStation("station-b");
	scene.AddStationForTest(secondStation);

	KeyAction action;
	action.KeyActionType = KeyAction::KEY_DOWN;
	action.KeyChar = ActivateChar;
	action.SetActionTime(GetTime());

	auto res = scene.OnAction(action);

	EXPECT_FALSE(res.IsEaten);
	EXPECT_EQ(0u, firstStation->NumTakes());
	EXPECT_EQ(0u, secondStation->NumTakes());
}

TEST(Trigger, FullUiQueueKeepsDroppedActivateReleaseWhenDitchStateArrivesLater)
{
	auto trigger = MakeSharedDefaultTrigger();
	base::Action action;
	const auto start = GetTime();
	for (std::size_t index = 0u; index < 63u; ++index)
	{
		action.SetActionTime(OffsetTime(start, static_cast<unsigned int>(index)));
		ASSERT_TRUE(trigger->QueueExternalControlAction(true, (index % 2u) == 0u, action).IsEaten);
	}
	action.SetActionTime(OffsetTime(start, 64u));
	EXPECT_TRUE(trigger->QueueExternalControlAction(true, false, action).IsEaten);
	action.SetActionTime(OffsetTime(start, 65u));
	EXPECT_TRUE(trigger->QueueExternalControlAction(false, true, action).IsEaten);
	action.SetActionTime(OffsetTime(start, 66u));
	EXPECT_TRUE(trigger->QueueExternalControlAction(false, false, action).IsEaten);
	EXPECT_EQ(3u, trigger->UiInputDropCount());
	trigger->OnTick(GetTime(), 0u, std::nullopt, std::nullopt);
	EXPECT_FALSE(trigger->IsActivateInputDown());
	EXPECT_FALSE(trigger->IsDitchInputDown());
}

TEST(Trigger, FullUiQueueKeepsLatestStateForEachActivateBinding)
{
	auto first = engine::DualBinding(
		engine::TriggerBinding(engine::TRIGGER_KEY, 70u, 1u),
		engine::TriggerBinding(engine::TRIGGER_KEY, 70u, 0u));
	auto second = engine::DualBinding(
		engine::TriggerBinding(engine::TRIGGER_KEY, 71u, 1u),
		engine::TriggerBinding(engine::TRIGGER_KEY, 71u, 0u));
	TriggerParams params;
	params.Activate = { first, second };
	auto trigger = std::make_shared<Trigger>(params);
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	trigger->SetReceiver(receiver);

	base::Action action;
	const auto start = GetTime();
	for (std::size_t index = 0u; index < 62u; ++index)
	{
		action.SetActionTime(OffsetTime(start, static_cast<unsigned int>(index)));
		ASSERT_TRUE(trigger->QueueExternalControlAction(true, false, action, 1u).IsEaten);
	}
	action.SetActionTime(OffsetTime(start, 62u));
	ASSERT_TRUE(trigger->QueueInputEvent(engine::TRIGGER_INPUT_UI, 0u,
		engine::TRIGGER_KEY, 70u, 1u, action).IsEaten);
	action.SetActionTime(OffsetTime(start, 63u));
	ASSERT_TRUE(trigger->QueueInputEvent(engine::TRIGGER_INPUT_UI, 0u,
		engine::TRIGGER_KEY, 70u, 0u, action).IsEaten);
	action.SetActionTime(OffsetTime(start, 64u));
	ASSERT_TRUE(trigger->QueueInputEvent(engine::TRIGGER_INPUT_UI, 0u,
		engine::TRIGGER_KEY, 71u, 1u, action).IsEaten);
	ASSERT_EQ(2u, trigger->UiInputDropCount());

	trigger->OnTick(GetTime(), 0u, std::nullopt, std::nullopt, 0u);
	CompleteQueuedStructuralAction(trigger);
	CompleteQueuedStructuralAction(trigger);
	ASSERT_EQ(2u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_REC_END, receiver->Actions()[1].ActionType);
}

TEST(Trigger, RejectsBindingsBeyondFixedIngressCapacity)
{
	TriggerParams oversized;
	oversized.Activate.resize(Trigger::MaxBindingCount + 1u);
	oversized.Ditch.resize(Trigger::MaxBindingCount + 1u);
	EXPECT_THROW({ Trigger trigger(oversized); }, std::invalid_argument);

	io::RigFile::Trigger fileTrigger;
	fileTrigger.Name = "oversized";
	EXPECT_FALSE(Trigger::FromFile(oversized, fileTrigger).has_value());
}

TEST(Trigger, AddBindingReportsFixedIngressCapacity)
{
	TriggerParams params;
	params.Activate.resize(Trigger::MaxBindingCount);
	params.Ditch.resize(Trigger::MaxBindingCount);
	Trigger trigger(params);
	EXPECT_FALSE(trigger.AddBinding(engine::DualBinding(), engine::DualBinding()));
}

TEST(Scene, PopupTouchUpClearsAnEarlierTouchCapture) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto capturedControl = std::make_shared<gui::GuiButton>(
		MakeSceneButtonParams({ 800, 500 }, { 20u, 20u }));
	scene.AddChild(capturedControl);

	ASSERT_TRUE(scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN,
		{ 810, 510 }, 0, LeftMouseButtonMask)).IsEaten);
	ASSERT_TRUE(scene.HasTouchCaptureForTest());

	auto popup = std::make_shared<gui::GuiButton>(
		MakeSceneButtonParams({ 800, 500 }, { 20u, 20u }));
	scene.OpenPopupForTest(popup);

	EXPECT_TRUE(scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_UP,
		{ 810, 510 }, 0, 0u)).IsEaten);
	EXPECT_FALSE(scene.HasTouchCaptureForTest());
}

TEST(Scene, HoverTargetsOnlyTheTopmostOverlappingGuiElement) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	base::GuiElementParams containerParams;
	containerParams.Position = { 800, 500 };
	containerParams.Size = { 100u, 60u };
	containerParams.MinSize = containerParams.Size;
	containerParams.GuiPassThrough = true;
	auto container = std::make_shared<base::GuiElement>(containerParams);
	auto underneath = std::make_shared<gui::GuiButton>(
		MakeSceneButtonParams({ 0, 0 }, { 80u, 40u }));
	auto topmost = std::make_shared<gui::GuiButton>(
		MakeSceneButtonParams({ 60, 0 }, { 40u, 40u }));
	container->AddChild(underneath);
	container->AddChild(topmost);
	scene.AddChild(container);

	scene.OnAction(MakeSceneTouchMove({ 870, 510 }, 0u));
	scene.ApplyHoverForTest();
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, container->GetState());
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, underneath->GetState());
	EXPECT_EQ(base::GuiElement::STATE_OVER, topmost->GetState());

	scene.OnAction(MakeSceneTouchMove({ 820, 510 }, 0u));
	scene.ApplyHoverForTest();
	EXPECT_EQ(base::GuiElement::STATE_OVER, underneath->GetState());
	EXPECT_EQ(base::GuiElement::STATE_NORMAL, topmost->GetState());
}

TEST(SceneDrag, LeftDragPansCameraDirectly) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto downRes = scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 800, 500 }, 0, LeftMouseButtonMask));
	ASSERT_TRUE(downRes.IsEaten);
	scene.OnAction(MakeSceneTouchMove({ 810, 490 }, LeftMouseButtonMask));

	auto cameraPos = scene.CameraPositionForTest();
	EXPECT_FLOAT_EQ(-10.0f, cameraPos.X);
	EXPECT_FLOAT_EQ(10.0f, cameraPos.Y);
	EXPECT_FLOAT_EQ(420.0f, cameraPos.Z);
}

TEST(SceneDrag, RightDragUsesDampedMotion) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto downRes = scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 800, 500 }, 2, RightMouseButtonMask));
	ASSERT_TRUE(downRes.IsEaten);
	scene.OnAction(MakeSceneTouchMove({ 820, 500 }, RightMouseButtonMask));

	auto cameraPos = scene.CameraPositionForTest();
	EXPECT_LT(cameraPos.X, 0.0f);
	EXPECT_GT(cameraPos.X, -10.0f);
	EXPECT_FLOAT_EQ(0.0f, cameraPos.Y);
	EXPECT_FLOAT_EQ(420.0f, cameraPos.Z);
}

TEST(SceneDrag, InertialDragCoastsAfterMouseStops) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 800, 500 }, 2, RightMouseButtonMask));
	scene.OnAction(MakeSceneTouchMove({ 840, 500 }, RightMouseButtonMask));
	auto cameraAfterMove = scene.CameraPositionForTest();

	scene.TickCameraForTest(256u, 44100u);
	auto cameraAfterCoast = scene.CameraPositionForTest();

	EXPECT_LT(cameraAfterCoast.X, cameraAfterMove.X);

	for (int i = 0; i < 360; ++i)
		scene.TickCameraForTest(256u, 44100u);

	auto cameraAfterSettle = scene.CameraPositionForTest();
	scene.TickCameraForTest(256u, 44100u);
	auto cameraAfterFinalTick = scene.CameraPositionForTest();

	EXPECT_NEAR(cameraAfterSettle.X, cameraAfterFinalTick.X, 0.1f);
	EXPECT_NEAR(cameraAfterSettle.Y, cameraAfterFinalTick.Y, 0.1f);
}

TEST(SceneDrag, RightOverrideAndReleaseStayContinuous) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 800, 500 }, 0, LeftMouseButtonMask));
	scene.OnAction(MakeSceneTouchMove({ 810, 500 }, LeftMouseButtonMask));
	auto leftCameraPos = scene.CameraPositionForTest();
	EXPECT_FLOAT_EQ(-10.0f, leftCameraPos.X);

	auto rightDownRes = scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 810, 500 }, 2, LeftMouseButtonMask | RightMouseButtonMask));
	ASSERT_TRUE(rightDownRes.IsEaten);
	auto cameraAfterRightDown = scene.CameraPositionForTest();
	EXPECT_FLOAT_EQ(leftCameraPos.X, cameraAfterRightDown.X);

	scene.OnAction(MakeSceneTouchMove({ 830, 500 }, LeftMouseButtonMask | RightMouseButtonMask));
	auto inertialCameraPos = scene.CameraPositionForTest();
	EXPECT_LT(inertialCameraPos.X, cameraAfterRightDown.X);

	scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_UP, { 830, 500 }, 2, LeftMouseButtonMask));
	auto cameraAfterRightUp = scene.CameraPositionForTest();
	EXPECT_NEAR(inertialCameraPos.X, cameraAfterRightUp.X, 0.001f);

	scene.OnAction(MakeSceneTouchMove({ 840, 500 }, LeftMouseButtonMask));
	auto blendedCameraPos = scene.CameraPositionForTest();
	EXPECT_LT(blendedCameraPos.X, cameraAfterRightUp.X);
	EXPECT_GT(blendedCameraPos.X, cameraAfterRightUp.X - 10.0f);
}

TEST(SceneDrag, FinalReleaseEndsBackgroundDrag) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 800, 500 }, 0, LeftMouseButtonMask));
	ASSERT_TRUE(scene.IsSceneTouchingForTest());

	scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_UP, { 800, 500 }, 0, 0u));
	EXPECT_FALSE(scene.IsSceneTouchingForTest());
}

TEST(CameraView, WheelZoomAtViewportCentreUsesNotches) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto wheelRes = scene.OnAction(MakeSceneWheel(1));
	ASSERT_TRUE(wheelRes.IsEaten);
	ASSERT_TRUE(scene.IsCameraTransitioningForTest());
	scene.SettleCameraForTest();

	auto cameraPos = scene.CameraPositionForTest();
	const auto expectedFrontZoomStep = 150.0f
		+ (350.0f * (420.0f - 80.0f) / (1350.0f - 80.0f));
	EXPECT_NEAR(420.0f - expectedFrontZoomStep, cameraPos.Z, 0.001f);
	EXPECT_FLOAT_EQ(0.0f, cameraPos.X);
	EXPECT_FLOAT_EQ(0.0f, cameraPos.Y);
}

TEST(CameraView, FrontWheelZoomKeepsCursorFocusUnderPointer) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	const utils::Position2d cursor{ 1050, 260 };
	const auto focus = scene.CameraFocusAtCursorForTest(cursor);
	EXPECT_LT(focus.Y, 0.0f);

	scene.OnAction(MakeSceneWheel(1, cursor));
	scene.TickCameraForTest(4410u, 44100u);
	const auto midTransitionFocus = scene.ProjectWorldPositionForTest(focus);
	EXPECT_NEAR(static_cast<float>(cursor.X), midTransitionFocus.x, 0.01f);
	EXPECT_NEAR(static_cast<float>(cursor.Y), midTransitionFocus.y, 0.01f);
	scene.SettleCameraForTest();
	const auto projectedFocus = scene.ProjectWorldPositionForTest(focus);
	EXPECT_NEAR(static_cast<float>(cursor.X), projectedFocus.x, 0.01f);
	EXPECT_NEAR(static_cast<float>(cursor.Y), projectedFocus.y, 0.01f);
}

TEST(CameraView, WheelZoomPreservesFrontPanPosition) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 800, 500 }, 0, LeftMouseButtonMask));
	scene.OnAction(MakeSceneTouchMove({ 810, 490 }, LeftMouseButtonMask));
	scene.OnAction(MakeSceneWheel(1));
	scene.SettleCameraForTest();

	auto cameraPos = scene.CameraPositionForTest();
	EXPECT_FLOAT_EQ(-10.0f, cameraPos.X);
	EXPECT_FLOAT_EQ(10.0f, cameraPos.Y);
	const auto expectedFrontZoomStep = 150.0f
		+ (350.0f * (420.0f - 80.0f) / (1350.0f - 80.0f));
	EXPECT_NEAR(420.0f - expectedFrontZoomStep, cameraPos.Z, 0.001f);
}

TEST(CameraView, TopDownWheelZoomKeepsOrthographicProjection) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	ASSERT_EQ(graphics::Camera::View::TopDown, scene.CameraViewForTest());
	ASSERT_FLOAT_EQ(1.0f, scene.CameraProjectionForTest()[3][3]);

	scene.OnAction(MakeSceneWheel(1));
	EXPECT_FLOAT_EQ(1.0f, scene.CameraProjectionForTest()[3][3]);
	scene.SettleCameraForTest();
	EXPECT_EQ(graphics::Camera::View::TopDown, scene.CameraViewForTest());
	EXPECT_FLOAT_EQ(-1.0f, scene.CameraPoseForTest().Forward.Y);
	const auto expectedTopDownZoomStep = 150.0f
		+ (350.0f * (800.0f - 280.0f) / (3200.0f - 280.0f));
	EXPECT_NEAR(800.0f - expectedTopDownZoomStep, scene.CameraPositionForTest().Y, 0.001f);
}

TEST(CameraView, TopDownWheelZoomKeepsCursorFocusUnderPointer) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	const utils::Position2d cursor{ 1020, 300 };
	const auto focus = scene.CameraFocusAtCursorForTest(cursor);
	EXPECT_GT(focus.Z, 0.0f);

	scene.OnAction(MakeSceneWheel(1, cursor));
	scene.TickCameraForTest(4410u, 44100u);
	const auto midTransitionFocus = scene.ProjectWorldPositionForTest(focus);
	EXPECT_NEAR(static_cast<float>(cursor.X), midTransitionFocus.x, 0.01f);
	EXPECT_NEAR(static_cast<float>(cursor.Y), midTransitionFocus.y, 0.01f);
	scene.SettleCameraForTest();
	const auto projectedFocus = scene.ProjectWorldPositionForTest(focus);
	EXPECT_NEAR(static_cast<float>(cursor.X), projectedFocus.x, 0.01f);
	EXPECT_NEAR(static_cast<float>(cursor.Y), projectedFocus.y, 0.01f);
}

TEST(CameraView, StationInteriorWheelChangesAndRemembersFieldOfView) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	ASSERT_EQ(graphics::Camera::View::StationInterior, scene.CameraViewForTest());
	const auto positionBeforeWheel = scene.CameraPositionForTest();

	const auto wheelResult = scene.OnAction(MakeSceneWheel(1));
	ASSERT_TRUE(wheelResult.IsEaten);
	EXPECT_FLOAT_EQ(72.0f, scene.StationInteriorFieldOfViewForTest());
	EXPECT_FLOAT_EQ(positionBeforeWheel.X, scene.CameraPositionForTest().X);
	EXPECT_FLOAT_EQ(positionBeforeWheel.Y, scene.CameraPositionForTest().Y);
	EXPECT_FLOAT_EQ(positionBeforeWheel.Z, scene.CameraPositionForTest().Z);
	EXPECT_FLOAT_EQ(0.0f, scene.CameraProjectionForTest()[3][3]);

	scene.OnAction(tab);
	scene.SettleCameraForTest();
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	EXPECT_EQ(graphics::Camera::View::StationInterior, scene.CameraViewForTest());
	EXPECT_FLOAT_EQ(72.0f, scene.StationInteriorFieldOfViewForTest());
}

TEST(CameraView, TabCyclesFrontInteriorAndTopDown) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	ASSERT_TRUE(scene.OnAction(tab).IsEaten);
	scene.SettleCameraForTest();
	EXPECT_EQ(graphics::Camera::View::StationInterior, scene.CameraViewForTest());
	EXPECT_FLOAT_EQ(1.0f, scene.CameraPoseForTest().Forward.Z);

	ASSERT_TRUE(scene.OnAction(tab).IsEaten);
	scene.SettleCameraForTest();
	EXPECT_EQ(graphics::Camera::View::TopDown, scene.CameraViewForTest());
	EXPECT_FLOAT_EQ(-1.0f, scene.CameraPoseForTest().Forward.Y);

	ASSERT_TRUE(scene.OnAction(tab).IsEaten);
	EXPECT_EQ(graphics::Camera::View::Front, scene.CameraViewForTest());
}

TEST(CameraView, RapidTabCyclesSettleAtTheLatestViewTarget) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	for (unsigned int press = 0u; press < 6u; ++press)
	{
		ASSERT_TRUE(scene.OnAction(tab).IsEaten);
		scene.TickCameraForTest(4410u, 44100u);
	}

	scene.SettleCameraForTest();
	const auto pose = scene.CameraPoseForTest();
	EXPECT_EQ(graphics::Camera::View::Front, scene.CameraViewForTest());
	EXPECT_FLOAT_EQ(420.0f, pose.Eye.Z);
	EXPECT_FLOAT_EQ(-1.0f, pose.Forward.Z);
	EXPECT_FLOAT_EQ(1.0f, pose.Up.Y);
}

TEST(CameraView, StationInteriorTemporarilyForcesLoopTakeSelectDepth) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	EXPECT_EQ(Scene::VIEW_LOOPTAKE, scene.CameraSelectDepthForTest());

	scene.OnAction(tab);
	scene.SettleCameraForTest();
	scene.UpdateCamera();
	EXPECT_EQ(Scene::VIEW_STATION, scene.CameraSelectDepthForTest());
}

TEST(CameraView, StationInteriorKeepsUserSelectedLoopDepth) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	scene.SetCameraSelectDepthForTest(Scene::VIEW_LOOP);
	scene.OnAction(tab);

	EXPECT_EQ(Scene::VIEW_LOOP, scene.CameraSelectDepthForTest());
}

TEST(CameraView, TopDownVerticalDragMovesPositiveWorldZ) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	scene.SettleCameraForTest();
	scene.OnAction(tab);
	scene.SettleCameraForTest();

	scene.OnAction(MakeSceneTouch(TouchAction::TOUCH_DOWN, { 800, 500 }, 0, LeftMouseButtonMask));
	scene.OnAction(MakeSceneTouchMove({ 800, 510 }, LeftMouseButtonMask));
	EXPECT_FLOAT_EQ(10.0f, scene.CameraPositionForTest().Z);
}

TEST(CameraView, StationInteriorFollowsLoopTakeAddition) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto firstStation = MakeTestStation("station-a");
	firstStation->SetModelPosition({ -200.0f, 0.0f, 0.0f });
	scene.AddStationForTest(firstStation);
	auto secondStation = MakeTestStation("station-b");
	secondStation->SetModelPosition({ 300.0f, 0.0f, 0.0f });
	scene.AddStationForTest(secondStation);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	secondStation->AddTake();
	scene.UpdateCameraStationFollowForTest();
	scene.SettleCameraForTest();

	EXPECT_EQ(graphics::Camera::View::StationInterior, scene.CameraViewForTest());
	EXPECT_FLOAT_EQ(300.0f, scene.CameraPositionForTest().X);
	EXPECT_FLOAT_EQ(0.0f, scene.CameraPositionForTest().Z);
}

TEST(CameraView, StationInteriorClearsFocusWhenTrackedStationIsRemoved) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto firstStation = MakeTestStation("station-a");
	firstStation->SetModelPosition({ -200.0f, 0.0f, 0.0f });
	scene.AddStationForTest(firstStation);
	auto removedStation = MakeTestStation("station-b");
	removedStation->SetModelPosition({ 300.0f, 0.0f, 0.0f });
	scene.AddStationForTest(removedStation);
	auto shiftedStation = MakeTestStation("station-c");
	shiftedStation->SetModelPosition({ 600.0f, 0.0f, 0.0f });
	scene.AddStationForTest(shiftedStation);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	removedStation->AddTake();
	scene.UpdateCameraStationFollowForTest();
	scene.SettleCameraForTest();
	ASSERT_FLOAT_EQ(300.0f, scene.CameraPositionForTest().X);

	scene.RemoveStationForTest(removedStation);
	scene.UpdateCameraStationFollowForTest();
	scene.SettleCameraForTest();
	EXPECT_FLOAT_EQ(-200.0f, scene.CameraPositionForTest().X);

	shiftedStation->AddTake();
	scene.UpdateCameraStationFollowForTest();
	scene.SettleCameraForTest();
	EXPECT_FLOAT_EQ(600.0f, scene.CameraPositionForTest().X);
}

TEST(CameraView, StationInteriorKeepsFocusWhenEarlierStationIsRemoved) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams({ 1400u, 900u }) };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto removedStation = MakeTestStation("station-a");
	scene.AddStationForTest(removedStation);
	auto trackedStation = MakeTestStation("station-b");
	trackedStation->SetModelPosition({ 300.0f, 0.0f, 0.0f });
	scene.AddStationForTest(trackedStation);

	KeyAction tab;
	tab.KeyChar = 9u;
	tab.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(tab);
	trackedStation->AddTake();
	scene.UpdateCameraStationFollowForTest();
	scene.SettleCameraForTest();
	ASSERT_FLOAT_EQ(300.0f, scene.CameraPositionForTest().X);

	scene.RemoveStationForTest(removedStation);
	scene.UpdateCameraStationFollowForTest();
	scene.SettleCameraForTest();
	EXPECT_FLOAT_EQ(300.0f, scene.CameraPositionForTest().X);
}

TEST(CameraView, StationInteriorClearsHoveredFocusWhenStationIsRemoved) {
	graphics::Camera camera(graphics::CameraParams(base::MoveableParams(), 0u));
	const auto firstIdentity = std::make_shared<int>(1);
	const auto hoveredIdentity = std::make_shared<int>(2);
	const utils::Position3d firstPosition{ -200.0f, 0.0f, 0.0f };
	const utils::Position3d hoveredPosition{ 300.0f, 0.0f, 0.0f };
	camera.RegisterStation(0u, firstIdentity, 0u);
	camera.RegisterStation(1u, hoveredIdentity, 0u);
	camera.CycleView({}, hoveredPosition, firstPosition, hoveredIdentity, firstIdentity, true);

	camera.CompleteStationObservation(1u, firstPosition);
	for (unsigned int tick = 0u; tick < 8u; ++tick)
		camera.TickBackgroundDrag(0.05f);
	EXPECT_FLOAT_EQ(-200.0f, camera.CurrentPose().Eye.X);
}

TEST(CameraView, StationInteriorObservesRevisionWhenStationShiftsIndex) {
	graphics::Camera camera(graphics::CameraParams(base::MoveableParams(), 0u));
	const auto removedIdentity = std::make_shared<int>(1);
	const auto shiftedIdentity = std::make_shared<int>(2);
	const utils::Position3d shiftedPosition{ 300.0f, 0.0f, 0.0f };
	camera.RegisterStation(0u, removedIdentity, 0u);
	camera.RegisterStation(1u, shiftedIdentity, 0u);
	camera.CycleView({}, {}, {}, {}, {}, true);

	camera.ObserveStation(0u, shiftedIdentity, 1u, shiftedPosition);
	camera.CompleteStationObservation(1u, shiftedPosition);
	for (unsigned int tick = 0u; tick < 8u; ++tick)
		camera.TickBackgroundDrag(0.05f);
	EXPECT_FLOAT_EQ(300.0f, camera.CurrentPose().Eye.X);
}

TEST(CameraView, StationResetUpdatesLoopTakeRevision) {
	auto station = MakeTestStation("station-reset");
	station->AddTake();
	const auto revision = station->LoopTakeRevision();
	station->Reset();
	EXPECT_GT(station->LoopTakeRevision(), revision);
	EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
	EXPECT_EQ(0u, station->NumTakes());
	station->CommitChanges();
	EXPECT_TRUE(station->GetLoopTakeSnapshot().empty());
	EXPECT_EQ(0u, station->NumTakes());
}

TEST(Trigger, TriggerFromFileRejectsInvalidMidiBindingSpecsFromNonJsonCallers) {
	io::RigFile::Trigger trigStruct{};
	trigStruct.Name = "TrigMidi";
	trigStruct.StationType = 0u;
	trigStruct.MidiTrigger = io::RigFile::Trigger::MidiTriggerBinding{};
	trigStruct.MidiTrigger->Device = "default";
	trigStruct.MidiTrigger->Activate = {
		io::RigFile::MidiTriggerEvent::NOTE,
		0u,
		128u,
		1u,
		false
	};
	trigStruct.MidiTrigger->Ditch = {
		io::RigFile::MidiTriggerEvent::CC,
		0u,
		64u,
		1u,
		false
	};

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto trigger = Trigger::FromFile(trigParams, trigStruct);

	EXPECT_FALSE(trigger.has_value());
}

// Regression: trigger-driven engine mutation from the job thread (MIDI/serial

// ---- Scene reset tests: key, MIDI, and serial trigger paths -------------

TEST(SceneReset, ConnectedEmptyPreservesTimingAndEachDisconnectClearsOnce) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.StopJobForTest();

	scene.SeedTimingForTest();
	scene.ObserveTimingAvailabilityForTest(true, 1u);
	scene.OnJobTick(GetTime());
	EXPECT_TRUE(scene.HasTimingForTest());
	EXPECT_FALSE(scene.IsSceneResetForTest());

	scene.ObserveTimingAvailabilityForTest(false, 1u);
	scene.OnJobTick(GetTime());
	EXPECT_FALSE(scene.HasTimingForTest());
	EXPECT_TRUE(scene.IsSceneResetForTest());

	scene.SeedTimingForTest();
	ASSERT_TRUE(scene.HasTimingForTest());
	scene.OnJobTick(GetTime());
	EXPECT_TRUE(scene.HasTimingForTest());

	// A fresh physical epoch while still empty preserves newly accepted timing,
	// and its later loss forms one new clear edge.
	scene.ObserveTimingAvailabilityForTest(true, 2u);
	scene.OnJobTick(GetTime());
	EXPECT_TRUE(scene.HasTimingForTest());
	EXPECT_FALSE(scene.IsSceneResetForTest());
	scene.ObserveTimingAvailabilityForTest(false, 2u);
	scene.OnJobTick(GetTime());
	EXPECT_FALSE(scene.HasTimingForTest());
	EXPECT_TRUE(scene.IsSceneResetForTest());
}

// FAILS before fix: _DispatchMidiTriggerEvent never called Reset() when ditch
