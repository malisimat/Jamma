
#include "gtest/gtest.h"
#include "actions/JobAction.h"

using actions::JobAction;

// ---------------------------------------------------------------------------
// JobAction::operator== tests
// ---------------------------------------------------------------------------

// Non-VST job types are still de-duped by JobType + SourceId only.
TEST(JobAction, NonVstJobsEqualWhenTypeAndSourceMatch)
{
	JobAction a, b;
	a.JobActionType = JobAction::JOB_UPDATELOOPS;
	a.SourceId = "station-1";
	b.JobActionType = JobAction::JOB_UPDATELOOPS;
	b.SourceId = "station-1";

	EXPECT_TRUE(a == b);
}

TEST(JobAction, NonVstJobsNotEqualWhenSourceDiffers)
{
	JobAction a, b;
	a.JobActionType = JobAction::JOB_UPDATELOOPS;
	a.SourceId = "station-1";
	b.JobActionType = JobAction::JOB_UPDATELOOPS;
	b.SourceId = "station-2";

	EXPECT_FALSE(a == b);
}

TEST(JobAction, JobsWithDifferentTypesAreNotEqual)
{
	JobAction a, b;
	a.JobActionType = JobAction::JOB_LOADVST;
	a.SourceId = "station-1";
	b.JobActionType = JobAction::JOB_UNLOADVST;
	b.SourceId = "station-1";

	EXPECT_FALSE(a == b);
}

// Two JOB_LOADVST jobs for the same station with different plugin paths must
// NOT be considered equal — the old code would drop the second as a duplicate.
TEST(JobAction, LoadVstJobsWithDifferentPathsAreNotEqual)
{
	JobAction a, b;
	a.JobActionType = JobAction::JOB_LOADVST;
	a.SourceId = "station-1";
	a.VstPath = L"C:\\Plugins\\Delay.vst3";

	b.JobActionType = JobAction::JOB_LOADVST;
	b.SourceId = "station-1";
	b.VstPath = L"C:\\Plugins\\Reverb.vst3";

	EXPECT_FALSE(a == b);
}

// Two JOB_LOADVST jobs for the same station with the same path ARE duplicates.
TEST(JobAction, LoadVstJobsWithSamePathAreEqual)
{
	JobAction a, b;
	a.JobActionType = JobAction::JOB_LOADVST;
	a.SourceId = "station-1";
	a.VstPath = L"C:\\Plugins\\Delay.vst3";

	b.JobActionType = JobAction::JOB_LOADVST;
	b.SourceId = "station-1";
	b.VstPath = L"C:\\Plugins\\Delay.vst3";

	EXPECT_TRUE(a == b);
}

// Two JOB_UNLOADVST jobs for the same station at different indices must not
// be deduped — each removes a distinct plugin.
TEST(JobAction, UnloadVstJobsWithDifferentIndicesAreNotEqual)
{
	JobAction a, b;
	a.JobActionType = JobAction::JOB_UNLOADVST;
	a.SourceId = "station-1";
	a.VstIndex = 0;

	b.JobActionType = JobAction::JOB_UNLOADVST;
	b.SourceId = "station-1";
	b.VstIndex = 1;

	EXPECT_FALSE(a == b);
}

// Two JOB_UNLOADVST jobs for the same station at the same index ARE duplicates.
TEST(JobAction, UnloadVstJobsWithSameIndexAreEqual)
{
	JobAction a, b;
	a.JobActionType = JobAction::JOB_UNLOADVST;
	a.SourceId = "station-1";
	a.VstIndex = 2;

	b.JobActionType = JobAction::JOB_UNLOADVST;
	b.SourceId = "station-1";
	b.VstIndex = 2;

	EXPECT_TRUE(a == b);
}
