#pragma once

#include <cstdint>

#include "MidiLoop.h"
#include "../audio/PlaybackRate.h"

namespace midi
{
	class MidiIndexedOutputSink final : public IMidiSink
	{
	public:
		MidiIndexedOutputSink(IMidiOutputSink& outputSink,
			unsigned int outputIndex,
			std::uint32_t midiBlockStart,
			std::uint32_t outputBlockStart,
			float velocityScale = 1.0f) noexcept;

		void OnEvent(const MidiEvent& ev) noexcept override;
		void SetPlaybackRate(const audio::PlaybackRateBlock& block, double recordingRate,
			double sourceFraction) noexcept;
		bool TryOnEvent(const MidiEvent& ev) noexcept override;
		bool TryOnVoiceEvent(const MidiEvent& ev, const MidiVoiceEvent& voice) noexcept override;

	private:
		IMidiOutputSink& _outputSink;
		unsigned int _outputIndex;
		std::uint32_t _midiBlockStart;
		std::uint32_t _outputBlockStart;
		float _velocityScale;
		const audio::PlaybackRateBlock* _rateBlock = nullptr;
		double _recordingRate = 1.0;
		double _sourceFraction = 0.0;
		std::optional<audio::PlaybackRateCursor> _rateCursor;
		double _sourceThrough = 0.0;
        double _sourceFrameStart = 0.0;
		std::uint32_t _rateFrame = 0u;
		std::int32_t _lastSourceOffset = 0;
		std::uint32_t _FrameForSource(std::uint32_t source) noexcept;
	};
}
