#include "gtest/gtest.h"

#include "actions/KeyAction.h"
#include "midi/MidiRouter.h"

namespace
{
	constexpr unsigned int PageUpKey = 0x21u;
	constexpr unsigned int PageDownKey = 0x22u;

	actions::KeyAction AltPageKey(unsigned int keyChar, bool isDown)
	{
		actions::KeyAction action{};
		action.KeyChar = keyChar;
		action.KeyActionType = isDown ? actions::KeyAction::KEY_DOWN : actions::KeyAction::KEY_UP;
		action.Modifiers = base::Action::MODIFIER_ALT;
		return action;
	}
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
	EXPECT_EQ(0u, router.ForcedChannelOverrideOneBased());

	for (int i = 0; i < 20; ++i)
		router.StepChannelOverrideUp();
	EXPECT_EQ(16u, router.ForcedChannelOverrideOneBased());

	for (int i = 0; i < 20; ++i)
		router.StepChannelOverrideDown();
	EXPECT_EQ(0u, router.ForcedChannelOverrideOneBased());
}

TEST(MidiRouterChannelOverride, SetterClampsOutOfRangeValues)
{
	midi::MidiRouter router;
	router.SetForcedChannelOverrideOneBased(99u);
	EXPECT_EQ(16u, router.ForcedChannelOverrideOneBased());
}

TEST(MidiRouterChannelOverride, AltPageUpStepsFromOmniToChannelOne)
{
	midi::MidiRouter router;
	auto result = router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN));

	EXPECT_TRUE(result.IsEaten);
	EXPECT_EQ(1u, router.ForcedChannelOverrideOneBased());

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_UP));
}

TEST(MidiRouterChannelOverride, AltPageDownStepsFromOneBackToOmni)
{
	midi::MidiRouter router;
	router.SetForcedChannelOverrideOneBased(1u);

	auto result = router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN));

	EXPECT_TRUE(result.IsEaten);
	EXPECT_EQ(0u, router.ForcedChannelOverrideOneBased());

	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_UP));
}

TEST(MidiRouterChannelOverride, DualHeldResetIsOrderIndependentAndSingleFire)
{
	midi::MidiRouter router;
	router.SetForcedChannelOverrideOneBased(7u);

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN));
	EXPECT_EQ(8u, router.ForcedChannelOverrideOneBased());

	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN));
	EXPECT_EQ(0u, router.ForcedChannelOverrideOneBased());

	router.SetForcedChannelOverrideOneBased(9u);
	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN));
	EXPECT_EQ(9u, router.ForcedChannelOverrideOneBased());

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN));
	EXPECT_EQ(9u, router.ForcedChannelOverrideOneBased());

	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_UP));
	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_UP));

	router.SetForcedChannelOverrideOneBased(5u);
	router.HandleChannelOverrideKey(AltPageKey(PageDownKey, actions::KeyAction::KEY_DOWN));
	router.HandleChannelOverrideKey(AltPageKey(PageUpKey, actions::KeyAction::KEY_DOWN));
	EXPECT_EQ(0u, router.ForcedChannelOverrideOneBased());
}

TEST(MidiRouterChannelOverride, UnrelatedKeysAreIgnored)
{
	midi::MidiRouter router;
	actions::KeyAction action{};
	action.KeyChar = 'A';
	action.KeyActionType = actions::KeyAction::KEY_DOWN;
	action.Modifiers = base::Action::MODIFIER_ALT;

	auto result = router.HandleChannelOverrideKey(action);
	EXPECT_FALSE(result.IsEaten);
	EXPECT_EQ(0u, router.ForcedChannelOverrideOneBased());
}