#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

int main()
{
	auto screen = ftxui::ScreenInteractive::TerminalOutput();
	screen.TrackMouse();
	screen.ForceHandleCtrlC(false);
	auto view = ftxui::Renderer([] {
		return ftxui::vbox({
			ftxui::text("JAMMA"),
			ftxui::text("Waiting for Jamma...")
		});
	});
	view = ftxui::CatchEvent(view, [&screen](ftxui::Event event) {
		if (event == ftxui::Event::CtrlC) return true;
		if (event == ftxui::Event::Escape)
		{
			screen.Exit();
			return true;
		}
		if (event.is_mouse())
		{
			const auto& mouse = event.mouse();
			if (mouse.button == ftxui::Mouse::Left && mouse.motion == ftxui::Mouse::Moved)
				return true;
		}
		return false;
	});
	screen.Loop(view);
	return 0;
}
