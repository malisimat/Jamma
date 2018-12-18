///////////////////////////////////////////////////////////
//
// Copyright(c) 2018-2019 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#include "Main.h"
#include "Window.h"


int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	AllocConsole();
	FILE* newStdout = nullptr;
	FILE* newStderr = nullptr;
	FILE* newStdin = nullptr;
	freopen_s(&newStdout, "CONOUT$", "w", stdout);
	freopen_s(&newStderr, "CONOUT$", "w", stderr);
	freopen_s(&newStdin, "CONIN$", "r", stdin);

	Scene scene(1024, 768);
	Window window(scene);

	if (window.Create(hInstance, nCmdShow) != 0)
		PostQuitMessage(1);

	MSG msg;
	bool active = true;
	while (active)
	{
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				active = false;
			}

			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}

		window.Render();
		window.Swap();
	}

	window.Destroy();

	return (int)msg.wParam;
}