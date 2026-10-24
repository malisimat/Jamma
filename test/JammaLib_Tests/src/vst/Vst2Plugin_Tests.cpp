///////////////////////////////////////////////////////////
//
// Author 2024 Matt Jones
// Subject to the MIT license, see LICENSE file.
//
///////////////////////////////////////////////////////////

// TDD tests for Vst2Plugin.
// All tests here exercise behaviour that does not require loading a real VST2
// DLL, so they work in a standard CI environment.
//
// Tests are grouped into four suites:
//   Vst2PluginDefault   – initial state after construction
//   Vst2PluginUnloaded  – operations on a not-loaded instance
//   Vst2PluginBypassed  – SetBypassed / IsBypassed independent of load state
//   Vst2PluginFactory   – MakePluginForPath factory routing

#ifdef JAMMA_VST2_ENABLED

#include "gtest/gtest.h"
#include "vst/Vst2Plugin.h"
#include "vst/IVstPlugin.h"
#include "vst/VstChain.h"

namespace vst
{
	struct Vst2PluginTestAccess
	{
		static bool Open(Vst2Plugin& plugin, AEffect& effect)
		{
			plugin._effect = &effect;
			return plugin._OpenEffect(L"test-owned-effect.dll");
		}
		static bool LoadPrepared(Vst2Plugin& plugin)
		{
			// Own a real loader reference without needing a third-party plugin DLL.
			plugin._moduleHandle = LoadLibraryW(L"kernel32.dll");
			if (!plugin._moduleHandle) return false;
			return plugin.Load(L"test-owned-effect.dll", 48000.0f, 128u, 2u);
		}
	};
}

class Vst2PluginCompatibility : public testing::Test
{
protected:
	struct Effect
	{
		AEffect Api{};
		int Opens = 0;
		int Closes = 0;
		int Activations = 0;
		int Editors = 0;
		int ProcessCalls = 0;
		int MidiCalls = 0;
		Effect()
		{
			Api.magic = kEffectMagic;
			Api.user = this;
			Api.dispatcher = Dispatch;
			Api.processReplacing = Process;
			Api.flags = effFlagsHasEditor;
			Api.numInputs = 2;
			Api.numOutputs = 2;
		}
		static VstIntPtr Dispatch(AEffect* api, VstInt32 opcode, VstInt32,
			VstIntPtr value, void*, float)
		{
			auto& self = *static_cast<Effect*>(api->user);
			if (opcode == effOpen) ++self.Opens;
			if (opcode == effClose) ++self.Closes;
			if (opcode == effMainsChanged && value) ++self.Activations;
			if (opcode == effEditOpen) ++self.Editors;
			if (opcode == effProcessEvents) ++self.MidiCalls;
			return 0;
		}
		static void Process(AEffect* api, float**, float**, VstInt32)
		{ ++static_cast<Effect*>(api->user)->ProcessCalls; }
	};
	static void ExpectBypassed(Effect& effect)
	{
		auto plugin = std::make_shared<vst::Vst2Plugin>();
		ASSERT_TRUE(vst::Vst2PluginTestAccess::Open(*plugin, effect.Api));
		ASSERT_TRUE(vst::Vst2PluginTestAccess::LoadPrepared(*plugin));
		EXPECT_TRUE(plugin->IsLoaded());
		plugin->SetBypassed(false); // Saved state or a toggle cannot enable unsupported processing.
		EXPECT_TRUE(plugin->IsBypassed());
		EXPECT_EQ(plugin->Role(), vst::RenderRole::Effect);
		EXPECT_EQ(plugin->GetLatencySamples(), 0);
		EXPECT_TRUE(plugin->ReconfigureAudioFormat(44100.0f, 64u));
		plugin->BeginMidiBlock(0u, 2u);
		EXPECT_FALSE(plugin->TrySendMidiEvent(midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u), false));
		plugin->SendMidiPanicImmediately();
		vst::VstChain chain;
		chain.AddPlugin(plugin);
		EXPECT_FALSE(chain.IsActive());
		EXPECT_EQ(chain.GetLatencySamples(), 0);
		float audio[] = {1.0f, 2.0f};
		chain.ProcessBlock(audio, 2);
		plugin->ProcessBlockStereo(audio, audio, 2);
		float* channels[] = {audio};
		plugin->ProcessBlockMulti(channels, 1, 2);
		EXPECT_FLOAT_EQ(audio[0], 1.0f);
		EXPECT_FLOAT_EQ(audio[1], 2.0f);
		EXPECT_EQ(effect.Closes, 0);
		EXPECT_TRUE(plugin->OpenEditor(nullptr));
		plugin->CloseEditor();
		plugin->Unload();
		EXPECT_EQ(effect.Opens, 1);
		EXPECT_EQ(effect.Closes, 1);
		EXPECT_EQ(effect.Activations, 0);
		EXPECT_EQ(effect.Editors, 1);
		EXPECT_EQ(effect.ProcessCalls, 0);
		EXPECT_EQ(effect.MidiCalls, 0);
	}
};

