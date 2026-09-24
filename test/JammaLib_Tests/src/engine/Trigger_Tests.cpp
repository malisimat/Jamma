
#include "gtest/gtest.h"
#include <sstream>
#include "resources/ResourceLib.h"
#include "midi/MidiEvent.h"
#include "engine/LoopTake.h"
#include "engine/Scene.h"
#include "engine/Station.h"
#include "engine/Trigger.h"
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
	virtual actions::ActionResult OnAction(actions::TriggerAction action)
	{
		_actions.push_back(action);

		return {
			true,
			"source-loop-take",
			"target-loop-take",
			actions::ACTIONRESULT_DEFAULT,
			nullptr,
			std::weak_ptr<base::GuiElement>()
		};
	}

	const std::vector<TriggerAction>& Actions() const
	{
		return _actions;
	}

private:
	std::vector<TriggerAction> _actions;
};

class ConfigurableTriggerReceiver :
	public ActionReceiver
{
public:
	explicit ConfigurableTriggerReceiver(bool eat = true) :
		_eat(eat)
	{
	}

	virtual actions::ActionResult OnAction(actions::TriggerAction action)
	{
		_actions.push_back(action);
		return {
			_eat,
			"source-loop-take",
			"target-loop-take",
			actions::ACTIONRESULT_DEFAULT,
			nullptr,
			std::weak_ptr<base::GuiElement>()
		};
	}

	const std::vector<TriggerAction>& Actions() const
	{
		return _actions;
	}

private:
	bool _eat;
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

	bool IsSceneResetForTest() const
	{
		return _isSceneReset.load(std::memory_order_relaxed);
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
		for (size_t index = 0u; index < _stations.size(); ++index)
			_camera.ObserveStation(index, _stations[index]->LoopTakeRevision(), _stations[index]->ModelPosition());
	}

	void TickCameraForTest(unsigned int samps, unsigned int sampleRate)
	{
		_camera.TickBackgroundDrag(samps, sampleRate);
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

	auto activateDown = trigger->QueueExternalControlAction(true, true, action);
	ASSERT_TRUE(activateDown.IsEaten);
	ASSERT_EQ(actions::ACTIONRESULT_ACTIVATE, activateDown.ResultType);
	ASSERT_TRUE(receiver->Actions().empty());
	trigger->OnTick(GetTime(), 0u, std::nullopt, std::nullopt);
	ASSERT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);
	ASSERT_EQ(engine::TRIGSTATE_RECORDING, trigger->GetState());
	ASSERT_TRUE(trigger->IsActivateInputDown());
	ASSERT_TRUE(trigger->QueueExternalControlAction(true, false, action).IsEaten);
	trigger->OnTick(GetTime(), 0u, std::nullopt, std::nullopt);
	ASSERT_FALSE(trigger->IsActivateInputDown());

	auto ditchDown = trigger->QueueExternalControlAction(false, true, action);
	ASSERT_TRUE(ditchDown.IsEaten);
	trigger->OnTick(GetTime(), 0u, std::nullopt, std::nullopt);
	ASSERT_TRUE(trigger->IsDitchDown());
	ASSERT_TRUE(trigger->IsDitchInputDown());

	auto ditchUp = trigger->QueueExternalControlAction(false, false, action);
	ASSERT_TRUE(ditchUp.IsEaten);
	ASSERT_EQ(actions::ACTIONRESULT_DITCH, ditchUp.ResultType);
	trigger->OnTick(GetTime(), 0u, std::nullopt, std::nullopt);
	ASSERT_EQ(TriggerAction::TRIGGER_DITCH, receiver->Actions()[1].ActionType);
	ASSERT_EQ(TriggerAction::TRIGGER_DITCH_UNMUTE, receiver->Actions()[2].ActionType);
	ASSERT_EQ(engine::TRIGSTATE_DEFAULT, trigger->GetState());
	ASSERT_FALSE(trigger->IsActivateInputDown());
	ASSERT_FALSE(trigger->IsDitchInputDown());
	ASSERT_FALSE(trigger->IsDitchDown());
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
	ASSERT_EQ(3, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_PUNCHIN_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(5, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(5, receiver->GetNumTimesCalled());

	receiver->SetExpected(TriggerAction::TRIGGER_OVERDUB_END);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	actionRes = trigger->OnAction(action);
	ASSERT_TRUE(receiver->GetLastMatched());
	ASSERT_EQ(6, receiver->GetNumTimesCalled());

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(6, receiver->GetNumTimesCalled());

	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	actionRes = trigger->OnAction(action);
	ASSERT_EQ(6, receiver->GetNumTimesCalled());
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
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
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
	EXPECT_TRUE(actionsBeforeTick[1].ApplyToSourceTake);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_END, actionsBeforeTick[2].ActionType);
	EXPECT_FALSE(actionsBeforeTick[2].ApplyToTargetTake);
	EXPECT_TRUE(actionsBeforeTick[2].ApplyToSourceTake);
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_END, actionsBeforeTick[3].ActionType);

	// Flush delayed queues; target-side punch actions should still be emitted after overdub ends.
	trigger->OnTick(GetTime(), cfg.Trigger.PreDelay + constants::MaxLoopFadeSamps, cfg, std::nullopt);

	auto actionsAfterTick = receiver->Actions();
	ASSERT_EQ(6u, actionsAfterTick.size());
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_START, actionsAfterTick[4].ActionType);
	EXPECT_TRUE(actionsAfterTick[4].ApplyToTargetTake);
	EXPECT_FALSE(actionsAfterTick[4].ApplyToSourceTake);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_END, actionsAfterTick[5].ActionType);
	EXPECT_TRUE(actionsAfterTick[5].ApplyToTargetTake);
	EXPECT_FALSE(actionsAfterTick[5].ApplyToSourceTake);
}

