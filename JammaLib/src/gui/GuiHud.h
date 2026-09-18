#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "GuiPanel.h"
#include "GuiStackPanel.h"
#include "GuiVu.h"
#include "CableInteraction.h"
#include "../io/RigFile.h"

namespace resources
{
	class ResourceLib;
	class ShaderResource;
}

namespace engine
{
	struct RoutingGraph;
	struct RigSnapshot;
	class Trigger;
}

namespace gui
{
	class GuiPopupManager;

	struct GuiHudParams : public base::GuiElementParams
	{
		GuiHudParams() :
			base::GuiElementParams()
		{}

		std::function<bool(const io::RigFile&)> SubmitRigEdit;
		std::function<bool()> EditsEnabled;
		GuiPopupManager* PopupManager = nullptr;
	};

	class GuiButton;
	class GuiLabel;
	class GuiPopup;
	class GuiScrollPanel;

	class GuiHud : public GuiPanel
	{
	public:
		explicit GuiHud(GuiHudParams params);

		struct StationAnchor
		{
			size_t StationIndex = 0u;
			std::string StationName;
			utils::Position2d screenPos;
			glm::vec4 color;
		};

		struct CableRoute
		{
			enum class Kind { Capture, Station };
			Kind RouteKind = Kind::Capture;
			size_t TriggerIndex = 0u;
			std::optional<io::RigFileRouting::Source> Source;
			std::optional<size_t> StationIndex;
		};

	public:
		virtual void Draw(base::DrawContext& ctx) override;
		virtual void SetSize(utils::Size2d size) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		void SetCableRevealHeld(bool held);
		void SetAudioInputPeak(unsigned int channel, float peak, unsigned int numSamps);
		void SetMidiInputPeak(unsigned int input, float peak, unsigned int numSamps);
		void SetRoutingConfig(unsigned int audioInputCount,
			std::vector<std::string> midiInputNames,
			const engine::RigSnapshot& routing);
		void SetStationAnchors(std::vector<StationAnchor> anchors);
		bool HasCableDrag() const noexcept { return _cableDrag.has_value(); }
		bool IsApplying() const { return _editsEnabled && !_editsEnabled(); }
		static std::vector<CableRoute> BuildCableRoutes(const engine::RoutingGraph& graph);
		static int RevealScrollOffset(int currentOffset, int viewportHeight,
			int contentHeight, int itemTop, int itemBottom);

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
		static constexpr unsigned int _RightRailPaddingH = 0u;
		static constexpr unsigned int _RightRailPaddingV = 12u;
		static constexpr unsigned int _RightRailOverhang = 34u;
		static constexpr unsigned int _RightRailTopInset = 60u;
		static constexpr unsigned int _RightRailSpacing = 10u;
		static constexpr unsigned int _TriggerButtonWidth = 120u;
		static constexpr unsigned int _TriggerButtonHeight = 100u;
		static constexpr unsigned int _TriggerFooterHeight = 56u;
		static constexpr unsigned int _TriggerControlSize = 34u;
		static constexpr unsigned int _AudioInputPeakHoldSamps = 3000u;
		static constexpr double _MidiInputFallRate = 0.003;
		static constexpr double _MidiInputHoldFallRate = 0.003;
		static constexpr unsigned int _MidiInputPeakHoldSamps = 320u;

		void _BuildPanels();
		void _BuildTopStrip();
		void _BuildTriggerRail();
		void _AddTrigger();
		void _OpenDeleteConfirmation(size_t triggerIndex);
		void _ConfirmDelete();
		bool _SubmitCandidate(const io::RigFile& candidate);
		bool _CanEditTrigger(size_t triggerIndex) const;
		void _RevealTrigger(size_t triggerIndex);
		void _RebuildPanels();
		void _LayoutPanels();
		bool _InitCableShader(resources::ResourceLib& resourceLib);
		bool _InitCableVertexArray();
		void _DrawCables(base::DrawContext& ctx);
		void _RebuildCableVertices();
		void _BuildInteractionGeometry(std::vector<CableInteraction::Endpoint>& endpoints,
			std::vector<CableInteraction::Cable>& cables) const;
		actions::ActionResult _BeginCableDrag(utils::Position2d point);
		void _CancelCableDrag();
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

		std::shared_ptr<GuiLabel> _MakeHeader(const std::string& text,
			unsigned int width,
			unsigned int horizontalInset = 0u) const;
		std::shared_ptr<GuiButton> _MakeSourceButton(const std::string& text,
			const glm::vec3& tint,
			unsigned int width) const;
		std::shared_ptr<GuiButton> _MakeTriggerButton(const std::string& text,
			std::weak_ptr<engine::Trigger> trigger) const;

		std::shared_ptr<GuiStackPanel> _topStrip;
		std::shared_ptr<GuiStackPanel> _topInputRow;
		std::shared_ptr<GuiPanel> _triggerRail;
		std::shared_ptr<GuiScrollPanel> _triggerScroll;
		std::shared_ptr<GuiStackPanel> _triggerList;
		std::shared_ptr<GuiButton> _addTriggerButton;
		std::shared_ptr<GuiPopup> _deletePopup;
		std::shared_ptr<base::ActionReceiver> _deletePopupReceiver;
		std::vector<std::shared_ptr<GuiButton>> _sourceButtons;
		std::vector<std::shared_ptr<GuiButton>> _triggerButtons;
		std::vector<std::shared_ptr<GuiButton>> _triggerCloseButtons;
		std::vector<std::shared_ptr<GuiLabel>> _triggerStatusLabels;
		std::vector<std::unique_ptr<GuiVu>> _inputVus;
		unsigned int _audioInputCount = 0u;
		std::vector<std::string> _midiInputNames;
		std::vector<std::string> _triggerNames;
		std::vector<std::weak_ptr<engine::Trigger>> _triggers;
		std::vector<io::RigFileRouting::Source> _sourceEndpoints;
		std::vector<io::RigFileRouting::TriggerResolution> _routingGraph;
		io::RigFile _displayedRig;
		std::uint64_t _displayedRevision = 0u;
		std::optional<CableInteraction::Drag> _cableDrag;
		std::optional<size_t> _deleteTriggerIndex;
		std::function<bool(const io::RigFile&)> _submitRigEdit;
		std::function<bool()> _editsEnabled;
		GuiPopupManager* _popupManager = nullptr;
		bool _revealNewestTrigger = false;
		int _lastTriggerScrollOffset = 0;
		bool _cableRevealHeld = false;
		float _cableRevealAlpha = 0.0f;
		std::vector<glm::vec4> _cableControlPoints;
		std::vector<glm::vec4> _cableColors;
		std::vector<glm::vec4> _cableRenderColors;
		std::weak_ptr<resources::ShaderResource> _cableShader;
		unsigned int _cableVertexArray = 0;
		unsigned int _cableVertexBuffer = 0;
		bool _cablesDirty = true;
		std::vector<StationAnchor> _stationAnchors;
		static constexpr int _CableSegments = 24;
		static constexpr float _SocketHitRadius = 14.0f;
		static constexpr float _CableHitRadius = 9.0f;
		static constexpr float _SnapRadius = 28.0f;
		static constexpr float _SnapHysteresis = 10.0f;
	};
}
