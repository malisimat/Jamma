#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include "../io/RigFile.h"
#include "../midi/MidiRouter.h"

namespace engine
{
	// Borrowed non-audio controls. Window/startup callers must keep the owning
	// Scene alive and must not retain this reference across Scene replacement.
	// Calls retain their existing owner-thread/serialization requirements; this
	// interface grants no access to lifecycle locks, writer leases or pumps.
	class SessionControls
	{
	public:
		enum class RigSaveMode { Save, SaveAs, StartupDefault };
		virtual ~SessionControls() = default;
		virtual void SetMidiInputInventory(midi::MidiInputInventory inventory) = 0;
		virtual midi::MidiConnectionResult GetMidiConnectionResult() const = 0;
		virtual void RequestMidiRefresh() noexcept = 0;
		virtual void RequestAudioRefresh() noexcept = 0;
		virtual void CloseAudio() = 0;
		virtual void SetSessionSaveFolder(std::wstring folder) = 0;
		virtual void SetDefaultJamSaver(std::wstring path, std::function<bool(const std::wstring&, bool)> saver) = 0;
		virtual void SetCurrentJamPath(std::wstring path) = 0;
		virtual const std::wstring& CurrentJamPath() const noexcept = 0;
		virtual void ReportFileOutcome(const std::string& status) = 0;
		virtual void RequestSessionLoad(std::wstring path) = 0;
		virtual void PickSessionForLoad() = 0;
		virtual bool ConsumePanelLoadRequest() = 0;
		virtual std::wstring TakeReadySessionLoad() = 0;
		virtual RigSaveMode RequestedRigSaveMode() const noexcept = 0;
	};
}
