
#include "gtest/gtest.h"
#include <sstream>
#include "resources/ResourceLib.h"
#include "engine/MidiEvent.h"
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

static void SendMidiEvent(const std::shared_ptr<Trigger>& trigger, const engine::MidiEvent& event, Time t)
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
	}

	void SetSharedMainMidiTriggerSlotForTest(std::uint8_t slot)
	{
		_sharedMainMidiTriggerSlot = slot;
	}

	void PushMainMidiEventForTest(std::uint8_t status,
		std::uint8_t data1,
		std::uint8_t data2,
		unsigned int sampleRate = 0u)
	{
		_PushMainMidiEvent(status, data1, data2, sampleRate);
	}

	void PumpMidiForTest()
	{
		_PumpMidi();
	}

	void DispatchMidiTriggerEventForTest(std::uint8_t deviceSlot,
		const engine::MidiEvent& event)
	{
		_DispatchMidiTriggerEvent(deviceSlot, event);
	}
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

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOff(0u, 0u, 60u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	engine::MidiEvent ccDown{ 0u, 0xB0u, 64u, 127u, 0u };
	engine::MidiEvent ccUp{ 0u, 0xB0u, 64u, 0u, 0u };
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

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	EXPECT_TRUE(receiver->Actions().empty());

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOff(0u, 0u, 60u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOff(0u, 0u, 60u), GetTime());
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

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOn(0u, 0u, 61u, 100u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOff(0u, 0u, 61u), GetTime());
	ASSERT_EQ(1u, receiver->Actions().size());

	SendMidiEvent(trigger.value(), engine::MidiEvent::MakeNoteOn(0u, 0u, 61u, 100u), GetTime());
	ASSERT_EQ(3u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH, receiver->Actions()[1].ActionType);
	EXPECT_EQ(TriggerAction::TRIGGER_DITCH_UNMUTE, receiver->Actions()[2].ActionType);
}

TEST(Trigger, SharedMainMidiIngressStillRecordsLoopMidiWhenTriggerEatsEvent) {
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
	scene.SetSharedMainMidiTriggerSlotForTest(0u);

	scene.PushMainMidiEventForTest(engine::MidiEvent::NoteOn, 60u, 100u);
	scene.PumpMidiForTest();

	ASSERT_EQ(1u, take->MidiLoopEventCount());
	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);
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
	scene.SetSharedMainMidiTriggerSlotForTest(0u);

	std::ostringstream captured;
	auto* oldBuf = std::cout.rdbuf(captured.rdbuf());

	scene.PushMainMidiEventForTest(engine::MidiEvent::NoteOn, 61u, 100u);
	scene.PushMainMidiEventForTest(0xB0u, 65u, 127u);
	scene.PumpMidiForTest();

	std::cout.flush();
	std::cout.rdbuf(oldBuf);

	EXPECT_TRUE(receiver->Actions().empty());
	EXPECT_TRUE(captured.str().empty());
}

TEST(Trigger, ConfiguredMidiTriggerDeviceUsesGlobalMainMidiInputForNow) {
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
	scene.SetSharedMainMidiTriggerSlotForTest(0u);

	scene.PushMainMidiEventForTest(engine::MidiEvent::NoteOn, 60u, 100u);
	scene.PumpMidiForTest();

	ASSERT_EQ(1u, receiver->Actions().size());
	EXPECT_EQ(TriggerAction::TRIGGER_REC_START, receiver->Actions()[0].ActionType);
}

TEST(Trigger, MidiTriggerRoutingStopsAfterFirstConsumerEatsEvent) {
	auto firstReceiver = std::make_shared<ConfigurableTriggerReceiver>(true);
	auto secondReceiver = std::make_shared<ConfigurableTriggerReceiver>(true);
	auto str = "{\"name\":\"TrigMidi\",\"stationtype\":0,\"trigger\":{\"type\":\"midi\",\"device\":\"default\",\"activate\":{\"kind\":\"note\",\"channel\":1,\"id\":60},\"ditch\":{\"kind\":\"cc\",\"channel\":1,\"id\":64}}}";
	auto firstStream = std::stringstream(str);
	auto secondStream = std::stringstream(str);
	auto firstJson = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(firstStream)).value());
	auto secondJson = std::get<io::Json::JsonPart>(io::Json::FromStream(std::move(secondStream)).value());
	auto firstStruct = io::RigFile::Trigger::FromJson(firstJson);
	auto secondStruct = io::RigFile::Trigger::FromJson(secondJson);
	ASSERT_TRUE(firstStruct.has_value());
	ASSERT_TRUE(secondStruct.has_value());

	TriggerParams trigParams;
	trigParams.DebounceMs = 0u;
	auto firstTrigger = Trigger::FromFile(trigParams, firstStruct.value());
	auto secondTrigger = Trigger::FromFile(trigParams, secondStruct.value());
	ASSERT_TRUE(firstTrigger.has_value());
	ASSERT_TRUE(secondTrigger.has_value());
	firstTrigger.value()->SetReceiver(firstReceiver);
	secondTrigger.value()->SetReceiver(secondReceiver);

	SceneParams sceneParams{ base::DrawableParams(),
		base::MoveableParams(),
		base::SizeableParams() };
	io::UserConfig userConfig = {};
	TestScene scene(sceneParams, userConfig);
	scene.RegisterMidiTriggerRouteForTest("default", firstTrigger.value(), 0u);
	scene.RegisterMidiTriggerRouteForTest("default", secondTrigger.value(), 0u);

	engine::MidiEvent event{};
	event.status = engine::MidiEvent::NoteOn;
	event.data1 = 60u;
	event.data2 = 100u;
	scene.DispatchMidiTriggerEventForTest(0u, event);

	ASSERT_EQ(1u, firstReceiver->Actions().size());
	EXPECT_TRUE(secondReceiver->Actions().empty());
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
