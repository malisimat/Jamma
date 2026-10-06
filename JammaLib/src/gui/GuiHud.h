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

	enum class RoutingEditAvailability
	{
		Ready,
		Applying,
		AudioCallbackInactive,
		TriggerBusy
	};

	struct GuiHudParams : public base::GuiElementParams
	{
		GuiHudParams() :
			base::GuiElementParams()
		{}

		std::function<bool(const io::RigFile&)> SubmitRigEdit;
		std::function<RoutingEditAvailability()> RoutingEditAvailabilityState;
		std::function<bool(std::uint64_t)> AcceptTriggerInput;
		GuiPopupManager* PopupManager = nullptr;
	};

	class GuiButton;
	class GuiLabel;
	class GuiPopup;
	class GuiScrollPanel;
	class GuiHudSocket;

	class GuiHud : public GuiPanel
	{
	public:
		explicit GuiHud(GuiHudParams params);

		struct StationAnchor
		{
			size_t StationIndex = 0u;
			std::string StationName;
			std::optional<glm::dvec2> ScreenPosition;
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
		void ClearPointerState() override;
		void SetCableRevealHeld(bool held);
		void SetAudioInputPeak(unsigned int channel, float peak, unsigned int numSamps);
		void SetMidiInputPeak(unsigned int input, float peak, unsigned int numSamps);
		void SetRoutingConfig(unsigned int audioInputCount,
			std::vector<std::string> midiInputNames,
			const engine::RigSnapshot& routing);
		void SetStationAnchors(std::vector<StationAnchor> anchors);
		void SetLoopEditorMode(bool enabled);
		bool HasCableDrag() const noexcept { return _cableDrag.has_value(); }
		bool IsApplying() const { return _RoutingEditAvailability() == RoutingEditAvailability::Applying; }
		static std::vector<CableRoute> BuildCableRoutes(const engine::RoutingGraph& graph);
		static int RevealScrollOffset(int currentOffset, int viewportHeight,
			int contentHeight, int itemTop, int itemBottom);
		static unsigned int SourceCardWidth(unsigned int viewportWidth, unsigned int count);
		// UI-owned geometry snapshot: visible real snap sockets and presented
		// curves with original route identity, including decorative continuations.
		void BuildInteractionGeometry(std::vector<CableInteraction::Endpoint>& endpoints,
			std::vector<CableInteraction::Cable>& cables) const;

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	private:
		static constexpr int _OuterMargin = 8;
		static constexpr int _TopPosY = 8;
		static constexpr unsigned int _TopStripHeight = 108u;
		static constexpr unsigned int _TopStripWidth = 760u;
		static constexpr unsigned int _TopStripMinWidth = 320u;
		static constexpr unsigned int _TopStripPadding = 8u;
		static constexpr unsigned int _TopStripSpacing = 10u;
		static constexpr unsigned int _SourceButtonWidth = 118u;
		static constexpr unsigned int _SourceButtonHeight = GuiStyle::ControlHeight;
		static constexpr unsigned int _SourceScrollBarHeight = 12u;
		static constexpr unsigned int _SourceViewportHeight = _SourceButtonHeight + _SourceScrollBarHeight + 4u;
		static constexpr unsigned int _SourcePanelGap = 8u;
		static constexpr unsigned int _RightRailWidth = 144u;
		static constexpr unsigned int _RightRailHeight = 460u;
		static constexpr unsigned int _RightRailMinHeight = 220u;
		static constexpr unsigned int _RightRailPaddingH = 0u;
		static constexpr unsigned int _RightRailPaddingV = 12u;
		static constexpr unsigned int _RightRailSpacing = 10u;
		static constexpr unsigned int _TriggerButtonWidth = 120u;
		static constexpr unsigned int _TriggerButtonHeight = 100u;
		static constexpr unsigned int _TriggerFooterHeight = 56u;
		static constexpr unsigned int _TriggerControlSize = 34u;
		static constexpr unsigned int _SocketSize = 24u;
		static constexpr unsigned int _CableEndSize = 10u;
		static constexpr int _TriggerInputPinTopOffset = 12;
		static constexpr int _TriggerOutputPinBottomOffset = 12;
		static constexpr int _TriggerInputFanHalfHeight = 7;
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
		RoutingEditAvailability _RoutingEditAvailability() const;
		void _UpdateRoutingEditPresentation();
		void _RevealTrigger(size_t triggerIndex);
		void _RebuildPanels();
		void _LayoutPanels();
		void _OpenSourceIdentity(const std::string& identity);
		void _LayoutSourceIdentity();
		utils::Rect2d _ContentClip(const std::shared_ptr<GuiScrollPanel>& scroll) const;
		bool _InitCableShader(resources::ResourceLib& resourceLib);
		bool _InitCableVertexArray();
		void _DrawCables(base::DrawContext& ctx);
		void _DrawCableSockets(base::DrawContext& ctx);
		void _DrawOverlayElement(base::DrawContext& ctx,
			const std::shared_ptr<base::GuiElement>& element) const;
		void _RebuildCableVertices();
		bool _CableVisible(const CableInteraction::Cable& cable) const;
		void _FilterCableHits(std::vector<CableInteraction::Endpoint>& endpoints,
			std::vector<CableInteraction::Cable>& cables, utils::Position2d point) const;
		void _UpdateCableHover(utils::Position2d point);
		void _UpdateSocketHighlights();
		bool _SourceVisible(size_t index) const;
		actions::ActionResult _BeginCableDrag(utils::Position2d point);
		void _CancelCableDrag();
		utils::Position2d _ElementCenter(const std::shared_ptr<base::GuiElement>& element) const;
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
			unsigned int width, bool midi, bool available);
		std::shared_ptr<GuiButton> _MakeTriggerButton(const std::string& text,
			std::weak_ptr<engine::Trigger> trigger) const;
		struct SourceWidgets
		{
			std::shared_ptr<GuiButton> Button;
			std::shared_ptr<base::GuiElement> Socket;
		};

		struct TriggerWidgets
		{
			std::shared_ptr<GuiButton> Card;
			std::shared_ptr<GuiButton> Close;
			std::shared_ptr<base::GuiElement> InputSocket;
			std::shared_ptr<base::GuiElement> OutputSocket;
		};


		std::shared_ptr<GuiStackPanel> _topStrip;
		std::shared_ptr<GuiStackPanel> _topSourceRow;
		std::shared_ptr<GuiStackPanel> _topInputRow;
		std::shared_ptr<GuiStackPanel> _topMidiRow;
		std::shared_ptr<GuiScrollPanel> _topAudioScroll;
		std::shared_ptr<GuiScrollPanel> _topMidiScroll;
		std::shared_ptr<GuiPanel> _triggerRail;
		std::shared_ptr<GuiScrollPanel> _triggerScroll;
		std::shared_ptr<GuiStackPanel> _triggerList;
		std::shared_ptr<GuiButton> _addTriggerButton;
		std::shared_ptr<GuiLabel> _routingStatusLabel;
		std::shared_ptr<GuiPopup> _deletePopup;
		// UI-owned read-only identity popup. Rig/job rebuilds do not mutate it;
		// resize and resource work stay on the existing UI/render paths.
		std::shared_ptr<GuiPanel> _sourceInfoPanel;
		std::shared_ptr<GuiScrollPanel> _sourceInfoScroll;
		std::shared_ptr<GuiLabel> _sourceInfoLabel;
		std::string _sourceIdentity;
		std::shared_ptr<base::ActionReceiver> _deletePopupReceiver;
		std::vector<SourceWidgets> _sourceWidgets;
		std::vector<TriggerWidgets> _triggerWidgets;
		std::vector<std::unique_ptr<GuiVu>> _inputVus;
		int _lastAudioScrollOffset = 0;
		int _lastMidiScrollOffset = 0;
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
		std::function<RoutingEditAvailability()> _routingEditAvailability;
		std::function<bool(std::uint64_t)> _acceptTriggerInput;
		std::optional<RoutingEditAvailability> _lastRoutingEditAvailability;
		GuiPopupManager* _popupManager = nullptr;
		bool _revealNewestTrigger = false;
		int _lastTriggerScrollOffset = 0;
		bool _cableRevealHeld = false;
		std::optional<CableInteraction::Endpoint> _hoveredCableEndpoint;
		std::optional<CableInteraction::Handle> _hoveredCableRoute;
		std::optional<CableInteraction::End> _hoveredCableEnd;
		std::shared_ptr<GuiHudSocket> _cableEndIcon;
		std::shared_ptr<GuiHudSocket> _stationSocketIcon;
		float _cableRevealAlpha = 0.0f;
		std::vector<glm::vec4> _cableControlPoints;
		std::vector<glm::vec4> _cableColors;
		std::vector<glm::vec4> _cableRenderColors;
		std::weak_ptr<resources::ShaderResource> _cableShader;
		unsigned int _cableVertexArray = 0;
		unsigned int _cableVertexBuffer = 0;
		bool _cablesDirty = true;
		bool _loopEditorMode = false;
		std::optional<utils::Position2d> _cableHoverPoint;
		std::vector<StationAnchor> _stationAnchors;
		static constexpr int _CableSegments = CableInteraction::CurveVertexCount;
		static constexpr size_t _CableBatchSize = 16u;
		static constexpr float _SocketHitRadius = 14.0f;
		static constexpr float _CableHitRadius = 9.0f;
		static constexpr float _SnapRadius = 28.0f;
		static constexpr float _SnapHysteresis = 10.0f;
	};
}
