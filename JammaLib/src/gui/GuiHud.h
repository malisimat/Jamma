#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "GuiPanel.h"
#include "GuiStackPanel.h"
#include "GuiVu.h"
#include "CableInteraction.h"
#include "../io/RigFile.h"
#include "../actions/TouchAction.h"
#include "TriggerLearnController.h"

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
		std::function<bool(const io::RigFile&)> SubmitTriggerRename;
		std::function<std::string()> TriggerLearnAdmission;
		std::function<std::vector<io::TriggerBindingLearn::Device>()> TriggerLearnDevices;
		std::function<std::shared_ptr<const io::TriggerBindingLearn::Observation>(std::optional<io::TriggerBindingLearn::Request>)> TriggerLearnInput;
		std::function<RoutingEditAvailability()> RoutingEditAvailabilityState;
		std::function<bool(std::uint64_t)> AcceptTriggerInput;
		std::function<void(const std::shared_ptr<base::GuiElement>&, bool)> FocusEditor;
		GuiPopupManager* PopupManager = nullptr;
	};

	class GuiButton;
	class GuiLabel;
	class GuiPopup;
	class GuiScrollPanel;
	class GuiHudSocket;
	class GuiTriggerRename;

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
		void InitResources(resources::ResourceLib& resources, bool force) override;
		virtual void SetSize(utils::Size2d size) override;
		virtual actions::ActionResult OnAction(actions::TouchAction action) override;
		virtual actions::ActionResult OnAction(actions::TouchMoveAction action) override;
		virtual actions::ActionResult OnAction(actions::KeyAction action) override;
		void ClearPointerState() override;
		bool RouteRenameTouch(actions::TouchAction action);
		bool HasLabelPress() const { return _labelPress.has_value(); }
		bool CapturesPointer(utils::Position2d localPoint) const;
		void CancelTriggerRenames();
		void UpdateTriggerLearn(); // Window owner, with Scene serialization held.
		void CancelTriggerLearn();
		bool TriggerLearnKeyboard(const actions::KeyAction& action, bool fresh);
		bool HasTriggerLearn() const { return _triggerLearn != nullptr; }
		void SetCableRevealHeld(bool held);
		void SetAltLearnHeld(bool held);
		void SetAudioInputPeak(unsigned int channel, float peak, unsigned int numSamps);
		void SetMidiInputPeak(unsigned int input, float peak, unsigned int numSamps);
		// Off-callback producers publish complete, latest-substitutable presentation data.
		void PublishRoutingConfig(unsigned int audioInputCount,
			std::vector<std::string> midiInputNames,
			std::shared_ptr<const engine::RigSnapshot> routing);
		// UI thread only, before resource traversal and meter updates.
		bool ApplyPendingRoutingConfig();
		// UI thread only (or initial setup before UI use).
		void SetRoutingConfig(unsigned int audioInputCount,
			std::vector<std::string> midiInputNames,
			const engine::RigSnapshot& routing);
		void SetStationAnchors(std::vector<StationAnchor> anchors);
		void SetLoopEditorMode(bool enabled);
		void SetQuantisationFeedback(const std::string& text);
		void SetClockStatus(unsigned long masterLengthSamps, unsigned int grainSamps);
		void SetPerformanceGroupFeedback(const std::string& text);
		utils::Rect2d OccupiedTopStripBounds() const;
		void SetTopPanelLayout(GuiTopPanelLayout layout);
		void SetEditModeAlpha(float alpha) noexcept { _editModeAlpha = std::clamp(alpha, 0.0f, 1.0f); }
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
		friend class GuiHudTestAccess;
		std::vector<std::optional<CableInteraction::Endpoint>> _BuildStationEndpoints(
			std::vector<CableInteraction::Endpoint>& endpoints) const;
		static void _FanSourceCableEnds(const std::vector<CableInteraction::Endpoint>& sources,
			std::vector<CableInteraction::Cable>& cables);
		static void _PresentCableEnds(std::vector<CableInteraction::Cable>& cables);
		static constexpr int _OuterMargin = 8;
		static constexpr int _TopPosY = 8;
		static constexpr unsigned int _TopStripHeight = 68u;
		static constexpr unsigned int _TopStripWidth = 760u;
		static constexpr unsigned int _TopStripMinWidth = 320u;
		static constexpr unsigned int _TopStripPadding = 8u;
		static constexpr unsigned int _TopStripSpacing = 10u;
		static constexpr unsigned int _SourceButtonWidth = 118u;
		static constexpr unsigned int _SourceButtonHeight = GuiStyle::ControlHeight;
		static constexpr unsigned int _SourceScrollBarHeight = 12u;
		static constexpr unsigned int _SourceViewportHeight = _SourceButtonHeight + _SourceScrollBarHeight + 4u;
		static constexpr int _SourcePanelGap = 8;
		static constexpr unsigned int _RightRailWidth = 156u;
		static constexpr unsigned int _RightRailHeight = 460u;
		static constexpr unsigned int _RightRailMinHeight = 220u;
		static constexpr unsigned int _RightRailPaddingH = 0u;
		static constexpr unsigned int _RightRailPaddingV = 12u;
		static constexpr unsigned int _RightRailSpacing = 10u;
		static constexpr unsigned int _TriggerButtonWidth = 136u;
		static constexpr unsigned int _TriggerButtonHeight = 132u;
		static constexpr unsigned int _TriggerFooterHeight = 56u;
		static constexpr unsigned int _TriggerControlSize = 34u;
		static constexpr unsigned int _TriggerCloseSize = 24u;
		static constexpr unsigned int _SocketSize = 24u;
		static constexpr int _SourceSocketY = -static_cast<int>(_SocketSize / 2u);
		static constexpr unsigned int _CableEndSize = 10u;
		static constexpr int _TriggerOutputPinBottomOffset = 12;
		static constexpr int _TriggerInputPinTopOffset = _TriggerOutputPinBottomOffset;
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
		void _OpenRename(size_t triggerIndex);
		void _OpenTriggerLearn(const std::string& id, bool activate, bool altHeld);
		std::string _SubmitRename(const std::string& name, const std::string& id, std::uint64_t revision);
		bool _SubmitCandidate(const io::RigFile& candidate);
		bool _CanEditTrigger(size_t triggerIndex) const;
		RoutingEditAvailability _RoutingEditAvailability() const;
		void _UpdateRoutingEditPresentation();
		void _RevealTrigger(size_t triggerIndex);
		void _RebuildPanels();
		void _LayoutPanels();
		utils::Rect2d _TopStripLayout(int preferredHeight);
		std::optional<GuiTopPanelLayout> _topPanelLayout;
		bool _retargetStrip = false, _stripRetargetActive = false;
		float _stripRetargetOffset = 0.0f, _stripRetargetProgress = 1.0f;
		float _stripWidthAnchorPhase = 0.0f, _stripTopAnchorPhase = 0.0f;
		int _stripRetargetLeft = 0;
		bool _stripTargetExpanded = true;
		void _OpenSourceIdentity(const std::string& identity);
		void _LayoutSourceIdentity();
		utils::Rect2d _ContentClip(const std::shared_ptr<GuiScrollPanel>& scroll) const;
		utils::Rect2d _SourceSocketClip(const std::shared_ptr<GuiScrollPanel>& scroll) const;
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
		std::optional<size_t> _SourceControlAt(utils::Position2d point) const;
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
			unsigned int width);
		std::shared_ptr<GuiButton> _MakeTriggerButton(const std::string& text,
			std::weak_ptr<engine::Trigger> trigger, std::string id);
		struct SourceWidgets
		{
			std::shared_ptr<GuiButton> Button;
			std::shared_ptr<base::GuiElement> Socket;
		};

		struct TriggerWidgets
		{
			std::shared_ptr<GuiButton> Card;
			std::shared_ptr<GuiButton> Close;
			std::shared_ptr<GuiButton> Edit;
			std::shared_ptr<GuiTriggerRename> Rename;
			std::shared_ptr<base::GuiElement> InputSocket;
			std::shared_ptr<base::GuiElement> OutputSocket;
		};


		struct RoutingPresentation
		{
			unsigned int AudioInputCount;
			std::vector<std::string> MidiInputNames;
			std::shared_ptr<const engine::RigSnapshot> Routing;
		};
		// Job/UI producers are serialized by Scene::_sceneMutex; UI consumes by exchange.
		// Pending updates coalesce, retaining immutable rig topology until consumption.
		// No callback access. Scene joins its job thread before HUD destruction, which
		// releases any unconsumed publication on the UI thread. All widgets remain UI-owned.
		std::atomic<std::shared_ptr<const RoutingPresentation>> _pendingRoutingPresentation;

		std::shared_ptr<GuiPanel> _topStrip;
		std::shared_ptr<GuiStackPanel> _topSourceRow;
		std::shared_ptr<GuiStackPanel> _topInputRow;
		std::shared_ptr<GuiStackPanel> _topMidiRow;
		std::shared_ptr<GuiScrollPanel> _topAudioScroll;
		std::shared_ptr<GuiScrollPanel> _topMidiScroll;
		std::shared_ptr<GuiPanel> _triggerRail;
		std::shared_ptr<GuiScrollPanel> _triggerScroll;
		std::shared_ptr<GuiStackPanel> _triggerList;
		std::shared_ptr<GuiButton> _addTriggerButton;
		std::shared_ptr<GuiPanel> _statusPanel;
		std::shared_ptr<GuiLabel> _routingStatusLabel;
		std::string _quantisationFeedback;
		std::string _clockStatusText = "Clock OFF | Subdiv --";
		std::string _performanceGroupFeedback; // UI/job owner under Scene serialization
		float _editModeAlpha = 0.0f;
		std::shared_ptr<GuiPopup> _deletePopup;
		// UI-owned read-only identity popup. Routing rebuilds do not mutate it;
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
		struct LabelPress
		{
			size_t TriggerIndex;
			std::string Id;
			std::uint64_t Revision;
			utils::Position2d Position;
			actions::TouchAction::TouchType Source;
			int Index;
			bool Moved = false;
		};
		std::optional<LabelPress> _labelPress;
		std::optional<size_t> _deleteTriggerIndex;
		std::function<bool(const io::RigFile&)> _submitRigEdit;
		std::function<bool(const io::RigFile&)> _submitTriggerRename;
		std::function<std::string()> _triggerLearnAdmission;
		std::function<std::vector<io::TriggerBindingLearn::Device>()> _triggerLearnDevices;
		std::function<std::shared_ptr<const io::TriggerBindingLearn::Observation>(std::optional<io::TriggerBindingLearn::Request>)> _triggerLearnInput;
		std::shared_ptr<TriggerLearnController> _triggerLearn;
		std::uint64_t _nextTriggerLearnSession = 0u;
		bool _learnEndPending = false;
		bool _altLearnHeld = false;
		std::function<void(const std::shared_ptr<base::GuiElement>&, bool)> _focusEditor;
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
