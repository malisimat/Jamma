
#include "gtest/gtest.h"
#include "resources/ResourceLib.h"
#include "engine/Trigger.h"
#include "io/UserConfig.h"

using base::ActionSender;
using base::ActionReceiver;
using engine::Trigger;
using engine::TriggerParams;
using engine::Timer;
using actions::TriggerAction;
using actions::KeyAction;

const unsigned int ActivateChar = 49;
const unsigned int DitchChar = 50;
const unsigned int ActivateMidi = 211;
const unsigned int DitchMidi = 212;

Time GetTime()
{
	return std::chrono::steady_clock::now();
}

Time OffsetTime(const Time t, unsigned int ms)
{
	return t + std::chrono::milliseconds(ms);
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
		_actions.push_back(action.ActionType);

		return {
			true,
			"source-loop-take",
			"target-loop-take",
			actions::ACTIONRESULT_DEFAULT,
			nullptr,
			std::weak_ptr<base::GuiElement>()
		};
	}

	const std::vector<TriggerAction::TriggerActionType>& Actions() const
	{
		return _actions;
	}

private:
	std::vector<TriggerAction::TriggerActionType> _actions;
};

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

TEST(Trigger, EndOverdubClearsDelayedPunchActions) {
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
	cfg.Trigger = { 0, 0 };

	auto action = KeyAction();
	action.SetUserConfig(cfg);

	// Start overdub (ditch down + activate down).
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	// Punch in and out; both trigger actions are delayed by input latency.
	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_UP;
	trigger->OnAction(action);

	// End overdub before delayed punch actions can fire.
	action.KeyChar = DitchChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	action.KeyChar = ActivateChar;
	action.KeyActionType = KeyAction::KEY_DOWN;
	trigger->OnAction(action);

	auto actionsBeforeTick = receiver->Actions();
	ASSERT_EQ(2u, actionsBeforeTick.size());
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_START, actionsBeforeTick[0]);
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_END, actionsBeforeTick[1]);

	// Flush delayed queues; no delayed punch actions should be emitted.
	trigger->OnTick(GetTime(), 1000000, cfg, std::nullopt);

	auto actionsAfterTick = receiver->Actions();
	ASSERT_EQ(2u, actionsAfterTick.size());
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_START, actionsAfterTick[0]);
	EXPECT_EQ(TriggerAction::TRIGGER_OVERDUB_END, actionsAfterTick[1]);
}
