#pragma once
#include "IVstPlugin.h"

namespace vst
{
	// UI/job metadata only; never queried by the audio callback.
	struct VstRackSlot
	{
		bool Pending = false;
		bool Failed = false;
		bool RoleWasUnknown = false;
		std::optional<RenderRole> RoleOverride;
		std::wstring Path;
		std::string Identity;
		std::shared_ptr<IVstPlugin> Plugin;
	};
}
