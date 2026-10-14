#pragma once
#include "GuiButton.h"
#include "CableInteraction.h"
#include "../engine/StationManagement.h"
#include <glm/mat4x4.hpp>
#include <functional>

namespace gui
{
	// Window/render owner only. Stable station identities survive layout/index changes.
	class GuiStationControls
	{
	public:
		static constexpr unsigned int CancelRemovalControlIndex = 8401u;
		static constexpr unsigned int DeleteRemovalControlIndex = 8402u;
		struct Intent { bool Prepend = false; std::shared_ptr<engine::Station> Remove; };
		void Update(const std::vector<std::shared_ptr<engine::Station>>& stations,
			const glm::mat4& view, utils::Size2d viewport, bool ctrl, bool enabled)
		{
			_enabled = enabled;
			if (!ctrl) CancelBinPress();
			const auto locals = engine::StationManagement::LocalCount(stations);
			std::vector<Control> next;
			const auto add = [&](utils::Position3d p, Intent intent, bool bin, bool active)
			{
				const auto projected = CableInteraction::ProjectAnchor(view * glm::vec4(p.X, p.Y, p.Z, 1.0f), viewport);
				if (!projected) return;
				// Keep real camera-projected anchors, including offscreen endpoints.
				const int x = static_cast<int>(projected->x), y = static_cast<int>(projected->y);
				auto old = std::find_if(_controls.begin(), _controls.end(), [&](const auto& c) {
					return c.Bin == bin && c.Action.Remove == intent.Remove && c.Action.Prepend == intent.Prepend; });
				auto button = old == _controls.end() ? MakeButton(bin) : old->Button;
				const int half = bin ? 24 : 32;
				button->SetPosition({x - half, y - half});
				button->SetEnabled(enabled && active);
				next.push_back({button, intent, bin});
			};
			std::shared_ptr<engine::Station> first, last;
			for (const auto& station : stations)
				if (station && !station->IsRemote())
				{
					if (!first) first = station;
					last = station;
					if (ctrl) { auto p = station->TopCapModelPosition(); p.Y += 65.0f; add(p, {false, station}, true, locals > 1u); }
				}
			if (first)
			{
				auto left = first->PresentationModelPosition(); left.X -= 600.0f;
				auto right = last->PresentationModelPosition(); right.X += 600.0f;
				add(left, {true, {}}, false, locals < io::JamFile::MaxStations);
				add(right, {false, {}}, false, locals < io::JamFile::MaxStations);
			}
			_controls = std::move(next);
		}
		void InitResources(resources::ResourceLib& resources)
		{ for (auto& c : _controls) c.Button->InitResources(resources, false); }
		void Draw(base::DrawContext& ctx)
	{
		for (auto& c : _controls)
		{
			auto opacity = ctx.WithOpacity(c.Button->IsEnabled() ? 1.0f : 0.35f);
			c.Button->Draw(ctx);
		}
	}
		bool Touch(actions::TouchAction action, const std::function<void(Intent)>& activate)
		{
			if (_cancelledRelease)
			{
				_cancelledRelease = false;
				if (action.State == actions::TouchAction::TOUCH_UP) return true;
			}
			if (_pressed)
			{
				auto pressed = *_pressed;
				if (action.State == actions::TouchAction::TOUCH_UP)
				{
					const bool inside = HitButton(*pressed.Button, pressed.Button->ParentToLocal(action.Position));
					pressed.Button->ClearPointerState(); _pressed.reset();
					if (_enabled && inside && pressed.Button->IsEnabled()) activate(pressed.Action);
				}
				return true;
			}
			for (auto& c : _controls)
				if (HitButton(*c.Button, c.Button->ParentToLocal(action.Position)))
				{
					if (action.State == actions::TouchAction::TOUCH_DOWN && action.Index == 0)
					{
						c.Button->OnAction(c.Button->ParentToLocal(action));
						_pressed = c;
					}
					return true;
				}
			return false;
		}
		bool Move(actions::TouchMoveAction action)
		{
			if (_pressed) { if (!action.MouseButtonsDown) Cancel(); return true; }
			for (auto& c : _controls)
			{
				c.Button->OnAction(c.Button->ParentToLocal(action));
				if (HitButton(*c.Button, c.Button->ParentToLocal(action.Position))) return true;
			}
			return false;
		}
		void CancelBinPress() { if (_pressed && _pressed->Bin) { _cancelledRelease = true; Cancel(); } }
		void Cancel() { if (_pressed) { _cancelledRelease = true; _pressed->Button->ClearPointerState(); } _pressed.reset(); }
		void ReleaseResources() { Cancel(); for (auto& c : _controls) c.Button->ReleaseResources(); }
	private:
		static bool HitButton(const GuiButton& button, utils::Position2d position)
	{
		// Disabled controls still own their rectangle; clicks cannot reach selection underneath.
		const auto size = button.GetSize();
		return position.X >= 0 && position.Y >= 0 && position.X < static_cast<int>(size.Width) && position.Y < static_cast<int>(size.Height);
	}
	static std::shared_ptr<GuiButton> MakeButton(bool bin)
		{
			auto p = GuiButtonParams::PanelButton(bin ? 48u : 64u);
			p.Size.Height = p.Size.Width; p.MinSize = p.Size;
			p.TextureShader = "texture";
			p.Texture = bin ? "station_bin" : "station_add";
			p.OverTexture = p.Texture + "_over"; p.DownTexture = p.Texture + "_down";
			return std::make_shared<GuiButton>(p);
		}
		struct Control { std::shared_ptr<GuiButton> Button; Intent Action; bool Bin; };
		std::vector<Control> _controls;
		std::optional<Control> _pressed;
		bool _enabled = true;
		bool _cancelledRelease = false;
	};
}
