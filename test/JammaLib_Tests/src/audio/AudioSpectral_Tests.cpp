#include "gtest/gtest.h"
#include "audio/AudioSpectralDsp.h"
#include "audio/AudioSpectralEnvelope.h"
#include "audio/AudioSpectralGainField.h"
#include "audio/AudioSpectralPlaybackSnapshot.h"
#include "audio/AudioSpectralResourceBudget.h"
#include "audio/AudioSpectralEditState.h"
#include "engine/Loop.h"
#include "editor/AudioSpectralEditor.h"
#include "editor/AudioSpectralPicking.h"
#include "base/BounceWriter.h"
#include "resources/ResourcePaths.h"
#include "graphics/LoopGridProjection.h"
#include "glm/gtc/matrix_transform.hpp"
#include <chrono>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <set>
#include <future>
#include "audio/Hanning.h"
#include <algorithm>
#include <cmath>
#include <limits>

class AudioSpectralTest : public testing::Test
{
protected:
	class BounceCapture final : public base::BounceWriter
	{
	public:
		std::vector<float> Samples;
		void WriteBlock(const std::shared_ptr<base::MultiAudioSink>, const float* samples,
			unsigned int count, unsigned int) override
		{
			Samples.assign(samples, samples + count);
		}
	};
	static std::shared_ptr<audio::AudioSpectralSource> Source(unsigned long length, bool silence = false)
	{
		auto source = std::make_shared<audio::AudioSpectralSource>();
		source->Identity = std::make_shared<audio::AudioSpectralSourceIdentity>();
		source->Generation = 1u;
		source->Layout = { length, length + 37ul, 8u, 48000.0f };
		source->Samples.resize(source->Layout.StoredSampleCount());
		for (std::size_t i = 0; i < source->Samples.size(); ++i)
			source->Samples[i] = silence ? 0.0f : static_cast<float>(0.3 * std::sin(i * 0.13) + 0.1 * std::cos(i * 0.017));
		return source;
	}
	static audio::AudioSpectralTransformConfig Transform()
	{
		audio::AudioSpectralTransformConfig transform;
		transform.WindowSamps = 256u;
		transform.HopSamps = 64u;
		return transform;
	}
	static std::shared_ptr<engine::Loop> RecordedLoop(unsigned long length = 4096ul)
	{
		audio::WireMixBehaviourParams behaviour;
		behaviour.Channels = { 0 };
		audio::AudioMixerParams mixer;
		mixer.Size = { 160, 320 };
		mixer.Position = { 6, 6 };
		mixer.Behaviour = behaviour;
		engine::LoopParams params;
		params.Wav = "hh";
		params.Size = { 80, 80 };
		params.Position = { 10, 22 };
		auto loop = std::make_shared<engine::Loop>(params, mixer);
		loop->SetSampleRate(48000.0f);
		loop->Record();
		std::vector<float> samples(constants::MaxLoopFadeSamps + length, 0.5f);
		base::AudioWriteRequest request;
		request.samples = samples.data();
		request.numSamps = static_cast<unsigned int>(samples.size());
		request.stride = 1;
		request.fadeCurrent = 0.0f;
		request.fadeNew = 1.0f;
		request.source = base::Audible::AUDIOSOURCE_ADC;
		loop->OnBlockWrite(request, 0);
		loop->EndWrite(request.numSamps, true);
		loop->Play(constants::MaxLoopFadeSamps, length, false);
		return loop;
	}
	template<class Predicate>
	static bool Pump(editor::AudioSpectralEditor& editor, const std::shared_ptr<engine::Loop>& loop, Predicate ready)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			// Simulated audio runs faster than wall time: state/receipt tests must
			// not depend on Windows Sleep(1) granularity for long source copies.
			for (unsigned int boundary = 0; boundary < 8; ++boundary) loop->EndMultiPlay(64u);
			editor.Update();
			if (ready()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		return false;
	}
	static graphics::AudioSpectralModel::Surface PickingSurface()
	{
		auto display = std::make_shared<graphics::AudioSpectralDisplay>();
		display->SourceIdentity = std::make_shared<audio::AudioSpectralSourceIdentity>();
		display->SourceGeneration = display->Revision = 1u;
		display->TimeSegments = 4u;
		display->FrequencySegments = 2u;
		display->MinimumFrequencyHz = 20.0f;
		display->NyquistHz = 24000.0f;
		display->MaximumFrequencyHz = 24000.0f;
		display->Heights.resize(15u, 0.0f);
		return { display, 100.0f, 1.0f, 0.0f };
	}
};

TEST_F(AudioSpectralTest, UnityRoundTripCoversShortAndNonHopLengthsAndPreservesContext)
{
	std::string reason;
	for (const auto length : { 1ul, 7ul, 63ul, 64ul, 65ul, 511ul, 1031ul })
	{
		SCOPED_TRACE(length);
		const auto source = Source(length);
		const auto analysis = audio::AudioSpectralDsp::Analyze(source, Transform(), reason);
		ASSERT_TRUE(analysis) << reason;
		const auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
		ASSERT_TRUE(gains) << reason;
		const auto exact = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
		ASSERT_TRUE(exact) << reason;
		EXPECT_TRUE(exact->UsesOriginal());
		EXPECT_EQ(exact->Samples().data(), source->Samples.data());
		const auto roundTrip = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason, {}, true);
		ASSERT_TRUE(roundTrip) << reason;
		const auto first = analysis->Geometry.TargetFirstStorage;
		const auto end = first + analysis->Geometry.TargetLength;
		for (unsigned long i = 0; i < source->Samples.size(); ++i)
		{
			if (i >= first && i < end) EXPECT_NEAR(roundTrip->Samples()[i], source->Samples[i], 2e-6f) << i;
			else EXPECT_FLOAT_EQ(roundTrip->Samples()[i], source->Samples[i]) << i;
		}
	}
}

TEST_F(AudioSpectralTest, BoostCannotCreateSoundFromZeroCoefficients)
{
	std::string reason;
	const auto analysis = audio::AudioSpectralDsp::Analyze(Source(1031ul, true), Transform(), reason);
	ASSERT_TRUE(analysis) << reason;
	auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(gains);
	EXPECT_FALSE(gains->PaintOvalSegment(*analysis, 0.0, 3000.0, 0.01, 3000.0, 1.0, true, reason));
	EXPECT_TRUE(gains->IsUnity());
	for (std::size_t frame = 0; frame < gains->FrameCount(); ++frame)
		for (unsigned int bin = 0; bin < gains->BinCount(); ++bin)
			gains->SetGainDb(frame, bin, audio::AudioSpectralConfig::MaximumGainDb);
	const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
	ASSERT_TRUE(pcm) << reason;
	for (const auto sample : pcm->Samples()) EXPECT_FLOAT_EQ(sample, 0.0f);
}

TEST_F(AudioSpectralTest, UniformSpectralGainScalesCapturedSignalWithoutChangingPhaseOrTail)
{
	std::string reason;
	const auto source = Source(1031ul);
	const auto analysis = audio::AudioSpectralDsp::Analyze(source, Transform(), reason);
	ASSERT_TRUE(analysis) << reason;
	auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(gains);
	const auto original = *gains;
	for (std::size_t frame = 0; frame < gains->FrameCount(); ++frame)
		for (unsigned int bin = 0; bin < gains->BinCount(); ++bin)
			gains->SetGainDb(frame, bin, 6.0f);
	EXPECT_TRUE(original.IsUnity()); // Published before-state shares immutable COW tiles.
	const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
	ASSERT_TRUE(pcm) << reason;
	const auto factor = static_cast<float>(std::pow(10.0, 6.0 / 20.0));
	const auto first = analysis->Geometry.TargetFirstStorage;
	const auto end = first + analysis->Geometry.TargetLength;
	for (unsigned long i = first; i < end; ++i)
		EXPECT_NEAR(pcm->Samples()[i], source->Samples[i] * factor, 3e-6f) << i;
	for (unsigned long i = end; i < source->Samples.size(); ++i)
		EXPECT_FLOAT_EQ(pcm->Samples()[i], source->Samples[i]);
}

TEST_F(AudioSpectralTest, DefaultTransformRoundTripCoversNonHopAlignedBody)
{
	std::string reason;
	const auto source = Source(4139ul);
	const auto analysis = audio::AudioSpectralDsp::Analyze(source, {}, reason);
	ASSERT_TRUE(analysis) << reason;
	const auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(gains);
	const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason, {}, true);
	ASSERT_TRUE(pcm) << reason;
	for (unsigned long i = analysis->Geometry.TargetFirstStorage;
		i < analysis->Geometry.TargetFirstStorage + analysis->Geometry.TargetLength; ++i)
		EXPECT_NEAR(pcm->Samples()[i], source->Samples[i], 3e-6f) << i;
}

TEST_F(AudioSpectralTest, Legacy4096TransformRoundTripCoversNonHopAlignedBody)
{
	std::string reason;
	const auto source = Source(4139ul);
	const auto analysis = audio::AudioSpectralDsp::Analyze(source, { 1u, 4096u, 1024u }, reason);
	ASSERT_TRUE(analysis) << reason;
	const auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(gains);
	const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason, {}, true);
	ASSERT_TRUE(pcm) << reason;
	for (unsigned long i = analysis->Geometry.TargetFirstStorage;
		i < analysis->Geometry.TargetFirstStorage + analysis->Geometry.TargetLength; ++i)
		EXPECT_NEAR(pcm->Samples()[i], source->Samples[i], 3e-6f) << i;
}

TEST_F(AudioSpectralTest, OvalExposureIsIndependentOfEventSubdivision)
{
	std::string reason;
	const auto analysis = audio::AudioSpectralDsp::Analyze(Source(48000ul), Transform(), reason);
	ASSERT_TRUE(analysis) << reason;
	auto whole = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(whole);
	auto split = *whole;
	ASSERT_TRUE(whole->PaintOvalSegment(*analysis, 0.97, 3000.0, 1.03, 3000.0, 0.2, true, reason));
	for (int i = 0; i < 10; ++i)
		split.PaintOvalSegment(*analysis, 0.97 + i * 0.006, 3000.0, 0.97 + (i + 1) * 0.006, 3000.0, 0.02, true, reason);
	for (std::size_t frame = 0; frame < whole->FrameCount(); ++frame)
		for (unsigned int bin = 0; bin < whole->BinCount(); ++bin)
			// Ten float updates near the gain ceiling can differ by several ULPs.
            EXPECT_NEAR(whole->GainDb(frame, bin), split.GainDb(frame, bin), 2e-5f);
}

TEST_F(AudioSpectralTest, AbsoluteBarIsIdempotentAndLaterEqualInfluenceRestoresUnity)
{
	std::string reason;
	const auto before = audio::AudioSpectralEnvelope::Unity(1000ul, reason);
	ASSERT_TRUE(before);
	auto stroke = audio::AudioSpectralEnvelopeStroke::Begin(before, 1000.0f, reason);
	ASSERT_TRUE(stroke);
	ASSERT_TRUE(stroke->PaintSegment(200.0, 0.0f, 200.0, 0.0f));
	const auto once = stroke->Snapshot(reason);
	ASSERT_TRUE(once);
	for (int i = 0; i < 100; ++i) ASSERT_TRUE(stroke->PaintSegment(200.0, 0.0f, 200.0, 0.0f));
	const auto repeated = stroke->Snapshot(reason);
	ASSERT_TRUE(repeated);
	EXPECT_TRUE(std::equal(once->Gains().begin(), once->Gains().end(), repeated->Gains().begin()));
	EXPECT_FLOAT_EQ(repeated->GainAt(200ul), 0.0f);
	EXPECT_FLOAT_EQ(repeated->GainAt(500ul), 1.0f);
	// A weaker feather cannot replace the full-strength zero at the authored centre.
	ASSERT_TRUE(stroke->PaintSegment(240.0, 2.0f, 240.0, 2.0f));
	EXPECT_FLOAT_EQ(stroke->Snapshot(reason)->GainAt(200ul), 0.0f);
	ASSERT_TRUE(stroke->PaintSegment(200.0, 1.0f, 200.0, 1.0f));
	EXPECT_FLOAT_EQ(stroke->Snapshot(reason)->GainAt(200ul), 1.0f);
	// A new absolute stroke can reopen an already muted original region.
	auto restore = audio::AudioSpectralEnvelopeStroke::Begin(once, 1000.0f, reason);
	ASSERT_TRUE(restore);
	ASSERT_TRUE(restore->PaintSegment(200.0, 1.0f, 200.0, 1.0f));
	EXPECT_FLOAT_EQ(restore->Snapshot(reason)->GainAt(200ul), 1.0f);
}

TEST_F(AudioSpectralTest, RelativeBandPreservesBaselineSilenceWrapAndReversesExactly)
{
	std::string reason;
	std::vector<float> initial(1000ul, 0.5f);
	initial[0] = 0.0f;
	initial[1] = audio::AudioSpectralEnvelope::MaximumGain();
	const auto before = audio::AudioSpectralEnvelope::FromGains(initial, reason);
	auto stroke = audio::AudioSpectralEnvelopeStroke::Begin(before, 1000.0f, reason, 0.02);
	ASSERT_TRUE(stroke) << reason;
	EXPECT_FALSE(stroke->SetRelativeBand(0.25, 1.0f));
	EXPECT_EQ(stroke->Snapshot(reason), before);
	ASSERT_TRUE(stroke->SetRelativeBand(0.25, 2.0f));
	const auto boosted = stroke->Snapshot(reason);
	ASSERT_TRUE(boosted);
	EXPECT_GT(boosted->GainAt(0), 0.0f);
	EXPECT_FLOAT_EQ(boosted->GainAt(1), audio::AudioSpectralEnvelope::MaximumGain());
	EXPECT_GT(boosted->GainAt(999), 0.5f);
	EXPECT_FLOAT_EQ(boosted->GainAt(998), boosted->GainAt(2));
	EXPECT_FLOAT_EQ(boosted->GainAt(20), 0.5f);
	EXPECT_FALSE(stroke->SetRelativeBand(0.25, 2.0f)); // Stationary targets are idempotent.
	ASSERT_TRUE(stroke->SetRelativeBand(0.25, 0.0f));
	EXPECT_FLOAT_EQ(stroke->Snapshot(reason)->GainAt(998), stroke->Snapshot(reason)->GainAt(2));
	ASSERT_TRUE(stroke->SetRelativeBand(0.25, 1.0f));
	EXPECT_FALSE(stroke->HasChanges());
	EXPECT_EQ(stroke->Snapshot(reason), before);
}

TEST_F(AudioSpectralTest, SpectralControlsWheelPrecedenceLimitsAndProjectedPanZoom)
{
	using Controls = editor::AudioSpectralControls;
	EXPECT_EQ(Controls::WheelBinding(true, true, true), Controls::WheelMode::Reserved);
	EXPECT_EQ(Controls::WheelBinding(false, true, true), Controls::WheelMode::Reserved);
	EXPECT_EQ(Controls::WheelBinding(false, false, true), Controls::WheelMode::Reserved);
	EXPECT_EQ(Controls::WheelBinding(false, false, false), Controls::WheelMode::Brush);
	Controls controls;
	const auto band = controls.BandRadius;
	for (int i = 0; i < 200; ++i) controls.Wheel(Controls::WheelMode::Brush, 1);
	EXPECT_DOUBLE_EQ(controls.TimeRadius, 2.0);
	EXPECT_DOUBLE_EQ(controls.FrequencyRadius, 4.0);
	EXPECT_DOUBLE_EQ(controls.BandRadius, band);
	for (int i = 0; i < 200; ++i) controls.Wheel(Controls::WheelMode::Brush, -1);
	EXPECT_DOUBLE_EQ(controls.TimeRadius, 0.005);
	EXPECT_DOUBLE_EQ(controls.FrequencyRadius, 0.0625);
	for (int i = 0; i < 200; ++i) controls.Wheel(Controls::WheelMode::Band, 1);
	EXPECT_DOUBLE_EQ(controls.BandRadius, 2.0);
	for (int i = 0; i < 200; ++i) controls.Wheel(Controls::WheelMode::Band, -1);
	EXPECT_DOUBLE_EQ(controls.BandRadius, 0.005);
	controls.Viewport = {0.25, 0.75};
	controls.BeginView(0.5);
	controls.MoveView(-256.0, 0.75);
	EXPECT_DOUBLE_EQ(controls.Viewport.Upper - controls.Viewport.Lower, 0.25);
	EXPECT_DOUBLE_EQ(controls.Viewport.Lower + 0.75 * 0.25, 0.5);
	controls.MoveView(-100000.0, 0.5);
	EXPECT_DOUBLE_EQ(controls.Viewport.Upper - controls.Viewport.Lower, 0.0625);
	controls.MoveView(100000.0, -100.0);
	EXPECT_EQ(controls.Viewport, (graphics::AudioSpectralViewport{0, 1}));
	for (int i = 0; i < 30; ++i) controls.Wheel(Controls::WheelMode::Zoom, 1);
	EXPECT_DOUBLE_EQ(controls.Viewport.Upper - controls.Viewport.Lower, 0.0625);
	for (int i = 0; i < 100; ++i) controls.Wheel(Controls::WheelMode::Reserved, 1);
	EXPECT_DOUBLE_EQ(controls.Viewport.Upper, controls.Viewport.Lower + 0.0625);
	for (int i = 0; i < 100; ++i) controls.Wheel(Controls::WheelMode::Reserved, -1);
	EXPECT_DOUBLE_EQ(controls.Viewport.Upper, controls.Viewport.Lower + 0.0625);
}

