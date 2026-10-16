#pragma once

#include <atomic>
#include <gl/glew.h>
#include <gl/gl.h>
#include "../audio/FallingValue.h"
#include "../base/GuiElement.h"
#include "../base/ResourceUser.h"
#include "../base/DrawContext.h"
#include "../graphics/GlDrawContext.h"
#include "../resources/ResourceLib.h"
#include "../resources/ShaderResource.h"
#include "../utils/CommonTypes.h"

namespace gui
{
	struct GuiVuParams : public base::GuiElementParams
	{
		GuiVuParams() :
			base::GuiElementParams(),
			FallRate(0.00005),
			HoldFallRate(0.00003),
			HoldSamps(12000u),
			UseDecibelScale(true)
		{
			Size = { 6u, 1u };
		}

		double FallRate;
		double HoldFallRate;
		unsigned int HoldSamps;
		bool UseDecibelScale;
	};

	class GuiVu : public base::ResourceUser
	{
	public:
		static const int LedHeight = 4;
		static const int LedGap    = 2;
		static const int LedPitch  = LedHeight + LedGap;
		static const int VuWidth   = 6;

	public:
		GuiVu();
		explicit GuiVu(GuiVuParams params);
		~GuiVu();

		// Non-copyable
		GuiVu(const GuiVu&) = delete;
		GuiVu& operator=(const GuiVu&) = delete;

	public:
		// Called from the render thread.
		void Draw(base::DrawContext& ctx);

		// Single producer (audio for mixer meters, UI for HUD meters).
		// Publishes scalar levels; never reads UI visibility.
		void SetPeak(float peak, unsigned int numSamps);
		float PeakValue() const noexcept;
		float DisplayValue() const noexcept;
		float HoldValue() const noexcept;

		void SetVisible(bool visible);
		bool IsVisible() const;

		void SetPosition(utils::Position2d pos);
		void SetSize(utils::Size2d size);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

	private:
		static unsigned int _CalcTotalLeds(unsigned int height);
		unsigned int _CalcCurrentLeds(double value, unsigned int totalLeds) const;

		bool _isVisible; // Render/UI owner only, including IsVisible().
		utils::Position2d _position;
		utils::Size2d     _size;

		// FallingValue is confined to this instance's producer; only SetPeak() touches it.
		audio::FallingValue _value;

		// Atomic snapshots written by the producer and read by the render thread.
		std::atomic<float> _peakValue;
		std::atomic<float> _displayValue;
		std::atomic<float> _displayHold;
		bool _useDecibelScale;

		GLuint _vertexArray;
		GLuint _vertexBuffer[2];
		std::weak_ptr<resources::ShaderResource> _shader;
	};
}