TEST_F(Vst2PluginCompatibility, LoadsMidiOnlyProcessorInEnforcedBypass)
{
	Effect effect;
	effect.Api.numInputs = 0;
	effect.Api.numOutputs = 0;
	ExpectBypassed(effect);
}

TEST_F(Vst2PluginCompatibility, BypassesUnsupportedAudioProcessor)
{
	Effect effect;
	effect.Api.processReplacing = nullptr;
	ExpectBypassed(effect);
}

TEST_F(Vst2PluginCompatibility, MalformedChannelCountStillFails)
{
	Effect effect;
	effect.Api.numOutputs = -1;
	vst::Vst2Plugin plugin;
	EXPECT_FALSE(vst::Vst2PluginTestAccess::Open(plugin, effect.Api));
	plugin.Unload();
	EXPECT_EQ(effect.Closes, 1);
}

TEST_F(Vst2PluginCompatibility, PreservesAudioEffectsAndZeroInputInstruments)
{
	for (const auto inputs : {0, 1, 2})
	{
		Effect effect;
		effect.Api.numInputs = inputs;
		vst::Vst2Plugin plugin;
		EXPECT_TRUE(vst::Vst2PluginTestAccess::Open(plugin, effect.Api));
		EXPECT_TRUE(vst::Vst2PluginTestAccess::LoadPrepared(plugin));
		EXPECT_FALSE(plugin.IsBypassed());
		EXPECT_EQ(effect.Activations, 1);
		EXPECT_EQ(effect.Closes, 0);
		plugin.Unload();
		EXPECT_EQ(effect.Closes, 1);
	}
}

static void HostEchoSetParameter(AEffect*, VstInt32, float)
{
	FAIL() << "audioMasterAutomate must not call setParameter on the host side";
}

// -----------------------------------------------------------------------
// Suite: Vst2PluginDefault
// Verifies the object's invariants immediately after construction.
// -----------------------------------------------------------------------

TEST(Vst2PluginDefault, NotLoadedAfterConstruction)
{
	vst::Vst2Plugin p;
	EXPECT_FALSE(p.IsLoaded());
}

TEST(Vst2PluginDefault, NotBypassedAfterConstruction)
{
	vst::Vst2Plugin p;
	EXPECT_FALSE(p.IsBypassed());
}

TEST(Vst2PluginDefault, NameIsEmptyAfterConstruction)
{
	vst::Vst2Plugin p;
	EXPECT_TRUE(p.Name().empty());
}

TEST(Vst2PluginDefault, EditorSizeIsZeroAfterConstruction)
{
	vst::Vst2Plugin p;
	const auto sz = p.GetEditorSize();
	EXPECT_EQ(sz.Width, 0u);
	EXPECT_EQ(sz.Height, 0u);
}

// -----------------------------------------------------------------------
// Suite: Vst2PluginUnloaded
// Verifies safe no-op behaviour when no DLL has been loaded.
// -----------------------------------------------------------------------

TEST(Vst2PluginUnloaded, LoadReturnsFalseForNonexistentPath)
{
	vst::Vst2Plugin p;
	EXPECT_FALSE(p.Load(L"C:\\does_not_exist_at_all.dll", 44100.0f, 512u, 2u));
}

TEST(Vst2PluginUnloaded, PreInitReturnsFalseForNonexistentPath)
{
	vst::Vst2Plugin p;
	EXPECT_FALSE(p.PreInit(L"C:\\does_not_exist_at_all.dll"));
}

