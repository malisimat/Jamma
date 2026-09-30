#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "../gui/GuiModel.h"
#include "../midi/MidiNote.h"
#include "../midi/LoopGridGeometry.h"
#include "../midi/MidiQuantisation.h"

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
		bool DrawSelectionRing;
		std::uint8_t CenterPitch;
	};

	class MidiModel : public virtual gui::GuiModel
	{
	public:
		struct EditorPreviewSpan
		{
			std::uint32_t Start = 0u, End = 0u;
			std::uint8_t Pitch = 60u;
			bool Fill = true;
		};

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
		void SetEditorMorph(float morph) noexcept { _editorMorph = std::clamp(morph, 0.0f, 1.0f); }
		void SetEditorActive(bool active) noexcept { _editorActive = active; }
		void SetClickPressed(bool pressed) noexcept { _clickPressed = pressed; }
		void SetEditorPlayFrac(float frac) noexcept { _editorPlayFrac = frac; }
		void SetEditorPitchRange(int bottomPitch, int visibleRows) noexcept;
		int EditorBottomPitch() const noexcept { return _editorBottomPitch; }
		int EditorVisibleRows() const noexcept { return _editorVisibleRows; }
		bool EditorGridResolved() const noexcept { return _editorGridResolved; }
		void SetEditorHover(float u, int pitch) noexcept;
		void SetEditorPreview(std::vector<EditorPreviewSpan> spans,
			std::uint32_t loopLength);
		void UpdateEditorGrid(std::uint32_t loopLength,
			const midi::MidiQuantisationSettings& settings, std::uint64_t transportStart);
		unsigned int NoteInstanceCount() const noexcept { return _backNoteInstanceCount; }
		unsigned int TotalInstanceCount() const noexcept { return _backInstanceCount; }
		void UpdateModel(const std::vector<midi::MidiNote>& spans, std::uint32_t loopLengthSamps);
		void QueueModelUpdate(const std::vector<midi::MidiNote>& spans, std::uint32_t loopLengthSamps);
		static std::vector<float> BuildBaseVerts(unsigned int segments);
		static std::vector<float> BuildBaseUvs(unsigned int segments);
		static std::vector<float> BuildEditorGridVertices(const midi::LoopGridGeometry* grid,
			std::uint32_t loopLength, int bottomPitch, int visibleRows);

		// Back-pointer to the owning loop so the renderer can read automation lanes.
		// The loop owns this model (shared_ptr), so the raw pointer outlives the model.
		void SetAutomationSource(const midi::MidiLoop* loop) noexcept { _automationSource = loop; }

	protected:
		std::weak_ptr<resources::ShaderResource> GetShader() override;
		void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) override;
	void _ReleaseResources() override;
	void DrawMesh(GLuint shaderProgram, unsigned int drawInstances) override;

	private:
		friend class MidiModelParams;
		static constexpr unsigned int BaseArcSegments = 32u;
		static constexpr unsigned int TimePitchAttribute = 3u;
		static constexpr unsigned int ShapeAttribute = 4u;
		// Automation curtain tessellation around the loop circumference. Higher counts
		// give a smoother undulating ribbon at the cost of more vertices (built once).
		static constexpr unsigned int AutomationArcSegments = 160u;
		static void AddTri(std::vector<float>& verts,
			float x1, float y1, float z1,
			float x2, float y2, float z2,
			float x3, float y3, float z3);
		static void AddUvTri(std::vector<float>& uvs,
			float u1, float v1, float u2, float v2, float u3, float v3);
		std::shared_ptr<ModelInstanceData> BuildInstanceData(const std::vector<midi::MidiNote>& spans,
			std::uint32_t loopLengthSamps) const;
		void ApplyPendingModelUpdate();
		float PitchOffset(std::uint8_t note) const noexcept;

		// --- Automation curtain rendering ---
		void _InitAutomationGl(resources::ResourceLib& resourceLib);
		void _ReleaseAutomationGl();
		void _DrawAutomation(GlDrawContext& glCtx);
		void _DrawEditorGrid(GlDrawContext& glCtx);

	private:
		MidiModelParams _midiParams;
		double _loopIndexFrac;
		float _editorMorph = 0.0f;
		bool _editorActive = false;
		bool _clickPressed = false;
		float _editorPlayFrac = 0.0f;
		int _editorBottomPitch = 48;
		int _editorVisibleRows = 24;
		float _editorHoverU = -1.0f;
		int _editorHoverPitch = -1;
		std::vector<float> _editorGridVertices;
		std::vector<EditorPreviewSpan> _editorPreviewSpans;
		std::uint32_t _editorPreviewLength = 0u;
		unsigned int _editorPreviewVertexCount = 0u;
		midi::MidiQuantisationSettings _editorGridSettings;
		std::uint64_t _editorGridTransportStart = 0u;
		std::uint32_t _editorGridLength = 0u;
		bool _editorGridSignatureValid = false;
		bool _editorGridResolved = false;
		bool _editorGridDirty = true;
		std::weak_ptr<resources::ShaderResource> _editorGridShader;
		unsigned int _editorGridVao = 0u;
		unsigned int _editorGridVbo = 0u;
		unsigned int _editorGridVertexCount = 0u;
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
