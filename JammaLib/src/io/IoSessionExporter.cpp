#include "stdafx.h"
#include "IoSessionExporter.h"
#include <algorithm>
#include <cmath>
#include <climits>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <random>
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
		const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
		const engine::Quantiser& quantisation,
		io::JamFile::GlobalMidiQuantState globalMidiQuantState,
		double transportOffsetLoopFrac,
		const io::UserConfig& userConfig,
		const audio::AudioStreamParams& streamParams,
		audio::AudioDevice* device,
		std::mutex& sceneMutex,
		const std::shared_ptr<ninjam::NinjamController>& ninjamController,
		const engine::PerformanceGroups* groups)
	{
		const auto exportDir = utils::PickDirectory(L"Choose export directory");
		if (exportDir.empty())
			return actions::ActionResult::NoAction();

		ExportSessionToDirectory(stations,
			rigSnapshot,
			quantisation,
			globalMidiQuantState,
			transportOffsetLoopFrac,
			userConfig,
			streamParams,
			device,
			sceneMutex,
			ninjamController,
			exportDir,
			groups);
		return actions::ActionResult::NoAction();
	}

	bool IoSessionExporter::ExportSessionToDirectory(const std::vector<std::shared_ptr<Station>>& stations,
		const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
		const engine::Quantiser& quantisation,
		io::JamFile::GlobalMidiQuantState globalMidiQuantState,
		double transportOffsetLoopFrac,
		const io::UserConfig& userConfig,
		const audio::AudioStreamParams& streamParams,
		audio::AudioDevice* device,
		std::mutex& sceneMutex,
		const std::shared_ptr<ninjam::NinjamController>& ninjamController,
		const std::wstring& exportDir, const engine::PerformanceGroups* groups)
    {
        // Device::Pause cannot establish callback exclusion. Legacy API is
        // intentionally limited to caller-owned offline material.
        if (device) return false;
        return ExportToDirectory(stations, rigSnapshot, quantisation, globalMidiQuantState,
            transportOffsetLoopFrac, userConfig, streamParams, nullptr, sceneMutex,
            ninjamController, exportDir, false, {}, {}, groups).Success;
    }

	IoSessionExporter::ExportResult IoSessionExporter::ExportSessionVersionToDirectory(const std::vector<std::shared_ptr<Station>>& stations,
		const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
		const engine::Quantiser& quantisation,
		io::JamFile::GlobalMidiQuantState globalMidiQuantState,
		double transportOffsetLoopFrac,
		const io::UserConfig& userConfig,
		const audio::AudioStreamParams& streamParams,
		audio::AudioHost* host,
		std::mutex& sceneMutex,
		const std::shared_ptr<ninjam::NinjamController>& ninjamController,
		const std::wstring& exportDir,
        const std::function<bool(std::string&)>& afterCapture,
        const std::function<bool(std::string&)>& beforeCapture, const engine::PerformanceGroups* groups)
    {
        return ExportToDirectory(stations, rigSnapshot, quantisation, globalMidiQuantState,
            transportOffsetLoopFrac, userConfig, streamParams, host, sceneMutex,
            ninjamController, exportDir, true, afterCapture, beforeCapture, groups);
    }

	IoSessionExporter::ExportResult IoSessionExporter::ExportToDirectory(const std::vector<std::shared_ptr<Station>>& stations,
		const std::shared_ptr<const engine::RigSnapshot>& rigSnapshot,
		const engine::Quantiser& quantisation,
		io::JamFile::GlobalMidiQuantState globalMidiQuantState,
		double transportOffsetLoopFrac,
		const io::UserConfig& userConfig,
		const audio::AudioStreamParams& streamParams,
		audio::AudioHost* host,
		std::mutex& sceneMutex,
		const std::shared_ptr<ninjam::NinjamController>& ninjamController,
		const std::wstring& exportDir, bool numbered,
        const std::function<bool(std::string&)>& afterCapture,
        const std::function<bool(std::string&)>& beforeCapture, const engine::PerformanceGroups* groups)
    {
        ExportResult result;
        const auto appendError = [&](const std::string& error)
        {
            if (!result.Error.empty()) result.Error += "; ";
            result.Error += error;
        };
        // All state below is operation-local, written/read only by the control
        // owner and destroyed after capture/disk completion. No shared publishers.
        struct AudioPauseGuard
        {
            audio::AudioHost* Host;
            audio::AudioHost::QuiescenceState State{};
            bool Active = false;
            ExportResult& Result;
            ~AudioPauseGuard() { Resume(); }
            bool Pause()
            {
                if (!Host) return true; // Offline/test material, no callback owner.
                Active = Host->BeginQuiescence(State, Result.Error);
                if (!Active) Result.AudioResumed = false; // Failed admission leaves engine explicitly stopped/error.
                return Active;
            }
            void Resume() noexcept
            {
                if (!Active) return;
                Active = false;
                std::string error;
                try
                {
                    Result.AudioResumed = Host->EndQuiescence(State, error);
                    if (!Result.AudioResumed) Result.Error += "; audio restoration failed: " + error;
                }
                catch (...)
                {
                    Result.AudioResumed = false;
                    // Keep the failure flag even if formatting/allocation failed.
                }
            }
        };
        struct AttemptFiles
        {
            std::vector<std::wstring> Paths;
            bool Committed = false;
            ~AttemptFiles()
            {
                if (!Committed)
                    for (const auto& path : Paths) DeleteFileW(path.c_str());
            }
            bool Write(const std::wstring& path, const std::string& bytes)
            {
                const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle == INVALID_HANDLE_VALUE) return false;
                // Capacity is reserved before creating any files. Add ownership
                // immediately after exclusive creation, never for somebody else's file.
                try { Paths.push_back(path); }
                catch (...) { CloseHandle(handle); DeleteFileW(path.c_str()); throw; }
                bool success = true;
                std::size_t offset = 0;
                while (offset < bytes.size())
                {
                    const auto size = static_cast<DWORD>(std::min<std::size_t>(bytes.size()-offset, 1024u*1024u));
                    DWORD written = 0;
                    if (!WriteFile(handle, bytes.data()+offset, size, &written, nullptr) || written != size)
                    { success = false; break; }
                    offset += written;
                }
                if (!FlushFileBuffers(handle)) success = false;
                if (!CloseHandle(handle)) success = false;
                return success;
            }
            bool WriteWav(const std::wstring& path, const std::vector<float>& samples, unsigned int sampleRate)
            {
                if (!sampleRate || sampleRate > static_cast<unsigned int>(LONG_MAX / 2) ||
                    samples.empty() || samples.size() > static_cast<std::size_t>((LONG_MAX - 44) / 2)) return false;
                SoundHeader header = { { 'R','I','F','F' }, 0, { 'W','A','V','E' },
                    { 'f','m','t',' ' }, 16, 1, 1, 0, 0, 2, 16, { 'd','a','t','a' }, 0 };
                header.dlength = static_cast<long>(samples.size() * 2);
                header.flength = header.dlength + 36;
                header.srate = static_cast<long>(sampleRate);
                header.bytes_per_sec = header.srate * 2;
                static_assert(sizeof(SoundHeader) == 44);
                std::string bytes(reinterpret_cast<const char*>(&header), sizeof(header));
                bytes.reserve(sizeof(header) + samples.size() * 2);
                for (const auto value : samples)
                {
                    const auto pcm = static_cast<int>(std::clamp(std::isfinite(value) ? value : 0.0f, -1.0f, 1.0f) * 32767.0f);
                    bytes.push_back(static_cast<char>(pcm & 0xff));
                    bytes.push_back(static_cast<char>((pcm >> 8) & 0xff));
                }
                return Write(path, bytes);
            }
        };
        AttemptFiles files;
        AudioPauseGuard pause{ host, {}, false, result };
        try
        {

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

		if (exportDir.empty()) { result.Cancelled = true; return result; }

        std::error_code directoryError;
        if (!std::filesystem::is_directory(exportDir, directoryError) || directoryError)
        { appendError("Save folder is unavailable"); return result; }
        std::vector<wchar_t> volumePath(32768, L'\0');
        if (!GetVolumePathNameW(exportDir.c_str(), volumePath.data(), static_cast<DWORD>(volumePath.size())))
        { appendError("Cannot establish destination filesystem"); return result; }
        const auto driveType = GetDriveTypeW(volumePath.data());
        if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_RAMDISK)
        { appendError("Atomic save requires a local filesystem"); return result; }
        std::uint64_t maxVersion = 0;
        if (numbered)
        {
            for (const auto& entry : std::filesystem::directory_iterator(exportDir))
            {
                const auto name = entry.path().filename().wstring();
                if (name.size() < 13 || name.compare(0, 8, L"session-") || name.substr(name.size()-4) != L".jam") continue;
                const auto digits = name.substr(8, name.size()-12);
                if (digits.empty() || digits.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
                std::uint64_t value = 0;
                for (auto digit : digits)
                {
                    if (value > (std::numeric_limits<std::uint64_t>::max() - (digit-L'0')) / 10)
                    { appendError("Session version exceeds numbering capacity"); return result; }
                    value = value * 10 + (digit-L'0');
                }
                maxVersion = std::max(maxVersion, value);
            }
        }
        if (maxVersion == std::numeric_limits<std::uint64_t>::max())
        { appendError("Session version numbering exhausted"); return result; }
        std::ostringstream version;
        version << "session-" << std::setw(4) << std::setfill('0') << (maxVersion + 1);
        const auto manifestStem = numbered ? version.str() : std::string("session");
        std::random_device random;
        std::ostringstream token;
        token << manifestStem << "-" << std::hex << GetCurrentProcessId() << "-" << random() << random();
        const auto assetPrefix = token.str() + "-";

		const auto sampleRate = (streamParams.SampleRate == 0u) ? userConfig.Audio.SampleRate : streamParams.SampleRate;

		io::JamFile jam;
		jam.Version = io::JamFile::VERSION_V;
		jam.Name = "export";
		// A portable .jam is deliberately local-only.  In particular, do not
		// capture the controller config here: it can contain credentials and live
		// remote timing has no meaning when restoring a local session.
		(void)ninjamController;
		jam.TimerTicks = 0;

		jam.GlobalMidiQuantStateValue = globalMidiQuantState;

		jam.TransportOffsetLoopFrac = transportOffsetLoopFrac;
		jam.Quantisation = utils::Timer::QUANTISE_OFF;

		std::vector<LoopSnapshot> loops;
		std::vector<MidiSnapshot> midiStreams;

        if (!pause.Pause()) return result;
        const auto finishCapture = [&]()
        {
            pause.Resume();
            if (afterCapture)
            {
                std::string restoreError;
                const bool restored = afterCapture(restoreError);
                result.AudioResumed = result.AudioResumed && restored;
                if (!restored) appendError("Audio restoration failed: " + restoreError);
            }
        };
        if (beforeCapture)
        {
            std::string captureError;
            if (!beforeCapture(captureError))
            {
                appendError(captureError.empty() ? "Session capture is unavailable" : captureError);
                finishCapture();
                return result;
            }
        }
        const bool captured = [&]() -> bool
        {
			std::scoped_lock lock(sceneMutex);
            jam.QuantiseSamps = quantisation.EffectiveSamps();
            jam.GlobalPhaseOffsetSamps = quantisation.GlobalPhaseOffsetSamps();
		if (groups)
		{
			for (std::size_t slot = 1u; slot <= 9u; ++slot)
			{
				if (const auto snapshot = groups->Mute.Get(slot)) jam.MuteGroups[slot - 1u] = *snapshot;
				if (const auto snapshot = groups->Selection.Get(slot)) jam.SelectionGroups[slot - 1u] = *snapshot;
			}
			if (const auto active = groups->Mute.ActiveSlot()) jam.MuteActiveGroup = static_cast<unsigned int>(*active - 1u);
			if (const auto active = groups->Selection.ActiveSlot()) jam.SelectionActiveGroup = static_cast<unsigned int>(*active - 1u);
		}

			const auto clock = quantisation.Clock();
			const auto hasSavedLoopContent = std::any_of(stations.begin(), stations.end(),
				[](const std::shared_ptr<Station>& station)
				{
					if (!station || station->IsRemote())
                        return false;
					const auto& takes = station->GetLoopTakes();
					return std::any_of(takes.begin(), takes.end(), [](const std::shared_ptr<LoopTake>& take)
						{ return take && (!take->GetLoops().empty() || !take->GetMidiLoops().empty()); });
				});
			if (!hasSavedLoopContent)
				jam.TransportInitialised = false;
			else if (clock && clock->SeedSourceLength() != 0ul)
			{
				jam.MasterLengthSamps = clock->SeedSourceLength();
				jam.AbsoluteSamplePos = clock->AbsoluteSamplePos();
				jam.Quantisation = clock->Quantisation();
			}
			else
			{
				std::cout << "Export: local master timer is not initialised" << std::endl;
	            result.Error = "Session capture failed"; return false;
			}

			for (std::size_t stationIndex = 0u; stationIndex < stations.size(); ++stationIndex)
			{
				const auto& station = stations[stationIndex];
				if (!station)
					continue;
				if (station->IsRemote())
					continue;

				io::JamFile::Station jamStation;
				jamStation.Id = station->Id();
				jamStation.Name = station->Name();
				jamStation.StationType = 0;
				jamStation.MasterLevel = station->MasterLevelForExport();
				jamStation.BusLevels = station->BusLevelsForExport();
				jamStation.VstChain = station->VstEntries();
				jamStation.StationPhaseOffsetSamps = station->StationPhaseOffsetSamps();
				jamStation.AllowedMidiChannels = station->AllowedMidiChannels();
				jamStation.HasLiveVstRoutes = true;
				jamStation.LiveVstRoutes = station->LiveVstChannelRoutes();
				jamStation.AudioRoutes = station->SnapshotAudioRoutesForExport();
				jamStation.HasAudioRoutes = true;
                if (numbered) for (auto& routes : jamStation.AudioRoutes)
                    std::erase_if(routes, [&](auto channel) { return channel >= streamParams.NumOutputChannels; });
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
					jamTake.Id = take->Id();
					jamTake.Name = take->Id();
					jamTake.MasterLevel = take->MasterLevelForExport();
					jamTake.BusLevels = take->BusLevelsForExport();
					jamTake.VstChain = take->VstEntries();
					jamTake.MidiQuantEnabled = take->MidiQuantisation().Enabled;
					jamTake.MidiQuantFraction = midi::MidiQuantisation::FractionIndex(take->MidiQuantisation().Fraction);
					jamTake.TakePhaseOffsetSamps = take->MidiQuantisation().PhaseOffsetSamps;
					jamTake.AudioRoutes = take->SnapshotAudioRoutesForExport();
					jamTake.HasAudioRoutes = true;
                    if (numbered) for (auto& routes : jamTake.AudioRoutes)
                        std::erase_if(routes, [&](auto channel) { return channel >= station->NumBusChannels(); });

					const auto audioLoops = take->GetLoops();
					for (std::size_t loopIndex = 0u; loopIndex < audioLoops.size(); ++loopIndex)
					{
						const auto& loop = audioLoops[loopIndex];
						if (!loop)
							continue;
						const auto stem = assetPrefix + "s" + std::to_string(stationIndex) + "_t" + std::to_string(takeIndex)
							+ "_a" + std::to_string(loopIndex);
						const auto wavFilename = stem + ".wav";

						auto samples = loop->ExportSamples(true);
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
			            result.Error = "Session capture failed"; return false;
					}
					jamTake.MidiPlayIndex = midiExport.PlayIndex;
					jamTake.MidiPlayLength = midiExport.LoopLengthSamps;
					jamTake.MidiQuantTransportStart = midiExport.QuantisationTransportStartSamps;
					if (midiExport.BaseIntervalSamps > 0u && midiExport.BaseDivisions > 0u)
					{
						jamTake.BaseIntervalSamps = midiExport.BaseIntervalSamps;
						jamTake.BaseDivisions = midiExport.BaseDivisions;
					}

					auto resolveAutomationTarget = [&](const midi::MidiLoop::AutomationLaneExport& exported,
						io::JamFile::AutomationLane& lane) -> bool
					{
						const auto* target = exported.TargetPlugin;
						const auto& saved = exported.SavedTarget;
						const auto matches = [&](const std::shared_ptr<vst::IVstPlugin>& plugin, const char* scope) {
							return plugin && (saved.Scope.empty() || saved.Scope == scope)
								&& (saved.PluginId.empty() ? plugin.get() == target : plugin->StateId() == saved.PluginId);
						};
						for (std::size_t pluginIndex = 0u; pluginIndex < jamStation.VstChain.size(); ++pluginIndex)
						{
							auto plugin = station->GetVstPlugin(pluginIndex);
							if (matches(plugin, "station"))
							{
								lane.TargetScope = "station";
								lane.TargetPluginId = plugin->StateId();
								lane.TargetPluginIndex = static_cast<unsigned int>(pluginIndex);
								return true;
							}
						}
						for (std::size_t pluginIndex = 0u; pluginIndex < jamTake.VstChain.size(); ++pluginIndex)
						{
							auto plugin = take->GetVstPlugin(pluginIndex);
							if (matches(plugin, "take"))
							{
								lane.TargetScope = "take";
								lane.TargetPluginId = plugin->StateId();
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
								if (matches(plugin, "loop"))
								{
									lane.TargetScope = "loop";
									lane.TargetPluginId = plugin->StateId();
									lane.TargetLoopIndex = static_cast<unsigned int>(loopIndex);
									lane.TargetPluginIndex = static_cast<unsigned int>(pluginIndex);
									return true;
								}
							}
						}
						// Missing/inactive/removed targets retain their exact identity and
						// curve. ID-aware import cannot bind a survivor at the old index.
						if (!saved.Scope.empty())
						{
							lane.TargetScope = saved.Scope;
							lane.TargetPluginId = !saved.PluginId.empty() && io::NativeMidiSidecar::ValidatePluginIdentity(saved.PluginId)
								? saved.PluginId : "unavailable:" + utils::GetGuid();
							lane.TargetPluginIndex = saved.PluginIndex;
							lane.TargetLoopIndex = saved.LoopIndex;
							return true;
						}
						return false;
					};
					for (std::size_t streamIndex = 0u; streamIndex < midiExport.Streams.size(); ++streamIndex)
					{
						const auto& stream = midiExport.Streams[streamIndex];
						const auto stem = assetPrefix + "s" + std::to_string(stationIndex) + "_t" + std::to_string(takeIndex)
							+ "_m" + std::to_string(streamIndex);
						const auto filename = stem + ".jammidi";
						NativeMidiSidecar::Stream sidecar;
						sidecar.LogicalLength = stream.Loop.LoopLengthSamps;
						sidecar.AutomationGlobalSampleOrigin = stream.Loop.AutomationGlobalSampleOrigin;
						sidecar.Events.reserve(stream.Loop.EventCount);
						for (std::size_t eventIndex = 0u; eventIndex < stream.Loop.EventCount; ++eventIndex)
						{
							const auto& event = stream.Loop.Events[eventIndex];
							sidecar.Events.push_back({ event.sampleOffset, event.status, event.data1, event.data2, event.flags });
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
							if (!resolveAutomationTarget(exportedLane, lane))
							{
								std::cout << "Export: unresolved automation target in take " << take->Id() << std::endl;
					            result.Error = "Session capture failed"; return false;
							}
							lane.Points.reserve(exportedLane.PointCount);
							for (std::size_t pointIndex = 0u; pointIndex < exportedLane.PointCount; ++pointIndex)
								lane.Points.push_back({ exportedLane.Points[pointIndex].first, exportedLane.Points[pointIndex].second });
							sidecar.Lanes.push_back(std::move(lane));
						}
						jamTake.MidiStreams.push_back({ filename, stream.Channel, stream.Device,
							stream.Loop.LoopLengthSamps, stream.Loop.AutomationGlobalSampleOrigin });
						auto& manifestStream = jamTake.MidiStreams.back();
						manifestStream.Id = stream.Id;
						const auto& overrides = stream.Loop.LoopQuantisation;
						manifestStream.MidiQuantEnabled = overrides.Enabled;
						if (overrides.Fraction)
							manifestStream.MidiQuantFraction = midi::MidiQuantisation::FractionIndex(*overrides.Fraction);
						manifestStream.PhaseOffsetSamps = overrides.PhaseOffsetSamps;

						MidiSnapshot midiSnapshot;
						midiSnapshot.FinalPath = exportDir + L"\\" + utils::DecodeUtf8(filename);
						midiSnapshot.TemporaryPath = midiSnapshot.FinalPath + L".tmp";
						midiSnapshot.Stream = std::move(sidecar);
						midiStreams.push_back(std::move(midiSnapshot));
					}

					jamStation.LoopTakes.push_back(std::move(jamTake));
				}

				jam.Stations.push_back(std::move(jamStation));
			}
			if (rigSnapshot)
			{
				for (const auto& runtime : rigSnapshot->Triggers)
				{
					if (!runtime.Instance || runtime.Id.empty())
						continue;
					io::JamFile::TriggerHistory history;
					history.TriggerId = runtime.Id;
					for (const auto& take : runtime.Instance->GetTakes())
						history.Takes.push_back({ static_cast<unsigned int>(take.SourceType), take.SourceTakeId, take.TargetTakeId });
					if (!history.Takes.empty())
						jam.TriggerHistories.push_back(std::move(history));
				}
			}
            return true;
        }();
        finishCapture();
        if (!captured) return result;

		if (jam.Stations.empty())
		{
			std::cout << "Export: nothing to export" << std::endl;
            appendError("Session capture or serialization failed"); return result;
		}

		std::stringstream jamStream;
		std::string serializationError;
		if (!io::JamFile::ToStream(jam, jamStream, &serializationError))
		{
			std::cout << "Export: failed to serialize session.jam: " << serializationError << std::endl;
            appendError("Session serialization failed: " + serializationError); return result;
		}

        // Audio has resumed before serialization and all disk work.
        // Manifest text is reparsed to verify schema before any publication.
        std::stringstream validation(jamStream.str());
        if (!JamFile::FromStream(std::move(validation)))
        { appendError("Serialized session failed validation"); return result; }
        files.Paths.reserve(loops.size() * 2 + midiStreams.size() * 2 + 1);
        for (const auto& loop : loops)
        {
            if (!files.WriteWav(loop.TemporaryPath, loop.Samples, sampleRate))
            { appendError("Failed writing WAV asset"); return result; }
        }
        for (const auto& midi : midiStreams)
        {
            std::stringstream bytes;
            std::string error;
            if (!NativeMidiSidecar::ToStream(midi.Stream, bytes, &error))
            { appendError("Failed serializing MIDI asset: " + error); return result; }
            if (!files.Write(midi.TemporaryPath, bytes.str()))
            { appendError("Failed writing MIDI asset"); return result; }
        }
        const auto jamPath = exportDir + L"\\" + utils::DecodeUtf8(manifestStem + ".jam");
        const auto temporaryJamPath = exportDir + L"\\" + utils::DecodeUtf8(assetPrefix + "manifest.tmp");
        if (!files.Write(temporaryJamPath, jamStream.str()))
        { appendError("Failed writing session manifest"); return result; }
        for (const auto& loop : loops)
        {
            if (!MoveFileExW(loop.TemporaryPath.c_str(), loop.FinalPath.c_str(), MOVEFILE_WRITE_THROUGH))
            { appendError("Failed publishing WAV asset"); return result; }
            try { files.Paths.push_back(loop.FinalPath); }
            catch (...) { DeleteFileW(loop.FinalPath.c_str()); throw; }
        }
        for (const auto& midi : midiStreams)
        {
            if (!MoveFileExW(midi.TemporaryPath.c_str(), midi.FinalPath.c_str(), MOVEFILE_WRITE_THROUGH))
            { appendError("Failed publishing MIDI asset"); return result; }
            try { files.Paths.push_back(midi.FinalPath); }
            catch (...) { DeleteFileW(midi.FinalPath.c_str()); throw; }
        }
        result.Path = jamPath;
        // Same-directory move, without REPLACE_EXISTING or COPY_ALLOWED: a
        // competing saver cannot replace an existing numbered version.
        if (!MoveFileExW(temporaryJamPath.c_str(), jamPath.c_str(), MOVEFILE_WRITE_THROUGH))
        { appendError("Manifest commit failed (possible version collision); retry Save"); return result; }
        files.Committed = true;
        result.Success = true;
        return result;
        }
        catch (const std::exception& error)
        {
            pause.Resume();
            result.Error += std::string("; session export failed: ") + error.what();
            return result;
        }
        catch (...)
        {
            pause.Resume();
            result.Error += "; session export failed";
            return result;
        }
    }
}