TEST_F(AudioSpectralTest, NavigationUsesCanonicalDisplayAndClipsPickingToVisibleRectangle)
{
    std::string reason;
    const auto analysis = audio::AudioSpectralDsp::Analyze(Source(48000ul), Transform(), reason);
    ASSERT_TRUE(analysis);
    const auto display = graphics::AudioSpectralModel::BuildDisplay(*analysis, nullptr, nullptr, 1, 16, 8, reason, {}, {0.25, 0.75}, 2);
    ASSERT_TRUE(display) << reason;
    EXPECT_TRUE(display->IncludesDc);
    EXPECT_EQ(display->Viewport, graphics::AudioSpectralViewport{});
    EXPECT_EQ(display->MinimumFrequencyHz, 20.0f);
    EXPECT_EQ(display->MaximumFrequencyHz, 24000.0f);
    auto surface = PickingSurface(); surface.Display = display;
    surface.TimeViewport = {0.23, 0.68}; surface.FrequencyViewport = {0.21, 0.74};
    const auto hit = editor::AudioSpectralPicking::Intersect(surface, {0, 200, 0}, {0, -1, 0});
    ASSERT_TRUE(hit);
    EXPECT_NEAR(hit->TimeU, 0.455, 1e-6);
    EXPECT_NEAR(hit->FrequencyV, 0.475, 1e-6);
    EXPECT_EQ(surface.Display, display);
    EXPECT_FALSE(editor::AudioSpectralPicking::Intersect(surface, {101, 200, 0}, {0, -1, 0}));
    // Boundary lies inside a cell; picking clips triangles exactly as fragments do.
    const auto edge = editor::AudioSpectralPicking::Intersect(surface, {-100, 200, 78}, {0, -1, 0});
    ASSERT_TRUE(edge);
    EXPECT_NEAR(edge->TimeU, 0.23, 1e-6); EXPECT_NEAR(edge->FrequencyV, 0.21, 1e-6);
    gui::GuiModelParams params; graphics::AudioSpectralModel model(params, 16, 8);
    ASSERT_TRUE(model.SetDisplay(display, reason));
    const auto old = graphics::AudioSpectralModel::BuildDisplay(*analysis, nullptr, nullptr, 1, 16, 8, reason, {}, {}, 1);
    EXPECT_FALSE(model.SetDisplay(old, reason));
    model.SetViewport({0.2, 0.5}, {0.3, 0.7});
    EXPECT_EQ(display->PresentationRevision, 2u); // Renderer view never mutates its source data receipt.
    const auto outsideVertex = graphics::AudioSpectralModel::LocalVertex(surface, 0, 0);
    EXPECT_FALSE(editor::AudioSpectralPicking::Intersect(surface, outsideVertex + glm::vec3(0, 200, 0), {0, -1, 0}));
}

TEST_F(AudioSpectralTest, ZoomPickingIgnoresHiddenHighTriangleBeforeVisibleSurface)
{
    auto surface = PickingSurface();
    auto display = std::make_shared<graphics::AudioSpectralDisplay>(*surface.Display);
    // Hidden first column is high, visible final half is flat. A horizontal ray
    // intersects the hidden slope first but must reach the clipped visible edge.
    for (unsigned int row = 0; row <= display->FrequencySegments; ++row)
    {
        display->Heights[row * 5] = display->Heights[row * 5 + 4] = 1.0f;
        display->Heights[row * 5 + 1] = 1.0f;
    }
    surface.Display = display; surface.TimeViewport = {0.4, 0.8};
    const auto hit = editor::AudioSpectralPicking::Intersect(surface, {-300, 3, 0}, {1, 0, 0});
    ASSERT_TRUE(hit);
    EXPECT_GE(hit->TimeU, 0.4 - 1e-6);
    EXPECT_LE(hit->TimeU, 0.8 + 1e-6);
    EXPECT_GE(hit->Distance, 200.0 - 1e-6);
}

TEST_F(AudioSpectralTest, NavigationDoesNotQueueOrCancelPresentationDuringAudioBacklog)
{
    const auto loop = RecordedLoop();
    const auto worker = std::make_shared<audio::AudioSpectralWorker>();
    editor::AudioSpectralEditor controller(worker); controller.Open(loop);
    ASSERT_TRUE(Pump(controller, loop, [&] { return controller.HasPreparedSurface(); }));
    const auto before = controller.EditState();
    std::promise<void> entered, resume, presented;
    const auto resumed = resume.get_future().share();
    std::atomic<bool> cancelled{false};
    struct ReleaseWorker { std::promise<void>& Resume; ~ReleaseWorker() { Resume.set_value(); } };
    {
        ReleaseWorker release{resume};
        worker->RequestContent(loop.get(), [&entered, resumed] { entered.set_value(); resumed.wait(); });
        ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
        worker->RequestPresentation([&presented] { presented.set_value(); }, [&cancelled] { cancelled.store(true); });
        for (int i = 0; i < 20; ++i)
        {
            controller.SetViewport({0.1, 0.5}, {0.2, 0.6}); controller.Update();
        }
        EXPECT_FALSE(cancelled.load());
        EXPECT_EQ(controller.EditState(), before);
        EXPECT_TRUE(controller.HasPreparedSurface());
    }
    EXPECT_EQ(presented.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_FALSE(cancelled.load());
}

TEST_F(AudioSpectralTest, TwoAxisZoomKeepsMouseAnchorsAndWheelModifierPriority)
{
    using Controls = editor::AudioSpectralControls;
    Controls controls;
    EXPECT_EQ(Controls::WheelBinding(true, true, false), Controls::WheelMode::TimeZoom);
    controls.TimeViewport = {0.2, 0.8}; controls.Viewport = {0.1, 0.9};
    controls.BeginZoom(0.25, 0.75);
    const auto startTime = controls.TimeViewport, startFrequency = controls.Viewport;
    controls.MoveZoom(-128, 0); EXPECT_EQ(controls.Viewport, startFrequency);
    controls.MoveZoom(0, -128); EXPECT_EQ(controls.TimeViewport, startTime);
    controls.Wheel(Controls::WheelMode::Zoom, 1, std::numeric_limits<double>::quiet_NaN());
    EXPECT_EQ(controls.TimeViewport, startTime);
    controls.BeginZoom(std::numeric_limits<double>::quiet_NaN(), 0.5); // Invalid begin leaves valid captured anchors intact.
    controls.MoveZoom(-128, -128);
    EXPECT_NEAR(controls.TimeViewport.Lower + 0.25 * (controls.TimeViewport.Upper - controls.TimeViewport.Lower), 0.35, 1e-12);
    EXPECT_NEAR(controls.Viewport.Lower + 0.75 * (controls.Viewport.Upper - controls.Viewport.Lower), 0.7, 1e-12);
    const auto frequency = controls.Viewport;
    controls.Wheel(Controls::WheelMode::TimeZoom, 1, 0.25);
    EXPECT_EQ(controls.Viewport, frequency);
    EXPECT_NEAR(controls.TimeViewport.Lower + 0.25 * (controls.TimeViewport.Upper - controls.TimeViewport.Lower), 0.35, 1e-12);
    const auto time = controls.TimeViewport;
    controls.Wheel(Controls::WheelMode::Zoom, 1, 0.75);
    EXPECT_EQ(controls.TimeViewport, time);
    EXPECT_NEAR(controls.Viewport.Lower + 0.75 * (controls.Viewport.Upper - controls.Viewport.Lower), 0.7, 1e-12);
    controls.MoveZoom(-100000, 100000);
    EXPECT_NEAR(controls.TimeViewport.Upper - controls.TimeViewport.Lower, 0.0625, 1e-12);
    EXPECT_EQ(controls.Viewport, graphics::AudioSpectralViewport{});
}

TEST_F(AudioSpectralTest, PhysicalRadiiChangeSupportButPreserveExposureAndSubdivisionRate)
{
	std::string reason;
	const auto analysis = audio::AudioSpectralDsp::Analyze(Source(48000ul), Transform(), reason);
	ASSERT_TRUE(analysis);
	const auto frame = analysis->Geometry.FrameCount / 2;
	const auto t = analysis->Geometry.FrameCentre(frame, analysis->Transform.HopSamps) / 48000.0;
	constexpr unsigned int bin = 16;
	const auto hz = bin * 48000.0 / analysis->Transform.WindowSamps;
	auto smallMap = audio::AudioSpectralGainField::ExposureMap::Create(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	auto large = audio::AudioSpectralGainField::ExposureMap::Create(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	auto split = audio::AudioSpectralGainField::ExposureMap::Create(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(smallMap); ASSERT_TRUE(large); ASSERT_TRUE(split);
	ASSERT_TRUE(smallMap->Accumulate(*analysis, t, hz, t, hz, 0.01, reason, 0.01, 0.25));
	ASSERT_TRUE(large->Accumulate(*analysis, t, hz, t, hz, 0.01, reason, 0.1, 1.0));
	for (int i = 0; i < 10; ++i) ASSERT_TRUE(split->Accumulate(*analysis, t, hz, t, hz, 0.001, reason, 0.1, 1.0));
	EXPECT_NEAR(smallMap->ExposureAt(frame, bin), large->ExposureAt(frame, bin), 1e-12);
	EXPECT_GT(large->DirtyCellCount(), smallMap->DirtyCellCount());
	for (std::size_t f = 0; f < analysis->Geometry.FrameCount; ++f)
		for (unsigned int b = 0; b < analysis->Geometry.BinCount; ++b)
			EXPECT_NEAR(large->ExposureAt(f, b), split->ExposureAt(f, b), 1e-12);
	EXPECT_FALSE(smallMap->Accumulate(*analysis, t, hz, t, hz, 0.01, reason, 0.0, 0.25));
	EXPECT_FALSE(reason.empty());
}

TEST_F(AudioSpectralTest, BroadbandDragHasExactFiniteSilenceUnityAndBoundedBoost)
{
	using audio::AudioSpectralEnvelope;
	EXPECT_FLOAT_EQ(AudioSpectralEnvelope::TargetFromDragPixels(0.0), 1.0f);
	EXPECT_FLOAT_EQ(AudioSpectralEnvelope::TargetFromDragPixels(-160.0), 0.0f);
	EXPECT_FLOAT_EQ(AudioSpectralEnvelope::TargetFromDragPixels(-10000.0), 0.0f);
	EXPECT_FLOAT_EQ(AudioSpectralEnvelope::TargetFromDragPixels(160.0), AudioSpectralEnvelope::MaximumGain());
	EXPECT_FLOAT_EQ(AudioSpectralEnvelope::TargetFromDragPixels(10000.0), AudioSpectralEnvelope::MaximumGain());
	EXPECT_FLOAT_EQ(AudioSpectralEnvelope::TargetFromDragPixels(std::numeric_limits<double>::quiet_NaN()), 1.0f);
}

TEST_F(AudioSpectralTest, BothSeamContributorsUseSameWrappedEnvelopeExactlyOnce)
{
	std::string reason;
	const auto source = Source(1031ul);
	auto pcm = std::make_shared<audio::AudioSpectralPcm>();
	pcm->Source = source;
	std::vector<float> gains(source->Layout.LogicalLength, 1.0f);
	for (unsigned long i = 0; i < source->Layout.FadeSamps; ++i) gains[gains.size() - source->Layout.FadeSamps + i] = i == 0 ? 0.0f : 0.25f;
	const auto envelope = audio::AudioSpectralEnvelope::FromGains(gains, reason);
	ASSERT_TRUE(envelope);
	const auto playback = audio::AudioSpectralPlaybackSnapshot::Create(pcm, envelope, 3u, 1u, 2u, reason);
	ASSERT_TRUE(playback) << reason;
	audio::Hanning fade(source->Layout.FadeSamps);
	for (unsigned long i = 0; i < source->Layout.FadeSamps; ++i)
	{
		const auto prefix = source->Layout.BodyOffset - source->Layout.FadeSamps + i;
		const auto ending = prefix + source->Layout.LogicalLength;
		const auto gain = gains[source->Layout.WrappedBodyIndex(ending)];
		EXPECT_FLOAT_EQ(playback->SampleAt(prefix), source->Samples[prefix] * gain);
		EXPECT_FLOAT_EQ(playback->SampleAt(ending), source->Samples[ending] * gain);
		EXPECT_NEAR(fade.Mix(playback->SampleAt(prefix), playback->SampleAt(ending), i),
			fade.Mix(source->Samples[prefix], source->Samples[ending], i) * gain, 1e-7f);
	}
}

TEST_F(AudioSpectralTest, TransformAdmissionPreservesLongLoopsAndFallsBackUnderSessionPressure)
{
    std::string reason;
    audio::AudioSpectralTransformConfig transform;
    std::shared_ptr<const audio::AudioSpectralResourceBudget::Reservation> resources;
    const auto shortLayout = Source(96000ul)->Layout;
    const auto longLayout = Source(1440000ul)->Layout;
    const auto preferred = audio::AudioSpectralDsp::EstimateWorkingMemory(shortLayout, {}, reason);
    const auto fallback = audio::AudioSpectralDsp::EstimateWorkingMemory(shortLayout, {1u, 4096u, 1024u}, reason);
    ASSERT_TRUE(preferred); ASSERT_TRUE(fallback);
    EXPECT_GT(preferred->WorkingBytes, fallback->WorkingBytes);
    auto budget = std::make_shared<audio::AudioSpectralResourceBudget>();
    ASSERT_TRUE(audio::AudioSpectralDsp::SelectTransform(shortLayout, budget, transform, resources, reason));
    EXPECT_EQ(transform.WindowSamps, 4096u);
    EXPECT_EQ(transform.HopSamps, 256u);
    EXPECT_EQ(resources->Bytes(), preferred->WorkingBytes);
    resources.reset(); EXPECT_EQ(budget->UsedBytes(), 0u);
    ASSERT_TRUE(audio::AudioSpectralDsp::SelectTransform(longLayout, budget, transform, resources, reason));
    EXPECT_EQ(transform.WindowSamps, 4096u); EXPECT_FALSE(reason.empty());
    resources.reset(); EXPECT_EQ(budget->UsedBytes(), 0u);
    budget = std::make_shared<audio::AudioSpectralResourceBudget>(preferred->WorkingBytes);
    auto pressure = budget->Reserve(preferred->WorkingBytes - fallback->WorkingBytes, reason);
    ASSERT_TRUE(pressure);
    ASSERT_TRUE(audio::AudioSpectralDsp::SelectTransform(shortLayout, budget, transform, resources, reason));
    EXPECT_EQ(transform.WindowSamps, 4096u);
    EXPECT_EQ(budget->UsedBytes(), preferred->WorkingBytes);
    resources.reset(); pressure.reset(); EXPECT_EQ(budget->UsedBytes(), 0u);
    budget = std::make_shared<audio::AudioSpectralResourceBudget>(fallback->WorkingBytes - 1u);
    EXPECT_FALSE(audio::AudioSpectralDsp::SelectTransform(shortLayout, budget, transform, resources, reason));
    EXPECT_EQ(budget->UsedBytes(), 0u);
    EXPECT_FALSE(audio::AudioSpectralDsp::SelectTransform(Source(48000ul * 60ul)->Layout, nullptr, transform, resources, reason));
}

TEST_F(AudioSpectralTest, CaptureSelectsTransformAgainstRetainedLongLayout)
{
    std::string reason;
    const auto loop = RecordedLoop();
    const auto retained = Source(1440000ul)->Layout;
    const auto capture = loop->RequestAudioSpectralSource(reason, nullptr, &retained);
    ASSERT_TRUE(capture) << reason;
    EXPECT_EQ(capture->SelectedTransform().WindowSamps, 4096u);
    EXPECT_EQ(capture->Layout().LogicalLength, 4096ul);
    loop->CancelAudioSpectralSourceCapture();
}

TEST_F(AudioSpectralTest, SessionBudgetRejectsExcessAndRetainsAdmissionUntilLastSourcePinReleases)
{
	std::string reason;
	const auto budget = std::make_shared<audio::AudioSpectralResourceBudget>(100u);
	auto token = budget->Reserve(60u, reason);
	ASSERT_TRUE(token) << reason;
	EXPECT_EQ(budget->UsedBytes(), 60u);
	EXPECT_FALSE(budget->Reserve(41u, reason));
	EXPECT_FALSE(reason.empty());
	EXPECT_EQ(budget->UsedBytes(), 60u);
	auto source = Source(100ul);
	source->Resources = token;
	auto workerPin = std::make_shared<audio::AudioSpectralPcm>();
	workerPin->Source = source;
	token.reset();
	source.reset();
	EXPECT_EQ(budget->UsedBytes(), 60u);
	EXPECT_FALSE(budget->Reserve(100u, reason));
	workerPin.reset();
	EXPECT_EQ(budget->UsedBytes(), 0u);
	const auto full = budget->Reserve(100u, reason);
	ASSERT_TRUE(full) << reason;
	EXPECT_EQ(budget->UsedBytes(), 100u);
}

TEST_F(AudioSpectralTest, WaveformOverviewRefreshesEnvelopeOnlyEditsFromComposedContent)
{
	std::string reason;
	const auto source = Source(4096ul);
	std::fill(source->Samples.begin(), source->Samples.end(), 0.75f);
	auto pcm = std::make_shared<audio::AudioSpectralPcm>();
	pcm->Source = source;
	const auto original = audio::AudioSpectralPlaybackSnapshot::Create(pcm, {}, 1u, 1u, 1u, reason);
	ASSERT_TRUE(original) << reason;
	std::vector<float> gains(source->Layout.LogicalLength, 1.0f);
	std::fill(gains.begin(), gains.begin() + gains.size() / 2, 0.0f);
	const auto gate = audio::AudioSpectralEnvelope::FromGains(gains, reason);
	ASSERT_TRUE(gate);
	const auto edited = audio::AudioSpectralPlaybackSnapshot::Create(pcm, gate, 2u, 1u, 2u, reason);
	ASSERT_TRUE(edited) << reason;
	EXPECT_EQ(edited->Pcm.get(), original->Pcm.get());
	for (unsigned int i = 0; i < audio::AudioSpectralPlaybackSnapshot::OverviewSegments; ++i)
	{
		EXPECT_FLOAT_EQ(original->Overview()[i].Minimum, 0.75f);
		EXPECT_FLOAT_EQ(original->Overview()[i].Maximum, 0.75f);
		const auto expected = i < audio::AudioSpectralPlaybackSnapshot::OverviewSegments / 2 ? 0.0f : 0.75f;
		EXPECT_FLOAT_EQ(edited->Overview()[i].Minimum, expected);
		EXPECT_FLOAT_EQ(edited->Overview()[i].Maximum, expected);
	}
}

TEST_F(AudioSpectralTest, LateSpectralResultCannotReplaceNewestCommittedBroadbandEnvelope)
{
	std::string reason;
	const auto loop = RecordedLoop();
	const auto capture = loop->RequestAudioSpectralSource(reason);
	ASSERT_TRUE(capture) << reason;
	capture->Prepare();
	for (unsigned int i = 0; i < 100 && capture->Status() == audio::AudioSpectralSourceCapture::State::Pending; ++i)
		loop->EndMultiPlay(64u);
	const auto source = capture->Source();
	ASSERT_TRUE(source);
	const auto analysis = audio::AudioSpectralDsp::Analyze(source, Transform(), reason);
	ASSERT_TRUE(analysis) << reason;
	const auto field = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(field);
	const auto gains = std::make_shared<const audio::AudioSpectralGainField>(*field);
	const auto edit = audio::AudioSpectralEditState::Create(source, Transform(), gains, {}, 1u, 1u, 1u, reason);
	ASSERT_TRUE(edit);
	ASSERT_TRUE(loop->PublishAudioSpectralEdit(edit, reason)) << reason;
	const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
	ASSERT_TRUE(pcm);
	const auto initial = audio::AudioSpectralPlaybackSnapshot::Create(pcm, {}, 1u, 1u, 1u, reason);
	ASSERT_TRUE(initial);
	ASSERT_TRUE(loop->PublishAudioSpectralPlayback(initial, reason)) << reason;
	std::vector<float> muted(source->Layout.LogicalLength, 0.0f);
	const auto envelope = audio::AudioSpectralEnvelope::FromGains(muted, reason);
	ASSERT_TRUE(envelope);
	const auto newest = audio::AudioSpectralEditState::Create(source, Transform(), gains, envelope, 2u, 1u, 2u, reason);
	ASSERT_TRUE(newest);
	ASSERT_TRUE(loop->PublishAudioSpectralEdit(newest, reason)) << reason;
	// A completed spectral receipt still has the old gate, while its PCM is useful.
	const auto late = audio::AudioSpectralPlaybackSnapshot::Create(pcm, {}, 2u, 2u, 1u, reason);
	ASSERT_TRUE(late);
	EXPECT_FALSE(loop->PublishAudioSpectralPlayback(late, reason));
	EXPECT_EQ(loop->AudioSpectralPlayback(), initial);
	const auto composed = audio::AudioSpectralPlaybackSnapshot::Create(pcm, envelope, 2u, 2u, 2u, reason);
	ASSERT_TRUE(composed);
	ASSERT_TRUE(loop->PublishAudioSpectralPlayback(composed, reason)) << reason;
	EXPECT_FLOAT_EQ(loop->AudioSpectralPlayback()->SampleAt(source->Layout.BodyOffset), 0.0f);
	// Undo/source cancellation barriers reject old results even with the current gate.
	loop->InvalidateAudioSpectralResultsBefore(3u, source->Generation);
	const auto obsolete = audio::AudioSpectralPlaybackSnapshot::Create(pcm, envelope, 3u, 2u, 2u, reason);
	ASSERT_TRUE(obsolete);
	EXPECT_FALSE(loop->PublishAudioSpectralPlayback(obsolete, reason));
	loop->EndMultiPlay(64u);
	EXPECT_EQ(loop->AppliedAudioSpectralRevision(), 0u);
	const auto restored = audio::AudioSpectralPlaybackSnapshot::Create(pcm, envelope, 3u, 3u, 2u, reason);
	ASSERT_TRUE(restored);
	ASSERT_TRUE(loop->PublishAudioSpectralPlayback(restored, reason)) << reason;
	loop->EndMultiPlay(64u);
	EXPECT_EQ(loop->AppliedAudioSpectralRevision(), 3u);
	loop->SetSampleRate(44100.0f);
	const auto freshCapture = loop->RequestAudioSpectralSource(reason);
	ASSERT_TRUE(freshCapture) << reason;
	freshCapture->Prepare();
	for (unsigned int i = 0; i < 100 && freshCapture->Status() == audio::AudioSpectralSourceCapture::State::Pending; ++i)
		loop->EndMultiPlay(64u);
	const auto freshSource = freshCapture->Source();
	ASSERT_TRUE(freshSource);
	ASSERT_NE(freshSource->Generation, source->Generation);
	loop->InvalidateAudioSpectralResultsBefore(100u, source->Generation);
	auto freshPcm = std::make_shared<audio::AudioSpectralPcm>();
	freshPcm->Source = freshSource;
	const auto freshPlayback = audio::AudioSpectralPlaybackSnapshot::Create(freshPcm, {}, 1u, 1u, 1u, reason);
	ASSERT_TRUE(freshPlayback);
	ASSERT_TRUE(loop->PublishAudioSpectralPlayback(freshPlayback, reason)) << reason;
	loop->EndMultiPlay(64u);
	EXPECT_EQ(loop->AppliedAudioSpectralRevision(), 1u); // Old generation barrier cannot poison a fresh source.
}

TEST_F(AudioSpectralTest, ControllerRelativeBandReleaseUndoRedoAndCancellation)
{
	const auto loop = RecordedLoop();
	// Pointer UV usually maps between PCM samples. Bar centres snap to the nearest
	// source sample so a finite downward drag still authors an exact zero.
	constexpr double centreSeconds = 1200.25 / 48000.0;
	actions::ActionUndoHistory history;
	editor::AudioSpectralEditor controller;
	controller.SetUndoHistory(&history);
	controller.Open(loop);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; })) << controller.Feedback();
    EXPECT_EQ(controller.Analysis()->Transform, controller.Source()->SelectedTransform);
    EXPECT_EQ(controller.Analysis()->Transform.WindowSamps, 4096u);
	ASSERT_TRUE(controller.BeginStroke(true, false, centreSeconds, 1000.0, 100.0f));
	controller.EndStroke(false);
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); }));
	EXPECT_FALSE(history.Undo()); // Unity anchoring alone does not create history.
	ASSERT_TRUE(controller.BeginStroke(true, false, centreSeconds, 1000.0, 100.0f));
	controller.MoveStroke(std::pair{centreSeconds, 1000.0}, -60.0f);
	controller.EndStroke(false);
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke() && loop->AudioSpectralPlayback(); })) << controller.Feedback();
	const auto muted = loop->AudioSpectralEdit();
	ASSERT_TRUE(muted && muted->Envelope);
	EXPECT_FLOAT_EQ(muted->Envelope->GainAt(1200ul), 0.0f);
	ASSERT_TRUE(Pump(controller, loop, [&] { const auto p = loop->AudioSpectralPlayback(); return p && p->EnvelopeRevision == muted->EnvelopeRevision; }));
	EXPECT_FLOAT_EQ(loop->AudioSpectralPlayback()->SampleAt(muted->Source->Layout.BodyOffset + 1200ul), 0.0f);
	ASSERT_TRUE(history.Undo());
	EXPECT_FALSE(history.Undo()); // Entire press/release is exactly one entry.
	EXPECT_TRUE(!loop->AudioSpectralEdit()->Envelope || loop->AudioSpectralEdit()->Envelope->IsUnity());
	ASSERT_TRUE(history.Redo());
	EXPECT_FLOAT_EQ(loop->AudioSpectralEdit()->Envelope->GainAt(1200ul), 0.0f);
	EXPECT_FALSE(history.Redo());
	const auto beforeCancel = loop->AudioSpectralEdit();
	ASSERT_TRUE(controller.BeginStroke(true, false, centreSeconds, 1000.0, 100.0f));
	controller.MoveStroke(std::pair{centreSeconds + 0.01, 1000.0}, 180.0f);
	ASSERT_TRUE(Pump(controller, loop, [&] { return loop->AudioSpectralEdit()->Envelope != beforeCancel->Envelope; }));
	EXPECT_FLOAT_EQ(loop->AudioSpectralEdit()->Envelope->GainAt(1200ul), 0.0f); // Sweep starts at neutral; initial zero stays exact.
	controller.EndStroke(true);
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); }));
	EXPECT_EQ(loop->AudioSpectralEdit()->Envelope, beforeCancel->Envelope);
	ASSERT_TRUE(history.Undo()); // Cancellation adds no second history entry.
	EXPECT_FALSE(history.Undo());
}

