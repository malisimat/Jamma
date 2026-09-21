#include "gtest/gtest.h"
#include "gui/CableInteraction.h"

using gui::CableInteraction;

class CableInteractionTests : public ::testing::Test
{
protected:
	static io::RigFile Rig()
	{
		io::RigFile rig;
		rig.Triggers.resize(2u);
		rig.Triggers[0].Name = "Trigger-1";
		rig.Triggers[0].InputChannels = { 1u };
		rig.Triggers[0].MidiInputs = io::RigFile::Trigger::MidiInputMode::Selected;
		rig.Triggers[0].MidiInputDevices = { "Keys" };
		rig.Triggers[0].StationTarget = "A";
		rig.Triggers[1].Name = "Trigger-2";
		rig.Triggers[1].MidiInputs = io::RigFile::Trigger::MidiInputMode::None;
		rig.Triggers[1].StationTarget = "";
		return rig;
	}

	static CableInteraction::Endpoint Adc(unsigned int channel, int x, int y, bool available = true)
	{
		return { CableInteraction::EndpointKind::AdcSource, { x, y }, {}, {}, {},
			io::RigFileRouting::Source{ io::RigFileRouting::SourceKind::Adc, channel, {}, available }, available };
	}

	static CableInteraction::Endpoint Input(size_t trigger, int x, int y)
	{
		return { CableInteraction::EndpointKind::TriggerInput, { x, y }, trigger };
	}
};

TEST_F(CableInteractionTests, SpreadPlacesSingleAtCentreAndManyAtInclusiveEnds)
{
	EXPECT_TRUE(CableInteraction::Spread(10, 30, 0u).empty());
	EXPECT_EQ((std::vector<int>{ 20 }), CableInteraction::Spread(10, 30, 1u));
	EXPECT_EQ((std::vector<int>{ 10, 20, 30 }), CableInteraction::Spread(10, 30, 3u));
}

TEST_F(CableInteractionTests, EndpointHitTestingUsesLargerCircularTargetAndNearestWins)
{
	const std::vector<CableInteraction::Endpoint> endpoints{ Adc(0u, 0, 0), Adc(1u, 8, 0) };
	EXPECT_TRUE(CableInteraction::HitTest(endpoints[0], { 3, 4 }, 5.0f));
	EXPECT_FALSE(CableInteraction::HitTest(endpoints[0], { 6, 0 }, 5.0f));
	EXPECT_EQ(1u, CableInteraction::HitEndpoint(endpoints, { 7, 0 }, 10.0f).value());
}

TEST_F(CableInteractionTests, CableBodyHitAndClosestEndUseScreenSpace)
{
	CableInteraction::Cable cable{ {}, Adc(0u, 0, 0), Input(0u, 100, 0) };
	EXPECT_EQ(0u, CableInteraction::HitCable({ cable }, { 50, 4 }, 5.0f).value());
	EXPECT_FALSE(CableInteraction::HitCable({ cable }, { 50, 7 }, 5.0f).has_value());
	EXPECT_EQ(CableInteraction::End::Start, CableInteraction::ClosestEnd(cable, { 20, 0 }));
	EXPECT_EQ(CableInteraction::End::Finish, CableInteraction::ClosestEnd(cable, { 80, 0 }));
}

TEST_F(CableInteractionTests, CompatibilityIsDirectionalAvailableAndExcludesDuplicateCapture)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), {}, { 0, 0 }, {} };
	EXPECT_FALSE(CableInteraction::Compatible(drag, Adc(1u, 0, 0), rig));
	EXPECT_TRUE(CableInteraction::Compatible(drag, Adc(2u, 0, 0), rig));
	EXPECT_FALSE(CableInteraction::Compatible(drag, Adc(2u, 0, 0, false), rig));
	EXPECT_FALSE(CableInteraction::Compatible(drag, Input(0u, 0, 0), rig));
	drag.OriginalSource = Adc(1u, 0, 0).Source;
	EXPECT_TRUE(CableInteraction::Compatible(drag, Adc(1u, 0, 0), rig));
}

TEST_F(CableInteractionTests, NearestSnapChoosesOnlyViableEndpoint)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), {}, { 3, 0 }, {} };
	const std::vector<CableInteraction::Endpoint> endpoints{ Adc(1u, 2, 0), Adc(2u, 8, 0), Adc(3u, 12, 0) };
	EXPECT_EQ(1u, CableInteraction::NearestViable(drag, endpoints, rig, 20.0f).value());
}

TEST_F(CableInteractionTests, HysteresisRetainsSnapUntilOuterRadiusIsExceeded)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), {}, { 0, 0 }, {} };
	const std::vector<CableInteraction::Endpoint> endpoints{ Adc(2u, 10, 0), Adc(3u, 35, 0) };
	CableInteraction::Update(drag, { 10, 0 }, endpoints, rig, 10.0f, 5.0f);
	ASSERT_TRUE(drag.Snap.has_value());
	CableInteraction::Update(drag, { 24, 0 }, endpoints, rig, 10.0f, 5.0f);
	EXPECT_EQ(10, drag.Snap->Position.X);
	CableInteraction::Update(drag, { 35, 0 }, endpoints, rig, 10.0f, 5.0f);
	EXPECT_EQ(35, drag.Snap->Position.X);
}

