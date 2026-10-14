#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>
#include "../gui/GuiModel.h"
#include "../audio/AudioSpectralDsp.h"
#include "../audio/AudioSpectralGainField.h"
#include "../audio/AudioSpectralEnvelope.h"

namespace editor { class AudioSpectralDisplayJob; }

namespace graphics
{
	// Normalized source interval. Requested bounds are UI-owned; the model
	// captures exact time/frequency bounds with the successful draw for picking.
	struct AudioSpectralViewport
	{
		double Lower = 0.0, Upper = 1.0;
		bool IsValid() const noexcept;
		bool operator==(const AudioSpectralViewport&) const = default;
	};
	// Row-major vertex heights: time varies fastest; both time endpoints exist.
	// The DC edge is row zero; positive rows run logarithmically to Nyquist.
	struct AudioSpectralDisplay
	{
        struct Rectangle { unsigned int Left, Right, Bottom, Top; };
        std::uint64_t BaselineRevision = 0;
        std::vector<Rectangle> DirtyRectangles;
        bool CanSubUpload(const AudioSpectralDisplay& baseline, bool trustedDamage = false) const noexcept;
		std::shared_ptr<const audio::AudioSpectralSourceIdentity> SourceIdentity;
		std::uint64_t SourceGeneration = 0u;
		std::uint64_t Revision = 0u;
		std::uint64_t PresentationRevision = 0u;
		unsigned int TimeSegments = 0u;
		unsigned int FrequencySegments = 0u;
		float MinimumFrequencyHz = 0.0f;
		float NyquistHz = 0.0f;
		float MaximumFrequencyHz = 0.0f;
		bool IncludesDc = true;
		AudioSpectralViewport Viewport;
		std::vector<float> Heights;
		bool IsValid() const noexcept;
		double FrequencyAt(double v) const noexcept;
		double FrequencyV(double hz) const noexcept;
	};

	class AudioSpectralModel : public gui::GuiModel
	{
	public:
		static constexpr unsigned int DefaultTimeSegments = 4095u;
		static constexpr unsigned int DefaultFrequencySegments = 256u;
		static constexpr float DepthRadiusScale = 0.78f;
		static constexpr float HeightRadiusScale = 0.25f;
		static constexpr float SurfaceBaseY = 3.0f;
		struct Surface
		{
			std::shared_ptr<const AudioSpectralDisplay> Display;
			float Radius = 100.0f;
			float Morph = 0.0f;
			float PlayFraction = 0.0f;
			// Exact transform stack used by the last successful scene draw.
			glm::mat4 ClipFromLocal{1.0f};
			bool HasDrawTransform = false;
			AudioSpectralViewport TimeViewport, FrequencyViewport;
		};

		explicit AudioSpectralModel(gui::GuiModelParams params,
			unsigned int timeSegments = DefaultTimeSegments,
			unsigned int frequencySegments = DefaultFrequencySegments);
		~AudioSpectralModel();
		// Worker-only canonical full-source projection; never feeds back into DSP.
		// Legacy viewport argument is validated but does not change sampling.
		static std::shared_ptr<const AudioSpectralDisplay> BuildDisplay(
			const audio::AudioSpectralAnalysis& analysis,
			const audio::AudioSpectralGainField* gains,
			const audio::AudioSpectralEnvelope* envelope, std::uint64_t revision,
			unsigned int timeSegments, unsigned int frequencySegments,
			std::string& reason, std::stop_token stop = {}, AudioSpectralViewport viewport = {},
			std::uint64_t presentationRevision = 0u,
            std::shared_ptr<const AudioSpectralDisplay> baseline = nullptr,
            std::vector<AudioSpectralDisplay::Rectangle> rectangles = {});
        static std::vector<AudioSpectralDisplay::Rectangle> ProjectDamage(
            const audio::AudioSpectralAnalysis&, const AudioSpectralDisplay&,
            std::size_t firstFrame, std::size_t lastFrame, unsigned int firstBin, unsigned int lastBin,
            unsigned long firstSample, unsigned long lastSample);
        bool SetCompletedDisplay(const editor::AudioSpectralDisplayJob&, std::string& reason);
		bool SetDisplay(std::shared_ptr<const AudioSpectralDisplay> display, std::string& reason);
		void SetEditorMorph(float morph);
		void SetRadius(float radius);
		void SetPlayFraction(double fraction);
		void SetViewport(AudioSpectralViewport time, AudioSpectralViewport frequency);
		void SetBrush(glm::vec2 centre, glm::vec2 radii, int mode);
		// Exact data/morph/radius of the last successful scene draw, retained for picking.
		Surface DisplayedSurface() const;
		std::shared_ptr<const AudioSpectralDisplay> Displayed() const;
		bool IsAvailable() const;
		unsigned int TimeSegments() const noexcept { return _timeSegments; }
		unsigned int FrequencySegments() const noexcept { return _frequencySegments; }
		std::string UnavailableReason() const;
		static glm::vec3 LocalVertex(const Surface& surface, unsigned int timeVertex,
			unsigned int frequencyVertex) noexcept;
		// Every cell has triangles (00,10,01) and (10,11,01), shared by GPU/picking.
		void Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass) override;

	protected:
		void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
		void _ReleaseResources() override;

	private:
        friend class editor::AudioSpectralDisplayJob;
        static std::shared_ptr<const AudioSpectralDisplay> _BuildDisplay(
            const audio::AudioSpectralAnalysis&, const audio::AudioSpectralGainField*,
            const audio::AudioSpectralEnvelope*, std::uint64_t, unsigned int, unsigned int,
            std::string&, std::stop_token, AudioSpectralViewport, std::uint64_t,
            std::shared_ptr<const AudioSpectralDisplay>, std::vector<AudioSpectralDisplay::Rectangle>);
		static gui::GuiModelParams _ModelParams(gui::GuiModelParams params);
		static float _CompressMagnitude(double magnitude) noexcept;
        bool _SetDisplay(std::shared_ptr<const AudioSpectralDisplay>, std::string& reason, bool trustedDamage);
		bool _UploadDisplay();
		void _Fail(std::string reason);
		const unsigned int _timeSegments;
		const unsigned int _frequencySegments;
		std::vector<glm::vec2> _grid;
		std::vector<unsigned int> _indices;
		unsigned int _gridBuffer = 0u;
		unsigned int _indexBuffer = 0u;
		unsigned int _heightTexture = 0u;
		mutable std::mutex _surfaceMutex; // UI/render/worker only; never audio.
		std::shared_ptr<const AudioSpectralDisplay> _pendingDisplay;
        bool _pendingTrustedDamage = false; // Set only by completed worker jobs, under surface mutex.
		std::shared_ptr<const AudioSpectralDisplay> _uploadedDisplay;
		Surface _displayedSurface;
		AudioSpectralViewport _timeViewport, _frequencyViewport; // UI writes/render reads under _surfaceMutex; no audio access.
		float _radius = 100.0f;
		float _morph = 0.0f;
		float _playFraction = 0.0f;
		glm::vec2 _brushCentre{-1.0f};
		glm::vec2 _brushRadii{0.0f};
		int _brushMode = 0;
		std::string _unavailableReason;
	};
}
