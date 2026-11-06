#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>
#include "../gui/GuiModel.h"
#include "../spectral/SpectralDsp.h"
#include "../spectral/SpectralGainField.h"
#include "../spectral/SpectralEnvelope.h"

namespace spectral { class SpectralDisplayJob; }

namespace graphics
{
	// CPU-only immutable mesh. An editor prepares one during startup and retains
	// its roughly 32 MiB arrays across opens; dynamic display/GL state stays per model.
	class SpectralTopology final
	{
	public:
		SpectralTopology(unsigned int timeSegments, unsigned int frequencySegments);
		unsigned int TimeSegments() const noexcept { return _timeSegments; }
		unsigned int FrequencySegments() const noexcept { return _frequencySegments; }
		const std::vector<glm::vec2>& Grid() const noexcept { return _grid; }
		const std::vector<unsigned int>& Indices() const noexcept { return _indices; }
	private:
		const unsigned int _timeSegments, _frequencySegments;
		std::vector<glm::vec2> _grid;
		std::vector<unsigned int> _indices;
	};

	// Immutable upload generation tied to one window GL context. Models retain
	// their generation until their VAO is detached, including during force reset.
	class SpectralMeshBuffers final
	{
	public:
		explicit SpectralMeshBuffers(const SpectralTopology& topology);
		~SpectralMeshBuffers();
		SpectralMeshBuffers(const SpectralMeshBuffers&) = delete;
		SpectralMeshBuffers& operator=(const SpectralMeshBuffers&) = delete;
		bool IsCurrent() const noexcept;
		unsigned int GridBuffer() const noexcept { return _gridBuffer; }
		unsigned int IndexBuffer() const noexcept { return _indexBuffer; }
	private:
		HGLRC _context = nullptr;
		unsigned int _gridBuffer = 0u, _indexBuffer = 0u;
	};

	// Editor-owned cache; GPU methods are current-context/render-thread only.
	class SpectralMeshResource final
	{
	public:
		explicit SpectralMeshResource(std::shared_ptr<const SpectralTopology> topology);
		const std::shared_ptr<const SpectralTopology>& Topology() const noexcept { return _topology; }
		std::shared_ptr<const SpectralMeshBuffers> Prepare();
		void Release() { _buffers.reset(); }
	private:
		const std::shared_ptr<const SpectralTopology> _topology;
		std::shared_ptr<const SpectralMeshBuffers> _buffers;
	};

	// Normalized source interval. Requested bounds are UI-owned; the model
	// captures exact time/frequency bounds with the successful draw for picking.
	struct SpectralViewport
	{
		double Lower = 0.0, Upper = 1.0;
		bool IsValid() const noexcept;
		bool operator==(const SpectralViewport&) const = default;
	};
	// Row-major vertex heights: time varies fastest; both time endpoints exist.
	// The DC edge is row zero; positive rows run logarithmically to Nyquist.
	struct SpectralDisplay
	{
        struct Rectangle { unsigned int Left, Right, Bottom, Top; };
        std::uint64_t BaselineRevision = 0;
        std::vector<Rectangle> DirtyRectangles;
        bool CanSubUpload(const SpectralDisplay& baseline, bool trustedDamage = false) const noexcept;
		std::shared_ptr<const spectral::SpectralSourceIdentity> SourceIdentity;
		std::uint64_t SourceGeneration = 0u;
		std::uint64_t Revision = 0u;
		std::uint64_t PresentationRevision = 0u;
		unsigned int TimeSegments = 0u;
		unsigned int FrequencySegments = 0u;
		float MinimumFrequencyHz = 0.0f;
		float NyquistHz = 0.0f;
		float MaximumFrequencyHz = 0.0f;
		bool IncludesDc = true;
		SpectralViewport Viewport;
		std::vector<float> Heights;
		bool IsValid() const noexcept;
		double FrequencyAt(double v) const noexcept;
		double FrequencyV(double hz) const noexcept;
	};

	class SpectralModel : public gui::GuiModel
	{
	public:
		static constexpr unsigned int DefaultTimeSegments = 4095u;
		static constexpr unsigned int DefaultFrequencySegments = 256u;
		static constexpr float DepthRadiusScale = 0.78f;
		static constexpr float HeightRadiusScale = 0.25f;
		static constexpr float SurfaceBaseY = 3.0f;
		struct Surface
		{
			std::shared_ptr<const SpectralDisplay> Display;
			float Radius = 100.0f;
			float Morph = 0.0f;
			float PlayFraction = 0.0f;
			// Exact transform stack used by the last successful scene draw.
			glm::mat4 ClipFromLocal{1.0f};
			bool HasDrawTransform = false;
			SpectralViewport TimeViewport, FrequencyViewport;
		};

