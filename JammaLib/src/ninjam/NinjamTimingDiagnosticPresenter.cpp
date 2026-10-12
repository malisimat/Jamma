#include "NinjamTimingDiagnosticPresenter.h"
#include <ostream>

using namespace ninjam;

void NinjamTimingDiagnosticPresenter::Present(const NinjamTimingDiagnostics& diagnostics,
	std::ostream& output)
{
	const auto present = [this, &output](const ninjam::NinjamTimingDiagnosticEvent& event)
	{
		output << "[NINJAM][TimingDiagnostic] reason="
			<< ninjam::NinjamTimingCoordinator::DiagnosticReasonName(event.Reason)
			<< " sessionEpoch=" << event.SessionEpoch
			<< " appliedSessionEpoch=" << event.AppliedSessionEpoch
			<< " desiredVersion=" << event.DesiredVersion
			<< " appliedVersion=" << event.AppliedVersion
			<< " generation=" << event.Generation
			<< " value=" << event.ValueSamps
			<< " limit=" << event.LimitSamps
			<< " occurrences=" << event.OccurrenceCount
			<< " cumulativeSuppressed=" << event.CumulativeSuppressedCount << '\n';
		_lastPresentedSequence = event.Sequence;
	};

	for (auto eventIndex = 0u; eventIndex < diagnostics.CapturedEventCount; ++eventIndex)
	{
		const auto& event = diagnostics.Events[eventIndex];
		if (event.Sequence > _lastPresentedSequence)
			present(event);
	}
	if (diagnostics.LatestEvent.Sequence > _lastPresentedSequence)
		present(diagnostics.LatestEvent);
	if (diagnostics.EventOverflowSummaryCount > _lastPresentedOverflowCount)
	{
		output << "[NINJAM][TimingDiagnostic] overflowSummary="
			<< diagnostics.EventOverflowSummaryCount
			<< " overflowTotal=" << diagnostics.EventOverflowCount << '\n';
		_lastPresentedOverflowCount = diagnostics.EventOverflowSummaryCount;
	}
}
