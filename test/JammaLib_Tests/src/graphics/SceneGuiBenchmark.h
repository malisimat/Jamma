#pragma once

// Shared unchanged between the feature and archived code baseline. Run this
// opt-in test alone in a process with an external timeout (Window may show a
// modal driver error). Hardware playback is a separate verification gate.
#include "io/WavReadWriter.h"
#include "graphics/GlDeleteQueue.h"
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <cstdlib>

class SceneGuiBenchmarkCleanup
{
public:
	SceneGuiBenchmarkCleanup(graphics::Window& window, engine::Scene& scene) : Win(window), EngineScene(scene)
	{
		std::lock_guard<std::mutex> lock(graphics::GlDeleteQueue::_Mutex());
		PreviousOwner = graphics::GlDeleteQueue::_RenderThreadId();
	}
	~SceneGuiBenchmarkCleanup()
	{
		Win.Release(); EngineScene.Shutdown();
		graphics::GlDeleteQueue::SetRenderThread(PreviousOwner);
	}
private:
	graphics::Window& Win;
	engine::Scene& EngineScene;
	std::thread::id PreviousOwner{};
};

class SceneGuiBenchmark
{
public:
	static void Run()
	{
		const auto directory = std::getenv("JAMMA_GUI_BENCHMARK_DIR");
		if (!directory || !*directory) GTEST_SKIP() << "Set JAMMA_GUI_BENCHMARK_DIR for the isolated same-scene benchmark.";
		ASSERT_EQ(nullptr, wglGetCurrentContext());
		{
			std::lock_guard<std::mutex> lock(graphics::GlDeleteQueue::_Mutex());
			ASSERT_TRUE(graphics::GlDeleteQueue::_Queue().empty());
		}
		const std::filesystem::path output(directory);
		std::filesystem::create_directories(output / "fixture");
		constexpr unsigned int rate = 48000, length = 48000;
		std::vector<float> samples(length);
		for (size_t index = 0; index < samples.size(); ++index)
			samples[index] = 0.0025f * static_cast<float>(std::sin(6.283185307179586 * 220.0 * index / rate));
		ASSERT_TRUE(io::WavReadWriter().Write((output / "fixture" / "quiet-tone.wav").wstring(), samples,
			static_cast<unsigned int>(samples.size()), rate));
		io::JamFile jam{}; io::RigFile rig{};
		jam.Name = "Scene GUI common benchmark"; jam.MasterLengthSamps = length; jam.QuantiseSamps = 6000;
		for (unsigned int index = 0; index < 8; ++index) {
			io::JamFile::Station station{}; station.Name = "Station " + std::to_string(index);
			for (unsigned int takeIndex = 0; takeIndex < 2; ++takeIndex) {
				io::JamFile::LoopTake take{}; take.Name = station.Name + " Take " + std::to_string(takeIndex);
				io::JamFile::Loop loop{}; loop.Name = "quiet-tone.wav";
				loop.Id = station.Name + take.Name; loop.Length = length; loop.Level = 0.25; loop.Speed = 1.0;
				loop.Mix.Mix = io::JamFile::LoopMix::MIX_WIRE; loop.Mix.Params = std::vector<unsigned long>{ 0, 1 };
				take.Loops.push_back(loop); station.LoopTakes.push_back(take);
			}
			jam.Stations.push_back(station);
		}
		for (unsigned int index = 0; index < 24; ++index) {
			io::RigFile::Trigger trigger{}; trigger.Name = "Agjpq route " + std::to_string(index);
			trigger.InputChannels = { index % 8 };
			trigger.StationTarget = jam.Stations[index % 8].Name; rig.Triggers.push_back(trigger);
		}
		resources::ResourceLib resources;
		const auto loaded = engine::Scene::FromFile(engine::SceneParams({ "" }, {}, { 1000u, 650u }), jam, rig,
			(output / "fixture").wstring());
		ASSERT_TRUE(loaded); auto scene = *loaded;
		scene->CommitChanges(); // Publish the loaded audio loops before inspecting them.
		ASSERT_EQ(8u, scene->SnapshotStations().size());
		for (const auto& station : scene->SnapshotStations()) {
			ASSERT_EQ(2u, station->GetLoopTakes().size());
			for (const auto& take : station->GetLoopTakes()) ASSERT_EQ(1u, take->GetLoops().size());
		}
		graphics::Window window(*scene, resources); SceneGuiBenchmarkCleanup cleanup(window, *scene);
		ASSERT_EQ(0, window.Create(GetModuleHandleW(nullptr), SW_HIDE));
		const auto ownedWindow = WindowFromDC(wglGetCurrentDC()); ASSERT_NE(nullptr, ownedWindow);
		std::ofstream metadata(output / "metadata.txt");
		metadata << "scene=8 stations,16 mono loops,24 triggers,24 logical capture routes\n"
			<< "renderer=" << glGetString(GL_RENDERER) << "\nversion=" << glGetString(GL_VERSION)
			<< "\nplayback=not started; underruns unmeasured\nconfiguration=Debug x64\n"
			<< "swap_interval=1; swap/total may include vsync wait\n"
			<< "idle=cable reveal held; input scenarios do not assert panel/scroll/cable changes\n";
		std::ofstream csv(output / "frames.csv"); ASSERT_TRUE(csv.good());
		csv << "scenario,frame,dispatch_ms,render_gpu_ms,swap_ms,total_ms\n" << std::fixed << std::setprecision(6);
		actions::KeyAction control; control.KeyChar = VK_OEM_3; control.KeyActionType = actions::KeyAction::KEY_DOWN;
		window.OnAction(control);
		for (int frame = 0; frame < 30; ++frame) { window.Render(); glFinish(); window.Swap(); }
		for (const auto scenario : { "idle-cables", "panel-edge-input", "wheel-input", "native-resize", "pointer-input" })
			for (unsigned int frame = 0; frame < 120; ++frame) {
				const auto begin = std::chrono::steady_clock::now();
				if (std::string(scenario) == "panel-edge-input" && frame % 30 == 0) Click(window, { 30, 10 });
				if (std::string(scenario) == "wheel-input") {
					actions::TouchAction wheel; wheel.Touch = actions::TouchAction::TOUCH_MOUSE;
					wheel.State = actions::TouchAction::TOUCH_DOWN; wheel.Index = 4;
					wheel.Value = frame < 60 ? -1 : 1; wheel.Position = { 920, 320 };
					window.OnAction(wheel);
				}
				if (std::string(scenario) == "native-resize" && frame % 10 == 0) {
					const utils::Size2d client = frame % 20 == 0 ? utils::Size2d{ 640, 480 } : utils::Size2d{ 1000, 650 };
					const auto outer = graphics::Window::AdjustSize(client, static_cast<DWORD>(GetWindowLongPtrW(ownedWindow, GWL_STYLE)));
					ASSERT_TRUE(SetWindowPos(ownedWindow, nullptr, 0, 0, outer.Width, outer.Height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE));
					ASSERT_EQ(client.Width, window.GetSize().Width); ASSERT_EQ(client.Height, window.GetSize().Height);
				}
				if (std::string(scenario) == "pointer-input") {
					if (frame % 30 == 0) {
						actions::TouchAction down; down.Touch = actions::TouchAction::TOUCH_MOUSE; down.Index = 0;
						down.State = actions::TouchAction::TOUCH_DOWN; down.Position = { 880, 580 }; window.OnAction(down);
					}
					actions::TouchMoveAction move; move.Touch = actions::TouchAction::TOUCH_MOUSE;
					move.Index = 0; move.MouseButtonsDown = 1; move.Position = { 500 + static_cast<int>(frame % 30) * 8, 350 };
					window.OnAction(move); if (frame % 30 == 29) window.CancelMouseCapture();
				}
				const auto dispatched = std::chrono::steady_clock::now();
				window.Render(); glFinish(); const auto rendered = std::chrono::steady_clock::now();
				window.Swap(); const auto end = std::chrono::steady_clock::now();
				csv << scenario << ',' << frame << ',' << Milliseconds(dispatched - begin) << ',' << Milliseconds(rendered - dispatched) << ','
					<< Milliseconds(end - rendered) << ',' << Milliseconds(end - begin) << '\n';
				ASSERT_EQ(GL_NO_ERROR, glGetError());
			}
		control.KeyActionType = actions::KeyAction::KEY_UP; window.OnAction(control);
		EXPECT_TRUE(csv.good());
	}
private:
	static double Milliseconds(std::chrono::steady_clock::duration value)
	{
		return std::chrono::duration<double, std::milli>(value).count();
	}
	static void Click(graphics::Window& window, utils::Position2d position)
	{
		actions::TouchAction touch; touch.Touch = actions::TouchAction::TOUCH_MOUSE;
		touch.Index = 0; touch.Position = position; touch.State = actions::TouchAction::TOUCH_DOWN; window.OnAction(touch);
		touch.State = actions::TouchAction::TOUCH_UP; window.OnAction(touch);
	}
};

TEST(GuiBenchmark, SameSceneFrameDistribution) { SceneGuiBenchmark::Run(); }
