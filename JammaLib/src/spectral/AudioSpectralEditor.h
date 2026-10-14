#pragma once

#include <memory>
#include <utility>
#include <deque>
#include <optional>
#include <string>
#include <chrono>
#include <map>
#include <algorithm>
#include <cmath>
#include "../actions/ActionUndoHistory.h"
#include "AudioSpectralResourceBudget.h"
#include "AudioSpectralSource.h"
#include "AudioSpectralAnalysisJob.h"
#include "AudioSpectralWorker.h"
#include "AudioSpectralDisplayJob.h"

namespace engine { class Loop; }

namespace spectral
{
	// UI-only gesture values; physical radii are copied once into a stroke.
	// View bounds are UI-owned; model copies them under its existing render mutex.
	class AudioSpectralControls
	{
	public:
		double TimeRadius = AudioSpectralConfig::BrushTimeRadiusSeconds;
		double FrequencyRadius = AudioSpectralConfig::BrushFrequencyRadiusOctaves;
		double BandRadius = AudioSpectralConfig::BarTimeRadiusSeconds;
		graphics::AudioSpectralViewport Viewport, TimeViewport;
		enum class WheelMode { Brush, Band, Zoom, TimeZoom, Reserved };
        enum class DragMode { Paint, Pan, Zoom, Reserved };
        static DragMode DragBinding(bool right, bool shift, bool control) noexcept
        { return control ? DragMode::Reserved : shift ? (right ? DragMode::Zoom : DragMode::Pan) : DragMode::Paint; }
		static WheelMode WheelBinding(bool alt, bool shift, bool control) noexcept
		{ return control ? WheelMode::Reserved : alt && shift ? WheelMode::TimeZoom : alt ? WheelMode::Band : shift ? WheelMode::Zoom : WheelMode::Brush; }
		void Wheel(WheelMode mode, double direction, double anchorFraction = 0.5) noexcept
		{
			if (!std::isfinite(anchorFraction) || mode == WheelMode::Reserved || !std::isfinite(direction) || direction == 0.0) return;
			direction = direction > 0.0 ? 1.0 : -1.0;
			const auto factor = std::pow(1.2, direction);
			if (mode == WheelMode::Band) BandRadius = std::clamp(BandRadius * factor, 0.005, 2.0);
			else if (mode == WheelMode::Brush)
			{
				TimeRadius = std::clamp(TimeRadius * factor, 0.005, 2.0);
				FrequencyRadius = std::clamp(FrequencyRadius * factor, 0.0625, 4.0);
			}
			else
			{
				auto& view = mode == WheelMode::TimeZoom ? TimeViewport : Viewport;
				const auto span = view.Upper - view.Lower;
				const auto nextSpan = std::clamp(span - direction * 0.09375, 0.0625, 1.0);
				SetAxis(view, view.Lower + (span - nextSpan) * std::clamp(anchorFraction, 0.0, 1.0), nextSpan);
			}
		}
        void BeginZoom(double timeFraction, double frequencyFraction) noexcept
        {
            if (!std::isfinite(timeFraction) || !std::isfinite(frequencyFraction)) return;
            _startTime = TimeViewport; _startFrequency = Viewport;
            _timeFraction = std::clamp(timeFraction, 0.0, 1.0);
            _frequencyFraction = std::clamp(frequencyFraction, 0.0, 1.0);
        }
        void MoveZoom(double horizontalPixels, double verticalPixels) noexcept
        {
            if (!std::isfinite(horizontalPixels) || !std::isfinite(verticalPixels)) return;
            const auto apply = [](graphics::AudioSpectralViewport& view, const graphics::AudioSpectralViewport& start, double anchor, double pixels)
            {
                if (pixels == 0.0) { view = start; return; }
                const auto oldSpan = start.Upper - start.Lower;
                const auto span = std::clamp(oldSpan * std::exp(std::clamp(pixels / 256.0, -20.0, 20.0)), 0.0625, 1.0);
                SetAxis(view, start.Lower + anchor * (oldSpan - span), span);
            };
            apply(TimeViewport, _startTime, _timeFraction, horizontalPixels);
            apply(Viewport, _startFrequency, _frequencyFraction, verticalPixels);
        }
        // Source under the captured pointer follows it; preserve both axis spans.
        void MovePan(double timeFraction, double frequencyFraction) noexcept
        {
            if (!std::isfinite(timeFraction) || !std::isfinite(frequencyFraction)) return;
            const auto timeSpan = _startTime.Upper - _startTime.Lower;
            const auto frequencySpan = _startFrequency.Upper - _startFrequency.Lower;
            SetAxis(TimeViewport, _startTime.Lower + (_timeFraction - timeFraction) * timeSpan, timeSpan);
            SetAxis(Viewport, _startFrequency.Lower + (_frequencyFraction - frequencyFraction) * frequencySpan, frequencySpan);
        }
	private:
        static void SetAxis(graphics::AudioSpectralViewport& view, double lower, double span) noexcept
        {
            view.Lower = std::clamp(lower, 0.0, 1.0 - span); view.Upper = view.Lower + span;
        }
        graphics::AudioSpectralViewport _startTime, _startFrequency;
        double _timeFraction = 0.5, _frequencyFraction = 0.5;

	};
	// UI-owned audio branch of the shared editor lifecycle. Loop alone owns the
	// mutable recording and callback copy; this controller requests/retries it.
	class AudioSpectralEditor
	{
	public:
		enum class State { Closed, WaitingForSource, AcquiringSource, AnalyzingSource, Ready, Unavailable };
		explicit AudioSpectralEditor(std::shared_ptr<AudioSpectralWorker> worker = std::make_shared<AudioSpectralWorker>(),
            unsigned int timeSegments = graphics::AudioSpectralModel::DefaultTimeSegments,
            unsigned int frequencySegments = graphics::AudioSpectralModel::DefaultFrequencySegments)
            : _worker(std::move(worker)), _timeSegments(timeSegments), _frequencySegments(frequencySegments) {}
        ~AudioSpectralEditor();
		void Open(const std::shared_ptr<engine::Loop>& loop);
		void Close();
		void Update();
		bool HasPreparedSurface() const noexcept { return _surfacePrepared; }
        std::uint64_t PreparedDisplayRevision() const noexcept { return _preparedDisplayRevision; }
		State Status() const noexcept { return _state; }
		const std::string& Feedback() const noexcept { return _feedback; }
		// Previous complete content can remain visible while reacquiring. Callers
		// must check Status/Loop identity before editing or publishing against it.
		std::shared_ptr<const AudioSpectralSource> Source() const noexcept { return _source; }

