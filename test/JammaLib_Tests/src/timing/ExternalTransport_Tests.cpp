#include "gtest/gtest.h"
#include "./timing/ExternalTransport.h"

#include <cstdlib>

using timing::ExternalTransport;
using timing::ExternalTransportMode;
using timing::ExternalTransportSnapshot;

namespace
{
	ExternalTransportSnapshot MakeSnapshot(unsigned int length,
		unsigned int position,
		unsigned long localAnchor)
	{
		ExternalTransportSnapshot snap;
		snap.IntervalLengthSamps = length;
		snap.IntervalPositionSamps = position;
		snap.SampleRate = 44100u;
		snap.LocalAnchorSamps = localAnchor;
		return snap;
	}
}

TEST(ExternalTransport, StartsDisconnected)
{
	ExternalTransport transport;
	EXPECT_FALSE(transport.IsConnected());
	auto state = transport.Published();
	ASSERT_TRUE(state != nullptr);
	EXPECT_EQ(ExternalTransportMode::Disconnected, state->Mode);
}

TEST(ExternalTransport, ConnectPublishesConnectedState)
{
	ExternalTransport transport;
	transport.Connect(1000ul);
	EXPECT_TRUE(transport.IsConnected());

	auto state = transport.Published();
	ASSERT_TRUE(state != nullptr);
	EXPECT_EQ(ExternalTransportMode::Connected, state->Mode);
	EXPECT_EQ(1000ul, state->MasterLoopLengthSamps);
	EXPECT_EQ(0ul, state->RemoteWrapCount);
}

TEST(ExternalTransport, IgnoresSnapshotsWhileDisconnected)
{
	ExternalTransport transport;
	transport.IngestSnapshot(MakeSnapshot(1000u, 500u, 500ul));

	auto state = transport.Published();
	ASSERT_TRUE(state != nullptr);
	EXPECT_EQ(ExternalTransportMode::Disconnected, state->Mode);
	EXPECT_EQ(0u, state->RemoteIntervalPositionSamps);
}

TEST(ExternalTransport, DerivesIntervalStartFromAnchorAndPosition)
{
	ExternalTransport transport;
	transport.Connect();

	// Non-wrapping snapshots stage but do not publish; force a wrap to observe.
	transport.IngestSnapshot(MakeSnapshot(1000u, 200u, 5200ul));
	transport.IngestSnapshot(MakeSnapshot(1000u, 100u, 5100ul)); // wrap (100 < 200)

	auto state = transport.Published();
	ASSERT_TRUE(state != nullptr);
	EXPECT_EQ(100u, state->RemoteIntervalPositionSamps);
	// Interval start = anchor - normalised position.
	EXPECT_EQ(5000ul, state->AuthoritativeIntervalStartSamps);
}

TEST(ExternalTransport, WrapDetectedExactlyOncePerInterval)
{
	ExternalTransport transport;
	transport.Connect();

	// Monotonically increasing position through one interval: no wrap yet.
	transport.IngestSnapshot(MakeSnapshot(1000u, 100u, 1100ul));
	transport.IngestSnapshot(MakeSnapshot(1000u, 400u, 1400ul));
	transport.IngestSnapshot(MakeSnapshot(1000u, 900u, 1900ul));
	EXPECT_EQ(0ul, transport.Published()->RemoteWrapCount);

	// Position drops -> exactly one wrap.
	transport.IngestSnapshot(MakeSnapshot(1000u, 50u, 2050ul));
	EXPECT_EQ(1ul, transport.Published()->RemoteWrapCount);

	// Continue increasing within the new interval: still one wrap.
	transport.IngestSnapshot(MakeSnapshot(1000u, 600u, 2600ul));
	EXPECT_EQ(1ul, transport.Published()->RemoteWrapCount);

	// Second wrap.
	transport.IngestSnapshot(MakeSnapshot(1000u, 20u, 3020ul));
	EXPECT_EQ(2ul, transport.Published()->RemoteWrapCount);
}

TEST(ExternalTransport, PublishesOnlyAtWrap)
{
	ExternalTransport transport;
	transport.Connect();
	// Initial connected publish has position 0.
	transport.IngestSnapshot(MakeSnapshot(1000u, 100u, 1100ul));
	// Not yet wrapped, published front should still show position 0.
	EXPECT_EQ(0u, transport.Published()->RemoteIntervalPositionSamps);

	transport.IngestSnapshot(MakeSnapshot(1000u, 400u, 1400ul));
	EXPECT_EQ(0u, transport.Published()->RemoteIntervalPositionSamps);

	// Wrap publishes the staged state.
	transport.IngestSnapshot(MakeSnapshot(1000u, 30u, 2030ul));
	EXPECT_EQ(30u, transport.Published()->RemoteIntervalPositionSamps);
}

