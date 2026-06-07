
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
using engine::Timer;
using actions::TriggerAction;
using actions::KeyAction;
using audio::MergeMixBehaviourParams;

const unsigned int ActivateChar = 49;
const unsigned int DitchChar = 50;

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

	void RegisterMidiTriggerRouteForTest(const std::string& deviceName,
		const std::shared_ptr<Trigger>& trigger,
		std::uint8_t deviceSlot)
	{
		_RegisterMidiTriggerRoute(deviceName, trigger);
		_midiTriggerRoutes.back().DeviceSlot = deviceSlot;
		_PublishMidiTriggerRoutes();
	}

	void AddMidiInputDeviceForTest(const std::string& deviceName,
		std::uint8_t deviceSlot)
	{
		auto currentInputs = _midiInputs.load(std::memory_order_acquire);
		auto updatedInputs = std::make_shared<std::vector<std::shared_ptr<MidiInputEndpoint>>>();
		if (currentInputs)
			updatedInputs->insert(updatedInputs->end(), currentInputs->begin(), currentInputs->end());

		auto endpoint = std::make_shared<MidiInputEndpoint>();
		endpoint->DeviceSlot = deviceSlot;
		endpoint->ConfiguredName = deviceName;
		updatedInputs->push_back(endpoint);

		_midiInputs.store(updatedInputs, std::memory_order_release);
	}

	void PushMainMidiEventForTest(std::uint8_t status,
		std::uint8_t data1,
		std::uint8_t data2,
		unsigned int sampleRate = 0u)
	{
		auto inputs = _midiInputs.load(std::memory_order_acquire);
		if (!inputs || inputs->empty())
			AddMidiInputDeviceForTest("default", 0u);

		_PushMidiEvent(0u, status, data1, data2, sampleRate);
	}

	void PushMidiEventForTest(std::uint8_t deviceSlot,
		std::uint8_t status,
		std::uint8_t data1,
		std::uint8_t data2,
		unsigned int sampleRate = 0u)
	{
		_PushMidiEvent(deviceSlot, status, data1, data2, sampleRate);
	}

	void PumpMidiForTest()
	{
		_PumpMidi();
	}

	void DispatchMidiTriggerEventForTest(std::uint8_t deviceSlot,
		const midi::MidiEvent& event)
	{
		_DispatchMidiTriggerEvent(deviceSlot, event);
	}

	void PushSerialTriggerEventForTest(const std::string& deviceName,
		unsigned int buttonIndex,
		bool isPressed)
	{
		_testSerialDeviceName = deviceName;
		io::SerialTriggerEvent event{};
		event.Device = &_testSerialDeviceName;
		event.ButtonIndex = buttonIndex;
		event.IsPressed = isPressed;

		std::scoped_lock lock(_serialIngressMutex);
		_serialIngress.Push(event);
	}

	void PumpSerialForTest()
	{
		_PumpSerial();
	}

	std::mutex& AudioMutexForTest()
	{
		return _audioMutex;
	}

	bool IsSceneResetForTest() const
	{
		return _isSceneReset.load(std::memory_order_relaxed);
	}

private:
	std::string _testSerialDeviceName;
};

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
	trigger->AddMidiInputChannel(3u);
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

TEST(Trigger, SharedMainMidiIngressStillRecordsLoopMidiWhenTriggerEatsEvent) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"midiinput\":[1],\"midiinputdevices\":[\"default\",\"Aux Keys\"],\"trigger\":{\"type\":\"midi\",\"device\":\"default\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(testStream)).value());
	auto trigStruct = io::RigFile::Trigger::FromJson(json);
	ASSERT_TRUE(trigStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto trigger = Trigger::FromFile(trigParams, trigStruct.value());
	ASSERT_TRUE(trigger.has_value());
	trigger.value()->SetReceiver(receiver);

	auto take = MakeTestLoopTake();
	take->Record({}, "station", { 0u });
	ASSERT_TRUE(take->IsArmed());

	auto station = MakeTestStation();
	station->AddTake(take);

	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(station);
	scene.RegisterMidiTriggerRouteForTest("default", trigger.value(), 0u);

	scene.PushMainMidiEventForTest(midi::MidiEvent::NoteOn, 60u, 100u);
	scene.PumpMidiForTest();

	ASSERT_EQ(1u, take->MidiLoopEventCount());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);
	ASSERT_EQ(2u, receiver->Actions()[0].MidiInputDevices.size());
	EXPECT_EQ(0, receiver->Actions()[0].MidiInputDevices[0].compare("default"));
	EXPECT_EQ(0, receiver->Actions()[0].MidiInputDevices[1].compare("Aux Keys"));
	EXPECT_TRUE(take->IsArmed());
}

