#include "gtest/gtest.h"

#include "actions/KeyAction.h"
#include "midi/MidiRouter.h"

static constexpr unsigned int PageUpKey = 0x21u;
static constexpr unsigned int PageDownKey = 0x22u;
static const std::vector<std::shared_ptr<engine::Station>> kEmptyStations{};
using KeyActionType = decltype(actions::KeyAction::KEY_UP);

static actions::KeyAction AltPageKey(unsigned int keyChar, KeyActionType keyActionType)
{
	actions::KeyAction action{};
	action.KeyChar = keyChar;
	action.KeyActionType = keyActionType;
	action.Modifiers = base::Action::MODIFIER_ALT;
	return action;
}

TEST(MidiRouterChannelOverride, RewriteLeavesSystemMessagesUnchanged)
{
	EXPECT_EQ(0xF8u, midi::MidiRouter::RewriteIncomingChannel(0xF8u, 8u));
	EXPECT_EQ(0xF1u, midi::MidiRouter::RewriteIncomingChannel(0xF1u, 8u));
}

TEST(MidiRouterChannelOverride, RewriteLeavesStatusUnchangedInOmniMode)
{
	EXPECT_EQ(0x93u, midi::MidiRouter::RewriteIncomingChannel(0x93u, 0u));
}

TEST(MidiRouterChannelOverride, RewriteReplacesOnlyChannelNibble)
{
	EXPECT_EQ(0x95u, midi::MidiRouter::RewriteIncomingChannel(0x92u, 6u));
	EXPECT_EQ(0xB0u, midi::MidiRouter::RewriteIncomingChannel(0xBFu, 1u));
	EXPECT_EQ(0xEFu, midi::MidiRouter::RewriteIncomingChannel(0xE4u, 16u));
}

TEST(MidiRouterChannelOverride, StepMethodsClampToSupportedRange)
{
	midi::MidiRouter router;

	router.StepChannelOverrideDown();
	EXPECT_EQ(0u, router.ForcedChannelOverride());

	for (int i = 0; i < 20; ++i)
		router.StepChannelOverrideUp();
	EXPECT_EQ(16u, router.ForcedChannelOverride());

	for (int i = 0; i < 20; ++i)
		router.StepChannelOverrideDown();
	EXPECT_EQ(0u, router.ForcedChannelOverride());
}

TEST(MidiRouterChannelOverride, SetterClampsOutOfRangeValues)
{
	midi::MidiRouter router;
	router.SetForcedChannelOverride(99u, kEmptyStations);
	EXPECT_EQ(16u, router.ForcedChannelOverride());
}

TEST(MidiRouterChannelOverride, AltPageUpStepsFromOmniToChannelOne)
{
	midi::MidiRouter router;
	auto result = router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN), kEmptyStations);

	EXPECT_TRUE(result.IsEaten);
	EXPECT_EQ(1u, router.ForcedChannelOverride());

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_UP), kEmptyStations);
}

TEST(MidiRouterChannelOverride, AltPageDownStepsFromOneBackToOmni)
{
	midi::MidiRouter router;
	router.SetForcedChannelOverride(1u, kEmptyStations);

	auto result = router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN), kEmptyStations);

	EXPECT_TRUE(result.IsEaten);
	EXPECT_EQ(0u, router.ForcedChannelOverride());

	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_UP), kEmptyStations);
}

TEST(MidiRouterChannelOverride, DualHeldResetIsOrderIndependentAndSingleFire)
{
	midi::MidiRouter router;
	router.SetForcedChannelOverride(7u, kEmptyStations);

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN), kEmptyStations);
	EXPECT_EQ(8u, router.ForcedChannelOverride());

	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN), kEmptyStations);
	EXPECT_EQ(0u, router.ForcedChannelOverride());

	router.SetForcedChannelOverride(9u, kEmptyStations);
	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN), kEmptyStations);
	EXPECT_EQ(9u, router.ForcedChannelOverride());

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN), kEmptyStations);
	EXPECT_EQ(9u, router.ForcedChannelOverride());

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_UP), kEmptyStations);
	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_UP), kEmptyStations);

	router.SetForcedChannelOverride(5u, kEmptyStations);
	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN), kEmptyStations);
	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN), kEmptyStations);
	EXPECT_EQ(0u, router.ForcedChannelOverride());
}

