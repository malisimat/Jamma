#include "stdafx.h"
#include "IoSessionExporter.h"
#include <sstream>
#include <utility>
#include <vector>
#include "../io/JamFile.h"
#include "../io/TextReadWriter.h"
#include "../io/WavReadWriter.h"
#include "../utils/PathUtils.h"

using namespace engine;

namespace io
{
	actions::ActionResult IoSessionExporter::ExportSession(const std::vector<std::shared_ptr<Station>>& stations,
		const timing::TimingQuantiser& quantisation,
		const io::UserConfig& userConfig,
		const audio::AudioStreamParams& streamParams,
		audio::AudioDevice* device,
		std::mutex& sceneMutex,
		const std::shared_ptr<ninjam::NinjamController>& ninjamController)
	{
		struct AudioPauseGuard
		{
			explicit AudioPauseGuard(audio::AudioDevice* deviceRef) : Device(deviceRef), WasPlaying(deviceRef && deviceRef->Pause()) {}
			~AudioPauseGuard() { Resume(); }

			void Resume()
			{
				if (WasPlaying && Device)
				{
					Device->Resume();
					WasPlaying = false;
				}
			}

			audio::AudioDevice* Device;
			bool WasPlaying;
		};

		struct LoopSnapshot
		{
			std::wstring Path;
			std::vector<float> Samples;
		};

		const auto exportDir = utils::PickDirectory(L"Choose export directory");
		if (exportDir.empty())
			return actions::ActionResult::NoAction();

		const auto sampleRate = (streamParams.SampleRate == 0u) ? userConfig.Audio.SampleRate : streamParams.SampleRate;

		io::JamFile jam;
		jam.Version = io::JamFile::VERSION_V;
		jam.Name = "export";
		jam.Ninjam = ninjamController ? ninjamController->Config() : io::JamFile::NinjamConfig{};
		jam.TimerTicks = 0;
		jam.QuantiseSamps = 0;
		jam.GlobalPhaseOffsetSamps = quantisation.GlobalPhaseOffsetSamps();
		jam.Quantisation = utils::Timer::QUANTISE_OFF;

		std::vector<LoopSnapshot> loops;

		{
			AudioPauseGuard pause(device);
			std::scoped_lock lock(sceneMutex);

			for (const auto& station : stations)
			{
				if (station->IsRemote())
					continue;

				io::JamFile::Station jamStation;
				jamStation.Name = station->Name();
				jamStation.StationType = 0;
				jamStation.VstChain = station->VstEntries();
				jamStation.StationPhaseOffsetSamps = station->StationPhaseOffsetSamps();
				jamStation.AllowedMidiChannels = station->AllowedMidiChannels();

				for (const auto& take : station->GetLoopTakes())
				{
					io::JamFile::LoopTake jamTake;
					jamTake.Name = take->Id();
					jamTake.VstChain = take->VstEntries();
					jamTake.TakePhaseOffsetSamps = take->MidiQuantisation().PhaseOffsetSamps;

					for (const auto& loop : take->GetLoops())
					{
						const auto wavFilename = loop->Id() + ".wav";

						auto samples = loop->ExportSamples();
						if (samples.empty())
							continue;

						jamTake.Loops.push_back(loop->ToJamFile(wavFilename));

						LoopSnapshot snap;
						snap.Path = exportDir + L"\\" + utils::DecodeUtf8(wavFilename);
						snap.Samples = std::move(samples);
						loops.push_back(std::move(snap));
					}

					if (!jamTake.Loops.empty())
						jamStation.LoopTakes.push_back(std::move(jamTake));
				}

				jam.Stations.push_back(std::move(jamStation));
			}
		}

		if (jam.Stations.empty())
		{
			std::cout << "Export: nothing to export" << std::endl;
			return actions::ActionResult::NoAction();
		}

		io::WavReadWriter wavWriter;
		unsigned int wavCount = 0;
		for (const auto& loop : loops)
		{
			if (wavWriter.Write(loop.Path, loop.Samples, (unsigned int)loop.Samples.size(), sampleRate))
				++wavCount;
		}

		std::stringstream jamStream;
		io::JamFile::ToStream(jam, jamStream);
		const auto jamPath = exportDir + L"\\session.jam";
		const auto wroteJamFile = io::TextReadWriter().Write(jamPath, jamStream.str(), 0, 0);
		if (!wroteJamFile)
		{
			std::cout << "Export: failed to write session.jam to "
				<< utils::EncodeUtf8(jamPath) << std::endl;
			return actions::ActionResult::NoAction();
		}

		std::cout << "Exported " << wavCount << " loop(s) + session.jam to "
			<< utils::EncodeUtf8(exportDir) << std::endl;

		return actions::ActionResult::NoAction();
	}
}
