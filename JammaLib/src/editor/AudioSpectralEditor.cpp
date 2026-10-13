#include "AudioSpectralEditor.h"
#include "../engine/Loop.h"
#include <algorithm>
#include <cmath>
#include <new>
#include <utility>
#include <iostream>
#include <string_view>
#include <limits>

using namespace editor;

struct AudioSpectralEditor::Stroke
{
	std::shared_ptr<const audio::AudioSpectralEditState> Before;
	std::shared_ptr<const audio::AudioSpectralEditState> Latest;
	std::shared_ptr<const audio::AudioSpectralAnalysis> Analysis;
	std::shared_ptr<audio::AudioSpectralGainField> Gains; // Worker only.
	std::unique_ptr<audio::AudioSpectralEnvelopeStroke> Envelope; // Worker only.
	std::shared_ptr<const audio::AudioSpectralEnvelope> PublishedEnvelope; // Worker only.
	// UI-owned latest target, replaced while busy. Worker captures a value copy;
	// release/acquire receipt hands back results. Cancellation drops UI ownership.
	std::optional<float> PendingTarget;
	// Initialized by UI before handoff and immutable for worker/UI readers.
	double AnchorSeconds = 0.0;
	double TimeRadius = audio::AudioSpectralConfig::BrushTimeRadiusSeconds;
	double FrequencyRadius = audio::AudioSpectralConfig::BrushFrequencyRadiusOctaves;
	double BandRadius = audio::AudioSpectralConfig::BarTimeRadiusSeconds;
    std::unique_ptr<audio::AudioSpectralGainField::ExposureMap> InputMap, SpareMap; // UI-owned except handed-off map.
	std::optional<std::pair<double, double>> Point;
	float AnchorY = 0, Target = 1;
	bool Broadband = false, Attenuate = false, Released = false;
	std::chrono::steady_clock::time_point Last = std::chrono::steady_clock::now();
	std::chrono::steady_clock::time_point LastBatch = Last;
    std::chrono::steady_clock::time_point LastDiagnostic = Last;
    double InputSeconds = 0, AppliedSeconds = 0;
};

struct AudioSpectralEditor::PaintReceipt
{
	std::atomic<bool> Complete{false};
	std::atomic<bool> Cancelled{false};
	std::shared_ptr<const audio::AudioSpectralGainField> Gains;
	std::shared_ptr<const audio::AudioSpectralEnvelope> Envelope;
	bool Changed = false;
    double ExposureSeconds = 0;
    std::unique_ptr<audio::AudioSpectralGainField::ExposureMap> InputMap; // Frozen for worker/UI readers until terminal release/acquire.
	std::string Reason;
};

class AudioSpectralEditor::EditUndo final : public base::ActionUndo
{
public:
	EditUndo(std::weak_ptr<engine::Loop> loop, std::shared_ptr<audio::AudioSpectralWorker> worker,
		std::shared_ptr<const audio::AudioSpectralEditState> before,
		std::shared_ptr<const audio::AudioSpectralEditState> after, std::shared_ptr<RevisionGate> gate)
		: ActionUndo({}), _loop(std::move(loop)), _worker(std::move(worker)),
		_before(std::move(before)), _after(std::move(after)), _gate(std::move(gate)) {}
	bool Undo() override { return Apply(_after, _before); }
	bool Redo() override { return Apply(_before, _after); }
	bool ValidSource() const
	{
		const auto loop = _loop.lock();
		return loop && loop->IsCurrentAudioSpectralSource(*_before->Source);
	}
private:
	bool Apply(const std::shared_ptr<const audio::AudioSpectralEditState>& expected,
		const std::shared_ptr<const audio::AudioSpectralEditState>& target)
	{
		const auto loop = _loop.lock();
		if (!loop || _gate->StrokeActive.load() || !loop->IsCurrentAudioSpectralSource(*target->Source)) return false;
		const auto current = loop->AudioSpectralEdit();
		if (!current || current->Gains != expected->Gains || current->Envelope != expected->Envelope) return false;
		std::string reason;
		auto next = audio::AudioSpectralEditState::Create(target->Source, target->Transform, target->Gains, target->Envelope,
			current->Revision + 1, current->SpectralRevision + (current->Gains != target->Gains),
			current->EnvelopeRevision + (current->Envelope != target->Envelope), reason);
		if (!next) return false;
		_gate->WorkStop.request_stop();
		_gate->WorkStop = std::stop_source{};
		loop->InvalidateAudioSpectralResultsBefore(next->SpectralRevision, next->Source->Generation);
		if (!loop->PublishAudioSpectralEdit(next, reason)) return false;
		_gate->SpectralFloor.store(next->SpectralRevision);
		_QueuePlayback(_worker, loop, next, nullptr, _gate);
		return true;
	}
	std::weak_ptr<engine::Loop> _loop;
	std::shared_ptr<audio::AudioSpectralWorker> _worker;
	std::shared_ptr<const audio::AudioSpectralEditState> _before, _after;
	std::shared_ptr<RevisionGate> _gate;
};

