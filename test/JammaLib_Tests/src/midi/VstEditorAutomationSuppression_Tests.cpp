#include "gtest/gtest.h"

#include "midi/MidiRouter.h"
#include "vst/IVstPlugin.h"

using midi::MidiRouter;

namespace
{
	// Suppression compares plugin pointer identity only; never dereferenced.
	const vst::IVstPlugin* FakePlugin(std::uintptr_t id) noexcept
	{
		return reinterpret_cast<const vst::IVstPlugin*>(id);
	}

	class SuppressionTestFixture : public ::testing::Test
	{
	protected:
		void SetUp() override { MidiRouter::ResetAutomationSuppressionForTest(); }
		void TearDown() override { MidiRouter::ResetAutomationSuppressionForTest(); }
	};
}

TEST_F(SuppressionTestFixture, UnknownParameterIsNotSuppressed)
{
	auto* plugin = FakePlugin(0x100u);
	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(plugin, 0u, 0u));
}

TEST_F(SuppressionTestFixture, FreshEntrySuppressesUntilExpiry)
{
	auto* plugin = FakePlugin(0x110u);
	MidiRouter::RefreshAutomationSuppression(plugin, 4u, /*now*/ 1000u, /*expiry*/ 2000u);

	EXPECT_TRUE(MidiRouter::IsParameterSuppressed(plugin, 4u, 1500u));
	EXPECT_TRUE(MidiRouter::IsParameterSuppressed(plugin, 4u, 1999u));
}

TEST_F(SuppressionTestFixture, ExpiredEntryNoLongerSuppresses)
{
	auto* plugin = FakePlugin(0x120u);
	MidiRouter::RefreshAutomationSuppression(plugin, 1u, /*now*/ 1000u, /*expiry*/ 2000u);

	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(plugin, 1u, 2000u));
	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(plugin, 1u, 2500u));
}

TEST_F(SuppressionTestFixture, DifferentParameterIsIndependent)
{
	auto* plugin = FakePlugin(0x130u);
	MidiRouter::RefreshAutomationSuppression(plugin, 2u, /*now*/ 0u, /*expiry*/ 5000u);

	EXPECT_TRUE(MidiRouter::IsParameterSuppressed(plugin, 2u, 1000u));
	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(plugin, 3u, 1000u));
}

TEST_F(SuppressionTestFixture, DifferentPluginIsIndependent)
{
	auto* pluginA = FakePlugin(0x140u);
	auto* pluginB = FakePlugin(0x141u);
	MidiRouter::RefreshAutomationSuppression(pluginA, 0u, /*now*/ 0u, /*expiry*/ 5000u);

	EXPECT_TRUE(MidiRouter::IsParameterSuppressed(pluginA, 0u, 1000u));
	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(pluginB, 0u, 1000u));
}

TEST_F(SuppressionTestFixture, RefreshExtendsExistingEntry)
{
	auto* plugin = FakePlugin(0x150u);
	MidiRouter::RefreshAutomationSuppression(plugin, 6u, /*now*/ 0u, /*expiry*/ 1000u);
	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(plugin, 6u, 1000u));

	// Extending the same (plugin, param) pushes the deadline out without
	// consuming a second slot.
	MidiRouter::RefreshAutomationSuppression(plugin, 6u, /*now*/ 900u, /*expiry*/ 3000u);
	EXPECT_TRUE(MidiRouter::IsParameterSuppressed(plugin, 6u, 2000u));
}

TEST_F(SuppressionTestFixture, ExpiredSlotIsReclaimedForNewParameter)
{
	// First parameter expires...
	auto* plugin = FakePlugin(0x160u);
	MidiRouter::RefreshAutomationSuppression(plugin, 0u, /*now*/ 0u, /*expiry*/ 1000u);

	// ...then a new parameter is registered after that deadline; it should reuse
	// the freed slot and suppress correctly.
	MidiRouter::RefreshAutomationSuppression(plugin, 1u, /*now*/ 2000u, /*expiry*/ 4000u);

	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(plugin, 0u, 2000u));
	EXPECT_TRUE(MidiRouter::IsParameterSuppressed(plugin, 1u, 3000u));
}

TEST_F(SuppressionTestFixture, NullPluginIsNeverSuppressed)
{
	MidiRouter::RefreshAutomationSuppression(nullptr, 0u, 0u, 5000u);
	EXPECT_FALSE(MidiRouter::IsParameterSuppressed(nullptr, 0u, 1000u));
}
