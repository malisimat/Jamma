#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include "../utils/CommonTypes.h"
#include "../actions/GuiAction.h"
#include "GuiElement.h"
#include "ActionSender.h"
#include "ActionUndo.h"
// Forward declaration to avoid circular include (AudioMixer.h -> GuiSlider.h).
namespace audio { class AudioMixer; }

namespace gui 
{

	class GuiSliderParams : public base::GuiElementParams
	{
	public:
		static constexpr unsigned int DefaultHeight = 34u;
		static constexpr unsigned int DefaultWidth = 104u;
		static constexpr unsigned int WideWidth = 320u;
		static constexpr unsigned int DefaultMinWidth = 36u;
		static constexpr unsigned int DefaultDragWidth = 10u;

		GuiSliderParams() :
			base::GuiElementParams(0, DrawableParams{ "" },
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
			InitValue(0.0),
			DragTexture(""),
			DragOverTexture(""),
			DragDownTexture(""),
			DragOutTexture(""),
			DragControlOffset({ 0,0 }),
			DragControlSize({ 1,1 }),
			DragGap({ 0,0 }),
			ScaleMarksEnabled(false)
		{
			GuiPassThrough = false;
		}

		GuiSliderParams(GuiElementParams params) :
			base::GuiElementParams(params),
			Orientation(SLIDER_VERTICAL),
			Min(0.0),
			Max(1.0),
			Steps(0),
			InitValue(0.0),
			DragTexture(""),
			DragOverTexture(""),
			DragDownTexture(""),
			DragOutTexture(""),
			DragControlOffset({ 0,0 }),
			DragControlSize({ 1,1 }),
			DragGap({ 0,0 }),
			ScaleMarksEnabled(false)
		{
			GuiPassThrough = false;
		}

		static void ApplyPanelTextures(GuiSliderParams& params)
		{
			params.TextureShader = "texture_tinted";
			params.Texture = "rounded_but";
			params.OverTexture = "rounded_but_over";
			params.DownTexture = "rounded_but_down";
			params.OutTexture = "rounded_but_on";
			params.DragOverTexture = "yellow";
		}

		static GuiSliderParams PanelHorizontal(const std::string& dragTexture,
			unsigned int width = DefaultWidth)
		{
			GuiSliderParams params;
			ApplyPanelTextures(params);
			params.Orientation = SLIDER_HORIZONTAL;
			params.Size = { width, DefaultHeight };
			params.MinSize = { DefaultMinWidth, DefaultHeight };
			params.DragTexture = dragTexture;
			params.DragControlSize = { DefaultDragWidth, DefaultHeight };
			params.DragControlOffset = { 0, 0 };
			return params;
		}

		enum SliderOrientation
		{
			SLIDER_VERTICAL,
			SLIDER_HORIZONTAL
		};

		enum class SliderScale
		{
			Linear,
			Decibels
		};

		// Values remain linear gain; only handle travel uses decibels.
		double ValueToFraction(double value) const;
		double FractionToValue(double fraction) const;

	public:
		SliderOrientation Orientation;
		double Min;
		double Max;
		SliderScale Scale = SliderScale::Linear;
		// Decibel faders reserve fraction zero for Min (normally silence).
		double MinDecibels = -60.0;
		unsigned int Steps;
		double InitValue;
		std::string DragTexture;
		std::string DragOverTexture;
		std::string DragDownTexture;
		std::string DragOutTexture;
		utils::Position2d DragControlOffset;
		utils::Size2d DragControlSize;
		utils::Size2d DragGap;
		bool ScaleMarksEnabled;
	};

	class GuiSlider :
		public base::GuiElement
	{
	public:
		enum class ScaleMarkKind
		{
			Endpoint,
			Unity,
			Major,
			Minor
		};

		struct ScaleMark
		{
			int CentreY;
			double Gain;
			ScaleMarkKind Kind;
		};

		struct ScaleTrackBounds
		{
			utils::Rect2d Bounds;
			bool Valid = false;
		};

		GuiSlider(GuiSliderParams guiParams);
		static std::vector<ScaleMark> BuildScaleMarks(const GuiSliderParams& params, utils::Size2d size);
		static ScaleTrackBounds BuildScaleTrackBounds(const GuiSliderParams& params, utils::Size2d size);

	public:
		using base::GuiElement::OnAction;

		double Value() const;
		void SetValue(double value);
		void SetValue(double value, bool bypassUpdate);
		void SetDragParams(utils::Position2d dragOffset,
			utils::Size2d dragSize,
			utils::Size2d dragGap);

		virtual std::string ClassName() const { return "GuiSlider"; }

		virtual	void SetSize(utils::Size2d size) override;
		virtual void Draw(base::DrawContext& ctx) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual void ClearPointerState() override;
		virtual bool Undo(std::shared_ptr<base::ActionUndo> undo) override;
		virtual bool Redo(std::shared_ptr<base::ActionUndo> undo) override;

		bool DragHandleIsOverForTest() const noexcept;

		// VU meter — connects this slider to its owning AudioMixer for display.
		void SetMixer(std::shared_ptr<audio::AudioMixer> mixer);
		void SetVuVisible(bool visible);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual bool _HitTest(utils::Position2d localPos) override;
		virtual void ApplyHoverPoint(utils::Position2d localPos) override;

		static double CalcValueOffset(GuiSliderParams params,
			utils::Size2d size,
			utils::Position2d dragPos,
			utils::Position2d initDragPos,
			double initValue);
		static utils::Position2d CalcDragPos(GuiSliderParams params, utils::Size2d size, double value);
		static unsigned int CalcDragLength(GuiSliderParams params, utils::Size2d size);
		void OnValueChange(bool bypassUpdate);
		void _EnsureScaleLayout();
		static void _BuildScaleMarks(const GuiSliderParams& params, utils::Size2d size, std::vector<ScaleMark>& marks);
		void _UpdateScaleImageWidths(unsigned int panelWidth);
		static graphics::ImageParams _MakeScaleImageParams(const std::string& texture, utils::Size2d size);
		bool _ScaleLayoutKeyMatches() const;
		void _StoreScaleLayoutKey();

	private:
		GuiSliderParams _sliderParams;
		bool _isDragging;
		utils::Position2d _initClickPos;
		utils::Position2d _initDragPos;
		base::GuiElement _dragElement;
		graphics::Image _scaleUnityImage;
		graphics::Image _scaleEndpointImage;
		graphics::Image _scaleMajorImage;
		graphics::Image _scaleMinorImage;
		graphics::Image _scaleTrackImage;
		std::vector<ScaleMark> _scaleMarks;
		ScaleTrackBounds _scaleTrackBounds;
		bool _scaleLayoutValid;
		utils::Size2d _scaleLayoutSize;
		utils::Size2d _scaleLayoutHandleSize;
		utils::Position2d _scaleLayoutHandleOffset;
		utils::Size2d _scaleLayoutGap;
		GuiSliderParams::SliderOrientation _scaleLayoutOrientation;
		GuiSliderParams::SliderScale _scaleLayoutScale;
		double _scaleLayoutMin;
		double _scaleLayoutMax;
		double _scaleLayoutMinDecibels;
		bool _scaleLayoutEnabled;
		unsigned int _scaleImagePanelWidth;
		utils::Rect2d _scaleAppliedTrackBounds;
		bool _scaleTrackSizeApplied;
		double _valueOffset;
		double _initValue;
		std::weak_ptr<audio::AudioMixer> _mixer;
	};
}