void AudioSpectralEditor::_QueuePlayback(const std::shared_ptr<audio::AudioSpectralWorker>& worker,
	std::weak_ptr<engine::Loop> weakLoop, std::shared_ptr<const audio::AudioSpectralEditState> edit,
	std::shared_ptr<const audio::AudioSpectralAnalysis> analysis, std::shared_ptr<RevisionGate> gate)
{
	const auto owner = weakLoop.lock();
	if (!owner || !edit) return;
	try
	{
	worker->RequestContent(owner.get(), [weakLoop, edit, analysis = std::move(analysis), gate, stop = gate->WorkStop.get_token()]() mutable
	{
		try
		{
			const auto loop = weakLoop.lock();
			if (!loop || stop.stop_requested() || !loop->IsCurrentAudioSpectralSource(*edit->Source)) return;
			std::string reason;
			gate->PlaybackFailed.store(false);
			const auto audible = loop->AudioSpectralPlayback();
			std::shared_ptr<const audio::AudioSpectralPcm> pcm;
			if (audible && audible->SpectralRevision == edit->SpectralRevision) pcm = audible->Pcm;
			else if (edit->Gains->IsUnity()) pcm = std::make_shared<const audio::AudioSpectralPcm>(audio::AudioSpectralPcm{edit->Source, nullptr});
			else
			{
				if (!analysis) analysis = audio::AudioSpectralDsp::Analyze(edit->Source, edit->Transform, reason, stop);
				if (!analysis) { if (!stop.stop_requested()) gate->PlaybackFailed.store(true); return; }
				pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *edit->Gains, reason, stop);
			}
			if (stop.stop_requested()) return;
			if (!pcm) { gate->PlaybackFailed.store(true); return; }
			if (edit->SpectralRevision < gate->SpectralFloor.load()) return;
			// Spectral receipts never carry an envelope. Read the latest accepted
			// layer after synthesis, and retry if UI acceptance races validation.
			for (unsigned attempt = 0; attempt != 3; ++attempt)
			{
				const auto latest = loop->AudioSpectralEdit();
				const auto prior = loop->AudioSpectralPlayback();
				if (!latest || !loop->IsCurrentAudioSpectralSource(*edit->Source)
					|| edit->SpectralRevision < gate->SpectralFloor.load()
					|| (prior && edit->SpectralRevision < prior->SpectralRevision)) return;
				auto snapshot = audio::AudioSpectralPlaybackSnapshot::Create(pcm, latest->Envelope,
					prior ? prior->Revision + 1 : 1, edit->SpectralRevision, latest->EnvelopeRevision, reason, stop);
				if (!snapshot) { if (!stop.stop_requested()) gate->PlaybackFailed.store(true); return; }
				if (loop->AudioSpectralEdit() != latest) continue;
				if (edit->SpectralRevision < gate->SpectralFloor.load()) return;
				if (loop->PublishAudioSpectralPlayback(snapshot, reason)) return;
			}
		}
		catch (const std::bad_alloc&) { gate->PlaybackFailed.store(true); }
	});
	}
	catch (const std::bad_alloc&) { gate->PlaybackFailed.store(true); }
}

AudioSpectralEditor::~AudioSpectralEditor()
{
	EndStroke(true);
	Close();
}

void AudioSpectralEditor::Open(const std::shared_ptr<engine::Loop>& loop)
{
	if (_stroke)
	{
		EndStroke(false);
		_closing = true;
		_pendingOpen = loop;
		return;
	}
	Close();
	_closing = false;
	_loop = loop;
	_editFailure.clear();
	if (loop)
		if (const auto edit = loop->AudioSpectralEdit())
		{
			_source = edit->Source;
			_budgetLayout = edit->Source->Layout;
		}
	if (loop)
	{
		gui::GuiModelParams params;
		params.ModelScale = 1.0f;
		params.ModelTextures = { "levels" };
		_model = std::make_shared<graphics::AudioSpectralModel>(params);
		loop->SetAudioSpectralModel(_model);
	}
	_state = State::WaitingForSource;
	Update();
}

void AudioSpectralEditor::Close()
{
	_pendingOpen.reset();
	if (_stroke)
	{
		EndStroke(false);
		_closing = true;
		return;
	}
	_closing = false;
	for (const auto& job : _displayJobs) job->Cancel();
	_displayJobs.clear();
	_requestedDisplayEdit.reset();
	_viewport = _requestedViewport = {};
	_presentationRevision = 0u;
	_model.reset();
	_surfacePrepared = false;
	_displayFailure.clear();
	if (_capture) _capture->Cancel();
	if (_analysisJob) _analysisJob->Cancel();
	_analysisJob.reset();
	_analysis.reset();
	_initialEdit.reset();
	_initialPlayback.reset();
	_budgetLayout.reset();
	_lastVisualPlayback.reset();
	_peakOverload = false;
	_gate.reset(); // Closed content/history retain their own gate; another Loop must not cancel it.
	if (const auto loop = _loop.lock())
		loop->CancelAudioSpectralSourceCapture();
	_capture.reset();
	_source.reset();
	_loop.reset();
	_state = State::Closed;
	_feedback.clear();
	_unavailableGeneration = 0u;
}

