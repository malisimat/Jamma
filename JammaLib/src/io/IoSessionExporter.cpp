#include "stdafx.h"
#include "IoSessionExporter.h"
#include <sstream>
#include <fstream>
#include <utility>
#include <vector>
#include "../io/JamFile.h"
#include "../io/TextReadWriter.h"
#include "../io/WavReadWriter.h"
#include "../io/NativeMidiSidecar.h"
#include "../midi/MidiQuantisation.h"
#include "../utils/PathUtils.h"

using namespace engine;

namespace io
{
	actions::ActionResult IoSessionExporter::ExportSession(const std::vector<std::shared_ptr<Station>>& stations,
		const engine::Quantiser& quantisation,
		io::JamFile::GlobalMidiQuantState globalMidiQuantState,
		double transportOffsetLoopFrac,
		const io::UserConfig& userConfig,
		const audio::AudioStreamParams& streamParams,
		audio::AudioDevice* device,
		std::mutex& sceneMutex,
		const std::shared_ptr<ninjam::NinjamController>& ninjamController)
	{
		const auto exportDir = utils::PickDirectory(L"Choose export directory");
		if (exportDir.empty())
			return actions::ActionResult::NoAction();

		ExportSessionToDirectory(stations,
			quantisation,
			globalMidiQuantState,
			transportOffsetLoopFrac,
			userConfig,
			streamParams,
			device,
			sceneMutex,
			ninjamController,
			exportDir);
		return actions::ActionResult::NoAction();
	}