TEST(Trigger, RoutedMidiTriggerIgnoresUnmatchedNoteAndCcEvents) {
	auto receiver = std::make_shared<SequenceTriggerReceiver>();
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"default\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(testStream)).value());
	auto trigStruct = io::RigFile::Trigger::FromJson(json);
	ASSERT_TRUE(trigStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto trigger = Trigger::FromFile(trigParams, trigStruct.value());
	ASSERT_TRUE(trigger.has_value());
	trigger.value()->SetReceiver(receiver);

	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.RegisterMidiTriggerRouteForTest("default", trigger.value(), 0u);

	std::ostringstream captured;
	auto* oldBuf = std::cout.rdbuf(captured.rdbuf());

	scene.PushMainMidiEventForTest(midi::MidiEvent::NoteOn, 61u, 100u);
	scene.PushMainMidiEventForTest(0xB0u, 65u, 127u);
	scene.PumpMidiForTest();

	std::cout.flush();
	std::cout.rdbuf(oldBuf);

	EXPECT_TRUE(receiver->Actions().empty());
	EXPECT_TRUE(captured.str().empty());
}

TEST(Trigger, ConfiguredMidiTriggerDeviceUsesResolvedRouteSlot) {
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

	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.RegisterMidiTriggerRouteForTest("TriggerPad", trigger.value(), 0u);

	scene.PushMainMidiEventForTest(midi::MidiEvent::NoteOn, 60u, 100u);
	scene.PumpMidiForTest();

	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);
}

TEST(Trigger, InitMidiReportsUnmatchedLoopRecordMidiDevices) {
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"midiinput\":[1],\"midiinputdevices\":[\"Aux Keys\"],\"trigger\":{\"type\":\"midi\",\"device\":\"TriggerPad\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto testStream = std::stringstream(str);
	auto json = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(testStream)).value());
	auto trigStruct = io::RigFile::Trigger::FromJson(json);
	ASSERT_TRUE(trigStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto trigger = Trigger::FromFile(trigParams, trigStruct.value());
	ASSERT_TRUE(trigger.has_value());

	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	userConfig.Midi.Devices.push_back({ "TriggerPad", false });
	TestScene scene(sceneParams, userConfig);
	scene.RegisterMidiTriggerRouteForTest("TriggerPad", trigger.value(), 0u);

	std::ostringstream captured;
	auto* oldBuf = std::cout.rdbuf(captured.rdbuf());

	scene.InitMidi();

	std::cout.flush();
	std::cout.rdbuf(oldBuf);

	EXPECT_NE(std::string::npos, captured.str().find("No active MIDI input matches loop-record device \"Aux Keys\""));
}

