#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "../gui/GuiModel.h"
#include "../midi/MidiNote.h"

namespace midi
{
	class MidiLoop;
}

namespace graphics
{
	class GlDrawContext;

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
		float DiscRadiusFactor;
		float DiscRadialThicknessFactor;
		float DiscHeightFactor;
		float DiscAlpha;
		std::uint8_t CenterPitch;
	};

	class MidiModel : public virtual gui::GuiModel
	{
	private:
		struct ModelInstanceData
		{
			std::vector<gui::GuiModel::InstanceAttribute> Attributes;
			unsigned int InstanceCount = 0u;
			unsigned int NoteCount = 0u;
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
		unsigned int NoteInstanceCount() const noexcept { return _backNoteInstanceCount; }
		unsigned int TotalInstanceCount() const noexcept { return _backInstanceCount; }
		void UpdateModel(const std::vector<midi::MidiNote>& spans, std::uint32_t loopLengthSamps);
		void QueueModelUpdate(const std::vector<midi::MidiNote>& spans, std::uint32_t loopLengthSamps);
		static std::vector<float> BuildBaseVerts(unsigned int segments);
		static std::vector<float> BuildBaseUvs(unsigned int segments);

		// Back-pointer to the owning loop so the renderer can read automation lanes.
		// The loop owns this model (shared_ptr), so the raw pointer outlives the model.
		void SetAutomationSource(const midi::MidiLoop* loop) noexcept { _automationSource = loop; }

	protected:
		std::weak_ptr<resources::ShaderResource> GetShader() override;
		void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		void _ReleaseResources() override;

	private:
		std::shared_ptr<ModelInstanceData> BuildInstanceData(const std::vector<midi::MidiNote>& spans,
			std::uint32_t loopLengthSamps) const;
		void ApplyPendingModelUpdate();
		float PitchOffset(std::uint8_t note) const noexcept;

		// --- Automation curtain rendering ---
		void _InitAutomationGl(resources::ResourceLib& resourceLib);
		void _ReleaseAutomationGl();
		void _DrawAutomation(GlDrawContext& glCtx);

	private:
		MidiModelParams _midiParams;
		double _loopIndexFrac;
		unsigned int _backNoteInstanceCount;
		std::atomic<std::shared_ptr<ModelInstanceData>> _pendingModelUpdate;

		// Automation display state. GL objects live on the render thread only.
		const midi::MidiLoop* _automationSource;
		std::atomic<std::uint32_t> _displayLengthSamps;
		bool _automationGlReady;
		std::weak_ptr<resources::ShaderResource> _automationShader;
		GLuint _curtainVao;
		GLuint _curtainVbo;
		unsigned int _curtainVertCount;
		GLuint _crownVao;
		GLuint _crownVbo;
		unsigned int _crownVertCount;
		GLuint _playVao;
		GLuint _playVbo;
		GLuint _dotVao;
		GLuint _dotVbo;
	};
}