void AudioSpectralEditor::Update()
{
	for (auto record = _historyRecords.begin(); record != _historyRecords.end();)
	{
		const auto action = record->Action.lock();
		if (!action || !std::static_pointer_cast<EditUndo>(action)->ValidSource())
		{
			if (action && _history) _history->Remove(action);
			record = _historyRecords.erase(record);
		}
		else ++record;
	}
	std::erase_if(_gates, [](const auto& entry) { return entry.second.expired(); });
	_AcceptPaint();
	_LogStroke();
	_UpdateStroke();
	if (_closing && !_stroke)
	{
		auto pending = std::exchange(_pendingOpen, nullptr);
		Close();
		if (pending) Open(pending);
		return;
	}
	const auto loop = _loop.lock();
	if (!loop)
	{
		Close();
		return;
	}
	if (_gate && _gate->Generation != loop->AudioSourceGeneration()) _gate->WorkStop.request_stop();
	if (!loop->CanEditAudioSource())
	{
		if (_gate) _gate->WorkStop.request_stop();
		EndStroke(true);
		if (_capture) loop->CancelAudioSpectralSourceCapture();
		if (_analysisJob) _analysisJob->Cancel();
		for (const auto& job : _displayJobs) job->Cancel();
		_displayJobs.clear();
		_requestedDisplayEdit.reset();
		_analysisJob.reset();
		_capture.reset();
		_state = State::WaitingForSource;
		_feedback = "Waiting for recording or punch to finish";
		return;
	}
	if (_analysisJob && !loop->IsCurrentAudioSpectralSource(*_analysisJob->Source))
	{
		_analysisJob->Cancel();
		_analysisJob.reset();
	}
	if (_analysisJob)
	{
		const auto status = _analysisJob->Status();
		if (status == audio::AudioSpectralAnalysisJob::State::Pending) return;
		if (status == audio::AudioSpectralAnalysisJob::State::Complete)
		{
			_analysis = _analysisJob->Result();
			_budgetLayout = _analysis->Source->Layout;
			// Unedited originals are retained only by the open editor. Loop begins
			// retaining layers/PCM when an actual edit is committed, avoiding source
			// copies accumulating merely by opening and closing different Loops.
			_initialEdit = _analysisJob->EditState();
			_initialPlayback = _analysisJob->Playback();
		}
		else if (status == audio::AudioSpectralAnalysisJob::State::Failed)
		{
			_feedback = _analysisJob->Reason();
			_unavailableGeneration = loop->AudioSourceGeneration();
			_state = State::Unavailable;
		}
		_analysisJob.reset();
	}
	if (_analysis && loop->IsCurrentAudioSpectralSource(*_analysis->Source))
	{
		const auto playback = loop->AudioSpectralPlayback();
		if (playback != _lastVisualPlayback)
		{
			_lastVisualPlayback = playback;
			_peakOverload = false;
			if (playback)
			{
				loop->RefreshVisualModel();
				for (const auto& range : playback->Overview())
					_peakOverload |= std::abs(range.Minimum) > 1.0f || std::abs(range.Maximum) > 1.0f;
			}
		}
		_state = State::Ready;
		_feedback = _analysis->Transform.HopSamps != audio::AudioSpectralConfig::HopSamps
            ? "Audio analysis ready (" + std::to_string(_analysis->Transform.HopSamps) + "-sample time steps)" : "Audio analysis ready";
		_UpdateDisplay();
		if (_peakOverload) _feedback = "Edited peaks exceed full scale";
		if (_stroke)
        {
            _feedback = "Painting; audio/display updates are queued";
            if (const auto level = BrushLevelDb())
                _feedback = !std::isfinite(*level) ? "Spectral silence; audio/display updates queued"
                    : *level >= -0.001 ? "Spectral ceiling (0 dB); audio/display updates queued"
                    : "Spectral level " + std::to_string(static_cast<int>(std::lround(*level))) + " dB; audio/display updates queued";
        }
		else if (const auto edit = EditState(); edit && (!playback || playback->SpectralRevision < edit->SpectralRevision || playback->EnvelopeRevision < edit->EnvelopeRevision))
			_feedback = "Preparing edited playback";
		if (_gate && _gate->PlaybackFailed.load()) _feedback = "Edited playback preparation failed; committed edits are retained";
		if (!_editFailure.empty()) _feedback = _editFailure;
		return;
	}
	if (_state == State::Unavailable && _unavailableGeneration == loop->AudioSourceGeneration())
		return;
	if (_source && loop->IsCurrentAudioSpectralSource(*_source))
	{
		const auto edit = loop->AudioSpectralEdit();
		_analysisJob = std::make_shared<audio::AudioSpectralAnalysisJob>(
			_source, edit ? edit->Transform : _source->SelectedTransform);
		_worker->RequestAnalysis(_analysisJob);
		_state = State::AnalyzingSource;
		_feedback = "Analyzing audio source";
		return;
	}
	if (_capture)
	{
		if (_capture->Status() == audio::AudioSpectralSourceCapture::State::Preparing
			|| _capture->Status() == audio::AudioSpectralSourceCapture::State::Pending)
			return;
		const auto failed = _capture->Status() == audio::AudioSpectralSourceCapture::State::Failed;
		const auto captured = _capture->Source();
		loop->CancelAudioSpectralSourceCapture();
		_capture.reset();
		if (failed)
		{
			_unavailableGeneration = loop->AudioSourceGeneration();
			_state = State::Unavailable;
			_feedback = "Not enough memory to prepare the audio source";
			return;
		}
		if (captured && loop->IsCurrentAudioSpectralSource(*captured))
		{
			_source = captured;
			_state = State::AnalyzingSource;
			_feedback = "Audio source captured";
			return;
		}
	}
	if (_state == State::Unavailable && _unavailableGeneration == loop->AudioSourceGeneration())
		return;
	_capture = loop->RequestAudioSpectralSource(_feedback, _resourceBudget,
        _budgetLayout ? &*_budgetLayout : nullptr);
	if (!_capture)
	{
		_unavailableGeneration = loop->AudioSourceGeneration();
		_state = State::Unavailable;
		return;
	}
	// Reserve for the largest retained dimensions until the new worker result
	// replaces the previous analysis; repeated punch/retry cannot bypass the cap.
	auto budgetLayout = _capture->Layout();
	if (_budgetLayout)
	{
		budgetLayout.LogicalLength = (std::max)(budgetLayout.LogicalLength, _budgetLayout->LogicalLength);
		budgetLayout.PhysicalLength = (std::max)(budgetLayout.PhysicalLength, _budgetLayout->PhysicalLength);
		budgetLayout.FadeSamps = (std::max)(budgetLayout.FadeSamps, _budgetLayout->FadeSamps);
	}
	if (!audio::AudioSpectralDsp::EstimateWorkingMemory(
		budgetLayout, _capture->SelectedTransform(), _feedback))
	{
		loop->CancelAudioSpectralSourceCapture();
		_capture.reset();
		_unavailableGeneration = loop->AudioSourceGeneration();
		_state = State::Unavailable;
		return;
	}
	_budgetLayout = budgetLayout;
		_worker->PrepareSource(_capture);
	_state = State::AcquiringSource;
	_feedback = "Preparing audio source";
}