TEST(Trigger, SceneRoutesAndRecordsSameChannelAcrossMultipleMidiDevices) {
	auto receiverA = std::make_shared<SequenceTriggerReceiver>();
	auto receiverB = std::make_shared<SequenceTriggerReceiver>();
	auto triggerAJson = std::stringstream("{\"name\":\"TrigA\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"Keys A\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}");
	auto triggerBJson = std::stringstream("{\"name\":\"TrigB\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"Keys B\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}");
	auto triggerAStruct = io::RigFile::Trigger::FromJson(std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(triggerAJson)).value()));
	auto triggerBStruct = io::RigFile::Trigger::FromJson(std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(triggerBJson)).value()));
	ASSERT_TRUE(triggerAStruct.has_value());
	ASSERT_TRUE(triggerBStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto triggerA = Trigger::FromFile(trigParams, triggerAStruct.value());
	auto triggerB = Trigger::FromFile(trigParams, triggerBStruct.value());
	ASSERT_TRUE(triggerA.has_value());
	ASSERT_TRUE(triggerB.has_value());
	triggerA.value()->SetReceiver(receiverA);
	triggerB.value()->SetReceiver(receiverB);

	auto take = MakeTestLoopTake();
	take->Record({}, "station", { 0u }, { "Keys A", "Keys B" });
	auto station = MakeTestStation();
	station->AddTake(take);

	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.AddStationForTest(station);
	scene.AddMidiInputDeviceForTest("Keys A", 0u);
	scene.AddMidiInputDeviceForTest("Keys B", 1u);
	scene.RegisterMidiTriggerRouteForTest("Keys A", triggerA.value(), 0u);
	scene.RegisterMidiTriggerRouteForTest("Keys B", triggerB.value(), 1u);

	scene.PushMidiEventForTest(0u, midi::MidiEvent::NoteOn, 60u, 100u);
	scene.PushMidiEventForTest(1u, midi::MidiEvent::NoteOn, 60u, 100u);
	scene.PumpMidiForTest();

	ASSERT_EQ(1u, receiverA->Actions().size());
	ASSERT_EQ(1u, receiverB->Actions().size());
	EXPECT_EQ(1u, take->MidiLoopEventCount(0u));
	EXPECT_EQ(1u, take->MidiLoopEventCount(1u));
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

TEST(Trigger, SerialSceneEventHitsAllMatchingTriggers) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	const auto triggerJson = std::string("{\"name\":\"TrigSerial\",\"stationtype\":0,\"pairs\":[{\"source\":\"serial\",\"device\":\"pedal-a\",\"activatedown\":0,\"activateup\":0,\"ditchdown\":1,\"ditchup\":1}]}");

	auto firstStation = MakeTestStation("station-a");
	firstStation->AddTrigger(MakeTriggerFromRigJson(triggerJson));
	firstStation->AddTrigger(MakeTriggerFromRigJson(triggerJson));
	scene.AddStationForTest(firstStation);

	auto secondStation = MakeTestStation("station-b");
	secondStation->AddTrigger(MakeTriggerFromRigJson(triggerJson));
	scene.AddStationForTest(secondStation);

	scene.PushSerialTriggerEventForTest("pedal-a", 0u, true);
	scene.PumpSerialForTest();

	EXPECT_EQ(2u, firstStation->NumTakes());
	EXPECT_EQ(1u, secondStation->NumTakes());
}

TEST(Trigger, MidiTriggerRoutingHitsAllMatchingRoutes) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	const auto triggerJson = std::string("{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"default\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}");

	auto firstStation = MakeTestStation("station-a");
	auto firstTrigger = MakeTriggerFromRigJson(triggerJson);
	auto secondTrigger = MakeTriggerFromRigJson(triggerJson);
	firstStation->AddTrigger(firstTrigger);
	firstStation->AddTrigger(secondTrigger);
	scene.AddStationForTest(firstStation);
	scene.RegisterMidiTriggerRouteForTest("default", firstTrigger, 0u);
	scene.RegisterMidiTriggerRouteForTest("default", secondTrigger, 0u);

	auto secondStation = MakeTestStation("station-b");
	auto thirdTrigger = MakeTriggerFromRigJson(triggerJson);
	secondStation->AddTrigger(thirdTrigger);
	scene.AddStationForTest(secondStation);
	scene.RegisterMidiTriggerRouteForTest("default", thirdTrigger, 0u);

	midi::MidiEvent event{};
	event.status = midi::MidiEvent::NoteOn;
	event.data1 = 60u;
	event.data2 = 100u;
	scene.DispatchMidiTriggerEventForTest(0u, event);

	EXPECT_EQ(2u, firstStation->NumTakes());
	EXPECT_EQ(1u, secondStation->NumTakes());
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
// pumps) must not run concurrently with Scene::CommitChanges publication.
// Holding _audioMutex while a pump runs proves the synchronisation point.
TEST(Trigger, PumpMidiBlocksWhileAudioMutexHeld) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	const auto triggerJson = std::string("{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"default\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}");

	auto station = MakeTestStation("station-a");
	auto trigger = MakeTriggerFromRigJson(triggerJson);
	station->AddTrigger(trigger);
	scene.AddStationForTest(station);
	scene.RegisterMidiTriggerRouteForTest("default", trigger, 0u);

	scene.PushMainMidiEventForTest(midi::MidiEvent::NoteOn, 60u, 100u);

	std::atomic<bool> pumpFinished{ false };
	{
		std::unique_lock<std::mutex> holdLock(scene.AudioMutexForTest());

		std::thread pumpThread([&] {
			scene.PumpMidiForTest();
			pumpFinished.store(true, std::memory_order_release);
		});

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		EXPECT_FALSE(pumpFinished.load(std::memory_order_acquire))
			<< "PumpMidi must block while _audioMutex is held by another thread";
		EXPECT_EQ(0u, station->NumTakes())
			<< "Trigger dispatch must not occur until _audioMutex is released";

		holdLock.unlock();
		pumpThread.join();
	}

	EXPECT_TRUE(pumpFinished.load(std::memory_order_acquire));
	EXPECT_EQ(1u, station->NumTakes());
}

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
// reduced total takes to zero.
TEST(SceneReset, MidiTriggerDitchWhileRecording_ResetsScene) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	const auto triggerJson = std::string(
		"{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{"
		"\"type\":\"midi\",\"device\":\"default\","
		"\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},"
		"\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}");

	auto station = MakeTestStation();
	auto trigger = MakeTriggerFromRigJson(triggerJson);
	station->AddTrigger(trigger);
	scene.AddStationForTest(station);
	scene.RegisterMidiTriggerRouteForTest("default", trigger, 0u);

	// MIDI activate: start recording
	midi::MidiEvent noteOn{};
	noteOn.status = midi::MidiEvent::NoteOn;
	noteOn.data1 = 60u;
	noteOn.data2 = 100u;
	scene.DispatchMidiTriggerEventForTest(0u, noteOn);

	EXPECT_EQ(1u, station->NumTakes());

	// Commit so _TryGetTake can find the take by ID in _loopTakes
	station->CommitChanges();

	// MIDI ditch: CC down then up
	midi::MidiEvent ccDown{ 0u, 0xB0u, 64u, 127u, 0u };
	midi::MidiEvent ccUp{ 0u, 0xB0u, 64u, 0u, 0u };
	scene.DispatchMidiTriggerEventForTest(0u, ccDown);
	scene.DispatchMidiTriggerEventForTest(0u, ccUp);

	EXPECT_EQ(0u, station->NumTakes());
	EXPECT_TRUE(scene.IsSceneResetForTest());
}

