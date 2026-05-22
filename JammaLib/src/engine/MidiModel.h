#pragma once

#include <cstdint>
#include <vector>

#include "../gui/GuiModel.h"
#include "MidiNoteSpan.h"

namespace engine
{
	class MidiModelParams : public gui::GuiModelParams
	{
	public:
		MidiModelParams();
		MidiModelParams(gui::GuiModelParams params);

	public:
		float Radius;
		float RadialThickness;
		float NoteHeight;
		float PitchStep;
		std::uint8_t CenterPitch;
	};

	class MidiModel : public virtual gui::GuiModel
	{
	public:
		MidiModel(MidiModelParams params);
		~MidiModel();

		MidiModel(const MidiModel&) = delete;
		MidiModel& operator=(const MidiModel&) = delete;

	public:
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

		double LoopIndexFrac() const noexcept { return _loopIndexFrac; }
		void SetLoopIndexFrac(double frac) noexcept;
		void UpdateModel(const std::vector<MidiNoteSpan>& spans, std::uint32_t loopLengthSamps);
		static std::vector<float> BuildBaseVerts(unsigned int segments);
		static std::vector<float> BuildBaseUvs(unsigned int segments);

	protected:
		std::weak_ptr<resources::ShaderResource> GetShader() override;

	private:
		float PitchOffset(std::uint8_t note) const noexcept;

	private:
		MidiModelParams _midiParams;
		double _loopIndexFrac;
	};
}