TEST_F(AudioSpectralTest, ControllerRelativeVerticalBandSurvivesWorkerBacklogAndFractionalAnchor)
{
	const auto loop = RecordedLoop();
	const auto worker = std::make_shared<audio::AudioSpectralWorker>();
	editor::AudioSpectralEditor controller(worker);
	actions::ActionUndoHistory history;
	controller.SetUndoHistory(&history);
	controller.Open(loop);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
	const auto before = controller.EditState();
	std::promise<void> entered, resume;
	const auto resumed = resume.get_future().share();
	struct ReleaseWorker { std::promise<void>& Resume; ~ReleaseWorker() { Resume.set_value(); } };
	{
		ReleaseWorker release{resume};
		worker->RequestContent(loop.get(), [&entered, resumed] { entered.set_value(); resumed.wait(); });
		ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
		ASSERT_TRUE(controller.BeginStroke(true, false, 1200.25 / 48000.0, 1000.0, 100.0f, 0.05, 0.25, 0.005));
		controller.MoveStroke(std::pair{1200.25 / 48000.0, 1000.0}, -60.0f);
		std::this_thread::sleep_for(std::chrono::duration<double>(audio::AudioSpectralConfig::StrokeUpdateSeconds));
		controller.Update(); // Freeze an attenuation target while worker remains busy.
		for (int i = 0; i < 2000; ++i) controller.MoveStroke(std::nullopt, 100.0f + (i % 160));
		controller.MoveStroke(std::nullopt, 180.0f); // Latest replaces every intervening target.
		controller.SetFrequencyViewport({0.25, 0.75}); // Rejected during captured edit.
		EXPECT_EQ(controller.EditState(), before);
		EXPECT_EQ(controller.Feedback().find("overloaded"), std::string::npos);
		controller.EndStroke(false);
	}
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); })) << controller.Feedback();
	const auto after = controller.EditState();
	ASSERT_TRUE(after->Envelope);
	EXPECT_FLOAT_EQ(after->Envelope->GainAt(1200), audio::AudioSpectralEnvelope::TargetFromDragPixels(80));
	EXPECT_FLOAT_EQ(after->Envelope->GainAt(3000), 1.0f); // A vertical adjustment leaves distant samples untouched.
	ASSERT_TRUE(history.Undo());
	EXPECT_EQ(controller.EditState()->Envelope, before->Envelope);
	EXPECT_FALSE(history.Undo());
}

TEST_F(AudioSpectralTest, ControllerNeutralAndReversedBandRetainNonUnityBaselineAndNoHistory)
{
	const auto loop = RecordedLoop();
	editor::AudioSpectralEditor controller;
	actions::ActionUndoHistory history;
	controller.SetUndoHistory(&history);
	controller.Open(loop);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
	const auto initial = controller.EditState();
	std::string reason;
	std::vector<float> values(initial->Source->Layout.LogicalLength, 0.5f);
	values[1200] = 0.0f;
	const auto envelope = audio::AudioSpectralEnvelope::FromGains(values, reason);
	const auto baseline = audio::AudioSpectralEditState::Create(initial->Source, initial->Transform,
		initial->Gains, envelope, initial->Revision + 1, initial->SpectralRevision, initial->EnvelopeRevision + 1, reason);
	ASSERT_TRUE(loop->PublishAudioSpectralEdit(baseline, reason));
	ASSERT_TRUE(controller.BeginStroke(true, false, 1200.0 / 48000.0, 1000.0, 100.0f));
	controller.EndStroke(false);
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); }));
	EXPECT_EQ(controller.EditState(), baseline);
	EXPECT_FALSE(history.Undo());
	ASSERT_TRUE(controller.BeginStroke(true, false, 1200.0 / 48000.0, 1000.0, 100.0f));
	controller.MoveStroke(std::nullopt, 180.0f);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.EditState()->Envelope != envelope; }));
	EXPECT_GT(controller.EditState()->Envelope->GainAt(1200), 0.0f);
	EXPECT_GT(controller.EditState()->Envelope->GainAt(1199), 0.5f);
	controller.MoveStroke(std::nullopt, 100.0f);
	controller.EndStroke(false);
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); }));
	EXPECT_EQ(controller.EditState()->Envelope, envelope);
	EXPECT_FALSE(history.Undo());
}

TEST_F(AudioSpectralTest, ControllerOrdinaryStationaryHoldChangesSoundAndHasOneUndoEntry)
{
	const auto loop = RecordedLoop();
	actions::ActionUndoHistory history;
	editor::AudioSpectralEditor controller;
	controller.SetUndoHistory(&history);
	controller.Open(loop);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; })) << controller.Feedback();
	const auto before = controller.EditState();
	ASSERT_TRUE(before);
	ASSERT_TRUE(controller.BeginStroke(false, false, 0.025, 20.0, 100.0f));
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	controller.EndStroke(false); // Release integrates the stationary elapsed hold.
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke() && loop->AudioSpectralPlayback(); })) << controller.Feedback();
	const auto painted = loop->AudioSpectralEdit();
	ASSERT_TRUE(painted);
	EXPECT_FALSE(painted->Gains->IsUnity());
	ASSERT_TRUE(Pump(controller, loop, [&] { const auto p = loop->AudioSpectralPlayback(); return p && p->SpectralRevision == painted->SpectralRevision; }));
	ASSERT_TRUE(history.Undo());
	EXPECT_EQ(loop->AudioSpectralEdit()->Gains, before->Gains);
	EXPECT_FALSE(history.Undo());
	ASSERT_TRUE(history.Redo());
	EXPECT_EQ(loop->AudioSpectralEdit()->Gains, painted->Gains);
}

