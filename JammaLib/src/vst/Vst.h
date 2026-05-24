#pragma once

// VST hosting — includes both VST3 and VST2 plugin hosts plus the common
// interface and chain.  Use MakePluginForPath() to create the right type
// based on file extension.
#include "IAnyVstPlugin.h"
#include "VstPlugin.h"
#include "Vst2Plugin.h"
#include "VstChain.h"

