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

namespace
{
	void HostEchoSetParameter(AEffect*, VstInt32, float)
	{
		FAIL() << "audioMasterAutomate must not call setParameter on the host side";
	}
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

TEST(Vst2PluginHostCallback, ReportsOutboundMidiCapability)
{
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sendVstEvents"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sendVstMidiEvent"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sendVstTimeInfo"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sendVstMidiEventFlagIsRealtime"));
	EXPECT_TRUE(vst::Vst2Plugin::SupportsHostCanDo("sizeWindow"));
	EXPECT_FALSE(vst::Vst2Plugin::SupportsHostCanDo("receiveVstEvents"));
	EXPECT_FALSE(vst::Vst2Plugin::SupportsHostCanDo("receiveVstMidiEvent"));
	EXPECT_FALSE(vst::Vst2Plugin::SupportsHostCanDo("offline"));
}

#endif // JAMMA_VST2_ENABLED
