///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

// TDD tests for vst::Vst3MidiMapping — pure MIDI-CC classification/lookup
// logic and the lock-free ParameterTableMailbox used to publish rebuilt
// controller-mapping tables from a non-RT thread to the audio thread.
// Deliberately independent of the VST3 SDK and JAMMA_VST3_ENABLED, so these
// tests always run.

#include "gtest/gtest.h"
#include "vst/Vst3MidiMapping.h"

using vst::Vst3MidiMapping::ChannelCount;
using vst::Vst3MidiMapping::ControllerCount;
using vst::Vst3MidiMapping::AfterTouchControllerNumber;
using vst::Vst3MidiMapping::PitchBendControllerNumber;
using vst::Vst3MidiMapping::NoParamId;
using vst::Vst3MidiMapping::MakeEmptyTable;
using vst::Vst3MidiMapping::ControllerValue;
using vst::Vst3MidiMapping::TryClassify;
using vst::Vst3MidiMapping::TryLookup;
using vst::Vst3MidiMapping::ParameterTableMailbox;
using vst::Vst3MidiMapping::ParameterTable;

static constexpr std::uint8_t ControlChangeStatus = 0xB0;
static constexpr std::uint8_t ChannelPressureStatus = 0xD0;
static constexpr std::uint8_t PitchBendStatus = 0xE0;
static constexpr std::uint8_t NoteOnStatus = 0x90;
static constexpr std::uint8_t ProgramChangeStatus = 0xC0;

static midi::MidiEvent MakeEvent(std::uint8_t status, std::uint8_t channel, std::uint8_t data1, std::uint8_t data2)
{
	return midi::MidiEvent{ 0u, static_cast<std::uint8_t>(status | (channel & midi::MidiEvent::ChannelMask)), data1, data2, 0u };
}

// -----------------------------------------------------------------------
// Suite: Vst3MidiMappingEmptyTable
// -----------------------------------------------------------------------

TEST(Vst3MidiMappingEmptyTable, AllEntriesAreNoParamId)
{
	const auto table = MakeEmptyTable();
	for (std::size_t channel = 0; channel < ChannelCount; ++channel)
		for (std::size_t ctrl = 0; ctrl < ControllerCount; ++ctrl)
			EXPECT_EQ(table[channel][ctrl], NoParamId) << "channel=" << channel << " ctrl=" << ctrl;
}

// -----------------------------------------------------------------------
// Suite: Vst3MidiMappingTryClassify
// -----------------------------------------------------------------------

TEST(Vst3MidiMappingTryClassify, ControlChangeMinValue)
{
	const auto event = MakeEvent(ControlChangeStatus, 0, 74, 0);
	ControllerValue out;
	ASSERT_TRUE(TryClassify(event, out));
	EXPECT_EQ(out.ControllerNumber, 74u);
	EXPECT_FLOAT_EQ(out.NormalizedValue, 0.0f);
}

TEST(Vst3MidiMappingTryClassify, ControlChangeMaxValue)
{
	const auto event = MakeEvent(ControlChangeStatus, 3, 1, 127);
	ControllerValue out;
	ASSERT_TRUE(TryClassify(event, out));
	EXPECT_EQ(out.ControllerNumber, 1u);
	EXPECT_FLOAT_EQ(out.NormalizedValue, 1.0f);
}

TEST(Vst3MidiMappingTryClassify, ChannelPressureMinValue)
{
	const auto event = MakeEvent(ChannelPressureStatus, 0, 0, 0);
	ControllerValue out;
	ASSERT_TRUE(TryClassify(event, out));
	EXPECT_EQ(out.ControllerNumber, AfterTouchControllerNumber);
	EXPECT_FLOAT_EQ(out.NormalizedValue, 0.0f);
}

TEST(Vst3MidiMappingTryClassify, ChannelPressureMaxValue)
{
	const auto event = MakeEvent(ChannelPressureStatus, 5, 127, 0);
	ControllerValue out;
	ASSERT_TRUE(TryClassify(event, out));
	EXPECT_EQ(out.ControllerNumber, AfterTouchControllerNumber);
	EXPECT_FLOAT_EQ(out.NormalizedValue, 1.0f);
}