TEST_F(AudioSpectralTest, StationaryExposureAccumulatesWhileContentWorkerIsBlocked)
{
	const auto loop = RecordedLoop();
	const auto worker = std::make_shared<audio::AudioSpectralWorker>();
	editor::AudioSpectralEditor controller(worker);
	controller.Open(loop);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
	std::promise<void> entered, resume;
	const auto resumed = resume.get_future().share();
	struct ReleaseWorker { std::promise<void>& Resume; ~ReleaseWorker() { Resume.set_value(); } };
	std::shared_ptr<const audio::AudioSpectralEditState> painted;
	{
		ReleaseWorker release{ resume };
		worker->RequestContent(loop.get(), [&entered, resumed] { entered.set_value(); resumed.wait(); });
		ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
		ASSERT_TRUE(controller.BeginStroke(false, true, 0.0, 20.0, 100.0f));
		for (int update = 0; update < 1000; ++update)
			controller.MoveStroke(std::pair{0.0, 20.0}, 100.0f);
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
		controller.MoveStroke(std::pair{0.0, 20.0}, 100.0f);
		EXPECT_EQ(controller.Feedback().find("overloaded"), std::string::npos);
		controller.EndStroke(false);
	}
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); })) << controller.Feedback();
	painted = loop->AudioSpectralEdit();
	ASSERT_TRUE(painted);
	const auto analysis = controller.Analysis();
	ASSERT_TRUE(analysis);
	const auto frame = static_cast<std::size_t>(-analysis->Geometry.FirstFrameCentre / analysis->Transform.HopSamps);
	// At least the explicit hold survives, regardless of worker completion rate.
	EXPECT_LE(painted->Gains->GainDb(frame, 0), -0.04 * audio::AudioSpectralConfig::BrushDbPerSecond + 0.01);
}
TEST_F(AudioSpectralTest, AbsoluteCeilingReachesSameLevelFromQuietAndLoudOriginals)
{
    std::string reason;
    float quietHeight = 0, loudHeight = 0;
    for (const auto amplitude : { 0.001, 0.1 })
    {
        const auto source = Source(48000ul);
        const audio::AudioSpectralTransformConfig transform;
        const auto hz = 87.0 * source->Layout.SampleRate / transform.WindowSamps;
        for (std::size_t i = 0; i < source->Samples.size(); ++i)
            source->Samples[i] = static_cast<float>(amplitude * std::cos(i * 6.283185307179586 * hz / source->Layout.SampleRate));
        const auto analysis = audio::AudioSpectralDsp::Analyze(source, transform, reason);
        ASSERT_TRUE(analysis) << reason;
        auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
        ASSERT_TRUE(gains);
        const auto frame = static_cast<std::size_t>((24000 - analysis->Geometry.FirstFrameCentre) / transform.HopSamps);
        const auto seconds = analysis->Geometry.FrameCentre(frame, transform.HopSamps) / source->Layout.SampleRate;
        ASSERT_TRUE(gains->PaintOvalSegment(*analysis, seconds, hz, seconds, hz, 10.0, true, reason));
        EXPECT_NEAR(gains->GainLinear(frame, 87) * analysis->Magnitude(frame, 87), 1.0, 2e-5);
        auto nextStroke = *gains;
        nextStroke.PaintOvalSegment(*analysis, seconds, hz, seconds, hz, 10.0, true, reason);
        EXPECT_FLOAT_EQ(nextStroke.GainDb(frame, 87), gains->GainDb(frame, 87));
        const auto display = graphics::AudioSpectralModel::BuildDisplay(*analysis, &nextStroke, nullptr, 1, 512, 128, reason);
        ASSERT_TRUE(display) << reason;
        const auto maximum = *std::max_element(display->Heights.begin(), display->Heights.end());
        if (amplitude < 0.01) quietHeight = maximum; else loudHeight = maximum;
    }
    EXPECT_NEAR(quietHeight, 90.0f / 114.0f, 1e-4f);
    EXPECT_NEAR(loudHeight, quietHeight, 1e-4f);
}
TEST_F(AudioSpectralTest, EqualExposureRaisesQuietMediumAndLoudHeightsAtTheSameRate)
{
    std::string reason;
    const auto original = audio::AudioSpectralDsp::Analyze(Source(1031ul), Transform(), reason);
    ASSERT_TRUE(original) << reason;
    for (const auto db : { -70.0, -35.0, -8.0 })
    {
        SCOPED_TRACE(db);
        auto analysis = *original;
        const auto amplitude = std::pow(10.0, db / 20.0);
        for (std::size_t frame = 0; frame < analysis.Geometry.FrameCount; ++frame)
            for (unsigned int bin = 0; bin < analysis.Geometry.BinCount; ++bin)
                analysis.Coefficients[frame * analysis.Geometry.BinCount + bin] = {
                    static_cast<float>(amplitude * analysis.WindowSum
                        / (bin == 0u || bin + 1u == analysis.Geometry.BinCount ? 1.0 : 2.0)), 0.0f };
        auto gains = audio::AudioSpectralGainField::Unity(analysis.Geometry.FrameCount, analysis.Geometry.BinCount, reason);
        ASSERT_TRUE(gains);
        const auto before = graphics::AudioSpectralModel::BuildDisplay(analysis, &*gains, nullptr, 1, 4, 3, reason);
        ASSERT_TRUE(before) << reason;
        ASSERT_TRUE(gains->PaintOvalSegment(analysis, 0.0, 20.0, 0.0, 20.0,
            audio::AudioSpectralConfig::StrokeUpdateSeconds, true, reason)) << reason;
        const auto after = graphics::AudioSpectralModel::BuildDisplay(analysis, &*gains, nullptr, 2, 4, 3, reason);
        ASSERT_TRUE(after) << reason;
        EXPECT_NEAR(after->Heights[0] - before->Heights[0], 2.0 / 114.0, 1e-6);
        const auto frame = static_cast<std::size_t>(-analysis.Geometry.FirstFrameCentre / analysis.Transform.HopSamps);
        EXPECT_NEAR(gains->GainDb(frame, 0), 2.0, 1e-6);
    }
}

TEST_F(AudioSpectralTest, ExposureNearCeilingClampsWithoutOvershootAndRemainsReversibleBelowIt)
{
    std::string reason;
    const auto original = audio::AudioSpectralDsp::Analyze(Source(1031ul), Transform(), reason);
    ASSERT_TRUE(original) << reason;
    auto analysis = *original;
    analysis.Coefficients[0] = { static_cast<float>(analysis.WindowSum * std::pow(10.0, -1.0 / 20.0)), 0.0f };
    const auto boosted = audio::AudioSpectralGainField::GainDbAfterExposure(analysis, 0, 0, 0.0f,
        audio::AudioSpectralConfig::StrokeUpdateSeconds, true);
    EXPECT_NEAR(20.0 * std::log10(analysis.Magnitude(0, 0)) + boosted, 0.0, 1e-6);
    EXPECT_FLOAT_EQ(audio::AudioSpectralGainField::GainDbAfterExposure(analysis, 0, 0, boosted, 1.0, true), boosted);
    const auto reduced = audio::AudioSpectralGainField::GainDbAfterExposure(analysis, 0, 0, boosted,
        audio::AudioSpectralConfig::StrokeUpdateSeconds, false);
    EXPECT_NEAR(reduced, boosted - 2.0, 1e-6);
}

TEST_F(AudioSpectralTest, DisplayInterpolatesUnequalCornerDbHeightsRatherThanLinearAmplitudes)
{
    std::string reason;
    const auto original = audio::AudioSpectralDsp::Analyze(Source(1031ul), Transform(), reason);
    ASSERT_TRUE(original) << reason;
    auto analysis = *original;
    const auto framePosition = (1031.0 / 4.0 - analysis.Geometry.FirstFrameCentre) / analysis.Transform.HopSamps;
    const auto frame = static_cast<std::size_t>(framePosition);
    const auto hz = 20.0 * std::pow(24000.0 / 20.0, 1.0 / 3.0);
    const auto binPosition = hz * analysis.Transform.WindowSamps / 48000.0;
    const auto bin = static_cast<unsigned int>(binPosition);
    const double db[2][2] = { { -70.0, -40.0 }, { -20.0, -5.0 } };
    for (unsigned int f = 0; f < 2; ++f)
        for (unsigned int b = 0; b < 2; ++b)
            analysis.Coefficients[(frame + f) * analysis.Geometry.BinCount + bin + b] = {
                static_cast<float>(std::pow(10.0, db[f][b] / 20.0) * analysis.WindowSum / 2.0), 0.0f };
    const auto display = graphics::AudioSpectralModel::BuildDisplay(analysis, nullptr, nullptr, 1, 4, 3, reason);
    ASSERT_TRUE(display) << reason;
    const auto expectedDb = std::lerp(std::lerp(db[0][0], db[0][1], binPosition - bin),
        std::lerp(db[1][0], db[1][1], binPosition - bin), framePosition - frame);
    EXPECT_NEAR(display->Heights[6], (expectedDb + 90.0) / 114.0, 1e-6);
}

TEST_F(AudioSpectralTest, AuthoredSpectralSilenceIsExactAndBoostRecoversRetainedOriginal)
{
    std::string reason;
    const auto analysis = audio::AudioSpectralDsp::Analyze(Source(1031ul), Transform(), reason);
    ASSERT_TRUE(analysis) << reason;
    auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
    ASSERT_TRUE(gains);
    for (std::size_t frame = 0; frame < gains->FrameCount(); ++frame)
        for (unsigned int bin = 0; bin < gains->BinCount(); ++bin)
            ASSERT_TRUE(gains->SetGainDb(frame, bin, audio::AudioSpectralConfig::SilenceGainDb));
    const auto silent = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
    ASSERT_TRUE(silent) << reason;
    for (unsigned long i = analysis->Geometry.TargetFirstStorage;
        i < analysis->Geometry.TargetFirstStorage + analysis->Geometry.TargetLength; ++i)
        EXPECT_FLOAT_EQ(silent->Samples()[i], 0.0f);
    const auto display = graphics::AudioSpectralModel::BuildDisplay(*analysis, &*gains, nullptr, 1, 64, 32, reason);
    ASSERT_TRUE(display) << reason;
    for (const auto height : display->Heights) EXPECT_FLOAT_EQ(height, 0.0f);
    ASSERT_TRUE(gains->PaintOvalSegment(*analysis, 0.01, 3000.0, 0.01, 3000.0, 0.1, true, reason));
    const auto restored = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
    ASSERT_TRUE(restored) << reason;
    bool audible = false;
    for (unsigned long i = analysis->Geometry.TargetFirstStorage;
        i < analysis->Geometry.TargetFirstStorage + analysis->Geometry.TargetLength; ++i)
        audible |= restored->Samples()[i] != 0.0f;
    EXPECT_TRUE(audible);
}
TEST_F(AudioSpectralTest, InputMapRasterizesOnlyTheWrappedBrushSupportAndRecyclesCleanly)
{
    std::string reason;
    for (const auto length : { 1031ul, 48000ul })
    {
        const auto analysis = audio::AudioSpectralDsp::Analyze(Source(length), Transform(), reason);
        ASSERT_TRUE(analysis) << reason;
        auto input = audio::AudioSpectralGainField::ExposureMap::Create(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
        ASSERT_TRUE(input) << reason;
        auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
        ASSERT_TRUE(gains);
        const auto period = length / 48000.0;
        const auto seconds = period - 0.003;
        ASSERT_TRUE(input->Accumulate(*analysis, seconds, 3000.0, seconds, 3000.0, 0.001, reason));
        ASSERT_TRUE(gains->ApplyExposureMap(*analysis, *input, true, reason));
        for (std::size_t frame = 0; frame < gains->FrameCount(); ++frame)
            for (unsigned int bin = 0; bin < gains->BinCount(); ++bin)
            {
                const auto x = std::remainder(analysis->Geometry.FrameCentre(frame, analysis->Transform.HopSamps)
                    / 48000.0 - seconds, period) / audio::AudioSpectralConfig::BrushTimeRadiusSeconds;
                const auto hz = (std::max)(20.0, bin * 48000.0 / analysis->Transform.WindowSamps);
                const auto y = std::log2(hz / 3000.0) / audio::AudioSpectralGainField::EffectiveFrequencyRadius(*analysis, hz, audio::AudioSpectralConfig::BrushFrequencyRadiusOctaves);
                const auto h = (std::max)(0.0, 1.0 - x*x - y*y);
                const auto expected = audio::AudioSpectralGainField::GainDbAfterExposure(*analysis, frame, bin, 0.0f, 0.001*(x*x+y*y <= 0.5625 ? 1.0 : h*h / (0.4375*0.4375)), true);
                EXPECT_NEAR(gains->GainDb(frame, bin), expected, 2e-5f) << length << '/' << frame << '/' << bin;
            }
        if (length == 48000ul) EXPECT_LT(input->DirtyCellCount(), gains->FrameCount() * gains->BinCount() / 10);
        input->Clear();
        EXPECT_TRUE(input->Empty());
        EXPECT_EQ(input->ElapsedSeconds(), 0.0);
        EXPECT_FALSE(gains->ApplyExposureMap(*analysis, *input, true, reason));
    }
}

TEST_F(AudioSpectralTest, ThousandsOfMovingSamplesAccumulateWithoutWaitingForReconstruction)
{
    const auto loop = RecordedLoop(48000ul);
    actions::ActionUndoHistory history;
    const auto worker = std::make_shared<audio::AudioSpectralWorker>();
    editor::AudioSpectralEditor controller(worker);
    controller.SetUndoHistory(&history);
    controller.Open(loop);
    ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
    const auto before = controller.EditState();
    std::promise<void> entered, resume;
    const auto resumed = resume.get_future().share();
    struct ReleaseWorker { std::promise<void>& Resume; ~ReleaseWorker() { Resume.set_value(); } };
    {
        ReleaseWorker release{resume};
        worker->RequestContent(loop.get(), [&entered, resumed] { entered.set_value(); resumed.wait(); });
        ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
        ASSERT_TRUE(controller.BeginStroke(false, false, 0.1, 20.0, 100.0f));
        for (int update = 0; update < 2000; ++update)
            controller.MoveStroke(std::pair{0.1 + 0.0005*std::sin(update*0.13), 20.0}, 100.0f);
        // Allow the configured rate to saturate this -6 dB fixture while the
        // worker stays blocked; the assertion concerns input ownership, not speed.
        std::this_thread::sleep_for(std::chrono::duration<double>(12.0 / audio::AudioSpectralConfig::BrushDbPerSecond));
        controller.MoveStroke(std::pair{0.1, 20.0}, 100.0f);
        const auto preview = controller.BrushLevelDb();
        ASSERT_TRUE(preview);
        EXPECT_NEAR(*preview, 0.0, 0.001); // UI map reaches red while synthesis is still blocked.
        EXPECT_EQ(controller.EditState()->Gains, before->Gains); // Immutable committed field hasn't been mutated.
        EXPECT_EQ(controller.Feedback().find("overloaded"), std::string::npos);
        controller.EndStroke(false);
    }
    ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); })) << controller.Feedback();
    EXPECT_FALSE(loop->AudioSpectralEdit()->Gains->IsUnity());
    ASSERT_TRUE(history.Undo());
    EXPECT_EQ(loop->AudioSpectralEdit()->Gains, before->Gains);
    EXPECT_FALSE(history.Undo()); // Every high-resolution sample belongs to one stroke.
}

