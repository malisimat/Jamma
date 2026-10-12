#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <deque>
#include "AudioSpectralSource.h"
#include "AudioSpectralAnalysisJob.h"
#include "AudioSpectralReconstructionJob.h"

namespace audio
{
	// Dedicated non-audio worker. Never allocate source PCM or perform FFT work
	// on UI/Scene trigger job threads. Normal editor close cancels without joining.
	class AudioSpectralWorker
	{
	public:
		AudioSpectralWorker();
		~AudioSpectralWorker(); // Joins only at editor-owner teardown.
		AudioSpectralWorker(const AudioSpectralWorker&) = delete;
		AudioSpectralWorker& operator=(const AudioSpectralWorker&) = delete;
		void PrepareSource(std::shared_ptr<AudioSpectralSourceCapture> request);
		void RequestAnalysis(std::shared_ptr<AudioSpectralAnalysisJob> job);
		void RequestReconstruction(std::shared_ptr<AudioSpectralReconstructionJob> job);
		// Opaque non-GL CPU work with cooperative bounded cancellation. Both
		// closures run off audio; coalescing/teardown cancel outside the queue lock.
		void RequestPresentation(std::function<void()> work, std::function<void()> cancel);
		void RequestEditing(std::function<void()> work, std::function<void()> cancel);
		void CancelPendingEditing();
        void RequestContent(const void* owner, std::function<void()> work);

	private:
		struct PresentationWork
		{
			std::function<void()> Run;
			std::function<void()> Cancel;
		};
		void _Run();
		// Queue/current handles and stopping flag share this UI/worker-only mutex.
		// One current payload and one latest queued payload of each kind are pinned;
		// only the current job allocates. Editor budgets additional retained results.
		std::mutex _requestMutex;
		std::condition_variable _wake;
		std::shared_ptr<AudioSpectralSourceCapture> _queuedSource;
		std::shared_ptr<AudioSpectralSourceCapture> _currentSource;
		std::shared_ptr<AudioSpectralAnalysisJob> _queuedAnalysis;
		std::shared_ptr<AudioSpectralAnalysisJob> _currentAnalysis;
		std::shared_ptr<AudioSpectralReconstructionJob> _queuedReconstruction;
		std::shared_ptr<AudioSpectralReconstructionJob> _currentReconstruction;
		std::shared_ptr<PresentationWork> _queuedPresentation;
		std::shared_ptr<PresentationWork> _currentPresentation;
		std::shared_ptr<PresentationWork> _queuedEditing;
		std::shared_ptr<PresentationWork> _currentEditing;
		struct ContentWork { const void* Owner; std::function<void()> Run; };
		std::deque<std::shared_ptr<ContentWork>> _queuedContent;
		bool _preferReconstruction = false; // Worker only: alternate ready display/synthesis.
		bool _preferEditing = true; // At most one edit batch before a ready content/display job.
		bool _stopping = false;
		std::thread _thread;
	};
}
