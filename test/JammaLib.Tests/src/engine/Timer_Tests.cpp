
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
// Candidates are 1×, 2×, 4×, 8×, 16×, … grain.
// Error convention: positive → recorded too long (actual > quantised, late trigger);
//                  negative → recorded too short (actual < quantised, early trigger).
// This matches QUANTISE_MULTIPLE so that LoopPlayPos works identically for both.

TEST(Timer, QuantisePower_Exactly1xGrain_ZeroError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	auto [len, err] = t.QuantiseLength(12000ul); // exactly 1×

	ASSERT_EQ(12000ul, len);
	ASSERT_EQ(0, err);
}

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

TEST(Timer, QuantisePower_NearerLower_SnapsDown_PositiveError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 30000 is 6000 above 2×(24000), 18000 below 4×(48000) → snap to 2×.
	// Actual > quantised (late trigger) → positive error.
	auto [len, err] = t.QuantiseLength(30000ul);

	ASSERT_EQ(24000ul, len);
	ASSERT_EQ(6000, err); // recorded 6000 samps too long relative to 2×
}

TEST(Timer, QuantisePower_NearerUpper_SnapsUp_NegativeError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 42000 is 18000 above 2×(24000), 6000 below 4×(48000) → snap to 4×.
	// Actual < quantised (early trigger) → negative error.
	auto [len, err] = t.QuantiseLength(42000ul);

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(-6000, err); // recorded 6000 samps too short relative to 4×
}

TEST(Timer, QuantisePower_ExactlyBetweenTwoPowers_SnapsUp) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 36000 is exactly halfway between 2×(24000) and 4×(48000).
	// Tie-break: snaps UP to the larger power.
	// Actual < quantised (early trigger) → negative error.
	auto [len, err] = t.QuantiseLength(36000ul);

	ASSERT_EQ(48000ul, len);
	ASSERT_EQ(-12000, err);
}

TEST(Timer, QuantisePower_LengthShorterThanOneGrain_SnapsDown_PositiveError) {
	Timer t;
	t.SetQuantisation(12000u, Timer::QUANTISE_POWER);

	// 4000 < 12000: compare against 0 and 1×(12000).
	// dLast = 4000 (distance to 0), dCur = 8000 (distance to 12000)
	// → snaps DOWN to 0 (the implicit lower bound before first power).
	// Actual > quantised (4000 > 0, late relative to 0) → positive error.
	auto [len, err] = t.QuantiseLength(4000ul);

	ASSERT_EQ(0ul, len);
	ASSERT_EQ(4000, err);
}

TEST(Timer, QuantisePower_NearOneGrain_DoesNotSnapToZero) {
	Timer t;
	t.SetQuantisation(88200u, Timer::QUANTISE_POWER);

	// Regression: lengths near 1× grain must quantise to 1×, not 0 or 2×.
	// Actual < quantised (85000 < 88200, early trigger) → negative error.
	auto [len, err] = t.QuantiseLength(85000ul);

	ASSERT_EQ(88200ul, len);
	ASSERT_EQ(-3200, err);
}

// ── Error-sign consistency between MULTIPLE and POWER ───────────────────────
// Both modes must use the same convention: positive error = recorded too long
// (late trigger, actual > quantised); negative error = recorded too short
// (early trigger, actual < quantised).  LoopPlayPos depends on this to compute
// the correct playback start position.

TEST(Timer, QuantisePower_ErrorSignConsistentWithMultiple_LateTrigger) {
	// A late trigger records more samples than the nearest quantised length.
	// Both QUANTISE_MULTIPLE and QUANTISE_POWER must return positive error.
	const auto grain = 44100u;
	const auto lateSamps = 512ul;
	const auto actual = (unsigned long)grain + lateSamps; // 1 grain + 512

	Timer tMultiple, tPower;
	tMultiple.SetQuantisation(grain, Timer::QUANTISE_MULTIPLE);
	tPower.SetQuantisation(grain, Timer::QUANTISE_POWER);

	auto [lenM, errM] = tMultiple.QuantiseLength(actual);
	auto [lenP, errP] = tPower.QuantiseLength(actual);

	// Both snap down to 1× grain
	ASSERT_EQ((unsigned long)grain, lenM);
	ASSERT_EQ((unsigned long)grain, lenP);

	// Late trigger → positive error in both modes.
	// (POWER currently returns -512 here, causing LoopPlayPos to move
	// the play position to the wrong end of the loop.)
	EXPECT_GT(errM, 0);
	EXPECT_GT(errP, 0);
	EXPECT_EQ(errM, errP);
}

TEST(Timer, QuantisePower_ErrorSignConsistentWithMultiple_EarlyTrigger) {
	// An early trigger records fewer samples than the nearest quantised length.
	// Both QUANTISE_MULTIPLE and QUANTISE_POWER must return negative error.
	const auto grain = 44100u;
	const auto earlySamps = 512ul;
	const auto actual = (unsigned long)grain * 2ul - earlySamps; // 2 grains - 512

	Timer tMultiple, tPower;
	tMultiple.SetQuantisation(grain, Timer::QUANTISE_MULTIPLE);
	tPower.SetQuantisation(grain, Timer::QUANTISE_POWER);

	auto [lenM, errM] = tMultiple.QuantiseLength(actual);
	auto [lenP, errP] = tPower.QuantiseLength(actual);

	// Both snap up to 2× grain
	ASSERT_EQ((unsigned long)grain * 2ul, lenM);
	ASSERT_EQ((unsigned long)grain * 2ul, lenP);

	// Early trigger → negative error in both modes.
	// (POWER currently returns +512 here.)
	EXPECT_LT(errM, 0);
	EXPECT_LT(errP, 0);
	EXPECT_EQ(errM, errP);
}