TEST_F(AudioSpectralTest, CurvedInputMapMatchesSequentialPaintingAcrossEveryCell)
{
    std::string reason;
    const auto analysis = audio::AudioSpectralDsp::Analyze(Source(48000ul), Transform(), reason);
    ASSERT_TRUE(analysis) << reason;
    auto input = audio::AudioSpectralGainField::ExposureMap::Create(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
    auto direct = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
    ASSERT_TRUE(input); ASSERT_TRUE(direct);
    auto batched = *direct;
    double t = 0.5, hz = 1000;
    for (int step = 0; step < 1000; ++step)
    {
        const auto nextT = 0.5 + 0.05*std::sin(step*0.02);
        const auto nextHz = 1000.0*std::exp2(0.25*std::sin(step*0.013));
        input->Accumulate(*analysis, t, hz, nextT, nextHz, 0.000001, reason);
        direct->PaintOvalSegment(*analysis, t, hz, nextT, nextHz, 0.000001, true, reason);
        t = nextT; hz = nextHz;
    }
    ASSERT_TRUE(batched.ApplyExposureMap(*analysis, *input, true, reason)) << reason;
    for (std::size_t frame = 0; frame < direct->FrameCount(); ++frame)
        for (unsigned int bin = 0; bin < direct->BinCount(); ++bin)
            EXPECT_NEAR(batched.GainDb(frame, bin), direct->GainDb(frame, bin), 5e-5f);
}
TEST_F(AudioSpectralTest, ProductionTransformStationaryFlowChangesDisplayAndAudibleContent)
{
	std::string reason;
	const auto source = Source(48000ul);
	const audio::AudioSpectralTransformConfig transform;
	const auto hz = 87.0 * source->Layout.SampleRate / transform.WindowSamps;
	for (std::size_t i = 0; i < source->Samples.size(); ++i)
		source->Samples[i] = static_cast<float>(0.2 * std::cos(i * 6.283185307179586 * hz / source->Layout.SampleRate));
	const auto analysis = audio::AudioSpectralDsp::Analyze(source, transform, reason);
	ASSERT_TRUE(analysis) << reason;
	auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
	ASSERT_TRUE(gains);
	const auto frame = static_cast<std::size_t>((24000 - analysis->Geometry.FirstFrameCentre) / transform.HopSamps);
	const auto seconds = analysis->Geometry.FrameCentre(frame, transform.HopSamps) / source->Layout.SampleRate;
	const auto before = graphics::AudioSpectralModel::BuildDisplay(*analysis, &*gains, nullptr, 1, 512, 128, reason);
	ASSERT_TRUE(before) << reason;
	for (int update = 0; update < 30; ++update)
		ASSERT_TRUE(gains->PaintOvalSegment(*analysis, seconds, hz, seconds, hz, 12.0 / audio::AudioSpectralConfig::BrushDbPerSecond / 30.0, true, reason)) << reason;
	EXPECT_NEAR(gains->GainDb(frame, 87), 12.0, 1e-4);
	const auto after = graphics::AudioSpectralModel::BuildDisplay(*analysis, &*gains, nullptr, 2, 512, 128, reason);
	ASSERT_TRUE(after) << reason;
	float greatestRise = 0.0f;
	for (std::size_t i = 0; i < before->Heights.size(); ++i)
		greatestRise = (std::max)(greatestRise, after->Heights[i] - before->Heights[i]);
	EXPECT_GT(greatestRise, 0.07f);
	const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
	ASSERT_TRUE(pcm) << reason;
	const auto centre = source->Layout.BodyOffset + static_cast<unsigned long>(seconds * source->Layout.SampleRate);
	double originalEnergy = 0, editedEnergy = 0;
	for (unsigned long i = centre - 128; i < centre + 128; ++i)
	{
		originalEnergy += source->Samples[i] * source->Samples[i];
		editedEnergy += pcm->Samples()[i] * pcm->Samples()[i];
	}
	EXPECT_GT(editedEnergy, originalEnergy * 3.0);
	for (int update = 0; update < 30; ++update)
		ASSERT_TRUE(gains->PaintOvalSegment(*analysis, seconds, hz, seconds, hz, 12.0 / audio::AudioSpectralConfig::BrushDbPerSecond / 30.0, false, reason)) << reason;
	EXPECT_NEAR(gains->GainDb(frame, 87), 0.0, 1e-4);
}
TEST_F(AudioSpectralTest, DenseDisplayHasFixedDimensionsWrappedSeamAndSharedPickingHeight)
{
    std::string reason;
    for (const auto length : { 4139ul, 96000ul })
    {
        const auto analysis = audio::AudioSpectralDsp::Analyze(Source(length), {}, reason);
        ASSERT_TRUE(analysis) << reason;
        const auto display = graphics::AudioSpectralModel::BuildDisplay(*analysis, nullptr, nullptr, 1u,
            graphics::AudioSpectralModel::DefaultTimeSegments,
            graphics::AudioSpectralModel::DefaultFrequencySegments, reason);
        ASSERT_TRUE(display) << reason;
        EXPECT_EQ(display->Heights.size(), 4096u * 257u);
        const auto sparse = graphics::AudioSpectralModel::BuildDisplay(*analysis, nullptr, nullptr, 1u, 1365u, 128u, reason);
        ASSERT_TRUE(sparse);
        for (unsigned int row = 0u; row <= 128u; ++row)
            for (unsigned int col = 0u; col <= 1365u; ++col)
                EXPECT_FLOAT_EQ(display->Heights[row * 2u * 4096u + col * 3u], sparse->Heights[row * 1366u + col]);
        for (unsigned int row = 0; row <= 256; ++row)
            EXPECT_FLOAT_EQ(display->Heights[row * 4096], display->Heights[row * 4096 + 4095]);
        auto surface = PickingSurface(); surface.Display = display;
        const auto point = graphics::AudioSpectralModel::LocalVertex(surface, 2048u, 128u);
        EXPECT_NEAR(point.y, graphics::AudioSpectralModel::SurfaceBaseY + display->Heights[128u * 4096u + 2048u] * 25.0f, 1e-5);
        const auto hit = editor::AudioSpectralPicking::Intersect(surface, {point.x, point.y + 100.0f, point.z}, {0, -1, 0});
        ASSERT_TRUE(hit);
        EXPECT_NEAR(hit->Distance, 100.0, 1e-5);
        EXPECT_NEAR(hit->TimeU, 2048.0/4095.0, 1e-6);
        EXPECT_NEAR(hit->FrequencyV, 0.5, 1e-6);
    }
}

TEST_F(AudioSpectralTest, ReopenedLegacyEditKeepsItsTransform)
{
    std::string reason;
    const auto loop = RecordedLoop();
    const auto capture = loop->RequestAudioSpectralSource(reason);
    ASSERT_TRUE(capture);
    capture->Prepare();
    for (unsigned int i = 0; i < 100 && capture->Status() == audio::AudioSpectralSourceCapture::State::Pending; ++i)
        loop->EndMultiPlay(64u);
    const auto source = capture->Source(); ASSERT_TRUE(source);
    EXPECT_EQ(source->SelectedTransform.WindowSamps, 4096u);
    const audio::AudioSpectralTransformConfig legacy{1u, 8192u, 1024u};
    const auto analysis = audio::AudioSpectralDsp::Analyze(source, legacy, reason); ASSERT_TRUE(analysis);
    const auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
    ASSERT_TRUE(gains);
    const auto edit = audio::AudioSpectralEditState::Create(source, legacy,
        std::make_shared<const audio::AudioSpectralGainField>(*gains), {}, 1u, 1u, 1u, reason);
    ASSERT_TRUE(edit); ASSERT_TRUE(loop->PublishAudioSpectralEdit(edit, reason));
    editor::AudioSpectralEditor controller; controller.Open(loop);
    ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; })) << controller.Feedback();
    EXPECT_EQ(controller.Analysis()->Transform, legacy);
    EXPECT_EQ(controller.EditState()->Transform, legacy);
    EXPECT_EQ(controller.EditState()->Gains->BinCount(), 4097u);
    controller.Close();
}

TEST_F(AudioSpectralTest, PickingFlatSilenceSupportsVerticalObliqueAndGridBoundaryRays)
{
	const auto surface = PickingSurface();
	const auto vertical = editor::AudioSpectralPicking::Intersect(surface, {0, 103, 0}, {0, -1, 0});
	ASSERT_TRUE(vertical);
	EXPECT_NEAR(vertical->TimeU, 0.5, 1e-7);
	EXPECT_NEAR(vertical->FrequencyV, 0.5, 1e-7);
	EXPECT_NEAR(vertical->Distance, 100.0, 1e-7);
	const auto oblique = editor::AudioSpectralPicking::Intersect(surface, {-50, 103, 39}, {0.5f, -1, -0.39f});
	ASSERT_TRUE(oblique);
	EXPECT_NEAR(oblique->TimeU, 0.5, 1e-6);
	EXPECT_NEAR(oblique->FrequencyV, 0.5, 1e-6);
	EXPECT_NEAR(oblique->Distance, std::sqrt(10000.0 + 2500.0 + 1521.0), 1e-4);
	const auto edge = editor::AudioSpectralPicking::Intersect(surface, {-100, 103, 78}, {0, -1, 0});
	ASSERT_TRUE(edge);
	EXPECT_NEAR(edge->TimeU, 0.0, 1e-7);
	EXPECT_NEAR(edge->FrequencyV, 0.0, 1e-7);
	EXPECT_FALSE(editor::AudioSpectralPicking::Intersect(surface, {-101, 103, 0}, {0, -1, 0}));
}

TEST_F(AudioSpectralTest, PickingRaisedRidgesReturnsNearestDoubleSidedTriangle)
{
	auto surface = PickingSurface();
	auto display = std::make_shared<graphics::AudioSpectralDisplay>(*surface.Display);
	for (unsigned int row = 0; row < 3; ++row)
	{
		display->Heights[row * 5 + 1] = 0.8f;
		display->Heights[row * 5 + 3] = 0.8f;
	}
	surface.Display = display;
	const auto first = editor::AudioSpectralPicking::Intersect(surface, {-150, 3.0f + 40.0f * graphics::AudioSpectralModel::HeightRadiusScale, 0}, {1, 0, 0});
	ASSERT_TRUE(first);
	EXPECT_NEAR(first->TimeU, 0.125, 1e-6);
	EXPECT_NEAR(first->FrequencyV, 0.5, 1e-6);
	EXPECT_NEAR(first->Distance, 75.0, 1e-5);
	const auto reverse = editor::AudioSpectralPicking::Intersect(surface, {150, 3.0f + 40.0f * graphics::AudioSpectralModel::HeightRadiusScale, 0}, {-1, 0, 0});
	ASSERT_TRUE(reverse);
	EXPECT_NEAR(reverse->TimeU, 0.875, 1e-6);
	EXPECT_NEAR(reverse->Distance, 75.0, 1e-5);
}

TEST_F(AudioSpectralTest, PickingRejectsMorphAndInvalidMetadataOrRays)
{
	auto surface = PickingSurface();
	surface.Morph = 0.99f;
	EXPECT_FALSE(editor::AudioSpectralPicking::Intersect(surface, {0, 103, 0}, {0, -1, 0}));
	surface.Morph = 1.0f;
	EXPECT_FALSE(editor::AudioSpectralPicking::Intersect(surface, {0, 103, 0}, {0, 0, 0}));
	EXPECT_FALSE(editor::AudioSpectralPicking::Intersect(surface, {0, 103, 0}, {0, std::numeric_limits<float>::quiet_NaN(), 0}));
	auto invalid = std::make_shared<graphics::AudioSpectralDisplay>(*surface.Display);
	invalid->Heights.pop_back();
	surface.Display = invalid;
	EXPECT_FALSE(editor::AudioSpectralPicking::Intersect(surface, {0, 103, 0}, {0, -1, 0}));
}

TEST_F(AudioSpectralTest, SpectralShaderUniformDeclarationsAreRegisteredInResourceManifest)
{
	std::ifstream manifest(resources::ResolveResourceListPath());
	ASSERT_TRUE(manifest.is_open());
	std::set<std::string> uniforms;
	std::string line;
	while (std::getline(manifest, line))
	{
		std::istringstream fields(line);
		std::string type, name, uniform;
		fields >> type >> name;
		if (type != "2" || name != "audio_spectral") continue;
		while (fields >> uniform) uniforms.insert(uniform);
	}
	ASSERT_FALSE(uniforms.empty());
	const std::regex declaration(R"(^[ \t]*uniform[ \t]+\w+[ \t]+(\w+)[ \t]*;)",
		std::regex::ECMAScript | std::regex::multiline);
	unsigned int declarations = 0;
	for (const auto* filename : {"shaders/audio_spectral.vert", "shaders/audio_spectral.frag"})
	{
		std::ifstream shader(resources::ResolveResourcePath(filename));
		ASSERT_TRUE(shader.is_open()) << filename;
		std::ostringstream contents;
		contents << shader.rdbuf();
		const auto source = contents.str();
		for (auto match = std::sregex_iterator(source.begin(), source.end(), declaration);
			match != std::sregex_iterator(); ++match)
		{
			++declarations;
			EXPECT_TRUE(uniforms.contains((*match)[1].str())) << filename << ": " << (*match)[1].str();
		}
	}
	EXPECT_GT(declarations, 0u);
}

TEST_F(AudioSpectralTest, PickingUsesRetainedDrawTransformUnderTranslatedScaledAndMovedViews)
{
	auto surface = PickingSurface();
	auto display = std::make_shared<graphics::AudioSpectralDisplay>(*surface.Display);
	std::fill(display->Heights.begin(), display->Heights.end(), 0.4f);
	surface.Display = display;
	const auto point = 0.25f * graphics::AudioSpectralModel::LocalVertex(surface, 1u, 0u)
		+ 0.25f * graphics::AudioSpectralModel::LocalVertex(surface, 2u, 0u)
		+ 0.5f * graphics::AudioSpectralModel::LocalVertex(surface, 1u, 1u);
	for (const auto movement : {0.0f, 50.0f})
	{
		const auto model = glm::scale(glm::translate(glm::mat4(1.0f), {40.0f + movement, 10.0f, -30.0f}), {1.2f, 0.8f, 1.1f});
		const auto view = glm::lookAt(glm::vec3(160.0f + movement, 280.0f, 300.0f),
			glm::vec3(40.0f + movement, 10.0f, -30.0f), glm::vec3(0, 1, 0));
		const auto projection = glm::perspective(glm::radians(45.0f), 1.6f, 0.1f, 2000.0f);
		surface.ClipFromLocal = projection * view * model;
		surface.HasDrawTransform = true;
		const auto pixel = graphics::LoopGridProjection::Project(surface.ClipFromLocal, glm::mat4(1.0f), point, 20000, 12500);
		ASSERT_TRUE(pixel);
		// Input must use the retained successful draw stack even if animation has
		// already advanced parent/model transforms for the next render.
		const auto ray = graphics::LoopGridProjection::UnprojectToLocalRay(surface.ClipFromLocal, glm::mat4(1.0f), *pixel, 20000, 12500);
		ASSERT_TRUE(ray);
		const auto hit = editor::AudioSpectralPicking::Intersect(surface, ray->Origin, ray->Direction);
		ASSERT_TRUE(hit);
		EXPECT_NEAR(hit->TimeU, 0.3125, 0.002);
		EXPECT_NEAR(hit->FrequencyV, 0.25, 0.002);
	}
}

TEST_F(AudioSpectralTest, ControllerCloseFinishesQueuedStrokeAndKeepsOneUndoEntry)
{
	const auto loop = RecordedLoop();
	actions::ActionUndoHistory history;
	editor::AudioSpectralEditor controller;
	controller.SetUndoHistory(&history);
	controller.Open(loop);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
	ASSERT_TRUE(controller.BeginStroke(true, false, 0.025, 1000.0, 100.0f));
	controller.MoveStroke(std::pair{0.025, 1000.0}, -60.0f);
	controller.Close();
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Closed && loop->AudioSpectralPlayback(); })) << controller.Feedback();
	const auto edit = loop->AudioSpectralEdit();
	ASSERT_TRUE(edit && edit->Envelope);
	EXPECT_FLOAT_EQ(edit->Envelope->GainAt(1200ul), 0.0f);
	ASSERT_TRUE(history.Undo());
	EXPECT_FALSE(history.Undo());
	ASSERT_TRUE(history.Redo());
	EXPECT_EQ(loop->AudioSpectralEdit()->Envelope, edit->Envelope);
}

TEST_F(AudioSpectralTest, ControllerSourceInvalidationCancelsQueuedStrokeWithoutHistory)
{
	const auto loop = RecordedLoop();
	actions::ActionUndoHistory history;
	editor::AudioSpectralEditor controller;
	controller.SetUndoHistory(&history);
	controller.Open(loop);
	ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
	const auto generation = loop->AudioSourceGeneration();
	ASSERT_TRUE(controller.BeginStroke(true, false, 0.025, 1000.0, 100.0f));
	controller.MoveStroke(std::pair{0.025, 1000.0}, -60.0f);
	loop->SetSampleRate(44100.0f);
	ASSERT_NE(loop->AudioSourceGeneration(), generation);
	ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); }));
	EXPECT_FALSE(history.Undo());
	EXPECT_FALSE(loop->AudioSpectralEdit());
	EXPECT_FALSE(loop->AudioSpectralPlayback());
	controller.Close();
}

TEST_F(AudioSpectralTest, ClosingThenOpeningDifferentGenerationPreservesPreviousLoopsFinalPublication)
{
	const auto first = RecordedLoop(480000ul);
	const auto second = RecordedLoop();
	second->SetSampleRate(44100.0f);
	ASSERT_NE(first->AudioSourceGeneration(), second->AudioSourceGeneration());
	actions::ActionUndoHistory history;
	editor::AudioSpectralEditor controller;
	controller.SetUndoHistory(&history);
	controller.Open(first);
	ASSERT_TRUE(Pump(controller, first, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; })) << controller.Feedback();
	ASSERT_TRUE(controller.BeginStroke(false, false, 0.5, 20.0, 100.0f));
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	controller.Close();
	// Stop pumping at Closed, without waiting for the previous Loop's queued
	// synthesis/publication. Opening another source must not cancel that gate.
	ASSERT_TRUE(Pump(controller, first, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Closed; }));
	const auto committed = first->AudioSpectralEdit();
	ASSERT_TRUE(committed);
	ASSERT_FALSE(committed->Gains->IsUnity());
	controller.Open(second);
	ASSERT_TRUE(Pump(controller, second, [&]
	{
		first->EndMultiPlay(64u);
		const auto playback = first->AudioSpectralPlayback();
		return controller.Status() == editor::AudioSpectralEditor::State::Ready
			&& playback && playback->SpectralRevision == committed->SpectralRevision;
	})) << controller.Feedback();
	EXPECT_EQ(first->AudioSpectralEdit(), committed);
	ASSERT_TRUE(history.Undo());
	EXPECT_FALSE(history.Undo());
}