TEST(MidiRouterChannelOverride, UnrelatedKeysAreIgnored)
{
	midi::MidiRouter router;
	actions::KeyAction action{};
	action.KeyChar = 'A';
	action.KeyActionType = actions::KeyAction::KEY_DOWN;
	action.Modifiers = base::Action::MODIFIER_ALT;

	auto result = router.HandleChannelOverrideKey(action, kEmptyStations);
	EXPECT_FALSE(result.IsEaten);
	EXPECT_EQ(0u, router.ForcedChannelOverride());
}

// --- DeriveStationEvent: trigger channel isolation from the station/live override ---
//
// MidiRouter::DeriveStationEvent is the exact seam the RtMidi callback and PumpMidi
// use to build the station/live-facing event copy from a raw ingress event. Trigger
// dispatch always uses the raw event untouched; only the derived copy is rewritten.

TEST(MidiRouterChannelOverride, DeriveStationEventAppliesOverrideWithoutMutatingRawEvent)
{
	// Physical channel 1 (channel nibble 0), as a pedal would transmit.
	const auto rawEvent = midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
	const auto stationEvent = midi::MidiRouter::DeriveStationEvent(rawEvent, 2u);

	EXPECT_EQ(0u, rawEvent.Channel());
	EXPECT_EQ(1u, stationEvent.Channel());
	EXPECT_EQ(midi::MidiEvent::NoteOn, stationEvent.MessageType());
}

TEST(MidiRouterChannelOverride, DeriveStationEventOmniModeIsPassThrough)
{
	const auto rawEvent = midi::MidiEvent::MakeNoteOn(50u, 5u, 64u, 90u);
	const auto stationEvent = midi::MidiRouter::DeriveStationEvent(rawEvent, 0u);

	EXPECT_EQ(rawEvent.status, stationEvent.status);
	EXPECT_EQ(5u, stationEvent.Channel());
}

TEST(MidiRouterChannelOverride, DeriveStationEventLeavesSystemMessagesUnchanged)
{
	const midi::MidiEvent rawEvent{ 10u, 0xF8u, 0u, 0u, 0u };
	const auto stationEvent = midi::MidiRouter::DeriveStationEvent(rawEvent, 9u);

	EXPECT_EQ(0xF8u, stationEvent.status);
}

TEST(MidiRouterChannelOverride, DeriveStationEventPreservesPayloadAndSampleOffset)
{
	const auto rawEvent = midi::MidiEvent::MakeNoteOn(4096u, 3u, 72u, 111u);
	const auto stationEvent = midi::MidiRouter::DeriveStationEvent(rawEvent, 8u);

	EXPECT_EQ(rawEvent.sampleOffset, stationEvent.sampleOffset);
	EXPECT_EQ(rawEvent.data1, stationEvent.data1);
	EXPECT_EQ(rawEvent.data2, stationEvent.data2);
	EXPECT_EQ(7u, stationEvent.Channel());
	EXPECT_EQ(3u, rawEvent.Channel());
}

TEST(MidiRouterChannelOverride, DeriveStationEventChannelDivergesFromRawTriggerChannel)
{
	// Approved behavior: a pedal on physical channel 1 (raw event, channel nibble 0)
	// must still match a trigger bound to channel 1, while the station/live copy
	// reflects the UI override (channel 2, nibble 1).
	const auto rawEvent = midi::MidiEvent::MakeNoteOn(0u, 0u, 36u, 127u);
	const auto stationEvent = midi::MidiRouter::DeriveStationEvent(rawEvent, 2u);

	EXPECT_EQ(0u, rawEvent.Channel());
	EXPECT_EQ(1u, stationEvent.Channel());
	EXPECT_NE(rawEvent.Channel(), stationEvent.Channel());
}