TEST(ExternalTransport, MasterLoopCountTracksWraps)
{
	ExternalTransport transport;
	transport.Connect();

	transport.IngestSnapshot(MakeSnapshot(1000u, 900u, 1900ul));
	transport.IngestSnapshot(MakeSnapshot(1000u, 10u, 2010ul)); // wrap 1
	EXPECT_EQ(1ul, transport.Published()->MasterLoopCount);

	transport.IngestSnapshot(MakeSnapshot(1000u, 990u, 2990ul));
	transport.IngestSnapshot(MakeSnapshot(1000u, 5u, 3005ul)); // wrap 2
	EXPECT_EQ(2ul, transport.Published()->MasterLoopCount);
}

TEST(ExternalTransport, PendingAlignmentStableUntilWrap)
{
	ExternalTransport transport;
	transport.Connect();
	transport.IngestSnapshot(MakeSnapshot(1000u, 300u, 1300ul));

	transport.BeginJoinAlignment(700ul);

	// Alignment is staged; it is not published until a wrap occurs.
	auto beforeWrap = transport.Published();
	EXPECT_FALSE(beforeWrap->PendingAlignment.HasPending);
	EXPECT_FALSE(beforeWrap->HasCommittedAlignment);

	// A non-wrapping snapshot must not disturb the staged alignment.
	transport.IngestSnapshot(MakeSnapshot(1000u, 500u, 1500ul));
	EXPECT_FALSE(transport.Published()->HasCommittedAlignment);
}

TEST(ExternalTransport, JoinAlignmentCommitsMasterZeroAtWrap)
{
	ExternalTransport transport;
	transport.Connect();
	transport.IngestSnapshot(MakeSnapshot(1000u, 300u, 1300ul));

	transport.BeginJoinAlignment(700ul);

	// Wrap: commit alignment, zero the master loop.
	transport.IngestSnapshot(MakeSnapshot(1000u, 40u, 2040ul));

	auto state = transport.Published();
	ASSERT_TRUE(state != nullptr);
	EXPECT_TRUE(state->HasCommittedAlignment);
	EXPECT_FALSE(state->PendingAlignment.HasPending);
	EXPECT_TRUE(state->PendingAlignment.Committed);
	EXPECT_EQ(0ul, state->MasterLoopPlayPositionSamps);
	EXPECT_EQ(state->RemoteWrapCount, state->LastCommittedRemoteWrap);
}

TEST(ExternalTransport, JoinAlignmentDeltaIsPhaseDifference)
{
	ExternalTransport transport;
	transport.Connect();
	transport.IngestSnapshot(MakeSnapshot(1000u, 300u, 1300ul));

	// Local master phase 700, remote phase 300 -> delta = 300 - 700 = -400.
	transport.BeginJoinAlignment(700ul);
	transport.IngestSnapshot(MakeSnapshot(1000u, 40u, 2040ul)); // wrap publishes

	auto state = transport.Published();
	ASSERT_TRUE(state != nullptr);
	EXPECT_EQ(700ul, state->PendingAlignment.LocalMasterPositionAtJoin);
	EXPECT_EQ(300u, state->PendingAlignment.RemoteIntervalPositionAtJoin);
	EXPECT_EQ(-400, state->PendingAlignment.AlignmentDeltaSamps);
}

TEST(ExternalTransport, DisconnectResetsRuntimeState)
{
	ExternalTransport transport;
	transport.Connect();
	transport.IngestSnapshot(MakeSnapshot(1000u, 900u, 1900ul));
	transport.IngestSnapshot(MakeSnapshot(1000u, 10u, 2010ul)); // wrap

	transport.Disconnect();
	EXPECT_FALSE(transport.IsConnected());

	auto state = transport.Published();
	ASSERT_TRUE(state != nullptr);
	EXPECT_EQ(ExternalTransportMode::Disconnected, state->Mode);
	EXPECT_EQ(0ul, state->RemoteWrapCount);
	EXPECT_EQ(0u, state->RemoteIntervalPositionSamps);

	// Reconnecting rebuilds runtime state from scratch (no stale wrap count).
	transport.Connect();
	EXPECT_EQ(0ul, transport.Published()->RemoteWrapCount);
}

// --- Master-relative re-anchoring ------------------------------------------------
// When the external transport wraps, a loop take's play position must be re-derived
// from its master-relative anchor back to where it would naturally have advanced to,
// NOT reset to zero.  These tests pin that round-trip property.

TEST(ExternalTransportReanchor, ReDerivesTakePositionExactlyAcrossWrap)
{
	constexpr unsigned long masterLen = 88200ul;
	constexpr unsigned long takeLen = 30000ul;

	// Observed mid-interval: master loop 3, offset 50000, take playing at 12345.
	const auto absBefore = ExternalTransport::AbsoluteMasterSample(3ul, masterLen, 50000ul);
	const auto takePosBefore = 12345ul;
	const auto anchor = ExternalTransport::TakeAnchorSample(absBefore, takePosBefore, takeLen);

	// Advance to the next remote wrap (offset 50000 -> 0, loop 3 -> 4).
	const auto gap = masterLen - 50000ul; // 38200 samples until the wrap
	const auto absAfter = ExternalTransport::AbsoluteMasterSample(4ul, masterLen, 0ul);

	const auto natural = (takePosBefore + gap) % takeLen; // free-running position
	const auto derived = ExternalTransport::TakePositionFromAnchor(absAfter, anchor, takeLen);

	EXPECT_EQ(natural, derived);
	// The whole point: it does NOT snap to zero.
	EXPECT_NE(0ul, derived);
}

