#pragma once

#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <optional>
#include <vector>
#include "GuiElement.h"
#include "Drawable.h"
#include "../graphics/Font.h"
#include "../resources/ResourceLib.h"

namespace gui
{
	enum class GuiTextVerticalAlign { Baseline, Bottom, Center, Top };
	struct GuiTextLineFrame
	{
		float BaselineY = 0.0f;
		float Bottom = 0.0f;
		float Top = 0.0f;
	};
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
		static constexpr unsigned int DefaultHeight = 22u;
		static constexpr unsigned int HeaderHeight = 22u;
		static constexpr unsigned int HeaderMinWidth = 40u;
		static constexpr unsigned int RowWidth = 260u;
		static constexpr unsigned int RowHeight = 24u;
		static constexpr unsigned int RowMinWidth = 40u;

		GuiLabelParams() :
			base::GuiElementParams()
		{
			Size = { 0u, DefaultHeight };
			MinSize = { 0u, DefaultHeight };
		}

		GuiLabelParams(base::GuiElementParams params,
			std::string string) :
			base::GuiElementParams(params),
			String(string)
		{}

		static GuiLabelParams PanelHeader(const std::string& text, int width,
			unsigned int height = HeaderHeight)
		{
			GuiLabelParams params;
			params.String = text;
			params.WrapWidth = width;
			params.Size = { width > 0 ? static_cast<unsigned int>(width) : 0u, height };
			params.MinSize = { width > 0 ? HeaderMinWidth : 0u, height };
			params.VerticalAlign = GuiTextVerticalAlign::Center;
			params.ClipText = true;
			return params;
		}

		// A flat fill for labels that do not already sit on a panel or button.
		void UseDarkFlatBackground(float opacity = 0.72f)
		{
			Texture = "panel_fill_flat";
			TextureShader = "texture_tinted";
			TintColor = {0.18f, 0.20f, 0.23f};
			TextureOpacity = opacity;
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
			params.VerticalAlign = GuiTextVerticalAlign::Center;
			params.ClipText = true;
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
				const unsigned int safeHeight = controlHeight;
				const unsigned int safeWidth = controlWidth;
				frame.PaddingX = std::min(paddingX, safeWidth / 2u);
				// Compact rows still need room for the smallest available font.
				const unsigned int paddingRoom = safeHeight > graphics::FontOptions::BasePixelHeight
					? (safeHeight - graphics::FontOptions::BasePixelHeight) / 2u : 0u;
				frame.PaddingY = std::min(paddingY, paddingRoom);
				frame.TextHeight = safeHeight - 2u * frame.PaddingY;
				frame.ContentWidth = safeWidth - 2u * frame.PaddingX;

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
		// Positive enables word wrapping within the assigned label width. Zero or
		// negative keeps a dynamic, single-line label.
		int WrapWidth = 0;
		std::optional<graphics::FontOptions::FontSize> FontSize;
		int TextInsetX = 0;
		int TextInsetY = 0;
		bool CenterHorizontally = false;
		bool Ellipsize = false;
		GuiTextVerticalAlign VerticalAlign = GuiTextVerticalAlign::Baseline;
		bool ClipText = false;
	};

	class GuiLabel :
		public base::GuiElement
	{
	public:
		GuiLabel(GuiLabelParams guiParams);
		void SetString(const std::string& str);
		std::optional<float> MeasureText(const std::string& text) const;
		static std::string FitText(const std::string& text, float width,
			const std::function<float(const std::string&)>& measure);
		static std::vector<std::string> WrapText(const std::string& text, float width,
			unsigned int maxLines,
			const std::function<float(const std::string&)>& measure);
		static GuiTextLineFrame ResolveLineFrame(float height, graphics::Font::VerticalMetrics metrics,
			GuiTextVerticalAlign alignment);
		std::optional<GuiTextLineFrame> LineFrame() const;
		std::shared_ptr<graphics::Font> ResolvedFont() const { return _font.lock(); }

	public:
		virtual void Draw(base::DrawContext& ctx) override;
	virtual utils::Size2d GetSize() const override;
		virtual utils::Size2d ContentSize() const override;
		virtual void SetSize(utils::Size2d size) override;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	private:
		void SyncVertexArray();
		bool InitVertexArray();
		bool _ResolveFont(resources::ResourceLib& resourceLib);
		unsigned int _LineCapacity(const graphics::Font& font) const;

	private:
		std::string _str;
		std::string _pendingStr;
		utils::Position2d _textInset;
		bool _centerHorizontally;
		bool _ellipsize;
		int _wrapWidth;
		std::optional<graphics::FontOptions::FontSize> _fontSize;
		GuiTextVerticalAlign _verticalAlign;
		bool _clipText;
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
