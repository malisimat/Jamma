#pragma once

#include <memory>

#include "../base/ActionUndo.h"
#include "../engine/LoopTake.h"

namespace actions
{
	struct MidiEditRevisionCursor
	{
		std::uint64_t Revision = 0u;
	};

	// One accepted editor gesture. Undo and redo use the same take boundary as
	// ordinary edits and fail without changing history when another writer has
	// invalidated the expected revision.
	class MidiLoopEditUndo final : public base::ActionUndo
	{
	public:
		MidiLoopEditUndo(std::weak_ptr<engine::LoopTake> take,
			std::weak_ptr<midi::MidiLoop> loop,
			midi::MidiLoop::EditState before,
			midi::MidiLoop::EditState after,
			std::uint64_t acceptedRevision,
			std::shared_ptr<MidiEditRevisionCursor> cursor)
			: ActionUndo({}), _take(std::move(take)), _loop(std::move(loop)),
			_before(std::move(before)), _after(std::move(after)),
			_cursor(std::move(cursor))
		{
			if (_cursor) _cursor->Revision = acceptedRevision;
		}

		bool Undo() override { return Apply(_after, _before); }
		bool Redo() override { return Apply(_before, _after); }

	private:
		static bool SameContent(const midi::MidiLoop::EditState& lhs,
			const midi::MidiLoop::EditState& rhs) noexcept
		{
			if (lhs.EventCount != rhs.EventCount || lhs.LoopLengthSamps != rhs.LoopLengthSamps
				|| lhs.Quantisation != rhs.Quantisation
				|| lhs.QuantisationTransportStartSamps != rhs.QuantisationTransportStartSamps)
				return false;
			for (std::size_t i = 0u; i < lhs.EventCount; ++i)
			{
				const auto& a = lhs.Events[i];
				const auto& b = rhs.Events[i];
				if (a.sampleOffset != b.sampleOffset || a.status != b.status
					|| a.data1 != b.data1 || a.data2 != b.data2) return false;
			}
			return true;
		}

		bool Apply(const midi::MidiLoop::EditState& expectedContent,
			const midi::MidiLoop::EditState& source)
		{
			auto take = _take.lock();
			auto loop = _loop.lock();
			if (!take || !loop || !_cursor)
				return false;
			midi::MidiLoop::EditState current;
			if (!loop->SnapshotForEdit(current) || current.Revision != _cursor->Revision
				|| !SameContent(current, expectedContent)) return false;
			auto replacement = source;
			replacement.Revision = current.Revision;
			std::uint64_t acceptedRevision = 0u;
			if (!take->PublishMidiEdit(loop, replacement, &acceptedRevision))
				return false;
			_cursor->Revision = acceptedRevision;
			return true;
		}

		std::weak_ptr<engine::LoopTake> _take;
		std::weak_ptr<midi::MidiLoop> _loop;
		midi::MidiLoop::EditState _before;
		midi::MidiLoop::EditState _after;
		std::shared_ptr<MidiEditRevisionCursor> _cursor;
	};
}