TEST(Trigger, MixedAudioMidiPunchDelaysAudioTargetButNotMidiTarget) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
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
	ASSERT_EQ(5u, actionsBeforeTick.size());
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_START, actionsBeforeTick[0].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_START, actionsBeforeTick[1].ActionType);
	EXPECT_FALSE(actionsBeforeTick[1].ApplyToTargetTake);
	EXPECT_TRUE(actionsBeforeTick[1].ApplyToSourceTake);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_START, actionsBeforeTick[2].ActionType);
	EXPECT_TRUE(actionsBeforeTick[2].ApplyToTargetTake);
	EXPECT_FALSE(actionsBeforeTick[2].ApplyToSourceTake);
	EXPECT_FALSE(actionsBeforeTick[2].ApplyToTargetAudio);
	EXPECT_TRUE(actionsBeforeTick[2].ApplyToTargetMidi);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_END, actionsBeforeTick[3].ActionType);
	EXPECT_FALSE(actionsBeforeTick[3].ApplyToTargetTake);
	EXPECT_TRUE(actionsBeforeTick[3].ApplyToSourceTake);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_END, actionsBeforeTick[4].ActionType);
	EXPECT_TRUE(actionsBeforeTick[4].ApplyToTargetTake);
	EXPECT_FALSE(actionsBeforeTick[4].ApplyToSourceTake);
	EXPECT_FALSE(actionsBeforeTick[4].ApplyToTargetAudio);
	EXPECT_TRUE(actionsBeforeTick[4].ApplyToTargetMidi);

	trigger->OnTick(GetTime(), cfg.Trigger.PreDelay + constants::MaxLoopFadeSamps, cfg, std::nullopt);

	auto actionsAfterTick = receiver->Actions();
	ASSERT_EQ(7u, actionsAfterTick.size());
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_START, actionsAfterTick[5].ActionType);
	EXPECT_TRUE(actionsAfterTick[5].ApplyToTargetTake);
	EXPECT_FALSE(actionsAfterTick[5].ApplyToSourceTake);
	EXPECT_TRUE(actionsAfterTick[5].ApplyToTargetAudio);
	EXPECT_FALSE(actionsAfterTick[5].ApplyToTargetMidi);
	EXPECT_EQ(TriggerAction::TRIGGER_PUNCHIN_END, actionsAfterTick[6].ActionType);
	EXPECT_TRUE(actionsAfterTick[6].ApplyToTargetTake);
	EXPECT_FALSE(actionsAfterTick[6].ApplyToSourceTake);
	EXPECT_TRUE(actionsAfterTick[6].ApplyToTargetAudio);
	EXPECT_FALSE(actionsAfterTick[6].ApplyToTargetMidi);
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


TEST(Trigger, KeySceneActionHitsAllMatchingTriggers) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto firstStation = MakeTestStation("station-a");
	firstStation->AddTrigger(MakeSharedDefaultTrigger());
	firstStation->AddTrigger(MakeSharedDefaultTrigger());
	scene.AddStationForTest(firstStation);

	auto secondStation = MakeTestStation("station-b");
	secondStation->AddTrigger(MakeSharedDefaultTrigger());
	scene.AddStationForTest(secondStation);

	KeyAction action;
	action.KeyActionType = KeyAction::KEY_DOWN;
	action.KeyChar = ActivateChar;
	action.SetActionTime(GetTime());

	auto res = scene.OnAction(action);

	ASSERT_TRUE(res.IsEaten);
	EXPECT_EQ(2u, firstStation->NumTakes());
	EXPECT_EQ(1u, secondStation->NumTakes());
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

	scene.OnTick(GetTime(), 256u, std::nullopt, std::nullopt);
	auto cameraAfterCoast = scene.CameraPositionForTest();

	EXPECT_LT(cameraAfterCoast.X, cameraAfterMove.X);

	for (int i = 0; i < 360; ++i)
		scene.OnTick(GetTime(), 256u, std::nullopt, std::nullopt);

	auto cameraAfterSettle = scene.CameraPositionForTest();
	scene.OnTick(GetTime(), 256u, std::nullopt, std::nullopt);
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
	EXPECT_FLOAT_EQ(170.0f, cameraPos.Z);
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
	EXPECT_FLOAT_EQ(170.0f, cameraPos.Z);
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
	EXPECT_FLOAT_EQ(550.0f, scene.CameraPositionForTest().Y);
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
	scene.OnTick(GetTime(), 0u, std::nullopt, std::nullopt);
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