TEST(Vst3MidiMappingTryClassify, PitchBendMinValue)
{
	const auto event = MakeEvent(PitchBendStatus, 0, 0, 0);
	ControllerValue out;
	ASSERT_TRUE(TryClassify(event, out));
	EXPECT_EQ(out.ControllerNumber, PitchBendControllerNumber);
	EXPECT_FLOAT_EQ(out.NormalizedValue, 0.0f);
}

TEST(Vst3MidiMappingTryClassify, PitchBendCenterValue)
{
	// Center is 8192 (0x2000): lsb=0x00, msb=0x40.
	const auto event = MakeEvent(PitchBendStatus, 0, 0x00, 0x40);
	ControllerValue out;
	ASSERT_TRUE(TryClassify(event, out));
	EXPECT_EQ(out.ControllerNumber, PitchBendControllerNumber);
	EXPECT_NEAR(out.NormalizedValue, 8192.0f / 16383.0f, 1e-5f);
}

TEST(Vst3MidiMappingTryClassify, PitchBendMaxValue)
{
	// Max is 16383 (0x3FFF): lsb=0x7F, msb=0x7F.
	const auto event = MakeEvent(PitchBendStatus, 0, 0x7F, 0x7F);
	ControllerValue out;
	ASSERT_TRUE(TryClassify(event, out));
	EXPECT_EQ(out.ControllerNumber, PitchBendControllerNumber);
	EXPECT_NEAR(out.NormalizedValue, 1.0f, 1e-5f);
}

TEST(Vst3MidiMappingTryClassify, NoteOnIsNotClassified)
{
	const auto event = MakeEvent(NoteOnStatus, 0, 60, 100);
	ControllerValue out;
	EXPECT_FALSE(TryClassify(event, out));
}

TEST(Vst3MidiMappingTryClassify, ProgramChangeIsNotClassified)
{
	const auto event = MakeEvent(ProgramChangeStatus, 0, 5, 0);
	ControllerValue out;
	EXPECT_FALSE(TryClassify(event, out));
}

// -----------------------------------------------------------------------
// Suite: Vst3MidiMappingTryLookup
// -----------------------------------------------------------------------

TEST(Vst3MidiMappingTryLookup, ReturnsMappedParamIdWhenPresent)
{
	auto table = MakeEmptyTable();
	table[2][74] = 12345u;

	std::uint32_t paramId = 0u;
	ASSERT_TRUE(TryLookup(table, 2, 74, paramId));
	EXPECT_EQ(paramId, 12345u);
}

TEST(Vst3MidiMappingTryLookup, ReturnsFalseWhenNoMapping)
{
	const auto table = MakeEmptyTable();

	std::uint32_t paramId = 0u;
	EXPECT_FALSE(TryLookup(table, 0, 1, paramId));
}

TEST(Vst3MidiMappingTryLookup, ReturnsFalseForOutOfRangeChannel)
{
	const auto table = MakeEmptyTable();

	std::uint32_t paramId = 0u;
	EXPECT_FALSE(TryLookup(table, static_cast<std::uint8_t>(ChannelCount), 0, paramId));
}

TEST(Vst3MidiMappingTryLookup, ReturnsFalseForOutOfRangeController)
{
	const auto table = MakeEmptyTable();

	std::uint32_t paramId = 0u;
	EXPECT_FALSE(TryLookup(table, 0, static_cast<std::uint8_t>(ControllerCount), paramId));
}

TEST(Vst3MidiMappingTryLookup, AllChannelsIndependentlyAddressable)
{
	auto table = MakeEmptyTable();
	for (std::size_t channel = 0; channel < ChannelCount; ++channel)
		table[channel][10] = static_cast<std::uint32_t>(channel + 1);

	for (std::size_t channel = 0; channel < ChannelCount; ++channel)
	{
		std::uint32_t paramId = 0u;
		ASSERT_TRUE(TryLookup(table, static_cast<std::uint8_t>(channel), 10, paramId));
		EXPECT_EQ(paramId, static_cast<std::uint32_t>(channel + 1));
	}
}