std::shared_ptr<const audio::AudioSpectralEditState> AudioSpectralEditor::EditState() const
{
	if (const auto loop = _loop.lock())
		if (const auto state = loop->AudioSpectralEdit()) return state;
	return _initialEdit;
}

std::shared_ptr<const audio::AudioSpectralPlaybackSnapshot> AudioSpectralEditor::Playback() const
{
	if (const auto loop = _loop.lock())
		if (const auto snapshot = loop->AudioSpectralPlayback()) return snapshot;
	return _initialPlayback;
}

void AudioSpectralEditor::_UpdateDisplay()
{
	if (!_model || !_analysis) return;
	const auto loop = _loop.lock();
	if (!loop) return;
	for (auto job = _displayJobs.begin(); job != _displayJobs.end();)
	{
		const auto status = (*job)->Status();
		if (status == AudioSpectralDisplayJob::State::Pending) { ++job; continue; }
		if (status == AudioSpectralDisplayJob::State::Complete
			&& (*job)->Viewport == _viewport
			&& loop->IsCurrentAudioSpectralSource(*(*job)->Analysis->Source))
		{
			if (_model->SetDisplay((*job)->Result(), _feedback)) { _surfacePrepared = true; _displayFailure.clear(); }
			else _displayFailure = _feedback;
		}
		else if (status == AudioSpectralDisplayJob::State::Failed && (*job)->Viewport == _viewport)
			_displayFailure = (*job)->Reason();
		job = _displayJobs.erase(job);
	}
	const auto edit = EditState();
	if (edit && (edit != _requestedDisplayEdit || _viewport != _requestedViewport))
	{
		_displayFailure.clear();
		auto job = std::make_shared<AudioSpectralDisplayJob>(_analysis, edit,
			_model->TimeSegments(), _model->FrequencySegments(), _viewport, ++_presentationRevision);
		_worker->RequestPresentation([job] { job->Run(); }, [job] { job->Cancel(); });
		// Worker cancellation marks superseded queued work terminal. Retain every
		// running receipt for explicit source-change/close cancellation, bounded
		// to current/queued plus a just-completed receipt at a worker/UI race.
		_displayJobs.erase(std::remove_if(_displayJobs.begin(), _displayJobs.end(),
			[](const auto& receipt) { return receipt->Status() == AudioSpectralDisplayJob::State::Cancelled; }),
			_displayJobs.end());
		_displayJobs.push_back(std::move(job));
		_requestedDisplayEdit = edit;
		_requestedViewport = _viewport;
	}
	if (!_surfacePrepared) _feedback = "Preparing spectral surface";
	if (!_displayFailure.empty()) _feedback = _displayFailure;
	const auto reason = _model->UnavailableReason();
	if (!reason.empty()) _feedback = reason;
}