// FAILS before fix: _PumpSerial never called Reset() on ditch-to-zero.
TEST(SceneReset, SerialTriggerDitchWhileRecording_ResetsScene) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	const auto triggerJson = std::string(
		"{\"name\":\"TrigSerial\",\"stationtype\":0,\"pairs\":[{"
		"\"source\":\"serial\",\"device\":\"pedal-a\","
		"\"activatedown\":0,\"activateup\":0,"
		"\"ditchdown\":1,\"ditchup\":1}]}");

	auto station = MakeTestStation();
	station->AddTrigger(MakeTriggerFromRigJson(triggerJson));
	scene.AddStationForTest(station);

	// Serial activate: button 0 pressed
	scene.PushSerialTriggerEventForTest("pedal-a", 0u, true);
	scene.PumpSerialForTest();

	EXPECT_EQ(1u, station->NumTakes());

	// Commit so _TryGetTake can find the take by ID in _loopTakes
	station->CommitChanges();

	// Serial ditch: button 1 down then up
	scene.PushSerialTriggerEventForTest("pedal-a", 1u, true);
	scene.PumpSerialForTest();
	scene.PushSerialTriggerEventForTest("pedal-a", 1u, false);
	scene.PumpSerialForTest();

	EXPECT_EQ(0u, station->NumTakes());
	EXPECT_TRUE(scene.IsSceneResetForTest());
}

TEST(Trigger, PumpSerialBlocksWhileAudioMutexHeld) {
	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);

	const auto triggerJson = std::string("{\"name\":\"TrigSerial\",\"stationtype\":0,\"pairs\":[{\"source\":\"serial\",\"device\":\"pedal-a\",\"activatedown\":0,\"activateup\":0,\"ditchdown\":1,\"ditchup\":1}]}");

	auto station = MakeTestStation("station-a");
	station->AddTrigger(MakeTriggerFromRigJson(triggerJson));
	scene.AddStationForTest(station);

	scene.PushSerialTriggerEventForTest("pedal-a", 0u, true);

	std::atomic<bool> pumpFinished{ false };
	{
		std::unique_lock<std::mutex> holdLock(scene.AudioMutexForTest());

		std::thread pumpThread([&] {
			scene.PumpSerialForTest();
			pumpFinished.store(true, std::memory_order_release);
		});

		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		EXPECT_FALSE(pumpFinished.load(std::memory_order_acquire))
			<< "PumpSerial must block while _audioMutex is held by another thread";
		EXPECT_EQ(0u, station->NumTakes())
			<< "Trigger dispatch must not occur until _audioMutex is released";

		holdLock.unlock();
		pumpThread.join();
	}

	EXPECT_TRUE(pumpFinished.load(std::memory_order_acquire));
	EXPECT_EQ(1u, station->NumTakes());
}