TEST_F(AudioSpectralTest, CommonLoopReadAndBounceUseComposedContentWithoutMovingPhaseAndMuteBothSeamContributors)
{
	std::string reason;
	const auto loop = RecordedLoop();
	const auto capture = loop->RequestAudioSpectralSource(reason);
	ASSERT_TRUE(capture) << reason;
	capture->Prepare();
	for (unsigned int i = 0; i < 100 && capture->Status() == audio::AudioSpectralSourceCapture::State::Pending; ++i)
		loop->EndMultiPlay(64u);
	const auto source = capture->Source();
	ASSERT_TRUE(source);
	ASSERT_GT(source->Layout.FadeSamps, 0u);
	ASSERT_LT(source->Layout.FadeSamps, source->Layout.LogicalLength);
	auto pcm = std::make_shared<audio::AudioSpectralPcm>();
	pcm->Source = source;
	// An already reconstructed spectral value doubles original PCM uniformly.
	pcm->ProcessedSamples = std::make_shared<const std::vector<float>>(source->Samples.size(), 1.0f);
	std::vector<float> gains(source->Layout.LogicalLength, 0.25f);
	std::fill(gains.end() - source->Layout.FadeSamps, gains.end(), 0.0f);
	const auto envelope = audio::AudioSpectralEnvelope::FromGains(gains, reason);
	ASSERT_TRUE(envelope);
	const auto playback = audio::AudioSpectralPlaybackSnapshot::Create(pcm, envelope, 1u, 1u, 1u, reason);
	ASSERT_TRUE(playback) << reason;
	loop->SetBodyPlayIndex(0ul);
	const auto phase = loop->PlayIndex();
	const auto length = loop->LoopLength();
	const auto physical = loop->PhysicalLoopLength();
	ASSERT_TRUE(loop->PublishAudioSpectralPlayback(playback, reason)) << reason;
	loop->EndMultiPlay(0u); // Adoption does not alter transport coordinates.
	EXPECT_EQ(loop->PlayIndex(), phase);
	loop->EndMultiPlay(static_cast<unsigned int>(length)); // Settle transition over exactly one complete turn.
	EXPECT_EQ(loop->PlayIndex(), phase);
	EXPECT_EQ(loop->LoopLength(), length);
	EXPECT_EQ(loop->PhysicalLoopLength(), physical);
	std::vector<float> body(16u);
	ASSERT_EQ(loop->ReadBlock(body.data(), 0, static_cast<unsigned int>(body.size())), body.size());
	for (const auto sample : body) EXPECT_FLOAT_EQ(sample, 0.25f); // Envelope composed exactly once.
	const auto seamOffset = static_cast<int>(length - source->Layout.FadeSamps);
	std::vector<float> seam((std::min)(source->Layout.FadeSamps, 16u));
	ASSERT_EQ(loop->ReadBlock(seam.data(), seamOffset, static_cast<unsigned int>(seam.size())), seam.size());
	for (const auto sample : seam) EXPECT_FLOAT_EQ(sample, 0.0f); // Prefix cannot leak through the Hanning mix.
	const auto bounce = std::make_shared<BounceCapture>();
	loop->WriteBlock(nullptr, bounce, 0, 16u);
	ASSERT_EQ(bounce->Samples.size(), body.size());
	EXPECT_EQ(bounce->Samples, body);
	loop->WriteBlock(nullptr, bounce, seamOffset, static_cast<unsigned int>(seam.size()));
	EXPECT_EQ(bounce->Samples, seam);
	EXPECT_EQ(loop->PlayIndex(), phase); // Repeated DAC/bounce reads share a phase until EndMultiPlay.
}

// Explicit diagnostic only: Debug timings are evidence, never latency guarantees.
TEST_F(AudioSpectralTest, DISABLED_ProfileHighFrequencyInputMap)
{
    std::string reason;
    for (const auto seconds : { 2ul, 10ul, 30ul })
    {
        const auto source = Source(seconds * 48000ul);
        audio::AudioSpectralTransformConfig transform;
        std::shared_ptr<const audio::AudioSpectralResourceBudget::Reservation> resources;
        ASSERT_TRUE(audio::AudioSpectralDsp::SelectTransform(source->Layout, nullptr, transform, resources, reason));
        const auto analysis = audio::AudioSpectralDsp::Analyze(source, transform, reason);
        ASSERT_TRUE(analysis) << reason;
        const auto estimate = audio::AudioSpectralDsp::EstimateWorkingMemory(source->Layout, transform, reason);
        ASSERT_TRUE(estimate) << reason;
        auto input = audio::AudioSpectralGainField::ExposureMap::Create(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
        ASSERT_TRUE(input) << reason;
        for (const bool minimum : {false,true})
        {
        input->Clear();
        std::vector<double> milliseconds;
        milliseconds.reserve(1000);
        double t = 0.5, hz = 18000.0, total = 0;
        for (int sample = 0; sample < 1000; ++sample)
        {
            const auto nextT = 0.5 + 0.04*std::sin(sample*0.02);
            const auto nextHz = 18000.0*std::exp2(0.1*std::sin(sample*0.013));
            const auto began = std::chrono::steady_clock::now();
            input->Accumulate(*analysis, t, hz, nextT, nextHz, 1.0/60.0, reason,
                minimum ? 0.005 : audio::AudioSpectralConfig::BrushTimeRadiusSeconds,
                minimum ? 0.0625 : audio::AudioSpectralConfig::BrushFrequencyRadiusOctaves);
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
            milliseconds.push_back(elapsed); total += elapsed;
            t = nextT; hz = nextHz;
        }
        std::sort(milliseconds.begin(), milliseconds.end());
#ifdef _DEBUG
        constexpr auto configuration = "Debug";
#else
        constexpr auto configuration = "Release";
#endif
        std::cout << "Input map profile " << configuration << " " << (minimum ? "minimum 5ms/.0625oct" : "default") << ": loop=" << seconds << "s / 1000 high-frequency pointer samples"
            << " mean_ms=" << total / milliseconds.size() << " p95_ms=" << milliseconds[950]
            << " max_ms=" << milliseconds.back() << " dirty_cells=" << input->DirtyCellCount()
            << " grid_cells=" << analysis->Geometry.FrameCount * analysis->Geometry.BinCount
            << " admission_MiB=" << estimate->WorkingBytes / (1024.0*1024.0) << std::endl;
        ASSERT_TRUE(reason.empty()) << reason;
        }
    }
}
TEST_F(AudioSpectralTest, DISABLED_ProfileSpectralPipeline)
{
	using Clock = std::chrono::steady_clock;
#ifdef _DEBUG
	constexpr const char* configuration = "Debug";
#else
	constexpr const char* configuration = "Release";
#endif
	const auto milliseconds = [](Clock::time_point first, Clock::time_point last)
	{
		return std::chrono::duration<double, std::milli>(last - first).count();
	};
	std::string reason;
	for (const auto seconds : { 2ul, 10ul, 30ul })
	{
		SCOPED_TRACE(seconds);
		const auto source = Source(seconds * 48000ul);
		audio::AudioSpectralTransformConfig transform;
        std::shared_ptr<const audio::AudioSpectralResourceBudget::Reservation> resources;
        ASSERT_TRUE(audio::AudioSpectralDsp::SelectTransform(source->Layout, nullptr, transform, resources, reason));
        const auto estimate = audio::AudioSpectralDsp::EstimateWorkingMemory(source->Layout, transform, reason);
		ASSERT_TRUE(estimate) << reason;
		const auto analysisStart = Clock::now();
		const auto analysis = audio::AudioSpectralDsp::Analyze(source, transform, reason);
		const auto analysisEnd = Clock::now();
		ASSERT_TRUE(analysis) << reason;
		auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
		ASSERT_TRUE(gains);
		ASSERT_TRUE(gains->SetGainDb(analysis->Geometry.FrameCount / 2, 80u, 6.0f));
		const auto reconstructStart = Clock::now();
		const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis, *gains, reason);
		const auto reconstructEnd = Clock::now();
		ASSERT_TRUE(pcm) << reason;
		const auto envelopeStart = Clock::now();
		const auto before = audio::AudioSpectralEnvelope::Unity(source->Layout.LogicalLength, reason);
		ASSERT_TRUE(before);
		const auto stroke = audio::AudioSpectralEnvelopeStroke::Begin(before, source->Layout.SampleRate, reason);
		ASSERT_TRUE(stroke);
		ASSERT_TRUE(stroke->PaintSegment(24000.0, 0.0f, 48000.0, 1.0f));
		const auto envelope = stroke->Snapshot(reason);
		ASSERT_TRUE(envelope);
		const auto playback = audio::AudioSpectralPlaybackSnapshot::Create(pcm, envelope, 1u, 1u, 1u, reason);
		const auto envelopeEnd = Clock::now();
		ASSERT_TRUE(playback) << reason;
        const auto displayStart = Clock::now();
        const auto display = graphics::AudioSpectralModel::BuildDisplay(*analysis, &*gains, envelope.get(), 1u,
            graphics::AudioSpectralModel::DefaultTimeSegments, graphics::AudioSpectralModel::DefaultFrequencySegments, reason);
        const auto displayEnd = Clock::now();
        ASSERT_TRUE(display) << reason;
        const auto sparseStart = Clock::now();
        const auto sparseDisplay = graphics::AudioSpectralModel::BuildDisplay(*analysis, &*gains, envelope.get(), 1u,
            1024u, graphics::AudioSpectralModel::DefaultFrequencySegments, reason);
        const auto sparseEnd = Clock::now();
        ASSERT_TRUE(sparseDisplay) << reason;
        std::cout << "Mesh comparison: 1024 columns " << milliseconds(sparseStart,sparseEnd)
            << " ms; 4095 columns " << milliseconds(displayStart,displayEnd) << " ms\n";
		std::cout << "Spectral profile " << configuration << ": " << seconds << " s / 48000 Hz; FFT " << transform.WindowSamps << "; hop " << transform.HopSamps << "; display " << milliseconds(displayStart, displayEnd) << " ms; estimate "
			<< static_cast<double>(estimate->WorkingBytes) / (1024.0 * 1024.0) << " MiB; analysis "
			<< milliseconds(analysisStart, analysisEnd) << " ms; reconstruction "
			<< milliseconds(reconstructStart, reconstructEnd) << " ms; envelope construction/snapshot + playback validation/overview "
			<< milliseconds(envelopeStart, envelopeEnd) << " ms\n";
        gains->SetOwned(analysis->Geometry.FrameCount/2,80,true);
        const auto recoveryStart=Clock::now();
        const auto recovered=audio::AudioSpectralDsp::Reconstruct(analysis,*gains,reason,{},false,envelope);
        const auto recoveryEnd=Clock::now(); ASSERT_TRUE(recovered) << reason;
        for(std::size_t frame=0;frame<gains->FrameCount();++frame) gains->SetOwned(frame,80,true);
        const auto spreadStart=Clock::now();
        const auto spread=audio::AudioSpectralDsp::Reconstruct(analysis,*gains,reason,{},false,envelope);
        const auto spreadEnd=Clock::now(); ASSERT_TRUE(spread) << reason;
        std::cout << "Recovery synthesis " << seconds << "s sparse=" << milliseconds(recoveryStart,recoveryEnd)
            << "ms all-frames=" << milliseconds(spreadStart,spreadEnd) << "ms\n";
	}
}

TEST_F(AudioSpectralTest, MinimumBrushBetweenFramesAndBinsReceivesFullRate)
{
    std::string reason;
    for (const auto hop : {256u, 1024u})
    {
        const auto analysis = audio::AudioSpectralDsp::Analyze(Source(48000ul), {1u,4096u,hop}, reason);
        ASSERT_TRUE(analysis) << reason;
        const auto frame = analysis->Geometry.FrameCount / 2u;
        const auto seconds = (analysis->Geometry.FrameCentre(frame,hop)+hop*0.5)/48000.0;
        for (const auto bin : {2u, 200u})
        {
            const auto spacing = 48000.0/4096.0;
            const auto hz = std::sqrt(bin*(bin+1.0))*spacing;
            auto input = audio::AudioSpectralGainField::ExposureMap::Create(analysis->Geometry.FrameCount,analysis->Geometry.BinCount,reason);
            ASSERT_TRUE(input);
            ASSERT_TRUE(input->Accumulate(*analysis,seconds,hz,seconds,hz,0.05,reason,0.005,0.025));
            for (const auto f : {frame,frame+1u})
                for (const auto b : {bin,bin+1u})
                    EXPECT_NEAR(input->ExposureAt(f,b),0.05,1e-12) << hop << '/' << f << '/' << b;
        }
    }
}

TEST_F(AudioSpectralTest, DenseSurfaceTopologyStaysWithinTextureAndHostLimits)
{
    constexpr auto time = graphics::AudioSpectralModel::DefaultTimeSegments;
    constexpr auto frequency = graphics::AudioSpectralModel::DefaultFrequencySegments;
    EXPECT_EQ(time,4095u);
    EXPECT_EQ(frequency,256u);
    constexpr auto cells = (time+1ull)*(frequency+1ull);
    constexpr auto hostBytes = cells*sizeof(glm::vec2) + time*frequency*6ull*sizeof(unsigned int)
        + 5ull*cells*sizeof(float); // pending/uploaded/drawn plus worker and retained result.
    EXPECT_LT(hostBytes,audio::AudioSpectralConfig::DisplayWorkingBytes);
}

TEST_F(AudioSpectralTest, CachedDisplayMatchesDirectCornersAcrossChangingEnvelope)
{
    std::string reason;
    const auto analysis = audio::AudioSpectralDsp::Analyze(Source(4139ul),Transform(),reason);
    ASSERT_TRUE(analysis);
    auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount,analysis->Geometry.BinCount,reason);
    ASSERT_TRUE(gains);
    gains->SetGainDb(analysis->Geometry.FrameCount/2u,5u,24.0f);
    std::vector<float> envelopeValues(4139ul);
    for (unsigned long i=0; i<4139ul; ++i) envelopeValues[i] = i%37 == 0 ? 0.0f : static_cast<float>(0.001+15.0*(i/4139.0));
    const auto envelope = audio::AudioSpectralEnvelope::FromGains(envelopeValues,reason);
    ASSERT_TRUE(envelope);
    const auto display = graphics::AudioSpectralModel::BuildDisplay(*analysis,&*gains,envelope.get(),1u,4095u,32u,reason);
    ASSERT_TRUE(display) << reason;
    for (unsigned int column=0; column<4095u; column+=31u)
    {
        const auto bodySample = column*4139.0/4095.0;
        const auto fp=(bodySample-analysis->Geometry.FirstFrameCentre)/analysis->Transform.HopSamps;
        const auto frame=static_cast<std::size_t>(fp);
        for (unsigned int row=0; row<=32; ++row)
        {
            const auto bp=display->FrequencyAt(row/32.0)*analysis->Transform.WindowSamps/48000.0;
            const auto bin=(std::min)(static_cast<unsigned int>(bp),analysis->Geometry.BinCount-1u);
            const auto nextBin=(std::min)(bin+1u,analysis->Geometry.BinCount-1u);
            const auto height=[&](std::size_t f,unsigned int b)
            {
                const auto magnitude=analysis->Magnitude(f,b)*gains->GainLinear(f,b)*envelope->GainAt(static_cast<unsigned long>(bodySample));
                return magnitude>0 ? static_cast<float>(std::clamp((20.0*std::log10(magnitude)+90.0)/114.0,0.0,1.0)) : 0.0f;
            };
            const auto expected=std::lerp(std::lerp(height(frame,bin),height(frame,nextBin),bp-bin),
                std::lerp(height(frame+1u,bin),height(frame+1u,nextBin),bp-bin),fp-frame);
            EXPECT_NEAR(display->Heights[row*4096u+column],expected,1e-6);
        }
    }
}

