#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include "GuiElement.h"
#include "Drawable.h"
#include "../graphics/Font.h"
#include "../resources/ResourceLib.h"

namespace gui
{
	class GuiLabelParams : public base::GuiElementParams
	{
	public:
		static constexpr unsigned int HeaderHeight = 22u;
		static constexpr unsigned int HeaderMinWidth = 40u;
		static constexpr unsigned int RowWidth = 260u;
		static constexpr unsigned int RowHeight = 24u;
		static constexpr unsigned int RowMinWidth = 40u;

		GuiLabelParams() :
			base::GuiElementParams()
		{}

		GuiLabelParams(base::GuiElementParams params,
			std::string string) :
			base::GuiElementParams(params),
			String(string)
		{}

		static GuiLabelParams PanelHeader(const std::string& text, unsigned int width)
		{
			GuiLabelParams params;
			params.String = text;
			params.Size = { width, HeaderHeight };
			params.MinSize = { HeaderMinWidth, HeaderHeight };
			return params;
		}

		static GuiLabelParams PanelScrollRow(const std::string& text)
		{
			GuiLabelParams params;
			params.String = text;
			params.Size = { RowWidth, RowHeight };
			params.MinSize = { RowMinWidth, RowHeight };
			return params;
		}

	public:
		std::string String;
	};

	class GuiLabel :
		public base::GuiElement
	{
	public:
		GuiLabel(GuiLabelParams guiParams);
		void SetString(const std::string& str);

	public:
		virtual void Draw(base::DrawContext& ctx) override;
		virtual utils::Size2d ContentSize() const override;
		virtual void SetSize(utils::Size2d size) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	private:
		void SyncVertexArray();
		bool InitVertexArray();
		bool _ResolveFont(resources::ResourceLib& resourceLib);

	private:
		std::string _str;
		std::string _pendingStr;
		mutable std::mutex _stringMutex;
		std::atomic<bool> _vertexArrayDirty;
		GLuint _vertexArray;
		GLuint _vertexBuffers[2];
		std::weak_ptr<resources::TextureResource> _texture;
		std::weak_ptr<resources::ShaderResource> _shader;
		std::weak_ptr<graphics::Font> _font;
		resources::ResourceLib* _resourceLib;
		graphics::FontOptions::FontSize _selectedFontSize;
		unsigned int _resolvedTextPixelHeight;
	};
}