void AudioSpectralEditor::SetFrequencyViewport(graphics::AudioSpectralViewport viewport)
{
	if (_stroke || !viewport.IsValid() || viewport == _viewport) return;
	_viewport = viewport;
	for (const auto& job : _displayJobs) job->Cancel();
	_UpdateDisplay();
}

bool AudioSpectralEditor::BeginStroke(bool broadband, bool attenuate, double seconds, double hz, float logicalY,
	double timeRadius, double frequencyRadius, double bandRadius)
{
	const auto loop = _loop.lock();
	const auto before = EditState();
	if (_stroke || _closing || _state != State::Ready || !loop || !before || !_analysis
		|| !loop->IsCurrentAudioSpectralSource(*before->Source)
		|| !std::isfinite(seconds) || !std::isfinite(hz) || hz < 0 || !std::isfinite(logicalY)
		|| !std::isfinite(timeRadius) || timeRadius <= 0.0 || timeRadius > 2.0
		|| !std::isfinite(frequencyRadius) || frequencyRadius <= 0.0 || frequencyRadius > 4.0
		|| !std::isfinite(bandRadius) || bandRadius <= 0.0 || bandRadius > 2.0) return false;
	hz = (std::max)(hz, audio::AudioSpectralConfig::DisplayMinimumFrequencyHz);
	const auto tiles = ((before->Gains->FrameCount() + 31) / 32) * ((before->Gains->BinCount() + 31) / 32);
	const std::uint64_t historyBytes = broadband ? static_cast<std::uint64_t>(before->Source->Layout.LogicalLength) * sizeof(float) * 2
		: tiles * sizeof(audio::AudioSpectralGainField::Tile) * 2;
	if (historyBytes > audio::AudioSpectralConfig::MaxHistoryBytes)
	{
		_feedback = "This stroke cannot fit the spectral Undo memory budget";
		_editFailure = _feedback;
		return false;
	}
	try
	{
		_gate = _gates[loop.get()].lock();
		if (!_gate || _gate->WorkStop.stop_requested() || _gate->Generation != before->Source->Generation || _gate->Identity != before->Source->Identity)
		{
			_gate = std::make_shared<RevisionGate>();
			_gate->Generation = before->Source->Generation;
			_gate->Identity = before->Source->Identity;
			_gates[loop.get()] = _gate;
		}
		auto stroke = std::make_shared<Stroke>();
		stroke->Before = stroke->Latest = before;
		stroke->PublishedEnvelope = before->Envelope;
		stroke->Analysis = _analysis;
		stroke->AnchorY = logicalY;
		stroke->AnchorSeconds = seconds;
		stroke->TimeRadius = timeRadius;
		stroke->FrequencyRadius = frequencyRadius;
		stroke->BandRadius = bandRadius;
		stroke->Broadband = broadband;
		stroke->Attenuate = attenuate;
		stroke->Point = std::pair{seconds, hz};
        if (!broadband)
        {
            stroke->InputMap = audio::AudioSpectralGainField::ExposureMap::Create(_analysis->Geometry.FrameCount, _analysis->Geometry.BinCount, _feedback);
            if (!stroke->InputMap) { _editFailure = _feedback; return false; }
            stroke->SpareMap = audio::AudioSpectralGainField::ExposureMap::Create(_analysis->Geometry.FrameCount, _analysis->Geometry.BinCount, _feedback);
            if (!stroke->InputMap || !stroke->SpareMap) { _editFailure = _feedback; return false; }
        }
		_stroke = std::move(stroke);
		_editFailure.clear();
		_gate->StrokeActive.store(true);
		_UpdateStroke();
		return true;
	}
	catch (const std::bad_alloc&) { _feedback = _editFailure = "Not enough memory to begin a stroke"; return false; }
}

