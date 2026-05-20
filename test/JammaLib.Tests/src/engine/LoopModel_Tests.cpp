
#include "gtest/gtest.h"
#include "engine/LoopModel.h"

using audio::BufferBank;
using engine::LoopModel;
using engine::LoopModelParams;

namespace
{
	constexpr auto GrainVertFloatCount = 72u;
	constexpr auto GrainUvFloatCount = 48u;
	constexpr auto MeshMinHeight = 1.0f;
	constexpr auto MeshHeightScale = 100.0f;

	class TestLoopModel :
		public LoopModel
	{
	public:
		TestLoopModel() :
			GuiModel(LoopModelParams()),
			LoopModel(LoopModelParams())
		{
		}

		using LoopModel::CalcGrainGeometry;
		using LoopModel::UpdateModel;

		const std::vector<float>& BackVerts() const
		{
			return _backVerts;
		}

		const std::vector<float>& BackUvs() const
		{
			return _backUvs;
		}
	};

	BufferBank MakeBuffer(unsigned long length)
	{
		auto buffer = BufferBank();
		buffer.Resize(length);
		return buffer;
	}
}

TEST(LoopModelMesh, CalcGrainGeometryUsesExpectedRingCoordinates)
{
	auto model = TestLoopModel();
	auto buffer = MakeBuffer(constants::GrainSamps * 4u);
	buffer[0] = -0.5f;
	buffer[1] = 0.75f;

	const auto tol = 1e-5f;
	const auto radius = 100.0f;
	const auto radialThickness = radius / 20.0f;
	const auto expectedMinSample = -0.5f;
	const auto expectedMaxSample = 0.75f;
	const auto expectedYMin = (MeshHeightScale * expectedMinSample) - MeshMinHeight;
	const auto expectedYMax = (MeshHeightScale * expectedMaxSample) + MeshMinHeight;
	const auto expectedInitialYUv = std::abs(-MeshMinHeight) / (MeshMinHeight + MeshHeightScale);

	auto [verts, uvs, yMin, yMax] = model.CalcGrainGeometry(buffer,
		1u,
		4u,
		0ul,
		-MeshMinHeight,
		MeshMinHeight,
		radius);

	ASSERT_EQ(GrainVertFloatCount, verts.size());
	ASSERT_EQ(GrainUvFloatCount, uvs.size());
	EXPECT_NEAR(expectedYMin, yMin, tol);
	EXPECT_NEAR(expectedYMax, yMax, tol);

	EXPECT_NEAR(0.0f, verts[0], tol);
	EXPECT_NEAR(-MeshMinHeight, verts[1], tol);
	EXPECT_NEAR(radius + radialThickness, verts[2], tol);
	EXPECT_NEAR(radius + radialThickness, verts[3], tol);
	EXPECT_NEAR(expectedYMax, verts[4], tol);
	EXPECT_NEAR(0.0f, verts[5], tol);
	EXPECT_NEAR(0.0f, uvs[0], tol);
	EXPECT_NEAR(expectedInitialYUv, uvs[1], tol);
	EXPECT_NEAR(expectedInitialYUv, uvs[5], tol);
	EXPECT_NEAR(0.25f, uvs[2], tol);
}

TEST(LoopModelMesh, CalcGrainGeometryReflectsUvAroundZero)
{
	auto model = TestLoopModel();
	auto buffer = MakeBuffer(constants::GrainSamps * 4u);

	const auto tol = 1e-5f;
	auto [verts, uvs, yMin, yMax] = model.CalcGrainGeometry(buffer,
		1u,
		4u,
		0ul,
		0.0f,
		0.0f,
		100.0f);

	ASSERT_EQ(GrainVertFloatCount, verts.size());
	ASSERT_EQ(GrainUvFloatCount, uvs.size());
	EXPECT_NEAR(0.0f, uvs[1], tol);
	EXPECT_NEAR(0.0f, uvs[5], tol);
	EXPECT_GT(uvs[3], 0.0f);
	EXPECT_LE(uvs[3], 1.0f);
}

TEST(LoopModelWaveform, DecimateWaveformReturnsRequestedSegmentCount)
{
	auto buffer = MakeBuffer(8ul);
	buffer[0] = -1.0f;
	buffer[1] = 0.5f;
	buffer[2] = -0.2f;
	buffer[3] = 0.8f;
	buffer[4] = -0.9f;
	buffer[5] = 0.4f;
	buffer[6] = -0.1f;
	buffer[7] = 0.9f;

	auto result = LoopModel::DecimateWaveform(buffer, 0ul, 8ul, 4u);

	ASSERT_EQ(4u, result.size());
	EXPECT_FLOAT_EQ(-1.0f, result[0].x);
	EXPECT_FLOAT_EQ(0.5f, result[0].y);
	EXPECT_FLOAT_EQ(-0.2f, result[1].x);
	EXPECT_FLOAT_EQ(0.8f, result[1].y);
	EXPECT_FLOAT_EQ(-0.9f, result[2].x);
	EXPECT_FLOAT_EQ(0.4f, result[2].y);
	EXPECT_FLOAT_EQ(-0.1f, result[3].x);
	EXPECT_FLOAT_EQ(0.9f, result[3].y);
}

