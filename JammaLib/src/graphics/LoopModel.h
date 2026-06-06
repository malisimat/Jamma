#pragma once

#include <array>
#include <mutex>
#include <vector>
#include <tuple>
#include <memory>
#include "../../include/Constants.h"
#include "../utils/ArrayUtils.h"
#include "../utils/VecUtils.h"
#include "../audio/BufferBank.h"
#include "../gui/GuiModel.h"

namespace engine
{
	class LoopModelParams :
		public gui::GuiModelParams
	{
	public:
		LoopModelParams() :
			gui::GuiModelParams(),
			LedHeight(10.0)
		{
		}

		LoopModelParams(gui::GuiModelParams params) :
			gui::GuiModelParams(params),
			LedHeight(10.0)
		{
		}

	public:
		double LedHeight;
	};

	class LoopModel :
		public virtual gui::GuiModel
	{
	public:
		enum LoopModelState
		{
			STATE_RECORDING = 0,
			STATE_PLAYING = 1,
			STATE_MUTED = 2,
			STATE_PICKING = 3,
			STATE_HIGHLIGHTING = 4
		};

	public:
		LoopModel(LoopModelParams params);
		~LoopModel();

		// Copy
		LoopModel(const LoopModel&) = delete;
		LoopModel& operator=(const LoopModel&) = delete;

	public:
		virtual void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

		double LoopIndexFrac() const;
		void SetLoopIndexFrac(double frac);
		void SetLoopState(LoopModelState state);
		void SetWaveformColorScale(float scale) noexcept;
		void UpdateModel(const audio::BufferBank& buffer,
			unsigned long loopLength,
			unsigned long offset,
			float radius,
			bool allowUnchangedSkip = false);
		void UpdateModel(const audio::BufferBank& buffer,
			unsigned long sourceLoopLength,
			unsigned long displayLoopLength,
			unsigned long offset,
			float radius,
			bool allowUnchangedSkip = false);

		// Waveform Decimation & Texture Data Generation
		static std::vector<glm::vec2> DecimateWaveform(const audio::BufferBank& buffer, unsigned long offset, unsigned long length, unsigned int numSegments);

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		virtual void _ReleaseResources() override;

		static unsigned int TotalNumLeds(unsigned int vuHeight, unsigned int ledHeight);
		static unsigned int CurrentNumLeds(unsigned int vuHeight, unsigned int ledHeight, double value);

		std::weak_ptr<resources::ShaderResource> GetShader() override;
		void UploadWaveformTexture();

		std::tuple<std::vector<float>, std::vector<float>, float, float>
			CalcGrainGeometry(const audio::BufferBank& buffer,
				unsigned int grain,
				unsigned int numGrains,
				unsigned long offset,
				float lastYMin,
				float lastYMax,
				float radius);

		std::tuple<std::vector<float>, std::vector<float>, float, float>
			CalcGrainGeometry(const audio::BufferBank& buffer,
				unsigned long sourceLoopLength,
				unsigned int grain,
				unsigned int numGrains,
				unsigned long offset,
				float lastYMin,
				float lastYMax,
				float radius);

		static void DecimateWaveformInto(const audio::BufferBank& buffer,
			unsigned long offset,
			unsigned long length,
			std::vector<glm::vec2>& outSegments);
		static std::tuple<std::vector<float>, std::vector<float>> BuildFixedGeometry(unsigned int numSegments, float radius);

	protected:
		static const utils::Size2d _LedGap;
		static const float _MinHeight;
		static const float _RadialThicknessFrac;
		static const float _HeightScale;
		static const float _UnitMeshRadius;
		static constexpr unsigned int _WaveformSegments = 2048u;
		static constexpr unsigned int _WaveformPboCount = 2u;
		static constexpr unsigned long _RecordingWaveformUpdateIntervalSamps =
			constants::DefaultSampleRate / 30ul;

		double _loopIndexFrac;
		LoopModelState _modelState;
		float _waveformRadius;
		float _waveformColorMultiplier;
		bool _hasWaveformData;
		bool _waveformNeedsUpload;
		unsigned int _waveformTexture;
		std::array<unsigned int, _WaveformPboCount> _waveformPbos;
		unsigned int _waveformWritePboIndex;
		std::vector<glm::vec2> _waveformDecimated;
		std::vector<glm::vec2> _waveformWorkDecimated;
		bool _hasWaveformUpdateSignature;
		unsigned long _lastWaveformSourceLength;
		unsigned long _lastWaveformDisplayLength;
		unsigned long _lastWaveformOffset;
		float _lastWaveformRadius;
		float _waveformColorScale;
		std::mutex _waveformMutex;
	};
}