		std::shared_ptr<const AudioSpectralEditState> EditState() const;
		std::shared_ptr<const AudioSpectralPlaybackSnapshot> Playback() const;
		std::shared_ptr<const AudioSpectralAnalysis> Analysis() const noexcept { return _analysis; }
		void SetUndoHistory(actions::ActionUndoHistory* history) noexcept { _history = history; }
		bool BeginStroke(bool broadband, bool attenuate, double seconds, double hz, double logicalY,
			double timeRadius = AudioSpectralConfig::BrushTimeRadiusSeconds,
			double frequencyRadius = AudioSpectralConfig::BrushFrequencyRadiusOctaves,
			double bandRadius = AudioSpectralConfig::BarTimeRadiusSeconds);
		void SetViewport(graphics::AudioSpectralViewport time, graphics::AudioSpectralViewport frequency);
		void MoveStroke(std::optional<std::pair<double, double>> point, double logicalY);
		void EndStroke(bool cancel);
		bool HasStroke() const noexcept { return !!_stroke; }
        std::optional<double> BrushLevelDb() const;

	private:
		struct RevisionGate
		{
			std::uint64_t Generation = 0;
			std::shared_ptr<const AudioSpectralSourceIdentity> Identity;
			std::atomic<std::uint64_t> SpectralFloor{0};
			std::atomic<bool> StrokeActive{false};
			std::atomic<bool> PlaybackFailed{false};
			std::stop_source WorkStop; // UI only; worker closures capture immutable tokens.
		};
		struct Stroke;
		struct PaintReceipt;
		class EditUndo;
		static void _QueuePlayback(const std::shared_ptr<AudioSpectralWorker>& worker,
			std::weak_ptr<engine::Loop> loop, std::shared_ptr<const AudioSpectralEditState> edit,
			std::shared_ptr<const AudioSpectralAnalysis> analysis, std::shared_ptr<RevisionGate> gate);
		void _UpdateStroke();
		void _FinishStroke();
		void _AcceptPaint();
		std::shared_ptr<AudioSpectralWorker> _worker;
        const unsigned int _timeSegments, _frequencySegments;
		std::shared_ptr<AudioSpectralResourceBudget> _resourceBudget = std::make_shared<AudioSpectralResourceBudget>();
		std::shared_ptr<Stroke> _stroke;
		std::shared_ptr<PaintReceipt> _paintReceipt;
		std::shared_ptr<RevisionGate> _gate;
		std::map<const engine::Loop*, std::weak_ptr<RevisionGate>> _gates;
		actions::ActionUndoHistory* _history = nullptr;
		struct HistoryRecord { std::weak_ptr<base::ActionUndo> Action; std::uint64_t Bytes; };
		std::deque<HistoryRecord> _historyRecords;
		std::shared_ptr<const AudioSpectralPlaybackSnapshot> _lastVisualPlayback;
		bool _peakOverload = false;
		bool _closing = false;
		std::shared_ptr<engine::Loop> _pendingOpen;
		std::weak_ptr<engine::Loop> _loop;
		std::shared_ptr<AudioSpectralSourceCapture> _capture;
		std::shared_ptr<const AudioSpectralSource> _source;
		std::shared_ptr<AudioSpectralAnalysisJob> _analysisJob;
		std::shared_ptr<const AudioSpectralAnalysis> _analysis;
		std::shared_ptr<graphics::AudioSpectralModel> _model;
		std::deque<std::shared_ptr<AudioSpectralDisplayJob>> _displayJobs;
		std::shared_ptr<const AudioSpectralEditState> _requestedDisplayEdit;
        std::shared_ptr<AudioSpectralDisplayJob::Cache> _displayCache = std::make_shared<AudioSpectralDisplayJob::Cache>();
        AudioSpectralDisplayJob::Damage _displayDamage; // UI receipt metadata, copied before worker handoff.
		// UI-owned, copied to jobs; worker only reads const request captures.
		// Close/source teardown cancels all jobs before releasing these requests.
		std::uint64_t _presentationRevision = 0u;
		bool _surfacePrepared = false;
        std::uint64_t _preparedDisplayRevision = 0;
		std::string _displayFailure;
		void _UpdateDisplay();
		std::shared_ptr<const AudioSpectralEditState> _initialEdit;
		std::shared_ptr<const AudioSpectralPlaybackSnapshot> _initialPlayback;
		std::optional<AudioSpectralSourceLayout> _budgetLayout;
		State _state = State::Closed;
		std::string _feedback;
		std::string _editFailure;
		std::uint64_t _unavailableGeneration = 0u;
	};
}
