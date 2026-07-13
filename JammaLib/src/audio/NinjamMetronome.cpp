#include "NinjamMetronome.h"

#include <algorithm>
#include <cmath>

using namespace audio;

void NinjamMetronome::Configure(unsigned int sampleRate)
{
	_normalTable = _BuildTable(sampleRate, 8.0f, 0.12589254f);
	_accentTable = _BuildTable(sampleRate, 12.0f, 0.19952623f);
	Reset();
}

void NinjamMetronome::Reset() noexcept
{
	_normalPlayIndex = static_cast<unsigned int>(_normalTable.size());
	_accentPlayIndex = static_cast<unsigned int>(_accentTable.size());
}

void NinjamMetronome::Mix(float* interleavedOutput,
	unsigned int numOutputChannels,
	unsigned int numFrames,
	const ninjam::NinjamMetronomeTimingResult& timing) noexcept
{
	if (!interleavedOutput || numOutputChannels == 0u || numFrames == 0u || !timing.valid)
		return;

	unsigned int onsetIndex = 0u;
	for (auto sample = 0u; sample < numFrames; ++sample)
	{
		bool accentAtSample = false;
		bool normalAtSample = false;
		while (onsetIndex < timing.onsetCount && timing.onsets[onsetIndex].offset == sample)
		{
			accentAtSample = accentAtSample || timing.onsets[onsetIndex].accent;
			normalAtSample = normalAtSample || !timing.onsets[onsetIndex].accent;
			onsetIndex++;
		}

		if (accentAtSample)
			_accentPlayIndex = 0u;
		else if (normalAtSample)
			_normalPlayIndex = 0u;

		_MixTable(interleavedOutput, numOutputChannels, sample, _normalTable, _normalPlayIndex);
		_MixTable(interleavedOutput, numOutputChannels, sample, _accentTable, _accentPlayIndex);
	}
}

std::vector<float> NinjamMetronome::_BuildTable(unsigned int sampleRate, float durationMs, float gain)
{
	if (sampleRate == 0u)
		return {};

	const auto tableSize = std::max(1u, static_cast<unsigned int>(std::ceil(sampleRate * durationMs / 1000.0f)));
	std::vector<float> table(tableSize);
	constexpr float pi = 3.14159265358979323846f;

	for (auto sample = 0u; sample < tableSize; ++sample)
	{
		const auto time = static_cast<float>(sample) / static_cast<float>(sampleRate);
		const auto phase = tableSize > 1u ? static_cast<float>(sample) / static_cast<float>(tableSize - 1u) : 0.0f;
		const auto attackPhase = std::min(1.0f, phase * 20.0f);
		const auto envelope = 0.5f - 0.5f * std::cos(pi * attackPhase);
		const auto decay = (1.0f - phase) * (1.0f - phase);
		const auto body = 0.58f * std::sin(2.0f * pi * 983.0f * time)
			+ 0.27f * std::sin(2.0f * pi * 1437.0f * time)
			+ 0.15f * std::sin(2.0f * pi * 2179.0f * time);
		const auto ring = 0.25f * std::sin(2.0f * pi * 3911.0f * time)
			* std::sin(2.0f * pi * 587.0f * time);
		table[sample] = gain * envelope * decay * (body + ring);
	}

	return table;
}

void NinjamMetronome::_MixTable(float* interleavedOutput,
	unsigned int numOutputChannels,
	unsigned int sampleOffset,
	const std::vector<float>& table,
	unsigned int& playIndex) noexcept
{
	if (playIndex >= table.size())
		return;

	const auto value = table[playIndex++];
	const auto outputOffset = static_cast<size_t>(sampleOffset) * numOutputChannels;
	for (auto channel = 0u; channel < numOutputChannels; ++channel)
		interleavedOutput[outputOffset + channel] += value;
}