void AudioSpectralEditor::MoveStroke(std::optional<std::pair<double, double>> point, float logicalY)
{
    if (!_stroke || _stroke->Released) return;
    auto& stroke = *_stroke;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(now - stroke.Last).count();
    stroke.Last = now;
    const auto target = audio::AudioSpectralEnvelope::TargetFromDragPixels(logicalY - stroke.AnchorY);
    if (stroke.Broadband)
    {
        if (std::isfinite(logicalY) && stroke.Target != target)
        {
            stroke.PendingTarget = target;
            stroke.Target = target;
        }
        _UpdateStroke();
        return; // Fixed first-click source time; later X and surface misses do not move it.
    }
    if (point && (!std::isfinite(point->first) || !std::isfinite(point->second) || point->second < 0)) point.reset();
    if (point) point->second = (std::max)(point->second, audio::AudioSpectralConfig::DisplayMinimumFrequencyHz);
    if (point)
    {
        const auto from = stroke.Point.value_or(*point);
        const auto exposure = stroke.Point ? elapsed : 0.0;
        if (!stroke.Broadband)
        {
            std::string reason;
            stroke.InputMap->Accumulate(*stroke.Analysis, from.first, from.second, point->first,
                point->second, exposure, reason, stroke.TimeRadius, stroke.FrequencyRadius);
            if (!reason.empty()) { _feedback = _editFailure = reason; EndStroke(true); return; }
            stroke.InputSeconds += exposure;
        }
    }
    // An outside miss breaks the path; reentry never bridges an unpainted gap.
    stroke.Point = point;
    stroke.Target = target;
    _UpdateStroke();
}
void AudioSpectralEditor::EndStroke(bool cancel)
{
	if (!_stroke) return;
	const auto loop = _loop.lock();
	if (cancel)
	{
		_gate->WorkStop.request_stop();
		_gate->WorkStop = std::stop_source{};
		if (_paintReceipt) _paintReceipt->Cancelled.store(true);
        _worker->CancelPendingEditing();
		_LogStroke("cancel");
        const auto before = _stroke->Before;
		const auto current = loop ? loop->AudioSpectralEdit() : nullptr;
		if (loop && current && loop->IsCurrentAudioSpectralSource(*before->Source)
			&& (current->Gains != before->Gains || current->Envelope != before->Envelope))
		{
			auto restored = audio::AudioSpectralEditState::Create(before->Source, before->Transform,
				before->Gains, before->Envelope, current->Revision + 1,
				current->SpectralRevision + (current->Gains != before->Gains),
				current->EnvelopeRevision + (current->Envelope != before->Envelope), _feedback);
			if (restored) loop->InvalidateAudioSpectralResultsBefore(restored->SpectralRevision, restored->Source->Generation);
			if (restored && loop->PublishAudioSpectralEdit(restored, _feedback))
			{
				_gate->SpectralFloor.store(restored->SpectralRevision);
				_QueuePlayback(_worker, loop, restored, _analysis, _gate);
			}
		}
		else if (loop && current) _QueuePlayback(_worker, loop, current, _analysis, _gate);
		_gate->StrokeActive.store(false);
        _stroke->InputMap.reset(); _stroke->SpareMap.reset(); // Drop UI scratch even if the worker still pins Stroke.
		_stroke.reset();
		_paintReceipt.reset();
		return;
	}
	if (!_stroke->Released)
	{
		if (!_stroke->Broadband) MoveStroke(_stroke->Point, _stroke->AnchorY);
		_stroke->Released = true;
	}
	_UpdateStroke();
}