		explicit SpectralModel(gui::GuiModelParams params,
			unsigned int timeSegments = DefaultTimeSegments,
			unsigned int frequencySegments = DefaultFrequencySegments);
		SpectralModel(gui::GuiModelParams params, std::shared_ptr<const SpectralTopology> topology);
		SpectralModel(gui::GuiModelParams params, std::shared_ptr<SpectralMeshResource> mesh);
		const std::shared_ptr<const SpectralTopology>& Topology() const noexcept { return _topology; }
		~SpectralModel();
		// Worker-only canonical full-source projection; never feeds back into DSP.
		// Legacy viewport argument is validated but does not change sampling.
		static std::shared_ptr<const SpectralDisplay> BuildDisplay(
			const spectral::SpectralAnalysis& analysis,
			const spectral::SpectralGainField* gains,
			const spectral::SpectralEnvelope* envelope, std::uint64_t revision,
			unsigned int timeSegments, unsigned int frequencySegments,
			std::string& reason, std::stop_token stop = {}, SpectralViewport viewport = {},
			std::uint64_t presentationRevision = 0u,
            std::shared_ptr<const SpectralDisplay> baseline = nullptr,
            std::vector<SpectralDisplay::Rectangle> rectangles = {});
        static std::vector<SpectralDisplay::Rectangle> ProjectDamage(
            const spectral::SpectralAnalysis&, const SpectralDisplay&,
            std::size_t firstFrame, std::size_t lastFrame, unsigned int firstBin, unsigned int lastBin,
            unsigned long firstSample, unsigned long lastSample);
        bool SetCompletedDisplay(const spectral::SpectralDisplayJob&, std::string& reason);
		bool SetDisplay(std::shared_ptr<const SpectralDisplay> display, std::string& reason);
		void SetEditorMorph(float morph);
		void SetRadius(float radius);
		void SetPlayFraction(double fraction);
		void SetViewport(SpectralViewport time, SpectralViewport frequency);
		void SetBrush(glm::vec2 centre, glm::vec2 radii, int mode);
		// Exact data/morph/radius of the last successful scene draw, retained for picking.
		Surface DisplayedSurface() const;
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
        friend class spectral::SpectralDisplayJob;
        static std::shared_ptr<const SpectralDisplay> _BuildDisplay(
            const spectral::SpectralAnalysis&, const spectral::SpectralGainField*,
            const spectral::SpectralEnvelope*, std::uint64_t, unsigned int, unsigned int,
            std::string&, std::stop_token, SpectralViewport, std::uint64_t,
            std::shared_ptr<const SpectralDisplay>, std::vector<SpectralDisplay::Rectangle>);
		static gui::GuiModelParams _ModelParams(gui::GuiModelParams params);
        bool _SetDisplay(std::shared_ptr<const SpectralDisplay>, std::string& reason, bool trustedDamage);
		bool _UploadDisplay();
		void _Fail(std::string reason);
		const std::shared_ptr<const SpectralTopology> _topology;
		const std::shared_ptr<SpectralMeshResource> _mesh;
		std::shared_ptr<const SpectralMeshBuffers> _meshBuffers;
		HGLRC _resourceContext = nullptr;
		const unsigned int _timeSegments;
		const unsigned int _frequencySegments;
		const std::vector<glm::vec2>& _grid;
		const std::vector<unsigned int>& _indices;
		unsigned int _gridBuffer = 0u;
		unsigned int _indexBuffer = 0u;
		unsigned int _heightTexture = 0u;
		mutable std::mutex _surfaceMutex; // UI/render/worker only; never audio.
		std::shared_ptr<const SpectralDisplay> _pendingDisplay;
        bool _pendingTrustedDamage = false; // Set only by completed worker jobs, under surface mutex.
		std::shared_ptr<const SpectralDisplay> _uploadedDisplay;
		Surface _displayedSurface;
		SpectralViewport _timeViewport, _frequencyViewport; // UI writes/render reads under _surfaceMutex; no audio access.
		float _radius = 100.0f;
		float _morph = 0.0f;
		float _playFraction = 0.0f;
		glm::vec2 _brushCentre{-1.0f};
		glm::vec2 _brushRadii{0.0f};
		int _brushMode = 0;
		std::string _unavailableReason;
	};
}