// -----------------------------------------------------------------------
// Suite: Vst3MidiMappingParameterTableMailbox
// -----------------------------------------------------------------------

TEST(Vst3MidiMappingParameterTableMailbox, InitiallyAdoptsEmptyTable)
{
	ParameterTableMailbox mailbox;
	int adopted = 0;
	mailbox.AdoptPending(adopted);

	std::uint32_t paramId = 0u;
	EXPECT_FALSE(TryLookup(mailbox.Slot(adopted), 0, 0, paramId));
}

TEST(Vst3MidiMappingParameterTableMailbox, PublishedTableIsAdoptedByConsumer)
{
	ParameterTableMailbox mailbox;

	auto* writeSlot = mailbox.AcquireWriteSlot();
	ASSERT_NE(writeSlot, nullptr);
	(*writeSlot)[0][1] = 999u;
	mailbox.Publish();

	int adopted = 0;
	mailbox.AdoptPending(adopted);

	std::uint32_t paramId = 0u;
	ASSERT_TRUE(TryLookup(mailbox.Slot(adopted), 0, 1, paramId));
	EXPECT_EQ(paramId, 999u);
}

TEST(Vst3MidiMappingParameterTableMailbox, AcquireWriteSlotFailsUntilPreviousPublishAdopted)
{
	ParameterTableMailbox mailbox;

	auto* firstSlot = mailbox.AcquireWriteSlot();
	ASSERT_NE(firstSlot, nullptr);
	(*firstSlot)[0][1] = 1u;
	mailbox.Publish();

	// Consumer has not yet adopted: a second acquire must fail.
	EXPECT_EQ(mailbox.AcquireWriteSlot(), nullptr);

	int adopted = 0;
	mailbox.AdoptPending(adopted);

	// Now that the consumer has caught up, the producer can acquire again.
	auto* secondSlot = mailbox.AcquireWriteSlot();
	EXPECT_NE(secondSlot, nullptr);
}

TEST(Vst3MidiMappingParameterTableMailbox, PublishWithoutAcquireIsNoOp)
{
	ParameterTableMailbox mailbox;

	// No AcquireWriteSlot() call preceded this — must not corrupt state.
	mailbox.Publish();

	int adopted = 0;
	mailbox.AdoptPending(adopted);

	std::uint32_t paramId = 0u;
	EXPECT_FALSE(TryLookup(mailbox.Slot(adopted), 0, 0, paramId));
}

TEST(Vst3MidiMappingParameterTableMailbox, ResetClearsPendingPublishAndData)
{
	ParameterTableMailbox mailbox;

	auto* writeSlot = mailbox.AcquireWriteSlot();
	ASSERT_NE(writeSlot, nullptr);
	(*writeSlot)[0][1] = 555u;
	mailbox.Publish();

	mailbox.Reset();

	int adopted = 0;
	mailbox.AdoptPending(adopted);

	std::uint32_t paramId = 0u;
	EXPECT_FALSE(TryLookup(mailbox.Slot(adopted), 0, 1, paramId));

	// After Reset(), the producer must be able to acquire immediately.
	EXPECT_NE(mailbox.AcquireWriteSlot(), nullptr);
}

TEST(Vst3MidiMappingParameterTableMailbox, SuccessiveRebuildsAlternateSlots)
{
	ParameterTableMailbox mailbox;
	int adopted = 0;

	auto* firstSlot = mailbox.AcquireWriteSlot();
	ASSERT_NE(firstSlot, nullptr);
	(*firstSlot)[0][1] = 111u;
	mailbox.Publish();
	mailbox.AdoptPending(adopted);

	auto* secondSlot = mailbox.AcquireWriteSlot();
	ASSERT_NE(secondSlot, nullptr);
	*secondSlot = MakeEmptyTable();
	(*secondSlot)[0][1] = 222u;
	mailbox.Publish();
	mailbox.AdoptPending(adopted);

	std::uint32_t paramId = 0u;
	ASSERT_TRUE(TryLookup(mailbox.Slot(adopted), 0, 1, paramId));
	EXPECT_EQ(paramId, 222u);
}
