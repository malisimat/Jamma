#pragma once

// VST hosting — includes both VST3 and VST2 plugin hosts plus the common
// interface and chain.  Use MakePluginForPath() to create the right type
// based on file extension.
#include "IVstPlugin.h"
#include "Vst3Plugin.h"
#include "Vst2Plugin.h"
#include "VstChain.h"

