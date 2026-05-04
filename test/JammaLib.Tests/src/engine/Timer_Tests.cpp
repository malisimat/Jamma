
#include "gtest/gtest.h"
#include "engine/Timer.h"

using engine::Timer;

TEST(Timer, ReportsQuantiseStateAfterSet) {
	Timer t;
	ASSERT_FALSE(t.IsQuantisable());
	ASSERT_EQ(0u, t.QuantiseSamps());
	ASSERT_EQ(Timer::QUANTISE_OFF, t.Quantisation());

	t.SetQuantisation(48000u, Timer::QUANTISE_MULTIPLE);

	ASSERT_TRUE(t.IsQuantisable());
	ASSERT_EQ(48000u, t.QuantiseSamps());
	ASSERT_EQ(Timer::QUANTISE_MULTIPLE, t.Quantisation());
}

TEST(Timer, ClearResetsQuantiseSampsButLeavesMode) {
	Timer t;
	t.SetQuantisation(12345u, Timer::QUANTISE_MULTIPLE);
	t.SetSeedSourceLength(67890ul);
	t.Clear();

	ASSERT_FALSE(t.IsQuantisable());
	ASSERT_EQ(0u, t.QuantiseSamps());
	ASSERT_EQ(0ul, t.SeedSourceLength());
	// Mode is intentionally not reset by Clear() in current behaviour.
	ASSERT_EQ(Timer::QUANTISE_MULTIPLE, t.Quantisation());
}

TEST(Timer, ReportsSeedSourceLengthWhenProvided) {
	Timer t;
	t.SetQuantisation(48000u, Timer::QUANTISE_POWER);
	t.SetSeedSourceLength(192000ul);

	ASSERT_EQ(192000ul, t.SeedSourceLength());
}
