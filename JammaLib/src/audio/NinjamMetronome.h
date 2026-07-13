#pragma once

#include <vector>
#include "../ninjam/NinjamMetronomeTiming.h"

namespace audio
{
	class NinjamMetronome
	{
	public:
		void Configure(unsigned int sampleRate);
		void Reset() noexcept;
		void Mix(float* interleavedOutput,
			unsigned int numOutputChannels,
			unsigned int numFrames,
			const ninjam::NinjamMetronomeTimingResult& timing) noexcept;

		const std::vector<float>& NormalTable() const noexcept { return _normalTable; }
		const std::vector<float>& AccentTable() const noexcept { return _accentTable; }

	private:
		static std::vector<float> _BuildTable(unsigned int sampleRate, float durationMs, float gain);
		void _MixTable(float* interleavedOutput,
			unsigned int numOutputChannels,
			unsigned int sampleOffset,
			const std::vector<float>& table,
			unsigned int& playIndex) noexcept;

		std::vector<float> _normalTable;
		std::vector<float> _accentTable;
		unsigned int _normalPlayIndex = 0u;
		unsigned int _accentPlayIndex = 0u;
	};
}