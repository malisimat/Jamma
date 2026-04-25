#include "gtest/gtest.h"

#include <memory>
#include <vector>

#include "engine/LoopRemote.h"
#include "engine/StationRemote.h"

using base::AudioSink;
using base::AudioWriteRequest;
using base::Audible;
using base::MultiAudioSink;
using engine::LoopParams;
using engine::LoopRemote;
using engine::Station;
using engine::StationParams;
using engine::StationRemote;

namespace
{
	class CaptureSink :
		public AudioSink
	{
	public:
		explicit CaptureSink(unsigned int numSamps) :
			Samples(numSamps, 0.0f)
		{
		}

		virtual void OnBlockWrite(const AudioWriteRequest& request, int writeOffset) override
		{
			for (auto i = 0u; i < request.numSamps; i++)
			{
				auto destIndex = _writeIndex + writeOffset + i;
				if (destIndex < Samples.size())
				{
					auto samp = request.samples[i * request.stride];
					Samples[destIndex] = (request.fadeNew * samp) + (request.fadeCurrent * Samples[destIndex]);
				}
			}
		}

		virtual void EndWrite(unsigned int numSamps, bool updateIndex) override
		{
			if (updateIndex)
				_writeIndex += numSamps;
		}

		std::vector<float> Samples;
	};

	class CaptureMultiSink :
		public MultiAudioSink
	{
	public:
		explicit CaptureMultiSink(unsigned int numSamps) :
			_left(std::make_shared<CaptureSink>(numSamps)),
			_right(std::make_shared<CaptureSink>(numSamps))
		{
		}

		virtual unsigned int NumInputChannels(Audible::AudioSourceType source) const override
		{
			return 2;
		}

		const std::vector<float>& Left() const { return _left->Samples; }
		const std::vector<float>& Right() const { return _right->Samples; }

	protected:
		virtual const std::shared_ptr<AudioSink> _InputChannel(unsigned int channel,
			Audible::AudioSourceType source) override
		{
			if (channel == 0u)
				return _left;
			if (channel == 1u)
				return _right;
			return nullptr;
		}

	private:
		std::shared_ptr<CaptureSink> _left;
		std::shared_ptr<CaptureSink> _right;
	};

	std::shared_ptr<StationRemote> MakeRemoteStation()
	{
		StationParams params;
		params.Name = "remote-user";
		params.Size = { 200, 280 };
		audio::MergeMixBehaviourParams merge;
		auto mixerParams = Station::GetMixerParams(params.Size, merge);
		auto station = std::make_shared<StationRemote>(params, mixerParams);
		station->SetNumBusChannels(2);
		station->SetNumDacChannels(2);
		station->EnsureRemoteTake();
		return station;
	}
}

TEST(StationRemote, IngestStereoBlockFeedsStationMixPath)
{
	const auto blockSize = 256u;
	auto station = MakeRemoteStation();
	auto sink = std::make_shared<CaptureMultiSink>(blockSize);

	std::vector<float> left(blockSize, 0.0f);
	std::vector<float> right(blockSize, 0.0f);
	for (auto i = 0u; i < blockSize; i++)
	{
		left[i] = (i % 7 == 0) ? 0.7f : 0.0f;
		right[i] = (i % 9 == 0) ? -0.5f : 0.0f;
	}

	station->IngestStereoBlock(left.data(), right.data(), blockSize);
	station->WriteBlock(sink, nullptr, 0, blockSize);

	bool hasLeft = false;
	bool hasRight = false;
	for (auto i = 0u; i < blockSize; i++)
	{
		hasLeft = hasLeft || (sink->Left()[i] != 0.0f);
		hasRight = hasRight || (sink->Right()[i] != 0.0f);
	}

	EXPECT_TRUE(hasLeft);
	EXPECT_TRUE(hasRight);
}

TEST(LoopRemote, MeasureMetadataTracksIngestProgress)
{
	audio::WireMixBehaviourParams wire;
	audio::AudioMixerParams mixerParams;
	mixerParams.Behaviour = wire;

	LoopParams params;
	params.Id = "remote-loop";
	params.TakeId = "remote-take";
	params.Wav = "remote-loop";
	auto loop = std::make_shared<LoopRemote>(params, mixerParams);

	loop->SetMeasureLength(1024);
	loop->SetMeasurePosition(1000);

	std::vector<float> block(128, 0.3f);
	loop->IngestSamples(block.data(), static_cast<unsigned int>(block.size()));

	EXPECT_EQ(1024u, loop->MeasureLength());
	EXPECT_EQ((1000u + 128u) % 1024u, loop->MeasurePosition());
}
