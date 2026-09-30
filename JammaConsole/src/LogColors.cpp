#include "LogColors.h"

ftxui::Color LogLineColor(std::string_view line, const LogPalette& palette)
{
	// Severity takes precedence over subsystem so failures remain conspicuous.
	const auto has = [line](std::string_view word) { return line.find(word) != std::string_view::npos; };
	if (has("error") || has("Error") || has("failed") || has("Failed")
		|| has("unreadable") || has("invalid") || has("Invalid")
		|| has("Could not") || has("cannot") || has("quitting"))
		return palette.Error;
	if (has("warning") || has("Warning") || has("WARN") || has("fallback")
		|| has("unavailable") || has("not connected") || has("Not connected")
		|| has("Unknown command") || has("recovery needed"))
		return palette.Warning;
	if (line.starts_with("[NINJAM]"))
	{
		const auto message = line.substr(8);
		if (message.find("<you>") != std::string_view::npos) return palette.OwnChat;
		if (message.find("(private)") != std::string_view::npos) return palette.PrivateChat;
		if (message.find('<') != std::string_view::npos) return palette.Chat;
		if (message.starts_with(" Commands:") || message.starts_with(" Servers:")
			|| message.starts_with("   ")) return palette.System;
		return palette.Ninjam;
	}
	if (line.starts_with("[ASIO] ") && !line.starts_with("[ASIO] Attempt")
		|| line.starts_with("[MIDI] ") && !line.starts_with("[MIDI] Request"))
		return palette.System;
	if (line.starts_with("[Vst") || line.starts_with("[Automation]")
		|| line.starts_with("[Input]") || line.starts_with("[Serial]")
		|| line.starts_with("[tap]"))
		return palette.Verbose;
	if (line.starts_with("[ASIO]")) return palette.Audio;
	if (line.starts_with("[MIDI")) return palette.Midi;
	if (line.starts_with("[JAM]") || line.starts_with("[LOAD]")) return palette.Jam;
	if (line.starts_with("[RIG]")) return palette.Rig;
	if (line.starts_with("[Station]") || line.starts_with("[Loop")) return palette.Loop;
	if (line.starts_with("[WINDOW]")) return palette.Window;
	if (line.starts_with("[CONSOLE]")) return palette.Console;
	if (line.starts_with("[BOOT]")) return palette.Boot;
	return palette.Other;
}
