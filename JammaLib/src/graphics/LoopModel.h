#pragma once

#include <algorithm>
#include <array>
#include <mutex>
#include <vector>
#include <tuple>
#include <memory>
#include "../../include/Constants.h"
#include "../utils/ArrayUtils.h"
#include "../utils/VecUtils.h"
#include "../audio/BufferBank.h"
#include "../spectral/AudioSpectralPlaybackSnapshot.h"
#include "../gui/GuiModel.h"
#include "CameraBounds.h"

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

		// Non-callback read through the existing waveform publication mutex.
		graphics::CameraBounds CameraLocalBounds() const;
		float SceneInnerRadius() const;
		float UnfittedHalfHeight() const;
		void SetSceneHeightScale(float scale) noexcept { _sceneHeightScale = std::clamp(scale, 0.0f, 1.0f); }
		static float FitSceneHeight(float largestHalfHeight, float bandHeight) noexcept;
		// Half-height is expressed in Loop-local geometry, before its model scale.
		void SetPickerHalfHeight(float halfHeight) noexcept { _pickerHalfHeight = (std::max)(0.0f, halfHeight); }
		static float PickerLocalHalfHeight(float availableHeight, float modelScale) noexcept;
		double LoopIndexFrac() const;
		void SetLoopIndexFrac(double frac);
		void SetLoopState(LoopModelState state);
		// Shader press state: 0 = released/painting, 1 = left click, 2 = mute click.
		void SetClickPressed(bool pressed, bool mute = false) noexcept { _clickPressed = pressed ? (mute ? 2.0f : 1.0f) : 0.0f; }
		void SetMuted(bool muted) noexcept { _muted = muted; }
		void SetWaveformColorScale(float scale) noexcept;
		void SetEditorMorph(float morph) noexcept { _editorMorph = std::clamp(morph, 0.0f, 1.0f); }
		void SetSpectralReveal(float reveal) noexcept { _spectralReveal = std::clamp(reveal, 0.0f, 1.0f); }
		void SetEditorActive(bool active) noexcept { _editorActive = active; }
		void UpdateModel(const audio::BufferBank& buffer,
			unsigned long loopLength,
			unsigned long offset,
			float radius,
			bool allowUnchangedSkip = false);
		// UI-owned bounded upload preparation from worker-decimated immutable audio.
		void UpdateEditedModel(const std::shared_ptr<const spectral::AudioSpectralPlaybackSnapshot>& snapshot,
			float radius);
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
		static constexpr float _DisplayScale = 1.3f;
		static const float _UnitMeshRadius;
		static constexpr unsigned int _WaveformSegments = 2048u;
		static constexpr unsigned int _PickerSegments = 128u;
		unsigned int _pickerVertexArray = 0u;
		std::array<unsigned int, 2> _pickerBuffers{};
		float _pickerHalfHeight = 1.0f;
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
		std::weak_ptr<const spectral::AudioSpectralPlaybackSnapshot> _lastEditedWaveform;
		bool _usesEditedWaveform = false;
		unsigned long _lastWaveformSourceLength;
		unsigned long _lastWaveformDisplayLength;
		unsigned long _lastWaveformOffset;
		float _lastWaveformRadius;
		float _waveformColorScale;
		float _spectralReveal = 0.0f;
		float _editorMorph = 0.0f;
		bool _editorActive = false;
		float _clickPressed = 0.0f;
		bool _muted = false;
		float _cameraHalfHeight = 1.0f;
		float _sceneHeightScale = 1.0f;
		mutable std::mutex _waveformMutex;
	};
}
