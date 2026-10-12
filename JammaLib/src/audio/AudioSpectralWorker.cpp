#include "AudioSpectralWorker.h"
#include <utility>

using namespace audio;

AudioSpectralWorker::AudioSpectralWorker() :
	_thread([this] { _Run(); })
{
}

AudioSpectralWorker::~AudioSpectralWorker()
{
	std::shared_ptr<AudioSpectralSourceCapture> queuedSource;
	std::shared_ptr<AudioSpectralSourceCapture> currentSource;
	std::shared_ptr<AudioSpectralAnalysisJob> queuedAnalysis;
	std::shared_ptr<AudioSpectralAnalysisJob> currentAnalysis;
	std::shared_ptr<AudioSpectralReconstructionJob> queuedReconstruction;
	std::shared_ptr<AudioSpectralReconstructionJob> currentReconstruction;
	std::shared_ptr<PresentationWork> queuedPresentation;
	std::shared_ptr<PresentationWork> currentPresentation;
	std::shared_ptr<PresentationWork> editing;
	std::shared_ptr<PresentationWork> currentEditing;
	std::deque<std::shared_ptr<ContentWork>> content;
	{
		std::scoped_lock lock(_requestMutex);
		_stopping = true;
		queuedSource = std::exchange(_queuedSource, nullptr);
		queuedAnalysis = std::exchange(_queuedAnalysis, nullptr);
		currentSource = _currentSource;
		currentAnalysis = _currentAnalysis;
		queuedReconstruction = std::exchange(_queuedReconstruction, nullptr);
		currentReconstruction = _currentReconstruction;
		queuedPresentation = std::exchange(_queuedPresentation, nullptr);
		currentPresentation = _currentPresentation;
		editing = std::exchange(_queuedEditing, nullptr);
		currentEditing = _currentEditing;
		content.swap(_queuedContent);
	}
	if (queuedSource) queuedSource->Cancel();
	if (currentSource) currentSource->Cancel();
	if (queuedAnalysis) queuedAnalysis->Cancel();
	if (currentAnalysis) currentAnalysis->Cancel();
	if (queuedReconstruction) queuedReconstruction->Cancel();
	if (currentReconstruction) currentReconstruction->Cancel();
	if (queuedPresentation && queuedPresentation->Cancel) queuedPresentation->Cancel();
	if (currentPresentation && currentPresentation->Cancel) currentPresentation->Cancel();
	if (editing && editing->Cancel) editing->Cancel();
	if (currentEditing && currentEditing->Cancel) currentEditing->Cancel();
	_wake.notify_one();
	_thread.join();
}

void AudioSpectralWorker::PrepareSource(std::shared_ptr<AudioSpectralSourceCapture> request)
{
	if (!request) return;
	std::shared_ptr<AudioSpectralSourceCapture> displaced;
	bool stopping;
	{
		std::scoped_lock lock(_requestMutex);
		stopping = _stopping;
		if (!stopping) displaced = std::exchange(_queuedSource, request);
	}
	if (displaced && displaced != request) displaced->Cancel();
	if (stopping) request->Cancel();
	else _wake.notify_one();
	// Last-owner release stays outside the queue mutex.
}

void AudioSpectralWorker::RequestAnalysis(std::shared_ptr<AudioSpectralAnalysisJob> job)
{
	if (!job) return;
	std::shared_ptr<AudioSpectralAnalysisJob> displaced;
	bool stopping;
	{
		std::scoped_lock lock(_requestMutex);
		stopping = _stopping;
		if (!stopping) displaced = std::exchange(_queuedAnalysis, job);
	}
	if (displaced && displaced != job) displaced->Cancel();
	if (stopping) job->Cancel();
	else _wake.notify_one();
	// New queued work never cancels an almost-finished running analysis. Source
	// invalidation is explicit through the owner's job.Cancel() operation.
}

void AudioSpectralWorker::RequestReconstruction(std::shared_ptr<AudioSpectralReconstructionJob> job)
{
	if (!job) return;
	std::shared_ptr<AudioSpectralReconstructionJob> displaced;
	bool stopping;
	{
		std::scoped_lock lock(_requestMutex);
		stopping = _stopping;
		if (!stopping) displaced = std::exchange(_queuedReconstruction, job);
	}
	if (displaced && displaced != job) displaced->Cancel();
	if (stopping) job->Cancel();
	else _wake.notify_one();
	// Paint updates replace queued revisions, never a useful running revision.
	// The owner explicitly cancels obsolete source work through job.Cancel().
}