TEST_F(CableInteractionTests, PreviewLeavesOriginalCableUntouchedAndCancelClearsOnlyDrag)
{
	CableInteraction::Drag drag{ {}, CableInteraction::End::Start, Input(0u, 100, 20), {}, { 30, 40 }, {} };
	const auto preview = CableInteraction::Preview(drag);
	EXPECT_EQ(30, preview.first.X);
	EXPECT_EQ(100, preview.second.X);
	std::optional<CableInteraction::Drag> state = drag;
	CableInteraction::Cancel(state);
	EXPECT_FALSE(state.has_value());
}

TEST_F(CableInteractionTests, ReleaseEmptySpaceRemovesCaptureAndSnapReplacesIt)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(0u, 100, 0), Adc(1u, 0, 0).Source, { 0, 0 }, {} };
	auto removed = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(removed.Candidate.has_value());
	EXPECT_TRUE(removed.Candidate->Triggers[0].InputChannels.empty());
	drag.Snap = Adc(2u, 0, 0);
	auto replaced = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(replaced.Candidate.has_value());
	EXPECT_EQ((std::vector<unsigned int>{ 2u }), replaced.Candidate->Triggers[0].InputChannels);
}

TEST_F(CableInteractionTests, ReleaseSourceToTriggerCreatesCaptureRoute)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, static_cast<size_t>(-1), CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Finish, Adc(3u, 0, 0), {}, { 100, 0 }, Input(1u, 100, 0) };
	auto release = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(release.Candidate.has_value());
	EXPECT_EQ((std::vector<unsigned int>{ 3u }), release.Candidate->Triggers[1].InputChannels);
}

TEST_F(CableInteractionTests, UnavailableFixedSourceCannotCreateCaptureRoute)
{
	auto rig = Rig();
	CableInteraction::Drag drag{ { 4u, static_cast<size_t>(-1), CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Finish, Adc(3u, 0, 0, false), {}, { 100, 0 }, Input(1u, 100, 0) };

	EXPECT_FALSE(CableInteraction::Compatible(drag, Input(1u, 100, 0), rig));
	const auto release = CableInteraction::ReleaseToCandidate(drag, rig);
	EXPECT_FALSE(release.Candidate.has_value());
	EXPECT_FALSE(release.Changed);
}

TEST_F(CableInteractionTests, ExplicitAnyMidiCanBeCreatedReplacedAndRemoved)
{
	auto rig = Rig();
	rig.Triggers[1].MidiInputs = io::RigFile::Trigger::MidiInputMode::None;
	CableInteraction::Endpoint any{ CableInteraction::EndpointKind::MidiSource, { 0, 0 }, {}, {}, {},
		io::RigFileRouting::Source{ io::RigFileRouting::SourceKind::Midi, 0u, "*", true } };
	CableInteraction::Drag create{ { 4u, static_cast<size_t>(-1), CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Finish, any, {}, { 100, 0 }, Input(1u, 100, 0) };
	auto created = CableInteraction::ReleaseToCandidate(create, rig);
	ASSERT_TRUE(created.Candidate.has_value());
	EXPECT_EQ(io::RigFile::Trigger::MidiInputMode::Any, created.Candidate->Triggers[1].MidiInputs);

	CableInteraction::Drag remove{ { 5u, 1u, CableInteraction::RouteKind::Capture, 0u },
		CableInteraction::End::Start, Input(1u, 100, 0), any.Source, { 0, 0 }, {} };
	auto removed = CableInteraction::ReleaseToCandidate(remove, created.Candidate.value());
	ASSERT_TRUE(removed.Candidate.has_value());
	EXPECT_EQ(io::RigFile::Trigger::MidiInputMode::None, removed.Candidate->Triggers[1].MidiInputs);
}

TEST_F(CableInteractionTests, StationCompatibilityAcceptsOnlyCorrectDirection)
{
	auto rig = Rig();
	CableInteraction::Endpoint output{ CableInteraction::EndpointKind::TriggerOutput, { 0, 0 }, 0u };
	CableInteraction::Endpoint station{ CableInteraction::EndpointKind::Station, { 100, 0 }, {}, 1u, "B" };
	CableInteraction::Drag targetDrag{ { 4u, 0u, CableInteraction::RouteKind::Station, 0u },
		CableInteraction::End::Finish, output, {}, { 0, 0 }, {} };
	EXPECT_TRUE(CableInteraction::Compatible(targetDrag, station, rig));
	EXPECT_FALSE(CableInteraction::Compatible(targetDrag, Input(0u, 100, 0), rig));
	targetDrag.MovingEnd = CableInteraction::End::Start;
	EXPECT_TRUE(CableInteraction::Compatible(targetDrag, output, rig));
	EXPECT_FALSE(CableInteraction::Compatible(targetDrag, station, rig));
}

TEST_F(CableInteractionTests, StationReleaseMovesOrUnplugsSingleTarget)
{
	auto rig = Rig();
	CableInteraction::Endpoint output{ CableInteraction::EndpointKind::TriggerOutput, { 0, 0 }, 0u };
	CableInteraction::Drag drag{ { 4u, 0u, CableInteraction::RouteKind::Station, 0u },
		CableInteraction::End::Finish, output, {}, { 100, 0 }, {} };
	auto unplugged = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(unplugged.Candidate.has_value());
	EXPECT_EQ("", unplugged.Candidate->Triggers[0].StationTarget.value());
	drag.Snap = CableInteraction::Endpoint{ CableInteraction::EndpointKind::Station,
		{ 100, 0 }, {}, 1u, "B" };
	auto moved = CableInteraction::ReleaseToCandidate(drag, rig);
	ASSERT_TRUE(moved.Candidate.has_value());
	EXPECT_EQ("B", moved.Candidate->Triggers[0].StationTarget.value());
}
