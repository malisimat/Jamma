#pragma once

#include <cstdint>
#include <iosfwd>
#include "NinjamTimingCoordinator.h"

namespace ninjam
{
	// Job-thread presentation only; diagnostic capture and timing authority stay
	// with NinjamTimingCoordinator. The caller applies the verbose-event gate.
	class NinjamTimingDiagnosticPresenter
	{
	public:
		void Present(const NinjamTimingDiagnostics& diagnostics, std::ostream& output);

	private:
		std::uint64_t _lastPresentedSequence = 0u;
		std::uint64_t _lastPresentedOverflowCount = 0u;
	};
}