TEST(LoopModelWaveform, DecimateWaveformHonorsOffsetAndLength)
{
	auto buffer = MakeBuffer(12ul);
	buffer[0] = -1.0f;
	buffer[1] = 1.0f;
	buffer[2] = -1.0f;
	buffer[3] = 1.0f;
	buffer[4] = -0.4f;
	buffer[5] = 0.1f;
	buffer[6] = -0.6f;
	buffer[7] = 0.3f;

	auto result = LoopModel::DecimateWaveform(buffer, 4ul, 4ul, 4u);

	ASSERT_EQ(4u, result.size());
	EXPECT_FLOAT_EQ(-0.4f, result[0].x);
	EXPECT_FLOAT_EQ(0.0f, result[0].y);
	EXPECT_FLOAT_EQ(0.0f, result[1].x);
	EXPECT_FLOAT_EQ(0.1f, result[1].y);
	EXPECT_FLOAT_EQ(-0.6f, result[2].x);
	EXPECT_FLOAT_EQ(0.0f, result[2].y);
	EXPECT_FLOAT_EQ(0.0f, result[3].x);
	EXPECT_FLOAT_EQ(0.3f, result[3].y);
}

TEST(LoopModelWaveform, DecimateWaveformRepeatsCoverageWhenSegmentsExceedSamples)
{
	auto buffer = MakeBuffer(2ul);
	buffer[0] = 0.25f;
	buffer[1] = -0.5f;

	auto result = LoopModel::DecimateWaveform(buffer, 0ul, 2ul, 4u);

	ASSERT_EQ(4u, result.size());
	EXPECT_FLOAT_EQ(0.0f, result[0].x);
	EXPECT_FLOAT_EQ(0.25f, result[0].y);
	EXPECT_FLOAT_EQ(0.0f, result[1].x);
	EXPECT_FLOAT_EQ(0.25f, result[1].y);
	EXPECT_FLOAT_EQ(-0.5f, result[2].x);
	EXPECT_FLOAT_EQ(0.0f, result[2].y);
	EXPECT_FLOAT_EQ(-0.5f, result[3].x);
	EXPECT_FLOAT_EQ(0.0f, result[3].y);
}

TEST(LoopModelWaveform, DecimateWaveformUsesNonOverlappingChunkPeaks)
{
	auto buffer = MakeBuffer(10ul);
	buffer[0] = 0.1f;
	buffer[1] = -0.9f;
	buffer[2] = 0.3f;
	buffer[3] = 0.4f;
	buffer[4] = -0.1f;
	buffer[5] = -0.8f;
	buffer[6] = 0.2f;
	buffer[7] = -0.05f;
	buffer[8] = 0.7f;
	buffer[9] = -0.2f;

	auto result = LoopModel::DecimateWaveform(buffer, 0ul, 10ul, 4u);

	ASSERT_EQ(4u, result.size());
	// Chunks are [0,2), [2,5), [5,7), [7,10)
	EXPECT_FLOAT_EQ(-0.9f, result[0].x);
	EXPECT_FLOAT_EQ(0.1f, result[0].y);
	EXPECT_FLOAT_EQ(-0.1f, result[1].x);
	EXPECT_FLOAT_EQ(0.4f, result[1].y);
	EXPECT_FLOAT_EQ(-0.8f, result[2].x);
	EXPECT_FLOAT_EQ(0.2f, result[2].y);
	EXPECT_FLOAT_EQ(-0.2f, result[3].x);
	EXPECT_FLOAT_EQ(0.7f, result[3].y);
}

TEST(LoopModelWaveform, DecimateWaveformReturnsEmptyForZeroSegments)
{
	auto buffer = MakeBuffer(16ul);
	buffer[0] = -0.25f;

	auto result = LoopModel::DecimateWaveform(buffer, 0ul, 16ul, 0u);

	EXPECT_TRUE(result.empty());
}

TEST(LoopModelWaveform, UpdateModelKeepsFixedMeshGeometry)
{
	auto model = TestLoopModel();
	auto initialVerts = model.BackVerts();
	auto initialUvs = model.BackUvs();

	ASSERT_FALSE(initialVerts.empty());
	ASSERT_FALSE(initialUvs.empty());

	auto bufferA = MakeBuffer(constants::GrainSamps * 4u);
	bufferA[0] = -0.1f;
	bufferA[1] = 0.2f;

	auto bufferB = MakeBuffer(constants::GrainSamps * 4u);
	bufferB[0] = -0.9f;
	bufferB[1] = 0.95f;

	model.UpdateModel(bufferA, bufferA.Length(), bufferA.Length(), 0ul, 120.0f);
	EXPECT_EQ(initialVerts, model.BackVerts());
	EXPECT_EQ(initialUvs, model.BackUvs());

	model.UpdateModel(bufferB, bufferB.Length(), bufferB.Length(), 0ul, 220.0f);
	EXPECT_EQ(initialVerts, model.BackVerts());
	EXPECT_EQ(initialUvs, model.BackUvs());
}

TEST(LoopModelWaveform, UpdateModelWithZeroLoopLengthKeepsFixedGeometry)
{
	auto model = TestLoopModel();
	auto initialVerts = model.BackVerts();
	auto initialUvs = model.BackUvs();

	auto buffer = MakeBuffer(constants::GrainSamps);
	buffer[0] = 0.25f;

	model.UpdateModel(buffer, 0ul, 0ul, 0ul, 100.0f);

	EXPECT_EQ(initialVerts, model.BackVerts());
	EXPECT_EQ(initialUvs, model.BackUvs());
}
