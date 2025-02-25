#pragma once

#include <memory>
#include "GuiElement.h"
#include "GuiLabel.h"
#include "ActionReceiver.h"
#include "../resources/ResourceLib.h"
#include "GlUtils.h"

namespace gui
{
	class GuiRouterParams :
		public base::GuiElementParams
	{
	public:
		GuiRouterParams() :
			base::GuiElementParams(0, DrawableParams{ "" },
				MoveableParams(utils::Position2d{ 0, 0 }, utils::Position3d{ 0, 0, 0 }, 1.0),
				SizeableParams{ 1,1 },
				"",
				"",
				"",
				{}),
			PinTexture(""),
			LinkTexture(""),
			DeviceInactiveTexture(""),
			DeviceActiveTexture(""),
			ChannelInactiveTexture(""),
			ChannelActiveTexture(""),
			HighlightTexture(""),
			LineShader("")
		{
		}

		GuiRouterParams(base::GuiElementParams guiParams) :
			base::GuiElementParams(guiParams),
			PinTexture(""),
			LinkTexture(""),
			DeviceInactiveTexture(""),
			DeviceActiveTexture(""),
			ChannelInactiveTexture(""),
			ChannelActiveTexture(""),
			HighlightTexture(""),
			LineShader("")
		{
		}

	public:
		std::string PinTexture;
		std::string LinkTexture;
		std::string DeviceInactiveTexture;
		std::string DeviceActiveTexture;
		std::string ChannelInactiveTexture;
		std::string ChannelActiveTexture;
		std::string HighlightTexture;
		std::string LineShader;
		std::weak_ptr<base::ActionReceiver> Receiver;
	};

	class GuiRouter :
		public base::GuiElement
	{
	public:
		class GuiRouterChannelParams :
			public base::GuiElementParams
		{
		public:
			GuiRouterChannelParams(base::GuiElementParams guiParams,
				unsigned int channel,
				bool isInput,
				bool isDevice) :
				base::GuiElementParams(guiParams),
				Channel(channel),
				IsInput(isInput),
				IsDevice(isDevice),
				ActiveTexture(""),
				HighlightTexture("")
			{
			}

		public:
			unsigned int Channel;
			bool IsInput;
			bool IsDevice;
			std::string ActiveTexture;
			std::string HighlightTexture;
		};

		class GuiRouterChannel :
			public base::GuiElement
		{
		public:
			GuiRouterChannel(GuiRouterChannelParams params);

		public:
			virtual void Draw(base::DrawContext& ctx) override;
			virtual actions::ActionResult OnAction(actions::TouchAction action) override;
			virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

			unsigned int Channel() const { return _params.Channel; }
			bool IsActive() const { return _isActive; }
			void SetActive(bool active) { _isActive = active; }

		protected:
			virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;

		protected:
			bool _isActive;
			graphics::Image _activeTexture;
			graphics::Image _highlightTexture;
			std::unique_ptr<GuiLabel> _label;
			GuiRouterChannelParams _params;
		};

	public:
		GuiRouter(GuiRouterParams guiParams,
			unsigned int numInputs,
			unsigned int numOutputs,
			bool isInputDevice,
			bool isOutputDevice);

	public:
		static const unsigned int StringToChan(std::string id);
		static const std::string ChanToString(unsigned int chan);

		virtual void Draw(base::DrawContext& ctx) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;

		bool AddRoute(unsigned int inputChan, unsigned int outputChan);
		bool RemoveRoute(unsigned int inputChan, unsigned int outputChan);
		void ClearRoutes();
		void SetReceiver(std::weak_ptr<base::ActionReceiver> receiver);

	protected:
		const static unsigned int _GetChannelPos(unsigned int index);
		const static unsigned int _GetChannel(unsigned int pos); // Inverse of GetChannelPos()

		virtual void _InitReceivers() override;
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;
		virtual bool _HitTest(utils::Position2d localPos) override;

		bool _InitShader(resources::ResourceLib& resourceLib);
		bool _InitVertexArray();
		void _DrawLines(base::DrawContext& ctx) const;

	protected:
		static const unsigned int _ChannelGapX;
		static const unsigned int _ChannelGapY;
		static const unsigned int _ChannelWidth;
		static const unsigned int _MaxRoutes;
		static const int _WireYOffset;

		GLuint _vertexArray;
		GLuint _vertexBuffer;
		bool _isDragging;
		bool _isDraggingInput;
		unsigned int _initDragChannel;
		utils::Position2d _currentDragPos;
		GuiRouterParams _routerParams;
		std::vector<std::shared_ptr<GuiRouterChannel>> _inputs;
		std::vector<std::shared_ptr<GuiRouterChannel>> _outputs;
		std::vector<std::pair<unsigned int, unsigned int>> _routes;
		std::weak_ptr<resources::ShaderResource> _lineShader;
	};
}
