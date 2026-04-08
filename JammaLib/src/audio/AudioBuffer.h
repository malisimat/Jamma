#pragma once

#include <algorithm>
#include <vector>
#include <memory>
#include <iostream>
#include "../include/Constants.h"
#include "AudioSource.h"

namespace audio
{
	class AudioBuffer :
		public virtual base::AudioSink,
		public virtual base::AudioSource
	{
	public:
		AudioBuffer();
		AudioBuffer(unsigned int size);
		~AudioBuffer();

	public:
		virtual AudioPlugType AudioPlug() const override { return AUDIOPLUG_BOTH; }
		virtual void EndPlay(unsigned int numSamps) override;
		virtual void OnBlockWrite(const base::AudioWriteRequest& request, int writeOffset) override;
		virtual void EndWrite(unsigned int numSamps, bool updateIndex) override;

		void SetSize(unsigned int size);
		unsigned int SampsRecorded() const;
		unsigned int BufSize() const;

		const float& operator[](unsigned int index) const;
		unsigned int Delay(unsigned int sampsDelay);
		unsigned int PlayIndex() const;
		bool IsContiguous(unsigned int startIndex, unsigned int numSamps) const;
		const float* BlockRead(unsigned int startIndex) const;

		// Reads numSamps from the current playback position.
		// Call Delay(...) first to set the playback position when needed.
		// Returns a pointer to contiguous data — either a direct pointer into
		// the ring buffer (zero-copy when no wrap-around) or tempBuf after
		// copying wrapped data. tempBuf must hold at least numSamps floats.
		const float* PlaybackRead(float* tempBuf, unsigned int numSamps);

	protected:
		std::vector<float>::iterator Start();
		std::vector<float>::iterator End();

	protected:
		void _SetWriteIndex(unsigned int index);

	protected:
		unsigned int _sampsRecorded;
		unsigned long _playIndex;
		std::vector<float> _buffer;
	};
}