	bool IoSessionExporter::ExportSessionToDirectory(const std::vector<std::shared_ptr<Station>>& stations,
		const engine::Quantiser& quantisation,
		io::JamFile::GlobalMidiQuantState globalMidiQuantState,
		double transportOffsetLoopFrac,
		const io::UserConfig& userConfig,
		const audio::AudioStreamParams& streamParams,
		audio::AudioDevice* device,
		std::mutex& sceneMutex,
		const std::shared_ptr<ninjam::NinjamController>& ninjamController,
		const std::wstring& exportDir)
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
			std::wstring TemporaryPath;
			std::wstring FinalPath;
			std::vector<float> Samples;
		};

		struct MidiSnapshot
		{
			std::wstring TemporaryPath;
			std::wstring FinalPath;
			NativeMidiSidecar::Stream Stream;
		};

		if (exportDir.empty())
			return false;

		const auto sampleRate = (streamParams.SampleRate == 0u) ? userConfig.Audio.SampleRate : streamParams.SampleRate;

		io::JamFile jam;
		jam.Version = io::JamFile::VERSION_V;
		jam.Name = "export";
		// A portable .jam is deliberately local-only.  In particular, do not
		// capture the controller config here: it can contain credentials and live
		// remote timing has no meaning when restoring a local session.
		(void)ninjamController;
		jam.TimerTicks = 0;
		jam.QuantiseSamps = quantisation.EffectiveSamps();
		jam.GlobalMidiQuantStateValue = globalMidiQuantState;
		jam.GlobalPhaseOffsetSamps = quantisation.GlobalPhaseOffsetSamps();
		jam.TransportOffsetLoopFrac = transportOffsetLoopFrac;
		jam.Quantisation = utils::Timer::QUANTISE_OFF;

		std::vector<LoopSnapshot> loops;
		std::vector<MidiSnapshot> midiStreams;

		{
			AudioPauseGuard pause(device);
			std::scoped_lock lock(sceneMutex);
			const auto clock = quantisation.Clock();
			if (!clock || clock->SeedSourceLength() == 0ul)
			{
				std::cout << "Export: local master timer is not initialised" << std::endl;
				return false;
			}
			jam.MasterLengthSamps = clock->SeedSourceLength();
			jam.AbsoluteSamplePos = clock->AbsoluteSamplePos();
			jam.Quantisation = clock->Quantisation();

			for (std::size_t stationIndex = 0u; stationIndex < stations.size(); ++stationIndex)
			{
				const auto& station = stations[stationIndex];
				if (!station)
					continue;
				if (station->IsRemote())
					continue;

				io::JamFile::Station jamStation;
				jamStation.Name = station->Name();
				jamStation.StationType = 0;
				jamStation.VstChain = station->VstEntries();
				jamStation.StationPhaseOffsetSamps = station->StationPhaseOffsetSamps();
				jamStation.AllowedMidiChannels = station->AllowedMidiChannels();
				const auto routes = station->SnapshotMidiVstRoutesForExport();
				for (std::size_t outputIndex = 0u; outputIndex < routes.PluginByMidiOutput.size(); ++outputIndex)
				{
					if (routes.PluginByMidiOutput[outputIndex] != midi::MidiVstRoutingSnapshot::NoPlugin)
						jamStation.MidiRoutes.push_back({ static_cast<unsigned int>(outputIndex), false,
							static_cast<unsigned int>(routes.PluginByMidiOutput[outputIndex]) });
				}
				if (routes.LivePlugin != midi::MidiVstRoutingSnapshot::NoPlugin)
					jamStation.MidiRoutes.push_back({ 0u, true, static_cast<unsigned int>(routes.LivePlugin) });

				const auto takes = station->GetLoopTakes();
				for (std::size_t takeIndex = 0u; takeIndex < takes.size(); ++takeIndex)
				{
					const auto& take = takes[takeIndex];
					if (!take)
						continue;
					io::JamFile::LoopTake jamTake;
					jamTake.Name = take->Id();
					jamTake.VstChain = take->VstEntries();
					jamTake.MidiQuantEnabled = take->MidiQuantisation().Enabled;
					jamTake.MidiQuantFraction = midi::MidiQuantisation::FractionIndex(take->MidiQuantisation().Fraction);
					jamTake.TakePhaseOffsetSamps = take->MidiQuantisation().PhaseOffsetSamps;

					const auto audioLoops = take->GetLoops();
					for (std::size_t loopIndex = 0u; loopIndex < audioLoops.size(); ++loopIndex)
					{
						const auto& loop = audioLoops[loopIndex];
						if (!loop)
							continue;
						const auto stem = "s" + std::to_string(stationIndex) + "_t" + std::to_string(takeIndex)
							+ "_a" + std::to_string(loopIndex);
						const auto wavFilename = stem + ".wav";

						auto samples = loop->ExportSamples();
						if (samples.empty())
							continue;

						auto jamLoop = loop->ToJamFile(wavFilename);
						jamTake.Loops.push_back(std::move(jamLoop));

						LoopSnapshot snap;
						snap.FinalPath = exportDir + L"\\" + utils::DecodeUtf8(wavFilename);
						// WavReadWriter validates the filename extension, so keep .wav on
						// the staged path while still making it distinct from the final asset.
						snap.TemporaryPath = snap.FinalPath + L".tmp.wav";
						snap.Samples = std::move(samples);
						loops.push_back(std::move(snap));
					}

					LoopTake::MidiExportState midiExport;
					if (!take->SnapshotMidiForExport(midiExport))
					{
						std::cout << "Export: could not snapshot MIDI take " << take->Id() << std::endl;
						return false;
					}
					jamTake.MidiPlayIndex = midiExport.PlayIndex;
					jamTake.MidiPlayLength = midiExport.LoopLengthSamps;
					jamTake.MidiQuantTransportStart = midiExport.QuantisationTransportStartSamps;
					auto resolveAutomationTarget = [&](const vst::IVstPlugin* target,
						io::JamFile::AutomationLane& lane) -> bool
					{
						if (!target)
							return false;
						for (std::size_t pluginIndex = 0u; pluginIndex < jamStation.VstChain.size(); ++pluginIndex)
						{
							auto plugin = station->GetVstPlugin(pluginIndex);
							if (plugin && plugin.get() == target)
							{
								lane.TargetScope = "station";
								lane.TargetPluginIndex = static_cast<unsigned int>(pluginIndex);
								return true;
							}
						}
						for (std::size_t pluginIndex = 0u; pluginIndex < jamTake.VstChain.size(); ++pluginIndex)
						{
							auto plugin = take->GetVstPlugin(pluginIndex);
							if (plugin && plugin.get() == target)
							{
								lane.TargetScope = "take";
								lane.TargetPluginIndex = static_cast<unsigned int>(pluginIndex);
								return true;
							}
						}
						for (std::size_t loopIndex = 0u; loopIndex < audioLoops.size(); ++loopIndex)
						{
							if (!audioLoops[loopIndex])
								continue;
							const auto loopVstEntryCount = audioLoops[loopIndex]->ToJamFile("unused.wav").VstChain.size();
							for (std::size_t pluginIndex = 0u; pluginIndex < loopVstEntryCount; ++pluginIndex)
							{
								auto plugin = audioLoops[loopIndex]->GetVstPlugin(pluginIndex);
								if (plugin && plugin.get() == target)
								{
									lane.TargetScope = "loop";
									lane.TargetLoopIndex = static_cast<unsigned int>(loopIndex);
									lane.TargetPluginIndex = static_cast<unsigned int>(pluginIndex);
									return true;
								}
							}
						}
						return false;
					};
					for (std::size_t streamIndex = 0u; streamIndex < midiExport.Streams.size(); ++streamIndex)
					{
						const auto& stream = midiExport.Streams[streamIndex];
						const auto stem = "s" + std::to_string(stationIndex) + "_t" + std::to_string(takeIndex)
							+ "_m" + std::to_string(streamIndex);
						const auto filename = stem + ".jammidi";
						NativeMidiSidecar::Stream sidecar;
						sidecar.LogicalLength = stream.Loop.LoopLengthSamps;
						sidecar.AutomationGlobalSampleOrigin = stream.Loop.AutomationGlobalSampleOrigin;
						sidecar.Events.reserve(stream.Loop.EventCount);
						for (std::size_t eventIndex = 0u; eventIndex < stream.Loop.EventCount; ++eventIndex)
						{
							const auto& event = stream.Loop.Events[eventIndex];
							sidecar.Events.push_back({ event.sampleOffset, event.status, event.data1, event.data2 });
						}
						for (const auto& exportedLane : stream.Loop.AutomationLanes)
						{
							if (exportedLane.MatchKey == midi::AutomationMapping::kInactive)
								continue;
							io::JamFile::AutomationLane lane;
							lane.Mapping = exportedLane.MatchKey == midi::AutomationMapping::MakeEditorMatchKey() ?
								io::JamFile::AutomationLane::MappingType::Editor :
								io::JamFile::AutomationLane::MappingType::Cc;
							lane.Channel = static_cast<std::uint8_t>((exportedLane.MatchKey >> 8u) & 0xffu);
							lane.Controller = static_cast<std::uint8_t>(exportedLane.MatchKey & 0xffu);
							lane.TargetParameterIndex = exportedLane.TargetParameterIndex;
							if (!resolveAutomationTarget(exportedLane.TargetPlugin, lane))
							{
								std::cout << "Export: unresolved automation target in take " << take->Id() << std::endl;
								return false;
							}
							lane.Points.reserve(exportedLane.PointCount);
							for (std::size_t pointIndex = 0u; pointIndex < exportedLane.PointCount; ++pointIndex)
								lane.Points.push_back({ exportedLane.Points[pointIndex].first, exportedLane.Points[pointIndex].second });
							sidecar.Lanes.push_back(std::move(lane));
						}
						jamTake.MidiStreams.push_back({ filename, stream.Channel, stream.Device,
							stream.Loop.LoopLengthSamps, stream.Loop.AutomationGlobalSampleOrigin });
						MidiSnapshot midiSnapshot;
						midiSnapshot.FinalPath = exportDir + L"\\" + utils::DecodeUtf8(filename);
						midiSnapshot.TemporaryPath = midiSnapshot.FinalPath + L".tmp";
						midiSnapshot.Stream = std::move(sidecar);
						midiStreams.push_back(std::move(midiSnapshot));
					}

					if (!jamTake.Loops.empty() || !jamTake.MidiStreams.empty())
						jamStation.LoopTakes.push_back(std::move(jamTake));
				}

				jam.Stations.push_back(std::move(jamStation));
			}
		}

		if (jam.Stations.empty())
		{
			std::cout << "Export: nothing to export" << std::endl;
			return false;
		}

		std::stringstream jamStream;
		if (!io::JamFile::ToStream(jam, jamStream))
		{
			std::cout << "Export: failed to serialize session.jam" << std::endl;
			return false;
		}

		io::WavReadWriter wavWriter;
		unsigned int wavCount = 0;
		for (const auto& loop : loops)
		{
			if (!wavWriter.Write(loop.TemporaryPath, loop.Samples, (unsigned int)loop.Samples.size(), sampleRate))
			{
				std::cout << "Export: failed to write WAV sidecar " << utils::EncodeUtf8(loop.FinalPath) << std::endl;
				return false;
			}
			++wavCount;
		}
		for (const auto& midi : midiStreams)
		{
			std::ofstream stream(midi.TemporaryPath, std::ios::binary | std::ios::trunc);
			std::string error;
			if (!stream || !NativeMidiSidecar::ToStream(midi.Stream, stream, &error))
			{
				std::cout << "Export: failed to write MIDI sidecar " << utils::EncodeUtf8(midi.FinalPath)
					<< ": " << error << std::endl;
				return false;
			}
		}

		const auto jamPath = exportDir + L"\\session.jam";
		const auto temporaryJamPath = jamPath + L".tmp";
		const auto wroteJamFile = io::TextReadWriter().Write(temporaryJamPath, jamStream.str(), 0, 0);
		if (!wroteJamFile)
		{
			std::cout << "Export: failed to write session.jam to "
				<< utils::EncodeUtf8(jamPath) << std::endl;
			return false;
		}
		auto replaceAtomically = [](const std::wstring& temporaryPath, const std::wstring& finalPath) noexcept
		{
			return MoveFileExW(temporaryPath.c_str(), finalPath.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		};
		for (const auto& loop : loops)
		{
			if (!replaceAtomically(loop.TemporaryPath, loop.FinalPath))
			{
				std::cout << "Export: failed to stage WAV sidecar " << utils::EncodeUtf8(loop.FinalPath) << std::endl;
				return false;
			}
		}
		for (const auto& midi : midiStreams)
		{
			if (!replaceAtomically(midi.TemporaryPath, midi.FinalPath))
			{
				std::cout << "Export: failed to stage MIDI sidecar " << utils::EncodeUtf8(midi.FinalPath) << std::endl;
				return false;
			}
		}
		if (!replaceAtomically(temporaryJamPath, jamPath))
		{
			std::cout << "Export: failed to publish session.jam to " << utils::EncodeUtf8(jamPath) << std::endl;
			return false;
		}

		std::cout << "Exported " << wavCount << " loop(s) + session.jam to "
			<< utils::EncodeUtf8(exportDir) << std::endl;

		return true;
	}
}