TEST_F(AudioSpectralTest, RelativeSweepPlateauSilenceBoundsAndLocalNeutral)
{
    using Stroke = audio::AudioSpectralEnvelopeStroke;
    std::string reason;
    std::vector<float> baseline(1000u, 0.5f); baseline[250] = 0.0f;
    const auto before = audio::AudioSpectralEnvelope::FromGains(baseline, reason);
    auto stroke = Stroke::Begin(before, 1000.0f, reason, 0.02);
    auto map = Stroke::RelativeMap::Create(1000ul, 1000.0f, 0.02, reason);
    Stroke::RelativePath path;
    ASSERT_TRUE(map->Accumulate(200, 0, 200, 80, path, reason));
    ASSERT_TRUE(map->Accumulate(200, 80, 400, 80, path, reason));
    ASSERT_TRUE(stroke->ApplyRelativeMap(*map));
    auto snapshot = stroke->Snapshot(reason);
    const auto target = audio::AudioSpectralEnvelope::TargetFromDragPixels(80);
    for (unsigned long i = 200; i <= 400; ++i)
        EXPECT_FLOAT_EQ(snapshot->GainAt(i), static_cast<float>(audio::AudioSpectralEnvelope::RelativeGain(baseline[i],target,std::pow(10.0,audio::AudioSpectralConfig::SpectralFloorDb/20.0),audio::AudioSpectralEnvelope::MaximumGain())));
    map->Clear();
    ASSERT_TRUE(map->Accumulate(400, 80, 400, 0, path, reason));
    ASSERT_TRUE(stroke->ApplyRelativeMap(*map));
    snapshot = stroke->Snapshot(reason);
    EXPECT_FLOAT_EQ(snapshot->GainAt(400), baseline[400]);
    EXPECT_FLOAT_EQ(snapshot->GainAt(200), baseline[200] * target);
    EXPECT_GT(snapshot->GainAt(250), 0.0f);
    EXPECT_FLOAT_EQ(snapshot->GainAt(500), baseline[500]);
    map->Clear();
    ASSERT_TRUE(map->Accumulate(200, 80, 200, 160, path, reason));
    ASSERT_TRUE(stroke->ApplyRelativeMap(*map));
    EXPECT_LE(stroke->Snapshot(reason)->GainAt(200), audio::AudioSpectralEnvelope::MaximumGain());
}

TEST_F(AudioSpectralTest, RelativeSweepDiagonalSubdivisionIncludesSnapshotFeathersAndHandoffs)
{
    using Stroke = audio::AudioSpectralEnvelopeStroke;
    std::string reason;
    const auto before = audio::AudioSpectralEnvelope::Unity(1000ul, reason);
    for (const auto interval : {std::pair{0.25, 0.75}, std::pair{100.25, 500.75}, std::pair{0.0, 1000.0}, std::pair{1000.0, 0.0}})
    {
        auto whole = Stroke::Begin(before, 1000.0f, reason, 0.02);
        auto split = Stroke::Begin(before, 1000.0f, reason, 0.02);
        auto map = Stroke::RelativeMap::Create(1000ul, 1000.0f, 0.02, reason);
        Stroke::RelativePath wholePath, splitPath;
        ASSERT_TRUE(map->Accumulate(interval.first, -80, interval.second, 120, wholePath, reason));
        whole->ApplyRelativeMap(*map); map->Clear();
        constexpr int subdivisions = 16;
        for (int i = 0; i < subdivisions; ++i)
        {
            const auto a = static_cast<double>(i) / subdivisions;
            const auto b = static_cast<double>(i + 1) / subdivisions;
            ASSERT_TRUE(map->Accumulate(std::lerp(interval.first, interval.second, a), std::lerp(-80.0, 120.0, a),
                std::lerp(interval.first, interval.second, b), std::lerp(-80.0, 120.0, b), splitPath, reason));
            if (i % 3 == 0) { split->ApplyRelativeMap(*map); map->Clear(); }
        }
        split->ApplyRelativeMap(*map);
        EXPECT_EQ(splitPath.Winner(), 1u);
        const auto expected = whole->Snapshot(reason), actual = split->Snapshot(reason);
        ASSERT_TRUE(expected && actual);
        for (unsigned long i = 0; i < 1000; ++i) EXPECT_NEAR(actual->GainAt(i), expected->GainAt(i), 1e-6) << i;
        if (interval.first == 0.0 || interval.second == 0.0)
            EXPECT_FLOAT_EQ(actual->GainAt(0), audio::AudioSpectralEnvelope::TargetFromDragPixels(120)); // Later seam visit wins.
    }
}

TEST_F(AudioSpectralTest, RelativeSweepVerticalMatchesLegacyBandAndReversalUsesLaterEqualWinner)
{
    using Stroke = audio::AudioSpectralEnvelopeStroke;
    std::string reason;
    std::vector<float> gains(1000ul, 0.5f); gains[0] = 0.0f;
    const auto before = audio::AudioSpectralEnvelope::FromGains(gains, reason);
    auto legacy = Stroke::Begin(before, 1000.0f, reason, 0.02);
    auto swept = Stroke::Begin(before, 1000.0f, reason, 0.02);
    auto map = Stroke::RelativeMap::Create(1000ul, 1000.0f, 0.02, reason);
    Stroke::RelativePath path;
    double previous = 0.0;
    for (const auto displacement : {80.0, -160.0, 0.0})
    {
        ASSERT_TRUE(map->Accumulate(0, previous, 0, displacement, path, reason));
        swept->ApplyRelativeMap(*map); map->Clear();
        legacy->SetRelativeBand(0.25, audio::AudioSpectralEnvelope::TargetFromDragPixels(displacement));
        const auto a = legacy->Snapshot(reason), b = swept->Snapshot(reason);
        EXPECT_TRUE(std::equal(a->Gains().begin(), a->Gains().end(), b->Gains().begin()));
        previous = displacement;
    }
    EXPECT_EQ(swept->Snapshot(reason), before);
    ASSERT_TRUE(map->Accumulate(200, 80, 400, 80, path, reason));
    ASSERT_TRUE(map->Accumulate(400, -160, 200, -160, path, reason));
    swept->ApplyRelativeMap(*map);
    const auto reversed = swept->Snapshot(reason);
    for (unsigned long i = 200; i <= 400; ++i) EXPECT_FLOAT_EQ(reversed->GainAt(i), 0.0f);
    EXPECT_LT(reversed->GainAt(199), before->GainAt(199));
    EXPECT_FLOAT_EQ(reversed->GainAt(500), before->GainAt(500));
}

TEST_F(AudioSpectralTest, FrozenBandProjectionIgnoresYAndPreservesDpiAndClickedTime)
{
    const auto clip = glm::perspective(glm::radians(42.0f), 1.5f, 0.1f, 1000.0f)
        * glm::lookAt(glm::vec3(80, 200, 240), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const utils::Position2d anchor{300, 220};
    const auto vertical = editor::AudioSpectralPicking::BandTime(clip, 10, 100, anchor, {300, 500}, 900, 600, 0.345, 2);
    ASSERT_TRUE(vertical); EXPECT_DOUBLE_EQ(*vertical, 0.345);
    const auto zoomed = editor::AudioSpectralPicking::BandTime(clip, 10, 100, anchor, {500, 220}, 900, 600, 0.345, 2, 0.25);
    const auto diagonal = editor::AudioSpectralPicking::BandTime(clip, 10, 100, anchor, {500, 500}, 900, 600, 0.345, 2);
    const auto horizontal = editor::AudioSpectralPicking::BandTime(clip, 10, 100, anchor, {500, 220}, 900, 600, 0.345, 2);
    const auto dpi = editor::AudioSpectralPicking::BandTime(clip, 10, 100, {600, 440}, {1000, 1000}, 1800, 1200, 0.345, 2);
    ASSERT_TRUE(diagonal && horizontal && dpi && zoomed);
    EXPECT_NEAR(*zoomed - 0.345, (*horizontal - 0.345) * 0.25, 1e-6);
    EXPECT_DOUBLE_EQ(*diagonal, *horizontal); EXPECT_DOUBLE_EQ(*diagonal, *dpi);
    EXPECT_GT(*diagonal, *vertical);
}

TEST_F(AudioSpectralTest, SharedInputBudgetBoundsCancelledMixedTransitionsAndLowOverlap)
{
    for (const auto coefficients : {1000ull, 2000ull, 8000ull})
    {
        constexpr std::uint64_t samples = 1000u;
        const auto shared = (std::max)(coefficients * audio::AudioSpectralConfig::InputMapBytesPerCoefficient,
            samples * audio::AudioSpectralConfig::BandMapBytesPerSample);
        EXPECT_GE(shared, coefficients * 12u + samples * 48u);
        EXPECT_GE(shared, coefficients * 24u + samples * 24u);
    }
    std::string reason;
    const auto source = Source(48000ul);
    const auto estimate = audio::AudioSpectralDsp::EstimateWorkingMemory(source->Layout, {1u, 4096u, 2048u}, reason);
    ASSERT_TRUE(estimate) << reason;
    const auto coefficientBytes = estimate->Geometry.FrameCount * estimate->Geometry.BinCount
        * audio::AudioSpectralConfig::InputMapBytesPerCoefficient;
    EXPECT_GT(static_cast<std::uint64_t>(48000ul) * audio::AudioSpectralConfig::BandMapBytesPerSample, coefficientBytes);
}

TEST_F(AudioSpectralTest, RelativeWinnerMetadataOnlyRepaintRefreshesRampsAndPreservesSilence)
{
    using Stroke = audio::AudioSpectralEnvelopeStroke;
    std::string reason;
    std::vector<float> baseline(1000ul, 1.0f);
    std::fill_n(baseline.begin(), 199, 0.0f);
    const auto before = audio::AudioSpectralEnvelope::FromGains(baseline, reason);
    auto stroke = Stroke::Begin(before, 1000.0f, reason, 0.05);
    auto map = Stroke::RelativeMap::Create(1000ul, 1000.0f, 0.05, reason);
    Stroke::RelativePath path;
    ASSERT_TRUE(map->Accumulate(179, 0, 179, 0, path, reason));
    ASSERT_TRUE(map->Accumulate(219, 80, 219, 80, path, reason));
    stroke->ApplyRelativeMap(*map);
    const auto ramped = stroke->Snapshot(reason);
    map->Clear();
    // The new band wins only baseline-zero samples; every raw gain stays equal.
    // Matching neighbouring target metadata removes a competing ramp.
    ASSERT_TRUE(map->Accumulate(179, 80, 179, 80, path, reason));
    EXPECT_TRUE(stroke->ApplyRelativeMap(*map));
    const auto refreshed = stroke->Snapshot(reason);
    EXPECT_NE(ramped->GainAt(199), refreshed->GainAt(199));
    const auto shoulder = 1.0 - std::pow(20.0 / 50.0, 2);
    EXPECT_FLOAT_EQ(refreshed->GainAt(199), static_cast<float>(1.0 + shoulder * shoulder
        * (audio::AudioSpectralEnvelope::TargetFromDragPixels(80) - 1.0))); // Same raw candidate as before repaint.
    for (unsigned long index = 0; index < 199; ++index)
    { EXPECT_GE(ramped->GainAt(index), 0.0f); EXPECT_GE(refreshed->GainAt(index), ramped->GainAt(index)); }
}

TEST_F(AudioSpectralTest, RelativeSweptControllerRetainsFullBackloggedPathAndOneUndo)
{
    using Stroke = audio::AudioSpectralEnvelopeStroke;
    const auto loop = RecordedLoop(8192ul);
    const auto worker = std::make_shared<audio::AudioSpectralWorker>();
    editor::AudioSpectralEditor controller(worker);
    actions::ActionUndoHistory history; controller.SetUndoHistory(&history); controller.Open(loop);
    ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
    const auto before = controller.EditState();
        ASSERT_EQ(before->Source->Layout.LogicalLength, 8192ul);
    std::string reason;
    const auto baseline = audio::AudioSpectralEnvelope::Unity(before->Source->Layout.LogicalLength, reason);
    auto expected = Stroke::Begin(baseline, 48000.0f, reason, 0.005);
    auto map = Stroke::RelativeMap::Create(baseline->Length(), 48000.0f, 0.005, reason);
    Stroke::RelativePath path;
    std::promise<void> entered, resume;
    const auto resumed = resume.get_future().share();
    struct Release { std::promise<void>& Resume; ~Release() { Resume.set_value(); } };
    {
        Release release{resume};
        worker->RequestContent(loop.get(), [&entered, resumed] { entered.set_value(); resumed.wait(); });
        ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
        ASSERT_TRUE(controller.BeginStroke(true, false, 1200.0 / 48000.0, 1000, 100, 0.05, 0.25, 0.005));
        controller.MoveStroke(std::nullopt, 180);
        ASSERT_TRUE(map->Accumulate(1200, 0, 1200, 80, path, reason));
        std::this_thread::sleep_for(std::chrono::duration<double>(audio::AudioSpectralConfig::StrokeUpdateSeconds));
        controller.Update(); // Freeze first map while subsequent path accumulates separately.
        double sample = 1200, drag = 80;
        for (const auto next : {std::pair{4000.0, 80.0}, std::pair{6500.0, -80.0}, std::pair{5500.0, 0.0},
            std::pair{7000.0, 60.0}, std::pair{7200.0, 60.0}})
        {
            controller.MoveStroke(std::pair{next.first / 48000.0, 1000.0}, 100 + next.second);
            ASSERT_TRUE(map->Accumulate(sample, drag, next.first, next.second, path, reason));
            sample = next.first; drag = next.second;
        }
        for (int i = 0; i < 1000; ++i) controller.MoveStroke(std::nullopt, 160); // Stationary holds cannot compound.
        controller.EndStroke(false);
        EXPECT_EQ(controller.EditState(), before);
    }
    ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); })) << controller.Feedback();
    expected->ApplyRelativeMap(*map);
    const auto desired = expected->Snapshot(reason), actual = controller.EditState()->Envelope;
    ASSERT_TRUE(actual && desired);
    for (unsigned long i = 0; i < baseline->Length(); ++i) EXPECT_NEAR(actual->GainAt(i), desired->GainAt(i), 1e-5) << i;
    EXPECT_FLOAT_EQ(actual->GainAt(3000), audio::AudioSpectralEnvelope::TargetFromDragPixels(80));
    ASSERT_TRUE(history.Undo()); EXPECT_EQ(controller.EditState()->Envelope, before->Envelope);
    EXPECT_FALSE(history.Undo());
}

TEST_F(AudioSpectralTest, CancelledMixedStrokeHandoffsPreserveNewStrokeAndUndo)
{
    for (const bool cancelledBand : {true, false})
    {
        const auto loop = RecordedLoop(8192ul);
        const auto worker = std::make_shared<audio::AudioSpectralWorker>();
        editor::AudioSpectralEditor controller(worker);
        actions::ActionUndoHistory history; controller.SetUndoHistory(&history); controller.Open(loop);
        ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
        const auto before = controller.EditState();
        ASSERT_EQ(before->Source->Layout.LogicalLength, 8192ul);
        std::promise<void> entered, resume;
        const auto resumed = resume.get_future().share();
        struct Release { std::promise<void>& Resume; ~Release() { Resume.set_value(); } };
        {
            Release release{resume};
            worker->RequestContent(loop.get(), [&entered, resumed] { entered.set_value(); resumed.wait(); });
            ASSERT_EQ(entered.get_future().wait_for(std::chrono::seconds(2)), std::future_status::ready);
            ASSERT_TRUE(controller.BeginStroke(cancelledBand, false, 1200.0 / 48000.0, 1000, 100));
            std::this_thread::sleep_for(std::chrono::duration<double>(audio::AudioSpectralConfig::StrokeUpdateSeconds));
            controller.MoveStroke(std::pair{3000.0 / 48000.0, 1000.0}, 180);
            controller.EndStroke(true); // Queued receipt dies; UI maps drop before next admission.
            ASSERT_TRUE(controller.BeginStroke(!cancelledBand, false, 4000.0 / 48000.0, 1000, 100));
            std::this_thread::sleep_for(std::chrono::duration<double>(audio::AudioSpectralConfig::StrokeUpdateSeconds));
            controller.MoveStroke(std::pair{6000.0 / 48000.0, 1000.0}, 180);
            controller.EndStroke(false);
        }
        ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); })) << controller.Feedback();
        const auto after = controller.EditState();
        if (cancelledBand) { EXPECT_EQ(after->Envelope, before->Envelope); EXPECT_NE(after->Gains, before->Gains); }
        else { EXPECT_EQ(after->Gains, before->Gains); ASSERT_TRUE(after->Envelope); EXPECT_GT(after->Envelope->GainAt(6000), 1.0f); }
        ASSERT_TRUE(history.Undo());
        EXPECT_EQ(controller.EditState()->Gains, before->Gains); EXPECT_EQ(controller.EditState()->Envelope, before->Envelope);
        EXPECT_FALSE(history.Undo());
    }
}

TEST_F(AudioSpectralTest, InvalidBroadbandPointRetainsCapturedCentreAndOrdinaryMissBreaksPath)
{
    const auto loop = RecordedLoop(); editor::AudioSpectralEditor controller; controller.Open(loop);
    ASSERT_TRUE(Pump(controller, loop, [&] { return controller.Status() == editor::AudioSpectralEditor::State::Ready; }));
    ASSERT_TRUE(controller.BeginStroke(true, false, 1200.25 / 48000.0, 1000, 100));
    controller.MoveStroke(std::pair{std::numeric_limits<double>::quiet_NaN(), 1000.0}, -60);
    controller.EndStroke(false);
    ASSERT_TRUE(Pump(controller, loop, [&] { return !controller.HasStroke(); }));
    ASSERT_TRUE(controller.EditState()->Envelope);
    EXPECT_FLOAT_EQ(controller.EditState()->Envelope->GainAt(1200), 0.0f);
    ASSERT_TRUE(controller.BeginStroke(false, false, 0.03, 1000, 100));
    controller.MoveStroke(std::pair{0.04, -1.0}, 100);
    EXPECT_FALSE(controller.BrushLevelDb().has_value());
    controller.EndStroke(true);
}

