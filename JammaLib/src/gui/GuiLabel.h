#pragma once

#include <memory>
#include <atomic>
#include <mutex>
#include <algorithm>
#include "GuiElement.h"
#include "Drawable.h"
#include "../graphics/Font.h"
#include "../resources/ResourceLib.h"

namespace gui
{
		struct GuiTextFrame
		{
			unsigned int PaddingX = 0u;
			unsigned int PaddingY = 0u;
			unsigned int ContentWidth = 1u;
			unsigned int TextHeight = 1u;
			int OffsetY = 0;
		};

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
				return PanelScrollRow(text, 0u);
			}

			static GuiLabelParams PanelScrollRow(const std::string& text, unsigned int horizontalInset)
		{
			GuiLabelParams params;
			params.String = text;
			params.Size = { RowWidth, RowHeight };
			params.MinSize = { RowMinWidth, RowHeight };
				params.TextInsetX = static_cast<int>(horizontalInset);
			return params;
		}

			static GuiTextFrame ResolveTextFrame(unsigned int controlWidth,
				unsigned int controlHeight,
				unsigned int paddingX,
				unsigned int paddingY,
				bool centerVertically)
			{
				GuiTextFrame frame;
				const unsigned int safeHeight = std::max(1u, controlHeight);
				const unsigned int safeWidth = std::max(1u, controlWidth);
				frame.PaddingX = std::min(paddingX, safeWidth / 2u);
				frame.PaddingY = std::min(paddingY, safeHeight / 2u);
				frame.TextHeight = resources::ResourceLib::ResolveTextPixelHeightFromControlBox(safeHeight, frame.PaddingY);
				frame.ContentWidth = std::max(1u, safeWidth > 2u * frame.PaddingX ? safeWidth - 2u * frame.PaddingX : 1u);

				if (centerVertically)
				{
					const unsigned int verticalGap = safeHeight > frame.TextHeight ? safeHeight - frame.TextHeight : 0u;
					frame.OffsetY = static_cast<int>(verticalGap / 2u);
				}
				else
				{
					frame.OffsetY = static_cast<int>(frame.PaddingY);
				}

				return frame;
			}

	public:
		std::string String;
			int TextInsetX = 0;
			int TextInsetY = 0;
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
		utils::Position2d _textInset;
		mutable std::mutex _stringMutex;
		std::atomic<bool> _vertexArrayDirty;
		GLuint _vertexArray;
		GLuint _vertexBuffers[2];
		std::weak_ptr<graphics::Font> _font;
		resources::ResourceLib* _resourceLib;
		graphics::FontOptions::FontSize _selectedFontSize;
		unsigned int _resolvedTextPixelHeight;
	};
}
