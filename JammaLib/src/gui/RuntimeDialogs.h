#pragma once

#include "GuiPopup.h"
#include "GuiPopupManager.h"
#include "../actions/GuiAction.h"
#include <deque>
#include <algorithm>
#include <cmath>
#include <limits>
#include <iostream>
#include <mutex>
#include <utility>

namespace gui
{
	// Decision tokens, popup resources and callbacks belong exclusively to the UI.
	using RuntimeDialogDecision = std::shared_ptr<std::optional<bool>>;

	class RuntimeDialogs
	{
		struct Request
		{
			std::string Title, Message, Accept;
			RuntimeDialogDecision Decision;
		};
		class Receiver : public base::ActionReceiver
		{
		public:
			std::optional<bool> Choice;
			actions::ActionResult OnAction(actions::GuiAction action) override
			{
				if (action.ElementType == actions::GuiAction::ACTIONELEMENT_BUTTON && action.Index <= 1u)
					Choice = action.Index == 1u;
				return actions::ActionResult::NoAction();
			}
		};
	public:
		using Fallback = std::function<void(const std::string&, const std::string&)>;
		// Non-callback producers transfer detached values. This event queue cannot
		// use latest-value publication: separate failures must each be delivered.
		void Notify(std::string title, std::string message)
		{
			std::scoped_lock lock(_noticeMutex);
			_notices.push_back({std::move(title),std::move(message),{}, {}});
		}
		RuntimeDialogDecision Confirm(std::string title, std::string message, std::string accept)
		{
			auto decision = std::make_shared<std::optional<bool>>();
			_pending.push_back({std::move(title),std::move(message),std::move(accept),decision});
			return decision;
		}
		void SetFallback(Fallback fallback) { _fallback = std::move(fallback); }
		void Pump(GuiPopupManager& manager, utils::Size2d viewport, bool rendererAvailable)
		{
			{
				std::scoped_lock lock(_noticeMutex);
				while (!_notices.empty())
				{ _pending.push_back(std::move(_notices.front())); _notices.pop_front(); }
			}
			if (_active)
			{
				if (_receiver->Choice || !manager.Contains(_popup) ||
					(_active->Decision && _active->Decision->has_value()))
					_Close(manager,_receiver->Choice.value_or(false));
				else if (!rendererAvailable)
				{ _DeliverFallback(*_active); _Close(manager,false); }
				else FitToViewport(viewport);
			}
			while (!_active && !_pending.empty())
			{
				auto request = std::move(_pending.front()); _pending.pop_front();
				if (request.Decision && request.Decision->has_value()) continue;
				if (!rendererAvailable || !_CanPresent(request))
				{
					_DeliverFallback(request);
					if (request.Decision) *request.Decision = false;
					continue;
				}
				if (manager.IsOpen()) { _pending.push_front(std::move(request)); break; }
				_Log(request);
				_active = std::move(request);
				auto params = GuiPopupParams::PanelDefault(); params.Size = {640u,320u}; params.MinSize = {1u,1u};
				_popup = std::make_shared<GuiPopup>(params);
				_receiver = std::make_shared<Receiver>();
				_popup->SetTitle(_active->Title);
				_popup->SetBodyLines({});
				_popup->ConfigureButtons(_active->Decision
					? GuiPopupButtonConfig{{{"Cancel",0u},{_active->Accept,1u}},true}
					: GuiPopupButtonConfig{{{"OK",0u}},true});
				_popup->SetButtonReceiver(_receiver);
				GuiScrollPanelParams scroll; scroll.Size = {1u,1u}; scroll.MinSize = {0u,0u}; scroll.ScrollBarWidth = 12u;
				_messageScroll = std::make_shared<GuiScrollPanel>(scroll);
				_displayMessage = _active->Message;
				std::replace(_displayMessage.begin(),_displayMessage.end(),'\n',' ');
				std::replace(_displayMessage.begin(),_displayMessage.end(),'\r',' ');
				auto label = GuiPopupParams::Header(_displayMessage,1u,24u);
				label.WrapWidth = (std::numeric_limits<int>::max)(); label.MinSize = {0u,0u};
				label.VerticalAlign = GuiTextVerticalAlign::Top; label.Ellipsize = false;
				_messageLabel = std::make_shared<GuiLabel>(label);
				_messageScroll->SetContent(_messageLabel); _popup->AddChild(_messageScroll);
				_popup->SetKeyAction([weak = std::weak_ptr<Receiver>(_receiver)](actions::KeyAction action) {
					if (auto receiver = weak.lock(); receiver && action.KeyChar == 13u &&
						action.KeyActionType == actions::KeyAction::KEY_UP) receiver->Choice = false;
					return true;
				});
				_popup->Init(); FitToViewport(viewport);
				manager.Open(_popup);
			}
		}
		bool InitResources(resources::ResourceLib& resources, bool forceInit)
		{
			if (!_ResourcesReady(resources)) return false;
			if (_popup)
			{
				_popup->InitResources(resources,forceInit);
				if (!_LabelsReady(*_popup)) return false;
				_LayoutMessage();
			}
			return true;
		}
		void ReleaseResources() { if (_popup) _popup->ReleaseResources(); }
		void FitToViewport(utils::Size2d viewport)
		{ if (_popup) { _popup->FitToViewport(viewport); _LayoutMessage(); } }
	private:
		static bool _CanPresent(const Request& request)
		{
			// Font currently interprets bytes as codepoints and has an ASCII atlas.
			// Keep the original UTF-8 for the native UI fallback and the log.
			const auto ascii = [](const std::string& text) {
				return std::all_of(text.begin(),text.end(),[](unsigned char ch) {
					return (ch >= graphics::Font::kGlyphStartCodepoint && ch <= graphics::Font::kGlyphEndCodepoint)
						|| ch == '\n' || ch == '\r';
				});
			};
			return ascii(request.Title) && ascii(request.Message) && ascii(request.Accept);
		}
		static bool _FontReady(const std::shared_ptr<graphics::Font>& font)
		{
			if (!font || !font->CanDraw()) return false;
			const auto metrics = font->Metrics();
			const auto advance = font->MeasureString("Cancel OK Remove");
			return std::isfinite(metrics.Ascent) && std::isfinite(metrics.Descent) && std::isfinite(metrics.LineGap)
				&& std::isfinite(metrics.Height()) && metrics.Height() > 0.0f
				&& std::isfinite(metrics.LineStep()) && metrics.LineStep() > 0.0f
				&& std::isfinite(advance) && advance > 0.0f;
		}
		static bool _ResourcesReady(resources::ResourceLib& resources)
		{
			const auto font = resources.GetFont(graphics::FontOptions::FONT_SMALL);
			if (!font || !_FontReady(font->lock())) return false;
			const auto ready = [&](const char* name, resources::Type type) {
				const auto resource = resources.GetResource(name);
				const auto value = resource ? resource->lock() : nullptr;
				return value && value->GetType() == type && value->GetId() != 0u;
			};
			for (const auto name : {"colour","texture","texture_tinted"})
				if (!ready(name,resources::SHADER)) return false;
			for (const auto name : {"rounded_but","rounded_but_over","rounded_but_down","blue"})
				if (!ready(name,resources::TEXTURE)) return false;
			return true;
		}
		static bool _LabelsReady(base::GuiElement& root)
		{
			if (!root.IsVisible()) return true;
			if (const auto label = dynamic_cast<const GuiLabel*>(&root); label && !_FontReady(label->ResolvedFont())) return false;
			for (unsigned int index = 0u; index < 256u; ++index)
			{
				const auto child = root.TryGetChild(static_cast<unsigned char>(index));
				if (!child) break;
				if (!_LabelsReady(*child)) return false;
			}
			return true;
		}
		static void _Log(const Request& request) noexcept
		{
			try { std::cerr << "[NOTICE] " << request.Title << ": " << request.Message << std::endl; } catch (...) {}
		}
		void _LayoutMessage()
		{
			const auto size = _popup->GetSize();
			const auto font = _messageLabel->ResolvedFont();
			if (_messageLayoutSize && *_messageLayoutSize == size && _messageLayoutFont.lock() == font) return;
			_messageLayoutSize = size; _messageLayoutFont = font;
			const auto padding = (std::min)(12u,size.Width / 2u);
			const auto bottom = (std::min)(60u,size.Height);
			_messageScroll->SetPosition({static_cast<int>(padding),static_cast<int>(bottom)});
			_messageScroll->SetSize({size.Width - 2u * padding,size.Height > 128u ? size.Height - 128u : 0u});
			// A second pass accounts for the scrollbar after wrapping changes height.
			for (auto pass = 0; pass < 2; ++pass)
			{
				const auto rect = _messageScroll->ContentRect();
				const auto width = static_cast<unsigned int>((std::max)(0,rect.Right - rect.Left));
				unsigned int height = 24u;
				if (_FontReady(font) && width)
				{
					const auto lines = GuiLabel::WrapText(_displayMessage,static_cast<float>(width),
						static_cast<unsigned int>(_displayMessage.size() + 1u),[&](const auto& text) { return font->MeasureString(text); });
					const auto metrics = font->Metrics();
					height = static_cast<unsigned int>(std::ceil(metrics.Height() +
						static_cast<float>(lines.empty() ? 0u : lines.size() - 1u) * (std::max)(1.0f,metrics.LineStep()))) + 2u;
				}
				_messageLabel->SetPosition({rect.Left,0}); _messageLabel->SetSize({width,height});
				_messageLabel->SetVisible(width > 0u);
				_messageScroll->SetSize(_messageScroll->GetSize());
			}
		}
		void _DeliverFallback(const Request& request)
		{
			_Log(request);
			if (_fallback) { try { _fallback(request.Title,request.Message); } catch (...) {} }
		}
		void _Close(GuiPopupManager& manager, bool accepted)
		{
			if (_active->Decision && !_active->Decision->has_value()) *_active->Decision = accepted;
			manager.Close(_popup);
			_popup->ReleaseResources(); _popup->DetachGuiOwnershipForRetirement();
			_popup.reset(); _messageScroll.reset(); _messageLabel.reset(); _receiver.reset(); _active.reset();
			_messageLayoutSize.reset(); _messageLayoutFont.reset();
		}
		// Worker writers and UI reader use this off-callback mutex only for transfer.
		// Scene joins producers before the presenter and queue are destroyed.
		std::mutex _noticeMutex;
		std::deque<Request> _notices;
		// Everything below is UI-owned; workers never touch confirmation tokens.
		std::deque<Request> _pending;
		std::optional<Request> _active;
		std::shared_ptr<GuiPopup> _popup;
		std::shared_ptr<Receiver> _receiver;
		std::shared_ptr<GuiScrollPanel> _messageScroll;
		std::shared_ptr<GuiLabel> _messageLabel;
		std::string _displayMessage;
		std::optional<utils::Size2d> _messageLayoutSize;
		std::weak_ptr<graphics::Font> _messageLayoutFont;
		Fallback _fallback;
	};
}