void AudioSpectralEditor::_UpdateStroke()
{
	if (!_stroke) return;
	const auto loop = _loop.lock();
	if (!loop || !loop->IsCurrentAudioSpectralSource(*_stroke->Before->Source)) { EndStroke(true); return; }
	if (_paintReceipt) return;
	if (_stroke->Broadband ? !_stroke->PendingTarget : _stroke->InputMap->Empty())
	{
		if (_stroke->Released) _FinishStroke();
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	if (!_stroke->Released
		&& std::chrono::duration<double>(now - _stroke->LastBatch).count() < audio::AudioSpectralConfig::StrokeUpdateSeconds) return;
	_stroke->LastBatch = now;
	const auto stroke = _stroke;
	auto receipt = std::make_shared<PaintReceipt>();
    std::optional<float> target;
    if (stroke->Broadband)
    {
        target = std::exchange(stroke->PendingTarget, std::nullopt);
    }
    else
    {
        receipt->InputMap = std::move(stroke->InputMap);
        stroke->InputMap = std::move(stroke->SpareMap);
    }
    _paintReceipt = receipt;
	_worker->RequestEditing([stroke, receipt, target]
	{
		try
		{
			if (receipt->InputMap) receipt->ExposureSeconds = receipt->InputMap->ElapsedSeconds();
            if (!receipt->Cancelled.load())
			{
				if (stroke->Broadband)
				{
					if (!stroke->Envelope)
					{
						auto envelope = stroke->Before->Envelope;
						if (!envelope) envelope = audio::AudioSpectralEnvelope::Unity(stroke->Before->Source->Layout.LogicalLength, receipt->Reason);
						if (envelope) stroke->Envelope = audio::AudioSpectralEnvelopeStroke::Begin(envelope,
							stroke->Before->Source->Layout.SampleRate, receipt->Reason, stroke->BandRadius);
					}
					if (stroke->Envelope)
					{
						const auto changed = target && stroke->Envelope->SetRelativeBand(
							stroke->AnchorSeconds * stroke->Before->Source->Layout.SampleRate, *target);
						if (changed)
						{
							auto snapshot = stroke->Envelope->Snapshot(receipt->Reason);
							const auto same = [](const auto& lhs, const auto& rhs)
							{
								if (!lhs || !rhs) return (!lhs || lhs->IsUnity()) && (!rhs || rhs->IsUnity());
								return std::equal(lhs->Gains().begin(), lhs->Gains().end(), rhs->Gains().begin());
							};
							if (snapshot && !same(snapshot, stroke->PublishedEnvelope))
							{
								receipt->Envelope = same(snapshot, stroke->Before->Envelope) ? stroke->Before->Envelope : snapshot;
								stroke->PublishedEnvelope = receipt->Envelope;
								receipt->Changed = true;
							}
						}
					}
				}
				else
				{
					if (!stroke->Gains) stroke->Gains = std::make_shared<audio::AudioSpectralGainField>(*stroke->Before->Gains);
					receipt->Changed = stroke->Gains->ApplyExposureMap(*stroke->Analysis, *receipt->InputMap, !stroke->Attenuate, receipt->Reason);
					if (receipt->Changed) receipt->Gains = std::make_shared<const audio::AudioSpectralGainField>(*stroke->Gains);
				}
			}
		}
		catch (const std::bad_alloc&) { receipt->Reason = "Not enough memory for this stroke"; }
				receipt->Complete.store(true, std::memory_order_release);
	}, [receipt] { receipt->Cancelled.store(true); receipt->Complete.store(true, std::memory_order_release); });
}

void AudioSpectralEditor::_AcceptPaint()
{
	if (!_paintReceipt || !_paintReceipt->Complete.load(std::memory_order_acquire)) return;
	auto receipt = std::exchange(_paintReceipt, nullptr);
	if (!_stroke) return;
    if (receipt->Cancelled.load()) { EndStroke(true); return; }
    if (receipt->InputMap)
    {
        receipt->InputMap->Clear(); // Terminal acquire: no worker reads remain.
        _stroke->SpareMap = std::move(receipt->InputMap);
    }
    _stroke->AppliedSeconds += receipt->ExposureSeconds;
	if (!receipt->Reason.empty()) { _feedback = _editFailure = receipt->Reason; EndStroke(true); return; }
	const auto loop = _loop.lock();
	if (!receipt->Changed || !loop) return;
	const auto current = EditState();
	if (!current || !loop->IsCurrentAudioSpectralSource(*_stroke->Before->Source)) { EndStroke(true); return; }
	auto edit = audio::AudioSpectralEditState::Create(current->Source, current->Transform,
		receipt->Gains ? receipt->Gains : current->Gains, _stroke->Broadband ? receipt->Envelope : current->Envelope,
		current->Revision + 1, current->SpectralRevision + !!receipt->Gains,
		current->EnvelopeRevision + _stroke->Broadband, _feedback);
	if (!edit || !loop->PublishAudioSpectralEdit(edit, _feedback)) { EndStroke(true); return; }
	_stroke->Latest = edit;
	_QueuePlayback(_worker, loop, edit, _analysis, _gate);
}

std::optional<double> AudioSpectralEditor::BrushLevelDb() const
{
    if (!_stroke || _stroke->Broadband || !_stroke->Point || !_stroke->Analysis) return std::nullopt;
    const auto edit = EditState();
    if (!edit) return std::nullopt;
    const auto& analysis = *_stroke->Analysis;
    const auto point = *_stroke->Point;
    const auto frame = static_cast<std::size_t>(std::clamp(std::round(
        (point.first * edit->Source->Layout.SampleRate - analysis.Geometry.FirstFrameCentre) / analysis.Transform.HopSamps),
        0.0, static_cast<double>(analysis.Geometry.FrameCount - 1u)));
    const auto bin = point.second <= audio::AudioSpectralConfig::DisplayMinimumFrequencyHz ? 0u
        : static_cast<unsigned int>(std::clamp(std::round(point.second * analysis.Transform.WindowSamps
            / edit->Source->Layout.SampleRate), 0.0, static_cast<double>(analysis.Geometry.BinCount - 1u)));
    const auto pending = _stroke->InputMap->ExposureAt(frame, bin)
        + (_paintReceipt && _paintReceipt->InputMap ? _paintReceipt->InputMap->ExposureAt(frame, bin) : 0.0);
    const auto gainDb = audio::AudioSpectralGainField::GainDbAfterExposure(analysis, frame, bin,
        edit->Gains->GainDb(frame, bin), pending, !_stroke->Attenuate);
    const auto gain = gainDb == audio::AudioSpectralConfig::SilenceGainDb ? 0.0 : std::pow(10.0, gainDb / 20.0);
    const auto magnitude = analysis.Magnitude(frame, bin) * gain;
    return magnitude > 0.0 ? 20.0 * std::log10(magnitude) : -std::numeric_limits<double>::infinity();
}
void AudioSpectralEditor::_LogStroke(const char* event)
{
    if (!_stroke || _stroke->Broadband) return;
    const auto now = std::chrono::steady_clock::now();
    if (std::string_view(event) == "tick"
        && std::chrono::duration<double>(now - _stroke->LastDiagnostic).count() < 1.0) return;
    _stroke->LastDiagnostic = now;
    const auto edit = EditState();
    const auto analysis = _stroke->Analysis;
    if (!edit || !analysis) return;
    const auto point = _stroke->Point.value_or(std::pair{0.0, 20.0});
    const auto sample = point.first * edit->Source->Layout.SampleRate;
    const auto frame = static_cast<std::size_t>(std::clamp(std::round(
        (sample - analysis->Geometry.FirstFrameCentre) / analysis->Transform.HopSamps),
        0.0, static_cast<double>(analysis->Geometry.FrameCount - 1u)));
    const auto bin = point.second <= audio::AudioSpectralConfig::DisplayMinimumFrequencyHz ? 0u
        : static_cast<unsigned int>(std::clamp(std::round(point.second * analysis->Transform.WindowSamps
            / edit->Source->Layout.SampleRate), 0.0, static_cast<double>(analysis->Geometry.BinCount - 1u)));
    const auto gain = edit->Gains->GainDb(frame, bin);
    const auto magnitude = analysis->Magnitude(frame, bin);
    const auto absolute = magnitude * edit->Gains->GainLinear(frame, bin);
    const auto envelope = edit->Envelope ? edit->Envelope->GainAt(static_cast<unsigned long>(std::clamp(sample,
        0.0, static_cast<double>(edit->Source->Layout.LogicalLength - 1u)))) : 1.0f;
    const auto playback = Playback();
    const auto surface = _model ? _model->Displayed() : nullptr;
    const auto preview = BrushLevelDb();
    std::cout << "[SpectralStroke] event=" << event << " generation=" << edit->Source->Generation
        << " length_s=" << edit->Source->Layout.LogicalLength / edit->Source->Layout.SampleRate
        << " rate_db_s=" << audio::AudioSpectralConfig::BrushDbPerSecond
        << " policy=exposure-map-v1"
        << " mode=" << (_stroke->Attenuate ? "attenuate" : "boost")
        << " t=" << point.first << " hz=" << point.second << " frame=" << frame << " bin=" << bin
        << " input_s=" << _stroke->InputSeconds << " applied_s=" << _stroke->AppliedSeconds
        << " pending=" << (_stroke->InputMap ? _stroke->InputMap->DirtyCellCount() : static_cast<std::size_t>(_stroke->PendingTarget.has_value())) << " busy=" << static_cast<bool>(_paintReceipt)
        << " gain_db=" << gain << " level_db=" << (absolute > 0 ? 20.0 * std::log10(absolute) : -999.0)
        << " limit=" << (gain == audio::AudioSpectralConfig::SilenceGainDb || absolute >= 0.99999)
        << " preview_db=" << (preview && std::isfinite(*preview) ? *preview : -999.0)
        << " original_db=" << (magnitude > 0 ? 20.0 * std::log10(magnitude) : -999.0)
        << " envelope=" << envelope << " composed_db=" << (absolute * envelope > 0 ? 20.0 * std::log10(absolute * envelope) : -999.0)
        << " edit_spec=" << edit->SpectralRevision << " edit_env=" << edit->EnvelopeRevision
        << " audio_spec=" << (playback ? playback->SpectralRevision : 0)
        << " audio_env=" << (playback ? playback->EnvelopeRevision : 0)
        << " display_edit=" << (surface ? surface->Revision : 0)
        << " paused=" << !_stroke->Point.has_value() << std::endl;
}
void AudioSpectralEditor::_FinishStroke()
{
	if (!_stroke) return;
	_LogStroke("finish");
    const auto before = _stroke->Before, after = _stroke->Latest;
	_gate->StrokeActive.store(false);
	if (_history && (before->Gains != after->Gains || before->Envelope != after->Envelope))
	{
		// Conservative complete-value accounting: COW sharing may use less. Trim
		// oldest spectral history only; other application Undo entries are retained.
		const auto tiles = ((after->Gains->FrameCount() + 31) / 32) * ((after->Gains->BinCount() + 31) / 32);
		const std::uint64_t bytes = before->Gains != after->Gains ? tiles * sizeof(audio::AudioSpectralGainField::Tile) * 2
			: static_cast<std::uint64_t>(after->Source->Layout.LogicalLength) * sizeof(float) * 2;
		std::uint64_t used = 0;
		std::erase_if(_historyRecords, [](const auto& record) { return record.Action.expired(); });
		for (const auto& record : _historyRecords) used += record.Bytes;
		while (!_historyRecords.empty() && used + bytes > audio::AudioSpectralConfig::MaxHistoryBytes)
		{
			const auto oldest = _historyRecords.front();
			if (auto action = oldest.Action.lock()) _history->Remove(action);
			used -= oldest.Bytes;
			_historyRecords.pop_front();
		}
		if (bytes <= audio::AudioSpectralConfig::MaxHistoryBytes)
		{
			auto undo = std::make_shared<EditUndo>(_loop, _worker, before, after, _gate);
			_history->Add(undo);
			_historyRecords.push_back({undo, bytes});
		}
		else _feedback = "Stroke exceeds the spectral Undo memory limit";
	}
    _stroke->InputMap.reset(); _stroke->SpareMap.reset(); // Worker closures must not retain completed UI scratch.
	_stroke.reset();
}
