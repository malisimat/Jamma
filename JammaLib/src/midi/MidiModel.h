#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "../gui/GuiModel.h"
#include "MidiNote.h"

namespace midi
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
	private:
		struct ModelInstanceData
		{
			std::vector<gui::GuiModel::InstanceAttribute> Attributes;
			unsigned int InstanceCount = 0u;
		};

	public:
		MidiModel(MidiModelParams params);
		~MidiModel();

		MidiModel(const MidiModel&) = delete;
		MidiModel& operator=(const MidiModel&) = delete;

	public:
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

		double LoopIndexFrac() const noexcept { return _loopIndexFrac; }
		void SetLoopIndexFrac(double frac) noexcept;
		unsigned int NoteInstanceCount() const noexcept { return _backInstanceCount; }
		void UpdateModel(const std::vector<MidiNote>& spans, std::uint32_t loopLengthSamps);
		void QueueModelUpdate(const std::vector<MidiNote>& spans, std::uint32_t loopLengthSamps);
		static std::vector<float> BuildBaseVerts(unsigned int segments);
		static std::vector<float> BuildBaseUvs(unsigned int segments);

	protected:
		std::weak_ptr<resources::ShaderResource> GetShader() override;

	private:
		std::shared_ptr<ModelInstanceData> BuildInstanceData(const std::vector<MidiNote>& spans,
			std::uint32_t loopLengthSamps) const;
		void ApplyPendingModelUpdate();
		float PitchOffset(std::uint8_t note) const noexcept;

	private:
		MidiModelParams _midiParams;
		double _loopIndexFrac;
		std::atomic<std::shared_ptr<ModelInstanceData>> _pendingModelUpdate;
	};
}
