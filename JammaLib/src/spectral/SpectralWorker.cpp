#include "SpectralWorker.h"
#include <utility>

using namespace spectral;

SpectralWorker::SpectralWorker() :
	_thread([this] { _Run(); })
{
    try { _visualThread = std::thread([this] { _RunVisual(); }); }
    catch (...)
    {
        { std::scoped_lock lock(_requestMutex); _stopping = true; }
        _wake.notify_all();
        _thread.join();
        throw;
    }
}

SpectralWorker::~SpectralWorker()
{
	std::shared_ptr<SpectralSourceCapture> queuedSource;
	std::shared_ptr<SpectralSourceCapture> currentSource;
	std::shared_ptr<SpectralAnalysisJob> queuedAnalysis;
	std::shared_ptr<SpectralAnalysisJob> currentAnalysis;
	std::shared_ptr<PresentationWork> queuedPresentation;
	std::shared_ptr<PresentationWork> currentPresentation;
	std::shared_ptr<PresentationWork> editing;
	std::deque<std::shared_ptr<ContentWork>> content;
	{
		std::scoped_lock lock(_requestMutex);
		_stopping = true;
		queuedSource = std::exchange(_queuedSource, nullptr);
		queuedAnalysis = std::exchange(_queuedAnalysis, nullptr);
		currentSource = _currentSource;
		currentAnalysis = _currentAnalysis;
		queuedPresentation = std::exchange(_queuedPresentation, nullptr);
		currentPresentation = _currentPresentation;
		editing = std::exchange(_queuedEditing, nullptr);
		content.swap(_queuedContent);
	}
	if (queuedSource) queuedSource->Cancel();
	if (currentSource) currentSource->Cancel();
	if (queuedAnalysis) queuedAnalysis->Cancel();
	if (currentAnalysis) currentAnalysis->Cancel();
	if (queuedPresentation && queuedPresentation->Cancel) queuedPresentation->Cancel();
	if (currentPresentation && currentPresentation->Cancel) currentPresentation->Cancel();
	if (editing && editing->Cancel) editing->Cancel();
    // A running edit retains frozen-map ownership until Run publishes terminal.
    // Teardown joins the lane; only displaced QUEUED edits use Cancel callbacks.
	_wake.notify_all();
	_thread.join();
	_visualThread.join();
}

void SpectralWorker::PrepareSource(std::shared_ptr<SpectralSourceCapture> request)
{
	if (!request) return;
	std::shared_ptr<SpectralSourceCapture> displaced;
	bool stopping;
	{
		std::scoped_lock lock(_requestMutex);
		stopping = _stopping;
		if (!stopping) displaced = std::exchange(_queuedSource, request);
	}
	if (displaced && displaced != request) displaced->Cancel();
	if (stopping) request->Cancel();
	else _wake.notify_all();
	// Last-owner release stays outside the queue mutex.
}

void SpectralWorker::RequestAnalysis(std::shared_ptr<SpectralAnalysisJob> job)
{
	if (!job) return;
	std::shared_ptr<SpectralAnalysisJob> displaced;
	bool stopping;
	{
		std::scoped_lock lock(_requestMutex);
		stopping = _stopping;
		if (!stopping) displaced = std::exchange(_queuedAnalysis, job);
	}
	if (displaced && displaced != job) displaced->Cancel();
	if (stopping) job->Cancel();
	else _wake.notify_all();
	// New queued work never cancels an almost-finished running analysis. Source
	// invalidation is explicit through the owner's job.Cancel() operation.
}

void SpectralWorker::RequestPresentation(std::function<void()> work, std::function<void()> cancel)
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
	else _wake.notify_all();
	// Last-owner destruction and cooperative cancellation stay outside the mutex.
}

void SpectralWorker::RequestEditing(std::function<void()> work, std::function<void()> cancel)
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
	else _wake.notify_all();
}

void SpectralWorker::CancelPendingEditing()
{
    std::shared_ptr<PresentationWork> displaced;
    {
        std::scoped_lock lock(_requestMutex);
        displaced = std::exchange(_queuedEditing, nullptr);
    }
    if (displaced && displaced->Cancel) displaced->Cancel();
    // Release cancelled map storage outside the mutex. Running jobs keep their immutable map.
}
void SpectralWorker::RequestContent(const void* owner, std::function<void()> work)
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
	_wake.notify_all();
}

void SpectralWorker::_Run()
{
	for (;;)
	{
		std::shared_ptr<SpectralSourceCapture> source;
		std::shared_ptr<SpectralAnalysisJob> analysis;
		std::shared_ptr<ContentWork> content;
		{
			std::unique_lock lock(_requestMutex);
			_wake.wait(lock, [this] { return _stopping || _queuedSource || _queuedAnalysis || !_queuedContent.empty(); });
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
			else if (!_queuedContent.empty())
			{
				content = std::move(_queuedContent.front());
				_queuedContent.pop_front();
			}
		}
		if (source) source->Prepare();
		else if (analysis) analysis->Run();
		else if (content) content->Run();
		{
			std::scoped_lock lock(_requestMutex);
			if (source) _currentSource.reset();
			else if (analysis) _currentAnalysis.reset();
		}
		// Local handles retain the payload across reset; bulk destruction happens
		// here, outside the queue lock, on the worker rather than audio.
	}
}

void SpectralWorker::_RunVisual()
{
    for (;;)
    {
        std::shared_ptr<PresentationWork> request;
        bool editing = false;
        {
            std::unique_lock lock(_requestMutex);
            _wake.wait(lock, [this] { return _stopping || _queuedEditing || _queuedPresentation; });
            if (_stopping) return;
            editing = _queuedEditing && (_preferEditing || !_queuedPresentation);
            if (editing) _currentEditing = request = std::exchange(_queuedEditing, nullptr);
            else _currentPresentation = request = std::exchange(_queuedPresentation, nullptr);
            _preferEditing = !editing;
        }
        request->Run();
        {
            std::scoped_lock lock(_requestMutex);
            if (editing) _currentEditing.reset();
            else _currentPresentation.reset();
        }
        // Retained maps/results and closures are destroyed outside the queue lock.
    }
}