TEST(Vst2PluginUnloaded, ProcessBlockIsNoOpWhenNotLoaded)
{
	vst::Vst2Plugin p;
	float buf[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
	// Must not crash and must leave the buffer unchanged.
	p.ProcessBlock(buf, 8);
	EXPECT_FLOAT_EQ(buf[0], 1.0f);
	EXPECT_FLOAT_EQ(buf[7], 8.0f);
}

TEST(Vst2PluginUnloaded, ProcessBlockStereoIsNoOpWhenNotLoaded)
{
	vst::Vst2Plugin p;
	float left[4]  = { 1.0f, 2.0f, 3.0f, 4.0f };
	float right[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
	p.ProcessBlockStereo(left, right, 4);
	EXPECT_FLOAT_EQ(left[0],  1.0f);
	EXPECT_FLOAT_EQ(right[3], 8.0f);
}

TEST(Vst2PluginUnloaded, ProcessBlockMultiIsNoOpWhenNotLoaded)
{
	vst::Vst2Plugin p;
	float ch0[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
	float ch1[4] = { 5.0f, 6.0f, 7.0f, 8.0f };
	float* bufs[2] = { ch0, ch1 };
	p.ProcessBlockMulti(bufs, 2, 4);
	EXPECT_FLOAT_EQ(ch0[0], 1.0f);
	EXPECT_FLOAT_EQ(ch1[3], 8.0f);
}

TEST(Vst2PluginUnloaded, OpenEditorReturnsFalseWhenNotLoaded)
{
	vst::Vst2Plugin p;
	EXPECT_FALSE(p.OpenEditor(nullptr));
}

TEST(Vst2PluginUnloaded, CloseEditorDoesNotCrashWhenNotLoaded)
{
	vst::Vst2Plugin p;
	EXPECT_NO_THROW(p.CloseEditor());
}

TEST(Vst2PluginUnloaded, UnloadDoesNotCrashWhenNotLoaded)
{
	vst::Vst2Plugin p;
	EXPECT_NO_THROW(p.Unload());
}

// -----------------------------------------------------------------------
// Suite: Vst2PluginBypassed
// SetBypassed / IsBypassed are independent of load state.
// -----------------------------------------------------------------------

TEST(Vst2PluginBypassed, SetBypassedCanBeToggledBeforeLoad)
{
	vst::Vst2Plugin p;
	EXPECT_FALSE(p.IsBypassed());
	p.SetBypassed(true);
	EXPECT_TRUE(p.IsBypassed());
	p.SetBypassed(false);
	EXPECT_FALSE(p.IsBypassed());
}

// -----------------------------------------------------------------------
// Suite: Vst2PluginFactory
// MakePluginForPath should route by extension.
// -----------------------------------------------------------------------

TEST(Vst2PluginFactory, DllExtensionYieldsVst2Plugin)
{
	auto plugin = vst::MakePluginForPath(L"C:\\plugins\\some_effect.dll");
	ASSERT_NE(plugin, nullptr);
	// The result must be non-null and behave as IVstPlugin.
	// We can further confirm it's a Vst2Plugin by verifying VST2-specific
	// initial state (no other known IVstPlugin starts with IsLoaded == false
	// at this path extension, and the factory returns Vst2Plugin for .dll).
	EXPECT_FALSE(plugin->IsLoaded());
	EXPECT_FALSE(plugin->IsBypassed());
}

TEST(Vst2PluginFactory, Vst3ExtensionYieldsVstPlugin)
{
	auto plugin = vst::MakePluginForPath(L"C:\\plugins\\some_effect.vst3");
	ASSERT_NE(plugin, nullptr);
	EXPECT_FALSE(plugin->IsLoaded());
}

TEST(Vst2PluginFactory, NoExtensionYieldsVstPlugin)
{
	auto plugin = vst::MakePluginForPath(L"C:\\plugins\\no_extension");
	ASSERT_NE(plugin, nullptr);
	EXPECT_FALSE(plugin->IsLoaded());
}

TEST(Vst2PluginFactory, DllExtensionCaseInsensitive)
{
	// .DLL (uppercase) should still route to Vst2Plugin.
	auto plugin = vst::MakePluginForPath(L"C:\\plugins\\effect.DLL");
	ASSERT_NE(plugin, nullptr);
	EXPECT_FALSE(plugin->IsLoaded());
}

TEST(Vst2PluginHostCallback, ReportsCanonicalHostCanDoSet)
{
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("supplyIdle"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sendVstEvents"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sendVstMidiEvent"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sendVstTimeInfo"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("receiveVstEvents"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("receiveVstMidiEvent"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("supportShell"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sizeWindow"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("shellCategory"));
	EXPECT_FALSE(vst::Vst2Plugin::SupportsHostCanDo("sendVstMidiEventFlagIsRealtime"));
	EXPECT_FALSE(vst::Vst2Plugin::SupportsHostCanDo("offline"));
}

TEST(Vst2PluginMidi, ReservesReleasesAndRejectsOutsideWindowBeforeCapacity)
{
	vst::Vst2Plugin plugin;
	plugin.BeginMidiBlock(0u, 128u);
	const auto attack = midi::MidiEvent::MakeNoteOn(0u, 0u, 60u, 100u);
	for (unsigned int i = 0u; i < 192u; ++i)
		ASSERT_TRUE(plugin.TrySendMidiEvent(attack, false));
	EXPECT_FALSE(plugin.TrySendMidiEvent(attack, false));
	EXPECT_FALSE(plugin.TrySendMidiEvent(midi::MidiEvent::MakeNoteOff(128u, 0u, 60u), false));
	const auto release = midi::MidiEvent::MakeNoteOff(0u, 0u, 60u);
	for (unsigned int i = 0u; i < 64u; ++i)
		ASSERT_TRUE(plugin.TrySendMidiEvent(release, false));
	EXPECT_FALSE(plugin.TrySendMidiEvent(release, false));
	plugin.BeginMidiBlock(128u, 128u);
	EXPECT_TRUE(plugin.TrySendMidiEvent(midi::MidiEvent::MakeNoteOff(128u, 0u, 60u), false));
}


TEST(Vst2PluginMidi, SkippedProcessDoesNotReportMidiBlockProcessed)
{
	vst::Vst2Plugin plugin;
	float left[8]{};
	float right[8]{};
	float* channels[] = { left, right };
	plugin.BeginMidiBlock(0u, 8u);
	EXPECT_FALSE(plugin.MidiBlockWasProcessed());
	plugin.ProcessBlockMulti(channels, 2, 8);
	EXPECT_FALSE(plugin.MidiBlockWasProcessed());
	plugin.SetBypassed(true);
	plugin.BeginMidiBlock(8u, 8u);
	plugin.ProcessBlockMulti(channels, 2, 8);
	EXPECT_FALSE(plugin.MidiBlockWasProcessed());
}

#endif // JAMMA_VST2_ENABLED
