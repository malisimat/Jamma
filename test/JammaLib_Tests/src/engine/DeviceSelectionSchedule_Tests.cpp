#include "gtest/gtest.h"
#include "engine/DeviceSelectionSchedule.h"
using engine::DeviceSelectionSchedule;

TEST(DeviceSelectionScheduleTests, FormatEditCannotBypassSharedChannelDeadline)
{
	DeviceSelectionSchedule schedule;
	const auto now = DeviceSelectionSchedule::Clock::time_point{};
	schedule.Changed(DeviceSelectionSchedule::Edit::Channels, now);
	schedule.Changed(DeviceSelectionSchedule::Edit::Format, now + std::chrono::seconds(1));
	EXPECT_FALSE(schedule.Eligible(now + std::chrono::seconds(2)));
	schedule.Changed(DeviceSelectionSchedule::Edit::Channels, now + std::chrono::seconds(2));
	EXPECT_FALSE(schedule.Eligible(now + std::chrono::seconds(4)));
	EXPECT_TRUE(schedule.Eligible(now + std::chrono::seconds(5)));
}
TEST(DeviceSelectionScheduleTests, DeviceSupersedesDeadlineAndFailureDoesNotRetry)
{
	DeviceSelectionSchedule schedule;
	const auto now = DeviceSelectionSchedule::Clock::time_point{};
	schedule.Changed(DeviceSelectionSchedule::Edit::Channels, now);
	schedule.Changed(DeviceSelectionSchedule::Edit::Device, now);
	EXPECT_TRUE(schedule.Eligible(now));
	schedule.Attempted(schedule.Revision, now);
	EXPECT_FALSE(schedule.Eligible(now + std::chrono::seconds(10)));
	schedule.Changed(DeviceSelectionSchedule::Edit::Format, now);
	EXPECT_FALSE(schedule.Eligible(now + std::chrono::milliseconds(499)));
	EXPECT_TRUE(schedule.Eligible(now + std::chrono::milliseconds(500)));
}
TEST(DeviceSelectionScheduleTests, LateCompletionKeepsNewestDraftEligible)
{
	DeviceSelectionSchedule schedule;
	const auto now = DeviceSelectionSchedule::Clock::time_point{};
	schedule.Changed(DeviceSelectionSchedule::Edit::Format, now);
	const auto admitted = schedule.Revision;
	schedule.Changed(DeviceSelectionSchedule::Edit::Device, now);
	schedule.Attempted(admitted, now);
	EXPECT_NE(schedule.Revision, schedule.AttemptedRevision);
	EXPECT_TRUE(schedule.Eligible(now + std::chrono::seconds(1)));
}
