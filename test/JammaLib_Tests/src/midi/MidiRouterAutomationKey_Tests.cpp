#include "gtest/gtest.h"

#include <memory>
#include <vector>

#include "actions/KeyAction.h"
#include "midi/MidiRouter.h"

namespace midi_tests
{
	actions::KeyAction BuildKey(unsigned int keyChar,
		int keyActionType,
		base::Action::Modifiers modifiers = base::Action::MODIFIER_NONE)
	{
		actions::KeyAction action;
		action.KeyChar = keyChar;
		action.KeyActionType = (keyActionType == actions::KeyAction::KEY_DOWN)
			? actions::KeyAction::KEY_DOWN
			: actions::KeyAction::KEY_UP;
		action.IsSystem = false;
		action.Modifiers = modifiers;
		return action;
	}

	class MidiRouterAutomationKeyTestFixture : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			midi::MidiRouter::SetAutomationRecordHeldForTest(false);
		}

		void TearDown() override
		{
			midi::MidiRouter::SetAutomationRecordHeldForTest(false);
		}
	};

	TEST_F(MidiRouterAutomationKeyTestFixture, InsertPressAndReleaseControlsAutomationRecord)
	{
		midi::MidiRouter router;
		const std::vector<std::shared_ptr<engine::Station>> stations;
		const std::vector<unsigned char> hoverPath;
		const std::shared_ptr<engine::LoopTake> hoveredTake;

		auto downResult = router.HandleAutomationKey(
			BuildKey(45u, actions::KeyAction::KEY_DOWN),
			stations,
			hoverPath,
			hoveredTake);

		EXPECT_TRUE(downResult.IsEaten);
		EXPECT_TRUE(midi::MidiRouter::IsAutomationRecordHeld());

		auto upResult = router.HandleAutomationKey(
			BuildKey(45u, actions::KeyAction::KEY_UP),
			stations,
			hoverPath,
			hoveredTake);

		EXPECT_TRUE(upResult.IsEaten);
		EXPECT_FALSE(midi::MidiRouter::IsAutomationRecordHeld());
	}

	TEST_F(MidiRouterAutomationKeyTestFixture, CtrlShiftADoesNotArmAutomationRecord)
	{
		midi::MidiRouter router;
		const std::vector<std::shared_ptr<engine::Station>> stations;
		const std::vector<unsigned char> hoverPath;
		const std::shared_ptr<engine::LoopTake> hoveredTake;

		auto oldShortcutResult = router.HandleAutomationKey(
			BuildKey(65u,
				actions::KeyAction::KEY_DOWN,
				static_cast<base::Action::Modifiers>(base::Action::MODIFIER_CTRL | base::Action::MODIFIER_SHIFT)),
			stations,
			hoverPath,
			hoveredTake);

		EXPECT_FALSE(oldShortcutResult.IsEaten);
		EXPECT_FALSE(midi::MidiRouter::IsAutomationRecordHeld());
	}
}
