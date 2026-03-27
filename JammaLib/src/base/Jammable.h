#pragma once

#include <memory>
#include "GuiElement.h"
#include "Tweakable.h"
#include "MultiAudioSource.h"
#include "MultiAudioSink.h"

namespace base
{
	enum SelectDepth
	{
		DEPTH_STATION,
		DEPTH_LOOPTAKE,
		DEPTH_LOOP
	};

	class JammableParams :
		public GuiElementParams,
		public TweakableParams
	{
	public:
		std::string Texture;
	};

	class Jammable :
		public GuiElement,
		public Tweakable,
		public MultiAudioSource
	{
	public:
		Jammable(JammableParams params) :
			GuiElement(params),
			Tweakable({}),
			MultiAudioSource(),
			_selectDepth(DEPTH_STATION),
			_jammableParams(params)
		{
		};

	public:
		virtual std::string ClassName() const override { return "Jammable"; }
		virtual SelectDepth Depth() const { return SelectDepth::DEPTH_STATION; }
		virtual MultiAudioPlugType MultiAudioPlug() const override { return MULTIAUDIOPLUG_BOTH; }
		virtual void Reset() { _selectDepth = DEPTH_STATION; }

		virtual void SetSelectDepth(SelectDepth depth)
		{
			_selectDepth = depth;

			for (const auto& child : _children)
			{
				if (auto jammable = std::dynamic_pointer_cast<Jammable>(child))
					jammable->SetSelectDepth(depth);
			}

		}
		virtual SelectDepth CurrentSelectDepth() const { return _selectDepth; }

	protected:
		virtual void _ArrangeChildren() {}

	protected:
		SelectDepth _selectDepth;
		JammableParams _jammableParams;
	};
}