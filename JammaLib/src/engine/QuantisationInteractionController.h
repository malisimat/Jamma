#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "../actions/ActionResult.h"
#include "../actions/TouchAction.h"
#include "../actions/TouchMoveAction.h"
#include "../base/Jammable.h"
#include "../graphics/CtrlHandleOverlay.h"
#include "../utils/CommonTypes.h"
#include "Quantisation.h"

namespace base
{
	class GuiElement;
}

namespace engine
{
	class Loop;
	class LoopTake;
	class Station;

	struct QuantisationInteractionContext
	{
		utils::Position2d CursorPos{};
		utils::Size2d ViewportSize{};
		base::SelectDepth SelectDepth = base::SelectDepth::DEPTH_STATION;
		std::vector<unsigned char> HoverPath;
		std::vector<unsigned char> HoverPath3d;
	};

	class QuantisationInteractionController
	{
	public:
		using ChildResolver = std::function<std::shared_ptr<base::GuiElement>(const std::vector<unsigned char>& path)>;

		QuantisationInteractionController(graphics::CtrlHandleOverlay& overlay,
			Quantisation& quantisation,
			std::vector<std::shared_ptr<Station>>& stations);

		void OnCtrlModifierChanged(bool held,
			Time now,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		void RefreshOverlay(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		void Tick(Time now);

		std::optional<actions::ActionResult> TryHandleTouchAction(actions::TouchAction action,
			unsigned int sampleRate,
			bool ctrlModifier,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		std::optional<actions::ActionResult> TryHandleTouchMove(actions::TouchMoveAction action,
			unsigned int sampleRate);

		int VisibleButtonCountForTest() const noexcept;
		std::optional<utils::Position2d> ButtonCenterForTest(int buttonIndex) const noexcept;

	private:
		enum class MidiPhaseDragRoute : std::uint8_t
		{
			Global,
			Local
		};

		enum class MidiPhaseDragTargetKind : std::uint8_t
		{
			Global,
			Station,
			LoopTake
		};

		struct MidiPhaseDragTarget
		{
			MidiPhaseDragTargetKind Kind = MidiPhaseDragTargetKind::Global;
			std::shared_ptr<Station> StationRef;
			std::shared_ptr<LoopTake> TakeRef;
			std::vector<std::shared_ptr<Station>> StationTargets;
			std::vector<std::shared_ptr<LoopTake>> TakeTargets;
		};

		struct CtrlOverlayContext
		{
			utils::Position2d Anchor{};
			int VisibleButtonCount = 1;
			std::vector<unsigned char> HoverPath;
			base::SelectDepth SelectDepth = base::SelectDepth::DEPTH_STATION;
		};

		void _CaptureContext(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		float _CtrlHandleAlpha(Time now) const;
		void _ApplyCtrlHandleAlpha(float alpha);
		int _VisibleButtonCount(const QuantisationInteractionContext& context) const;
		base::SelectDepth _SelectDepth(const QuantisationInteractionContext& context) const noexcept;
		std::shared_ptr<base::GuiElement> _HoverElement(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;

		actions::ActionResult _BeginMidiPhaseDrag(actions::TouchAction action,
			MidiPhaseDragRoute route,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		actions::ActionResult _UpdateMidiPhaseDrag(actions::TouchMoveAction action,
			unsigned int sampleRate);
		actions::ActionResult _EndMidiPhaseDrag(actions::TouchAction action,
			unsigned int sampleRate);

		actions::ActionResult _BeginFractionDrag(actions::TouchAction action,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver);
		actions::ActionResult _UpdateFractionDrag(actions::TouchMoveAction action);
		actions::ActionResult _EndFractionDrag(actions::TouchAction action);

		void _ForEachTake(const std::function<void(const std::shared_ptr<Station>& station,
			const std::shared_ptr<LoopTake>& take)>& visit) const;
		std::shared_ptr<Station> _StationForTake(const std::shared_ptr<LoopTake>& take) const;
		std::shared_ptr<LoopTake> _TakeForLoop(const std::shared_ptr<Loop>& loop) const;
		std::shared_ptr<LoopTake> _TakeFromElement(const std::shared_ptr<base::GuiElement>& element) const;
		std::shared_ptr<LoopTake> _FirstTakeForStation(const std::shared_ptr<Station>& station) const;
		std::vector<std::shared_ptr<LoopTake>> _ResolveFractionDragTargets(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;
		std::shared_ptr<LoopTake> _ResolveFractionDragTake(const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;
		MidiPhaseDragTarget _ResolveMidiPhaseDragTarget(MidiPhaseDragRoute route,
			const QuantisationInteractionContext& context,
			const ChildResolver& childResolver) const;
		std::int32_t _MidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target) const noexcept;
		void _SetMidiPhaseOffsetForTarget(const MidiPhaseDragTarget& target,
			std::int32_t offsetSamps) noexcept;

		graphics::CtrlHandleOverlay& _overlay;
		Quantisation& _quantisation;
		std::vector<std::shared_ptr<Station>>& _stations;

		bool _ctrlHandleHeld = false;
		Time _ctrlHandleReleasedAt;
		std::optional<CtrlOverlayContext> _ctrlOverlayContext;
		bool _isMidiPhaseDragging = false;
		utils::Position2d _midiPhaseDragStartPosition;
		std::int32_t _midiPhaseDragStartOffsetSamps = 0;
		MidiPhaseDragTarget _midiPhaseDragTarget;
		bool _isFractionDragging = false;
		int _fractionDragStartY = 0;
		std::shared_ptr<LoopTake> _fractionDragTake;
		std::vector<std::shared_ptr<LoopTake>> _fractionDragTargets;
		midi::MidiQuantisationFraction _fractionDragStartFraction;
		bool _fractionDragMoved = false;
	};
}