void AudioSpectralWorker::RequestPresentation(std::function<void()> work, std::function<void()> cancel)
{
	if (!work) return;
	// Allocate the small opaque holder before locking; queue/current operations
	// then move/copy handles only, never closure storage or large payloads.
	auto request = std::make_shared<PresentationWork>(PresentationWork{ std::move(work), std::move(cancel) });
	std::shared_ptr<PresentationWork> displaced;
	bool stopping;
	{
		std::scoped_lock lock(_requestMutex);
		stopping = _stopping;
		if (!stopping) displaced = std::exchange(_queuedPresentation, request);
	}
	if (displaced && displaced->Cancel) displaced->Cancel();
	if (stopping && request->Cancel) request->Cancel();
	else _wake.notify_one();
	// Last-owner destruction and cooperative cancellation stay outside the mutex.
}

void AudioSpectralWorker::RequestEditing(std::function<void()> work, std::function<void()> cancel)
{
	auto request = std::make_shared<PresentationWork>(PresentationWork{ std::move(work), std::move(cancel) });
	std::shared_ptr<PresentationWork> displaced;
	bool stopping;
	{
		std::scoped_lock lock(_requestMutex);
		stopping = _stopping;
		if (!stopping) displaced = std::exchange(_queuedEditing, request);
	}
	if (displaced && displaced->Cancel) displaced->Cancel();
	if (stopping && request->Cancel) request->Cancel();
	else _wake.notify_one();
}

void AudioSpectralWorker::RequestContent(const void* owner, std::function<void()> work)
{
	auto request = std::make_shared<ContentWork>(ContentWork{ owner, std::move(work) });
	std::shared_ptr<ContentWork> displaced;
	{
		std::scoped_lock lock(_requestMutex);
		if (_stopping) return;
		for (auto& pending : _queuedContent)
			if (pending->Owner == owner) { displaced = std::exchange(pending, request); break; }
		if (!displaced) _queuedContent.push_back(request);
	}
	_wake.notify_one();
}

void AudioSpectralWorker::_Run()
{
	for (;;)
	{
		std::shared_ptr<AudioSpectralSourceCapture> source;
		std::shared_ptr<AudioSpectralAnalysisJob> analysis;
		std::shared_ptr<AudioSpectralReconstructionJob> reconstruction;
		std::shared_ptr<PresentationWork> presentation;
		std::shared_ptr<PresentationWork> editing;
		std::shared_ptr<ContentWork> content;
		{
			std::unique_lock lock(_requestMutex);
			_wake.wait(lock, [this] { return _stopping || _queuedSource || _queuedAnalysis || _queuedPresentation || _queuedReconstruction || _queuedEditing || !_queuedContent.empty(); });
			if (_stopping) return;
			if (_queuedSource)
			{
				source = std::exchange(_queuedSource, nullptr);
				_currentSource = source;
			}
			else if (_queuedAnalysis)
			{
				analysis = std::exchange(_queuedAnalysis, nullptr);
				_currentAnalysis = analysis;
			}
			else if (_queuedEditing && (_preferEditing || (!_queuedPresentation && !_queuedReconstruction && _queuedContent.empty())))
			{
				editing = std::exchange(_queuedEditing, nullptr);
				_currentEditing = editing;
				_preferEditing = false;
			}
			else if (_queuedPresentation && ((!_queuedReconstruction && _queuedContent.empty()) || !_preferReconstruction))
			{
				presentation = std::exchange(_queuedPresentation, nullptr);
				_currentPresentation = presentation;
				_preferReconstruction = true;
				_preferEditing = true;
			}
			else if (!_queuedContent.empty())
			{
				_preferReconstruction = false;
				_preferEditing = true;
				content = std::move(_queuedContent.front());
				_queuedContent.pop_front();
			}
			else
			{
				_preferReconstruction = false;
				_preferEditing = true;
				reconstruction = std::exchange(_queuedReconstruction, nullptr);
				_currentReconstruction = reconstruction;
			}
		}
		if (source) source->Prepare();
		else if (analysis) analysis->Run();
		else if (presentation) presentation->Run();
		else if (editing) editing->Run();
		else if (content) content->Run();
		else reconstruction->Run();
		{
			std::scoped_lock lock(_requestMutex);
			if (source) _currentSource.reset();
			else if (analysis) _currentAnalysis.reset();
			else if (reconstruction) _currentReconstruction.reset();
			else if (editing) _currentEditing.reset();
			else if (presentation) _currentPresentation.reset();
		}
		// Local handles retain the payload across reset; bulk destruction happens
		// here, outside the queue lock, on the worker rather than audio.
	}
}