TEST(ExternalTransportReanchor, DifferentTakeLengthsEachDeriveBack)
{
	constexpr unsigned long masterLen = 88200ul;
	const unsigned long takeLens[] = { 44100ul, 30000ul, 17640ul, 100000ul };

	const auto absBefore = ExternalTransport::AbsoluteMasterSample(7ul, masterLen, 61234ul);
	const auto gap = masterLen - 61234ul;
	const auto absAfter = ExternalTransport::AbsoluteMasterSample(8ul, masterLen, 0ul);

	for (const auto takeLen : takeLens)
	{
		const auto takePosBefore = 9876ul % takeLen;
		const auto anchor = ExternalTransport::TakeAnchorSample(absBefore, takePosBefore, takeLen);
		const auto natural = (takePosBefore + gap) % takeLen;
		const auto derived = ExternalTransport::TakePositionFromAnchor(absAfter, anchor, takeLen);
		EXPECT_EQ(natural, derived) << "takeLen=" << takeLen;
	}
}

TEST(ExternalTransportReanchor, StaysCloseUnderObservationRounding)
{
	// The observed master offset is only known to job-tick granularity, so the
	// anchor carries a bounded error.  The re-derived position must stay within that
	// same small granularity of the true free-running position (never a big jump).
	constexpr unsigned long masterLen = 88200ul;
	constexpr unsigned long takeLen = 30000ul;
	constexpr unsigned long jobTickSamps = 1024ul; // ~job-tick granularity

	const auto trueOffset = 50123ul;
	const auto observedOffset = (trueOffset / jobTickSamps) * jobTickSamps; // rounded down

	const auto absTrue = ExternalTransport::AbsoluteMasterSample(2ul, masterLen, trueOffset);
	const auto absObserved = ExternalTransport::AbsoluteMasterSample(2ul, masterLen, observedOffset);

	const auto takePosBefore = 4321ul;
	// Anchor is built from the (rounded) observed sample.
	const auto anchor = ExternalTransport::TakeAnchorSample(absObserved, takePosBefore, takeLen);

	const auto gap = masterLen - trueOffset;
	const auto absAfter = ExternalTransport::AbsoluteMasterSample(3ul, masterLen, 0ul);

	const auto natural = (takePosBefore + gap) % takeLen;
	const auto derived = ExternalTransport::TakePositionFromAnchor(absAfter, anchor, takeLen);

	// Circular distance between derived and natural must be within the rounding.
	long long diff = static_cast<long long>(derived) - static_cast<long long>(natural);
	const long long len = static_cast<long long>(takeLen);
	if (diff > len / 2) diff -= len;
	else if (diff < -(len / 2)) diff += len;
	EXPECT_LE(std::llabs(diff), static_cast<long long>(jobTickSamps));

	(void)absTrue;
}

TEST(ExternalTransportReanchor, ZeroAnchorMustNotBeUsed)
{
	// When no anchor has been set (_masterAnchorSample == 0), calling
	// TakePositionFromAnchor with anchor=0 returns 0 (start-of-loop) — wrong.
	// This pins the contract that RepositionFromAnchor must guard against zero.
	const auto wrongPos = ExternalTransport::TakePositionFromAnchor(88200ul, 0ul, 44100ul);
	EXPECT_EQ(0ul, wrongPos); // confirms zero anchor != correct position
}

TEST(ExternalTransportReanchor, AnchorStableAcrossMultipleWraps)
{
	// The anchor is set once at play time and must give the correct re-derived
	// position at wrap 1, wrap 2, and wrap 3 without being updated.
	constexpr unsigned long masterLen = 44100ul;
	constexpr unsigned long takeLen = 20000ul;
	constexpr unsigned long playPosBefore = 7777ul;

	const auto absAtPlay = ExternalTransport::AbsoluteMasterSample(0ul, masterLen, 10000ul);
	const auto anchor = ExternalTransport::TakeAnchorSample(absAtPlay, playPosBefore, takeLen);

	for (unsigned long wrap = 1ul; wrap <= 3ul; ++wrap)
	{
		const auto absNow = ExternalTransport::AbsoluteMasterSample(wrap, masterLen, 0ul);
		const auto samplesTravelled = absNow - absAtPlay;
		const auto expected = (playPosBefore + samplesTravelled) % takeLen;
		const auto derived = ExternalTransport::TakePositionFromAnchor(absNow, anchor, takeLen);
		EXPECT_EQ(expected, derived) << "wrap=" << wrap;
	}
}

