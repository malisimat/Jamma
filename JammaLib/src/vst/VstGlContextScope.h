///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

#pragma once

#include <windows.h>

namespace vst
{
	// RAII guard that snapshots the current OpenGL context on construction
	// and restores it on destruction. Some plugins (VST2 and VST3 alike —
	// e.g. Battery 4, several VSTGUI-based VST3 plugins) make their own GL
	// context current during module init, effOpen/effEditOpen/effEditIdle,
	// or IPlugView::attached()/removed(). Those calls run on Jamma's OpenGL
	// render/UI thread, so without restoring our context afterwards the
	// framebuffer becomes incomplete and the whole app paints white.
	//
	// Shared by Vst2Plugin and Vst3Plugin; construct one around any call that
	// may create or switch a UI graphics context.
	struct VstGlContextScope
	{
		VstGlContextScope() noexcept : _rc(wglGetCurrentContext()), _dc(wglGetCurrentDC()) {}

		~VstGlContextScope()
		{
			// Only restore if there was a context to begin with (non-render
			// threads, e.g. the job-thread fallback, have none — leave them
			// untouched).
			if (_rc)
				wglMakeCurrent(_dc, _rc);
		}

		VstGlContextScope(const VstGlContextScope&) = delete;
		VstGlContextScope& operator=(const VstGlContextScope&) = delete;

		HGLRC _rc;
		HDC _dc;
	};
}
