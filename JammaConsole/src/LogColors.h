#pragma once

#include <ftxui/screen/color.hpp>
#include <string_view>

// Presentation policy for the companion console. Override palette fields when
// console color preferences are introduced; log producers remain independent.
struct LogPalette
{
	ftxui::Color Error = ftxui::Color::RGB(255, 92, 96);
	ftxui::Color Warning = ftxui::Color::RGB(255, 166, 74);
	ftxui::Color System = ftxui::Color::White;
	ftxui::Color Verbose = ftxui::Color::RGB(145, 151, 160);
	ftxui::Color Boot = ftxui::Color::RGB(255, 222, 110);
	ftxui::Color Console = ftxui::Color::RGB(244, 215, 115);
	ftxui::Color Audio = ftxui::Color::RGB(245, 208, 99);
	ftxui::Color Midi = ftxui::Color::RGB(210, 217, 98);
	ftxui::Color Jam = ftxui::Color::RGB(245, 195, 102);
	ftxui::Color Rig = ftxui::Color::RGB(228, 211, 113);
	ftxui::Color Loop = ftxui::Color::RGB(247, 190, 125);
	ftxui::Color Window = ftxui::Color::RGB(229, 199, 133);
	ftxui::Color Ninjam = ftxui::Color::RGB(123, 213, 224);
	ftxui::Color Chat = ftxui::Color::CyanLight;
	ftxui::Color OwnChat = ftxui::Color::GreenLight;
	ftxui::Color PrivateChat = ftxui::Color::MagentaLight;
	ftxui::Color Other = ftxui::Color::RGB(236, 202, 112);
};

ftxui::Color LogLineColor(std::string_view line, const LogPalette& palette);
