
#include "gtest/gtest.h"
#include "engine/Timer.h"

using engine::Timer;

// ── Initialisation / lifecycle ───────────────────────────────────────────────

TEST(Timer, DefaultStateIsNotQuantisable) {
	Timer t;
	ASSERT_FALSE(t.IsQuantisable());
	ASSERT_EQ(0u, t.QuantiseSamps());
	ASSERT_EQ(Timer::QUANTISE_OFF, t.Quantisation());
	ASSERT_EQ(0ul, t.SeedSourceLength());
}

TEST(Timer, ClearResetsQuantiseSampsAndSeedLengthButLeavesMode) {
	Timer t;
	t.SetQuantisation(12345u, Timer::QUANTISE_MULTIPLE);
	t.SetSeedSourceLength(67890ul);
	t.Clear();

	ASSERT_FALSE(t.IsQuantisable());
	ASSERT_EQ(0u, t.QuantiseSamps());
	ASSERT_EQ(0ul, t.SeedSourceLength());
	// Mode is intentionally not reset by Clear().
	ASSERT_EQ(Timer::QUANTISE_MULTIPLE, t.Quantisation());
}

TEST(Timer, SetQuantisationResetsSeedSourceLength) {
	Timer t;
	t.SetQuantisation(48000u, Timer::QUANTISE_POWER);
	t.SetSeedSourceLength(192000ul);
	ASSERT_EQ(192000ul, t.SeedSourceLength());

	// Changing quantisation must clear the seed length so stale reclock data
	// doesn't mislead the local-tempo queue path.
	t.SetQuantisation(24000u, Timer::QUANTISE_MULTIPLE);
	ASSERT_EQ(0ul, t.SeedSourceLength());
}

// ── QUANTISE_OFF ─────────────────────────────────────────────────────────────

TEST(Timer, QuantiseOff_ReturnsSampleCountAndZeroError) {
	Timer t; // no SetQuantisation → QUANTISE_OFF, grain = 0

	auto [len, err] = t.QuantiseLength(99999ul);

	ASSERT_EQ(99999ul, len);
	ASSERT_EQ(0, err);
}

// ── QUANTISE_MULTIPLE ────────────────────────────────────────────────────────
// Error convention: positive → recorded too long (snap shortened the loop);
//                  negative → recorded too short (snap lengthened the loop).

TEST(Timer, QuantiseMultiple_ExactMultiple_ZeroError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_MULTIPLE); // 250 ms @ 48 kHz

	auto [len, err] = t.QuantiseLength(36000ul); // exactly 3×

	ASSERT_EQ(36000ul, len);
	ASSERT_EQ(0, err);
}

TEST(Timer, QuantiseMultiple_NearerLower_SnapsDown_PositiveError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_MULTIPLE);

	// 38000 is 2000 past 3×(36000), but 10000 before 4×(48000) → snap to 3×.
	auto [len, err] = t.QuantiseLength(38000ul);

	ASSERT_EQ(36000ul, len);
	ASSERT_EQ(2000, err);  // recorded 2000 samps too long
}

TEST(Timer, QuantiseMultiple_NearerUpper_SnapsUp_NegativeError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_MULTIPLE);

	// 46000 is 10000 past 3×(36000), but 2000 before 4×(48000) → snap to 4×.
	auto [len, err] = t.QuantiseLength(46000ul);

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(-2000, err);  // recorded 2000 samps too short
}

TEST(Timer, QuantiseMultiple_ExactlyBetweenTwoMultiples_SnapsUp) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_MULTIPLE);

	// 42000 is exactly halfway between 3×(36000) and 4×(48000).
	// Tie-break: snaps UP to the larger multiple.
	auto [len, err] = t.QuantiseLength(42000ul);

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(-6000, err);
}

TEST(Timer, QuantiseMultiple_LengthShorterThanOneGrain_SnapsToZero) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_MULTIPLE);

	// 2000 < 12000 → nLow = 0, lenLow = 0, lenHigh = 12000.
	// dLow = 2000, dHigh = 10000 → snap to 0 (nearest lower multiple = 0).
	auto [len, err] = t.QuantiseLength(2000ul);

	ASSERT_EQ(0ul, len);
	ASSERT_EQ(2000, err);
}

// ── QUANTISE_POWER ───────────────────────────────────────────────────────────
// Candidates are 2×, 4×, 8×, 16×, … grain (powers of 2 starting at 2).
// Error convention: positive → snap lengthened the loop (recorded too short);
//                  negative → snap shortened the loop (recorded too long).

TEST(Timer, QuantisePower_Exactly2xGrain_ZeroError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	auto [len, err] = t.QuantiseLength(24000ul); // exactly 2×

	ASSERT_EQ(24000ul, len);
	ASSERT_EQ(0, err);
}

TEST(Timer, QuantisePower_Exactly4xGrain_ZeroError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	auto [len, err] = t.QuantiseLength(48000ul); // exactly 4×

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(0, err);
}

TEST(Timer, QuantisePower_Exactly8xGrain_ZeroError) {
	Timer t;
	t.SetQuantisation(6000u, Timer::QUANTISE_POWER);

	auto [len, err] = t.QuantiseLength(48000ul); // exactly 8×

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(0, err);
}

TEST(Timer, QuantisePower_NearerLower_SnapsDown_NegativeError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 30000 is 6000 above 2×(24000), 18000 below 4×(48000) → snap to 2×.
	auto [len, err] = t.QuantiseLength(30000ul);

	ASSERT_EQ(24000ul, len);
	ASSERT_EQ(-6000, err); // recorded 6000 samps too long relative to 2×
}

TEST(Timer, QuantisePower_NearerUpper_SnapsUp_PositiveError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 42000 is 18000 above 2×(24000), 6000 below 4×(48000) → snap to 4×.
	auto [len, err] = t.QuantiseLength(42000ul);

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(6000, err); // recorded 6000 samps too short relative to 4×
}

TEST(Timer, QuantisePower_ExactlyBetweenTwoPowers_SnapsUp) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 36000 is exactly halfway between 2×(24000) and 4×(48000).
	// Tie-break: snaps UP to the larger power.
	auto [len, err] = t.QuantiseLength(36000ul);

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(12000, err);
}

TEST(Timer, QuantisePower_LengthShorterThanOneGrain_SnapsDown_NegativeError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 4000 < 12000: first candidate is 2×(24000).
	// dLast = 4000 (distance to 0), dCur = 20000 (distance to 24000)
	// → snaps DOWN to 0 (the implicit lower bound before first power).
	auto [len, err] = t.QuantiseLength(4000ul);

	ASSERT_EQ(0ul, len);
	ASSERT_EQ(-4000, err);
}
