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
#include "../audio/AudioSpectralResourceBudget.h"
#include "../audio/AudioSpectralSource.h"
#include "../audio/AudioSpectralAnalysisJob.h"
#include "../audio/AudioSpectralWorker.h"
#include "AudioSpectralDisplayJob.h"

namespace engine { class Loop; }

namespace editor
{
	// UI-only gesture values; physical radii are copied once into a stroke.
	// Viewport values are copied into immutable display requests, never shared mutable.
	class AudioSpectralControls
	{
	public:
		double TimeRadius = audio::AudioSpectralConfig::BrushTimeRadiusSeconds;
		double FrequencyRadius = audio::AudioSpectralConfig::BrushFrequencyRadiusOctaves;
		double BandRadius = audio::AudioSpectralConfig::BarTimeRadiusSeconds;
		graphics::AudioSpectralViewport Viewport;
		enum class WheelMode { Brush, Band, Zoom, Pan };
		static WheelMode WheelBinding(bool alt, bool shift, bool control) noexcept
		{ return alt ? WheelMode::Band : shift ? WheelMode::Zoom : control ? WheelMode::Pan : WheelMode::Brush; }
		void Wheel(WheelMode mode, double direction) noexcept
		{
			if (!std::isfinite(direction) || direction == 0.0) return;
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
				const auto span = Viewport.Upper - Viewport.Lower;
				const auto nextSpan = mode == WheelMode::Zoom ? std::clamp(span - direction * 0.09375, 0.0625, 1.0) : span;
				SetView(Viewport.Lower + (span - nextSpan) * 0.5
					+ (mode == WheelMode::Pan ? direction * 3.0 / 128.0 : 0.0), nextSpan);
			}
		}
		void BeginView(double anchorFraction) noexcept
		{
			_startSpan = Viewport.Upper - Viewport.Lower;
			_anchor = Viewport.Lower + std::clamp(anchorFraction, 0.0, 1.0) * _startSpan;
		}
		void MoveView(double horizontalLogicalPixels, double pointerFraction) noexcept
		{
			if (!std::isfinite(horizontalLogicalPixels) || !std::isfinite(pointerFraction)) return;
			const auto span = std::clamp(_startSpan + horizontalLogicalPixels / 1024.0, 0.0625, 1.0);
			SetView(_anchor - pointerFraction * span, span);
		}
	private:
		void SetView(double lower, double span) noexcept
		{
			Viewport.Lower = std::clamp(lower, 0.0, 1.0 - span);
			Viewport.Upper = Viewport.Lower + span;
		}
		double _startSpan = 1.0, _anchor = 0.5;
	};
	// UI-owned audio branch of the shared editor lifecycle. Loop alone owns the
	// mutable recording and callback copy; this controller requests/retries it.
	class AudioSpectralEditor
	{
	public:
		enum class State { Closed, WaitingForSource, AcquiringSource, AnalyzingSource, Ready, Unavailable };
		explicit AudioSpectralEditor(std::shared_ptr<audio::AudioSpectralWorker> worker = std::make_shared<audio::AudioSpectralWorker>())
            : _worker(std::move(worker)) {}
        ~AudioSpectralEditor();
		void Open(const std::shared_ptr<engine::Loop>& loop);
		void Close();
		void Update();
		bool HasPreparedSurface() const noexcept { return _surfacePrepared; }
		State Status() const noexcept { return _state; }
		const std::string& Feedback() const noexcept { return _feedback; }
		// Previous complete content can remain visible while reacquiring. Callers
		// must check Status/Loop identity before editing or publishing against it.
		std::shared_ptr<const audio::AudioSpectralSource> Source() const noexcept { return _source; }

		std::shared_ptr<const audio::AudioSpectralEditState> EditState() const;
		std::shared_ptr<const audio::AudioSpectralPlaybackSnapshot> Playback() const;
		std::shared_ptr<const audio::AudioSpectralAnalysis> Analysis() const noexcept { return _analysis; }
		void SetUndoHistory(actions::ActionUndoHistory* history) noexcept { _history = history; }
		bool BeginStroke(bool broadband, bool attenuate, double seconds, double hz, float logicalY,
			double timeRadius = audio::AudioSpectralConfig::BrushTimeRadiusSeconds,
			double frequencyRadius = audio::AudioSpectralConfig::BrushFrequencyRadiusOctaves,
			double bandRadius = audio::AudioSpectralConfig::BarTimeRadiusSeconds);
		void SetFrequencyViewport(graphics::AudioSpectralViewport viewport);
		void MoveStroke(std::optional<std::pair<double, double>> point, float logicalY);
		void EndStroke(bool cancel);
		bool HasStroke() const noexcept { return !!_stroke; }
        std::optional<double> BrushLevelDb() const;

	private:
		struct RevisionGate
		{
			std::uint64_t Generation = 0;
			std::shared_ptr<const audio::AudioSpectralSourceIdentity> Identity;
			std::atomic<std::uint64_t> SpectralFloor{0};
			std::atomic<bool> StrokeActive{false};
			std::atomic<bool> PlaybackFailed{false};
			std::stop_source WorkStop; // UI only; worker closures capture immutable tokens.
		};
		struct Stroke;
		struct PaintReceipt;
		class EditUndo;
		static void _QueuePlayback(const std::shared_ptr<audio::AudioSpectralWorker>& worker,
			std::weak_ptr<engine::Loop> loop, std::shared_ptr<const audio::AudioSpectralEditState> edit,
			std::shared_ptr<const audio::AudioSpectralAnalysis> analysis, std::shared_ptr<RevisionGate> gate);
		void _UpdateStroke();
		void _FinishStroke();
		void _AcceptPaint();
        void _LogStroke(const char* event = "tick");
		std::shared_ptr<audio::AudioSpectralWorker> _worker;
		std::shared_ptr<audio::AudioSpectralResourceBudget> _resourceBudget = std::make_shared<audio::AudioSpectralResourceBudget>();
		std::shared_ptr<Stroke> _stroke;
		std::shared_ptr<PaintReceipt> _paintReceipt;
		std::shared_ptr<RevisionGate> _gate;
		std::map<const engine::Loop*, std::weak_ptr<RevisionGate>> _gates;
		actions::ActionUndoHistory* _history = nullptr;
		struct HistoryRecord { std::weak_ptr<base::ActionUndo> Action; std::uint64_t Bytes; };
		std::deque<HistoryRecord> _historyRecords;
		std::shared_ptr<const audio::AudioSpectralPlaybackSnapshot> _lastVisualPlayback;
		bool _peakOverload = false;
		bool _closing = false;
		std::shared_ptr<engine::Loop> _pendingOpen;
		std::weak_ptr<engine::Loop> _loop;
		std::shared_ptr<audio::AudioSpectralSourceCapture> _capture;
		std::shared_ptr<const audio::AudioSpectralSource> _source;
		std::shared_ptr<audio::AudioSpectralAnalysisJob> _analysisJob;
		std::shared_ptr<const audio::AudioSpectralAnalysis> _analysis;
		std::shared_ptr<graphics::AudioSpectralModel> _model;
		std::deque<std::shared_ptr<AudioSpectralDisplayJob>> _displayJobs;
		std::shared_ptr<const audio::AudioSpectralEditState> _requestedDisplayEdit;
		// UI-owned, copied to jobs; worker only reads const request captures.
		// Close/source teardown cancels all jobs before releasing these requests.
		graphics::AudioSpectralViewport _viewport, _requestedViewport;
		std::uint64_t _presentationRevision = 0u;
		bool _surfacePrepared = false;
		std::string _displayFailure;
		void _UpdateDisplay();
		std::shared_ptr<const audio::AudioSpectralEditState> _initialEdit;
		std::shared_ptr<const audio::AudioSpectralPlaybackSnapshot> _initialPlayback;
		std::optional<audio::AudioSpectralSourceLayout> _budgetLayout;
		State _state = State::Closed;
		std::string _feedback;
		std::string _editFailure;
		std::uint64_t _unavailableGeneration = 0u;
	};
}
