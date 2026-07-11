#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>
#include "GuiPanel.h"
#include "GuiStackPanel.h"
#include "GuiVu.h"

namespace resources
{
	class ResourceLib;
	class ShaderResource;
}

namespace gui
{
	struct GuiHudParams : public base::GuiElementParams
	{
		GuiHudParams() :
			base::GuiElementParams()
		{}
	};

	class GuiButton;
	class GuiLabel;

	class GuiHud : public GuiPanel
	{
	public:
		explicit GuiHud(GuiHudParams params);

		struct StationAnchor
		{
			utils::Position2d screenPos;
			glm::vec4 color;
		};

	public:
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void SetSize(utils::Size2d size) override;
		void SetStationAnchors(std::vector<StationAnchor> anchors);
		void SetCableRevealHeld(bool held);
		void SetAudioInputPeak(unsigned int channel, float peak, unsigned int numSamps);
		void SetAudioInputPeaks(const std::vector<float>& peaks, unsigned int numSamps);
		void SetRoutingConfig(unsigned int audioInputCount,
			std::vector<std::string> midiInputNames,
			std::vector<std::string> triggerNames);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	private:
		static constexpr int _OuterMargin = 20;
		static constexpr int _TopPosY = 18;
		static constexpr unsigned int _TopStripHeight = 88u;
		static constexpr unsigned int _TopStripWidth = 760u;
		static constexpr unsigned int _TopStripMinWidth = 320u;
		static constexpr unsigned int _TopStripPadding = 12u;
		static constexpr unsigned int _TopStripSpacing = 10u;
		static constexpr unsigned int _SourceButtonWidth = 118u;
		static constexpr unsigned int _SourceButtonHeight = 34u;
		static constexpr unsigned int _RightRailWidth = 134u;
		static constexpr unsigned int _RightRailHeight = 460u;
		static constexpr unsigned int _RightRailMinHeight = 220u;
		static constexpr unsigned int _RightRailPadding = 12u;
		static constexpr unsigned int _RightRailSpacing = 10u;
		static constexpr unsigned int _TriggerButtonWidth = 120u;
		static constexpr unsigned int _TriggerButtonHeight = 100u;

		void _BuildPanels();
		void _BuildTopStrip();
		void _BuildTriggerRail();
		void _RebuildPanels();
		void _LayoutPanels();
		bool _InitCableShader(resources::ResourceLib& resourceLib);
		bool _InitCableVertexArray();
		void _DrawCables(base::DrawContext& ctx);
		void _RebuildCableVertices();
		utils::Position2d _ButtonCenter(const std::shared_ptr<GuiButton>& button) const;
		utils::Position2d _TriggerAnchorFromTopLeft(const std::shared_ptr<GuiButton>& button,
			int offsetX,
			int offsetFromTopY) const;
		utils::Position2d _TriggerAnchorFromBottomLeft(const std::shared_ptr<GuiButton>& button,
			int offsetX,
			int offsetFromBottomY) const;
		void _AppendCurve(const utils::Position2d& start,
			const utils::Position2d& end,
			const glm::vec4& color);
		void _AppendStationCurve(const utils::Position2d& start,
			const utils::Position2d& end,
			const glm::vec4& color);

		std::shared_ptr<GuiLabel> _MakeHeader(const std::string& text, unsigned int width) const;
		std::shared_ptr<GuiButton> _MakeSourceButton(const std::string& text,
			const glm::vec3& tint,
			unsigned int width) const;
		std::shared_ptr<GuiButton> _MakeTriggerButton(const std::string& text, const glm::vec3& tint) const;

		std::shared_ptr<GuiStackPanel> _topStrip;
		std::shared_ptr<GuiStackPanel> _topInputRow;
		std::shared_ptr<GuiStackPanel> _triggerRail;
		std::vector<std::shared_ptr<GuiButton>> _sourceButtons;
		std::vector<std::shared_ptr<GuiButton>> _triggerButtons;
		std::vector<std::unique_ptr<GuiVu>> _audioInputVus;
		unsigned int _audioInputCount = 4u;
		std::vector<std::string> _midiInputNames;
		std::vector<std::string> _triggerNames;
		bool _cableRevealHeld = false;
		float _cableRevealAlpha = 0.0f;
		std::vector<glm::vec4> _cableControlPoints;
		std::vector<glm::vec4> _cableColors;
		std::weak_ptr<resources::ShaderResource> _cableShader;
		unsigned int _cableVertexArray = 0;
		unsigned int _cableVertexBuffer = 0;
		bool _cablesDirty = true;
		std::vector<StationAnchor> _stationAnchors;
		static constexpr int _CableSegments = 24;
	};
}