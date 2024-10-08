#pragma once

#include <algorithm>
#include <functional>
#include "../utils/CommonTypes.h"
#include "GuiElement.h"
#include "ActionSender.h"
#include "ActionUndo.h"
#include "../resources/TextureResource.h"

namespace gui
{

	class GuiSliderParams : public base::GuiElementParams
	{
	public:
		GuiSliderParams() :
			base::GuiElementParams(DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			Orientation(SLIDER_VERTICAL),
			Min(0.0),
			Max(1.0),
			Steps(0),
			DragTexture(""),
			DragOverTexture(""),
			DragDownTexture(""),
			DragOutTexture(""),
			DragControlOffset({ 0,0 }),
			DragControlSize({ 1,1 }),
			DragGap({ 0,0 })
		{
		}

		GuiSliderParams(GuiElementParams params) :
			base::GuiElementParams(params),
			Orientation(SLIDER_VERTICAL),
			Min(0.0),
			Max(1.0),
			Steps(0),
			DragTexture(""),
			DragOverTexture(""),
			DragDownTexture(""),
			DragOutTexture(""),
			DragControlOffset({ 0,0 }),
			DragControlSize({ 1,1 }),
			DragGap({ 0,0 })
		{
		}

		enum SliderOrientation
		{
			SLIDER_VERTICAL,
			SLIDER_HORIZONTAL
		};

	public:
		SliderOrientation Orientation;
		double Min;
		double Max;
		unsigned int Steps;
		double InitValue;
		std::string DragTexture;
		std::string DragOverTexture;
		std::string DragDownTexture;
		std::string DragOutTexture;
		utils::Position2d DragControlOffset;
		utils::Size2d DragControlSize;
		utils::Size2d DragGap;
	};

	class GuiSlider :
		public base::GuiElement
	{
	public:
		GuiSlider(GuiSliderParams guiParams);

	public:
		double Value() const;
		void SetValue(double value);
		void SetValue(double value, bool bypassUpdate);
		void SetDragParams(utils::Position2d dragOffset,
			utils::Size2d dragSize,
			utils::Size2d dragGap);

		virtual std::string ClassName() const { return "GuiSlider"; }

		virtual	void SetSize(utils::Size2d size) override;
		virtual bool HitTest(utils::Position2d pos) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual bool Undo(std::shared_ptr<base::ActionUndo> undo) override;
		virtual bool Redo(std::shared_ptr<base::ActionUndo> undo) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		static double CalcValueOffset(GuiSliderParams params,
			utils::Size2d size,
			utils::Position2d dragPos,
			utils::Position2d initDragPos,
			double initValue);
		static utils::Position2d CalcDragPos(GuiSliderParams params, utils::Size2d size, double value);
		static unsigned int CalcDragLength(GuiSliderParams params, utils::Size2d size);
		void OnValueChange(bool bypassUpdate);

	private:
		GuiSliderParams _sliderParams;
		bool _isDragging;
		utils::Position2d _initClickPos;
		utils::Position2d _initDragPos;
		base::GuiElement _dragElement;
		double _valueOffset;
		double _initValue;
	};
}