TEST_F(AudioSpectralTest, DISABLED_RelativeSweepInputPerformanceDiagnostic)
{
    using Stroke = audio::AudioSpectralEnvelopeStroke;
    for (const unsigned long seconds : {2ul, 10ul, 30ul})
    {
        std::string reason;
        const auto length = seconds * 48000ul;
        auto map = Stroke::RelativeMap::Create(length, 48000.0f, 0.05, reason);
        ASSERT_TRUE(map) << reason;
        Stroke::RelativePath path;
        auto start = std::chrono::steady_clock::now();
        ASSERT_TRUE(map->Accumulate(0, 80, length, 80, path, reason));
        const auto full = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        map->Clear();
        start = std::chrono::steady_clock::now();
        constexpr int count = 100;
        for (int i = 0; i < count; ++i)
            ASSERT_TRUE(map->Accumulate(static_cast<double>(length) * i / count, 80,
                static_cast<double>(length) * (i + 1) / count, 80, path, reason));
        const auto average = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / count;
        std::cout << "RelativeInput Debug " << seconds << " s: full-loop " << full
            << " ms; 1/100-loop pointer step mean " << average << " ms\n";
    }
}

TEST_F(AudioSpectralTest, DetailAdmissionLeavesRoomForAnotherShortEditedLoop)
{
    std::string reason;
    const auto budget=std::make_shared<audio::AudioSpectralResourceBudget>();
    audio::AudioSpectralTransformConfig first,second;
    std::shared_ptr<const audio::AudioSpectralResourceBudget::Reservation> r1,r2;
    ASSERT_TRUE(audio::AudioSpectralDsp::SelectTransform(Source(480000ul)->Layout,budget,first,r1,reason));
    EXPECT_EQ(first.WindowSamps,4096u);
    EXPECT_EQ(first.HopSamps,512u);
    EXPECT_FALSE(reason.empty());
    ASSERT_TRUE(audio::AudioSpectralDsp::SelectTransform(Source(4096ul)->Layout,budget,second,r2,reason)) << reason;
    EXPECT_LE(budget->UsedBytes(),audio::AudioSpectralConfig::MaxWorkingBytes);
}

TEST_F(AudioSpectralTest, MaskedExposureRecoversOnlyTouchedCoefficientsAndRetainsOriginalSilence)
{
    std::string reason;
    for (const float mask : {0.0f, 1.0e-30f, 0.2f})
    {
        const auto analysis = audio::AudioSpectralDsp::Analyze(Source(4096), Transform(), reason);
        ASSERT_TRUE(analysis) << reason;
        auto gains = audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount, analysis->Geometry.BinCount, reason);
        const auto envelope = audio::AudioSpectralEnvelope::FromGains(std::vector<float>(4096, mask), reason);
        auto exposure = audio::AudioSpectralGainField::ExposureMap::Create(gains->FrameCount(), gains->BinCount(), reason);
        EXPECT_FALSE(gains->ApplyExposureMap(*analysis,*exposure,true,reason,envelope.get()));
        EXPECT_FALSE(gains->HasOwnedCoefficients());
        ASSERT_TRUE(exposure->Accumulate(*analysis, 0.03, 1000, 0.03, 1000, 0.01, reason, 0.005, 0.0625));
        ASSERT_TRUE(gains->ApplyExposureMap(*analysis, *exposure, true, reason, envelope.get()));
        ASSERT_TRUE(gains->HasOwnedCoefficients());
        std::size_t picked = gains->FrameCount(); unsigned int pickedBin = 0;
        for (std::size_t frame=0; frame<gains->FrameCount(); ++frame)
            for (unsigned int bin=0; bin<gains->BinCount(); ++bin)
                if (exposure->ExposureAt(frame,bin) > 0.0 && analysis->Magnitude(frame,bin)>0.0)
                {
                    EXPECT_TRUE(gains->IsOwned(frame,bin)); picked=frame; pickedBin=bin;
                }
                else { EXPECT_FALSE(gains->IsOwned(frame,bin)); EXPECT_FLOAT_EQ(gains->GainDb(frame,bin),0.0f); }
        ASSERT_LT(picked,gains->FrameCount());
        const auto before = gains->GainDb(picked,pickedBin);
        ASSERT_TRUE(gains->ApplyExposureMap(*analysis,*exposure,true,reason,envelope.get()));
        EXPECT_NEAR(gains->GainDb(picked,pickedBin)-before,60.0*exposure->ExposureAt(picked,pickedBin),0.0001);
        const auto pcm = audio::AudioSpectralDsp::Reconstruct(analysis,*gains,reason,{},false,envelope);
        ASSERT_TRUE(pcm) << reason; EXPECT_TRUE(pcm->EnvelopeComposed);
        EXPECT_EQ(pcm->ComposedEnvelope,envelope);
        double power=0; for (const auto sample : pcm->Samples()) power+=sample*sample;
        EXPECT_GT(power,0.0);
        const auto zero = audio::AudioSpectralDsp::Analyze(Source(4096,true),Transform(),reason);
        auto silent = audio::AudioSpectralGainField::Unity(zero->Geometry.FrameCount,zero->Geometry.BinCount,reason);
        EXPECT_FALSE(silent->ApplyExposureMap(*zero,*exposure,true,reason,envelope.get()));
        EXPECT_FALSE(silent->HasOwnedCoefficients());
    }
}

TEST_F(AudioSpectralTest, OwnedCoefficientAltTargetsRecoverFromZeroAndReverseExactly)
{
    std::string reason;
    const auto analysis=audio::AudioSpectralDsp::Analyze(Source(4096),Transform(),reason);
    auto before=audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount,analysis->Geometry.BinCount,reason);
    const std::size_t frame=20; const unsigned int bin=5;
    before->SetOwned(frame,bin,true); before->SetGainDb(frame,bin,audio::AudioSpectralConfig::SilenceGainDb);
    auto gains=*before;
    const auto unity=audio::AudioSpectralEnvelope::Unity(4096,reason);
    auto band=audio::AudioSpectralEnvelopeStroke::Begin(unity,48000,reason,0.02);
    auto map=audio::AudioSpectralEnvelopeStroke::RelativeMap::Create(4096,48000,0.02,reason);
    audio::AudioSpectralEnvelopeStroke::RelativePath path;
    const auto centre=audio::AudioSpectralGainField::EnvelopeIndex(*analysis,frame);
    ASSERT_TRUE(map->Accumulate(centre,80,centre,80,path,reason));
    band->ApplyRelativeMap(*map);
    ASSERT_TRUE(gains.ApplyOwnedBand(*analysis,*before,*band,reason));
    EXPECT_GT(gains.GainLinear(frame,bin),0.0);
    map->Clear(); ASSERT_TRUE(map->Accumulate(centre,0,centre,0,path,reason)); band->ApplyRelativeMap(*map);
    ASSERT_TRUE(gains.ApplyOwnedBand(*analysis,*before,*band,reason));
    EXPECT_EQ(gains.GainDb(frame,bin),before->GainDb(frame,bin)); EXPECT_TRUE(gains.IsOwned(frame,bin));
    EXPECT_DOUBLE_EQ(audio::AudioSpectralEnvelope::RelativeGain(0,1,1e-5,16),0);
    EXPECT_DOUBLE_EQ(audio::AudioSpectralEnvelope::RelativeGain(0,2,1e-5,16),2e-5);
}

TEST_F(AudioSpectralTest, RecoveryTwoToneWolaHasBoundedTransitionSupportAndNoUnrelatedTone)
{
    std::string reason;
    auto source=Source(8192);
    for (std::size_t i=0;i<source->Samples.size();++i)
    {
        const auto t=static_cast<double>(i)-source->Layout.BodyOffset;
        source->Samples[i]=static_cast<float>(0.2*std::sin(t*2.0*3.14159265358979323846*8.0/256.0)
            +0.2*std::sin(t*2.0*3.14159265358979323846*32.0/256.0));
    }
    const auto analysis=audio::AudioSpectralDsp::Analyze(source,Transform(),reason);
    ASSERT_TRUE(analysis) << reason;
    auto gains=audio::AudioSpectralGainField::Unity(analysis->Geometry.FrameCount,analysis->Geometry.BinCount,reason);
    const auto mute=audio::AudioSpectralEnvelope::FromGains(std::vector<float>(8192,0.0f),reason);
    auto map=audio::AudioSpectralGainField::ExposureMap::Create(gains->FrameCount(),gains->BinCount(),reason);
    ASSERT_TRUE(map->Accumulate(*analysis,4096.0/48000.0,1500,4096.0/48000.0,1500,1.5,reason,0.015,0.0625));
    ASSERT_TRUE(gains->ApplyExposureMap(*analysis,*map,true,reason,mute.get()));
    const auto pcm=audio::AudioSpectralDsp::Reconstruct(analysis,*gains,reason,{},false,mute);
    ASSERT_TRUE(pcm) << reason;
    std::int64_t first=8192,last=0;
    for (std::size_t frame=0;frame<gains->FrameCount();++frame)
        if(gains->FrameHasOwned(frame))
        {
            first=(std::min)(first,analysis->Geometry.FrameCentre(frame,64)-128);
            last=(std::max)(last,analysis->Geometry.FrameCentre(frame,64)+128);
        }
    double selectedReal=0,selectedImaginary=0,otherReal=0,otherImaginary=0,leakage=0;
    for(unsigned long i=0;i<8192;++i)
    {
        const auto sample=pcm->Samples()[source->Layout.BodyOffset+i];
        if(static_cast<std::int64_t>(i)<first || static_cast<std::int64_t>(i)>=last)
            leakage=(std::max)(leakage,std::abs(static_cast<double>(sample)));
        if(i>=3840 && i<4352)
        {
            selectedReal+=sample*std::cos(i*2.0*3.14159265358979323846*8.0/256.0);
            selectedImaginary+=sample*std::sin(i*2.0*3.14159265358979323846*8.0/256.0);
            otherReal+=sample*std::cos(i*2.0*3.14159265358979323846*32.0/256.0);
            otherImaginary+=sample*std::sin(i*2.0*3.14159265358979323846*32.0/256.0);
        }
    }
    const auto selected=std::hypot(selectedReal,selectedImaginary)/256.0;
    const auto unrelated=std::hypot(otherReal,otherImaginary)/256.0;
    EXPECT_GT(selected,0.1); EXPECT_LT(unrelated,selected*0.001); EXPECT_EQ(leakage,0.0);
    std::cout << "Recovery WOLA selected=" << selected << " unrelated=" << unrelated
        << " outside-support=" << leakage << " support=" << first << "," << last << std::endl;
    // Every coefficient owned, unity payload: pass1 uses all prefix/body weights.
    for(std::size_t frame=0;frame<gains->FrameCount();++frame)
        for(unsigned int bin=0;bin<gains->BinCount();++bin)
        { gains->SetOwned(frame,bin,true); gains->SetGainDb(frame,bin,0); }
    const auto roundtrip=audio::AudioSpectralDsp::Reconstruct(analysis,*gains,reason,{},false,mute);
    ASSERT_TRUE(roundtrip) << reason;
    for(unsigned long i=analysis->Geometry.TargetFirstStorage;i<analysis->Geometry.TargetFirstStorage+analysis->Geometry.TargetLength;++i)
        EXPECT_NEAR(roundtrip->Samples()[i],source->Samples[i],2e-6);
    const auto malformed=audio::AudioSpectralEnvelope::Unity(2,reason);
    EXPECT_FALSE(audio::AudioSpectralDsp::Reconstruct(analysis,*gains,reason,{},false,malformed));
}

TEST_F(AudioSpectralTest, ControllerAltSilenceOrdinaryRecoveryUndoRedoReopenAndExactPublicationPair)
{
    std::string reason;
    const auto loop=RecordedLoop(8192);
    actions::ActionUndoHistory history;
    editor::AudioSpectralEditor controller;
    controller.SetUndoHistory(&history); controller.Open(loop);
    ASSERT_TRUE(Pump(controller,loop,[&]{return controller.Status()==editor::AudioSpectralEditor::State::Ready;}));
    const auto centre=4096.0/48000.0;
    ASSERT_TRUE(controller.BeginStroke(true,false,centre,20,100,0.005,0.0625,0.04));
    controller.MoveStroke(std::pair{centre,20.0},-60); controller.EndStroke(false);
    ASSERT_TRUE(Pump(controller,loop,[&]{return !controller.HasStroke();}));
    const auto muted=controller.EditState(); ASSERT_TRUE(muted->Envelope);
    EXPECT_FLOAT_EQ(muted->Envelope->GainAt(4096),0);
    ASSERT_TRUE(controller.BeginStroke(false,false,centre,20,0,0.005,0.0625));
    std::this_thread::sleep_for(std::chrono::milliseconds(60)); controller.MoveStroke(std::pair{centre,20.0},0);
    controller.EndStroke(false);
    ASSERT_TRUE(Pump(controller,loop,[&]{return !controller.HasStroke();}));
    const auto recovered=controller.EditState(); EXPECT_TRUE(recovered->Gains->HasOwnedCoefficients());
    EXPECT_EQ(recovered->Envelope,muted->Envelope);
    ASSERT_TRUE(Pump(controller,loop,[&]{const auto p=loop->AudioSpectralPlayback(); return p && p->SpectralRevision==recovered->SpectralRevision && p->EnvelopeRevision==recovered->EnvelopeRevision;}));
    const auto playback=loop->AudioSpectralPlayback(); EXPECT_FALSE(playback->Envelope);
    EXPECT_TRUE(playback->Pcm->EnvelopeComposed); EXPECT_EQ(playback->Pcm->ComposedEnvelope,recovered->Envelope);
    ASSERT_TRUE(history.Undo()); EXPECT_EQ(controller.EditState()->Gains,muted->Gains);
    ASSERT_TRUE(Pump(controller,loop,[&]{const auto p=loop->AudioSpectralPlayback(); return p && p->SpectralRevision==controller.EditState()->SpectralRevision;}));
    EXPECT_FALSE(loop->AudioSpectralPlayback()->Pcm->EnvelopeComposed);
    ASSERT_TRUE(history.Redo()); EXPECT_EQ(controller.EditState()->Gains,recovered->Gains);
    controller.Close(); controller.Open(loop);
    ASSERT_TRUE(Pump(controller,loop,[&]{return controller.Status()==editor::AudioSpectralEditor::State::Ready;}));
    EXPECT_EQ(controller.EditState()->Gains,recovered->Gains);
    auto stale=std::make_shared<audio::AudioSpectralPcm>(*playback->Pcm);
    const auto latest=controller.EditState();
    EXPECT_FALSE(audio::AudioSpectralPlaybackSnapshot::Create(stale,nullptr,99,latest->SpectralRevision,latest->EnvelopeRevision,reason));
    const auto newEnvelope=audio::AudioSpectralEnvelope::Unity(latest->Source->Layout.LogicalLength,reason);
    const auto newEdit=audio::AudioSpectralEditState::Create(latest->Source,latest->Transform,latest->Gains,newEnvelope,
        latest->Revision+1,latest->SpectralRevision,latest->EnvelopeRevision+1,reason);
    ASSERT_TRUE(loop->PublishAudioSpectralEdit(newEdit,reason));
    const auto oldPair=audio::AudioSpectralPlaybackSnapshot::Create(stale,nullptr,99,
        stale->ComposedSpectralRevision,stale->ComposedEnvelopeRevision,reason);
    ASSERT_TRUE(oldPair) << reason;
    EXPECT_FALSE(loop->PublishAudioSpectralPlayback(oldPair,reason));
    EXPECT_FALSE(audio::AudioSpectralPlaybackSnapshot::Create(stale,nullptr,100,
        newEdit->SpectralRevision,newEdit->EnvelopeRevision,reason));
}

TEST_F(AudioSpectralTest, OwnershipBitmapAdmissionRetainsThirtySecondBaselineTransform)
{
    std::string reason;
    const audio::AudioSpectralSourceLayout layout{1440000,1440037,8,48000};
    const auto estimate=audio::AudioSpectralDsp::EstimateWorkingMemory(layout,{1,4096,1024},reason);
    ASSERT_TRUE(estimate) << reason;
    EXPECT_LE(estimate->WorkingBytes,audio::AudioSpectralConfig::MaxWorkingBytes);
    EXPECT_EQ(sizeof(audio::AudioSpectralGainField::Tile),1024*sizeof(float)+128);
    std::cout << "Ownership 30s baseline peak MiB=" << estimate->WorkingBytes/(1024.0*1024.0) << std::endl;
}