// ---- Scene reset tests -------------------------------------------------
// Tests 1-3: regression (key-trigger paths that already work).
// Tests 4-5: MIDI and serial activate paths - FAIL before the fix because
//            _DispatchMidiTriggerEvent and _PumpSerial never set
//            _isSceneReset = false on ACTIONRESULT_ACTIVATE.

TEST(SceneReset, KeyTriggerDitchWhileRecording_ResetsScene) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto station = MakeTestStation();
	station->AddTrigger(MakeSharedDefaultTrigger());
	scene.AddStationForTest(station);

	// Activate: start recording
	KeyAction action;
	action.SetActionTime(GetTime());
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	scene.OnAction(action);

	EXPECT_FALSE(scene.IsSceneResetForTest());
	EXPECT_EQ(1u, station->NumTakes());

	// Commit so _TryGetTake can find the take by ID in _loopTakes
	station->CommitChanges();

	// Ditch key down then up: fires Ditch(), removes take
	action.SetActionTime(GetTime());
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	scene.OnAction(action);

	action.SetActionTime(GetTime());
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(action);

	EXPECT_EQ(0u, station->NumTakes());
	EXPECT_TRUE(scene.IsSceneResetForTest());
}

TEST(SceneReset, KeyTriggerDebouncedDitch_ResetsSceneViaOnTick) {
	constexpr unsigned int debounceMs = 100u;

	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto station = MakeTestStation();
	station->AddTrigger(MakeSharedDefaultTrigger(debounceMs));
	scene.AddStationForTest(station);

	auto curTime = GetTime();

	// Activate
	KeyAction action;
	action.SetActionTime(curTime);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	scene.OnAction(action);

	EXPECT_FALSE(scene.IsSceneResetForTest());
	EXPECT_EQ(1u, station->NumTakes());

	// Commit so _TryGetTake can find the take by ID in _loopTakes
	station->CommitChanges();

	// Ditch DOWN: first ditch is debounce-bypassed, sets _isDitchDown
	action.SetActionTime(curTime);
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	scene.OnAction(action);

	// Ditch UP immediately (same timestamp): within debounce window, deferred
	action.SetActionTime(curTime);
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(action);

	// Take still present: deferred ditch has not fired yet
	EXPECT_EQ(1u, station->NumTakes());
	EXPECT_FALSE(scene.IsSceneResetForTest());

	// Simulate audio tick well past debounce window: deferred ditch fires,
	// then scene sees zero takes and clears timing state
	scene.OnTick(OffsetTime(curTime, debounceMs * 2), 256u, std::nullopt, std::nullopt);

	EXPECT_EQ(0u, station->NumTakes());
	EXPECT_TRUE(scene.IsSceneResetForTest());
}

TEST(SceneReset, KeyTriggerDitchInOverdub_ResetsScene) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	auto station = MakeTestStation();
	station->AddTrigger(MakeSharedDefaultTrigger());
	scene.AddStationForTest(station);

	auto curTime = GetTime();
	KeyAction action;

	// Hold ditch, then press activate: starts overdub from DEFAULT state (no
	// prior recording), adding the very first take via TRIGGER_OVERDUB_START
	action.SetActionTime(curTime);
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	scene.OnAction(action);

	action.SetActionTime(curTime);
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	scene.OnAction(action);

	// The overdub start returns ACTIONRESULT_ACTIVATE, so _isSceneReset = false
	EXPECT_FALSE(scene.IsSceneResetForTest());
	EXPECT_EQ(1u, station->NumTakes());

	// Commit so _TryGetTake can find the take by ID in _loopTakes
	station->CommitChanges();

	// While in OVERDUBBING state, press ditch again then release: calls Ditch(),
	// removing the only take and triggering scene reset
	action.SetActionTime(curTime);
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	scene.OnAction(action);

	action.SetActionTime(curTime);
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_UP;
	scene.OnAction(action);

	EXPECT_EQ(0u, station->NumTakes());
	EXPECT_TRUE(scene.IsSceneResetForTest());
}

// FAILS before fix: _DispatchMidiTriggerEvent never called Reset() when ditch
