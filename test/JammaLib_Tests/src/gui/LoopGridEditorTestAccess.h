#pragma once
#include "midi/LoopGridEditor.h"
#include "graphics/LoopGridProjection.h"
namespace midi
{
	class LoopGridEditorTestAccess
	{
	public:
		static double MidiPixelsPerSample(const LoopGridEditor& editor) { const auto loop=editor._midiLoop.lock(); return loop ? editor._PixelsPerSample(loop->CompletedLengthForEditor()) : 0.0; }
		static std::optional<MidiGridGesture::Point> MidiHitPoint(const LoopGridEditor& editor, utils::Position2d pixel)
		{ return editor._PointAt(pixel, false); }
		static bool HasMidiEdgeAt(const LoopGridEditor& editor, utils::Position2d pixel)
		{
			const auto point = editor._PointAt(pixel, false); const auto loop = editor._midiLoop.lock();
			MidiLoop::EditState state;
			return point && loop && loop->SnapshotForEdit(state) && MidiGridTargets::Build(state).ResolveEdge(point->EdgePosition(),
				point->Pitch, editor._PixelsPerSample(state.LoopLengthSamps), state.LoopLengthSamps).NoteIndex.has_value();
		}
		static std::optional<utils::Position2d> MidiPoint(const LoopGridEditor& editor, double u, int pitch)
		{
			const auto loop = editor._midiLoop.lock();
			if (!loop || !loop->Model()) return std::nullopt;
			const auto model = loop->Model(); const auto radius = editor._MidiRadius(loop->CompletedLengthForEditor());
			const auto v = (pitch + 0.5 - model->EditorBottomPitch()) / model->EditorVisibleRows();
			return graphics::LoopGridProjection::Project(editor._ViewProjection(), editor._ModelMatrix(),
				{static_cast<float>((u - 0.5) * 2.0 * radius), 2.0f, static_cast<float>((0.5 - v) * 1.56 * radius)},
				static_cast<int>(editor._size.Width), static_cast<int>(editor._size.Height));
		}
		static unsigned int DisplayedChannel(const LoopGridEditor& editor) { return static_cast<unsigned int>(editor._channelInput->Value()); }
		static utils::Position2d ButtonPoint(const LoopGridEditor& editor) { return editor._button->GlobalPosition() + utils::Position2d{10, 10}; }
		static bool HasCancelledButtonRelease(const LoopGridEditor& editor) { return editor._consumeButtonRelease; }
		static bool ButtonVisible(const LoopGridEditor& editor) { return editor._button->IsVisible(); }
		static const std::string& Feedback(const LoopGridEditor& editor) { return editor._feedbackText; }
		static std::size_t SelectionHistorySize(const LoopGridEditor& editor) { return editor._selectionHistory.size(); }
	};
}
