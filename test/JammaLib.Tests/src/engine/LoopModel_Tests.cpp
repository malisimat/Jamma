
#include "gtest/gtest.h"
#include "engine/LoopModel.h"

using audio::BufferBank;
using engine::LoopModel;
using engine::LoopModelParams;

namespace
{
	class TestLoopModel :
		public LoopModel
	{
	public:
		TestLoopModel() :
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

	const auto radius = 100.0f;
	const auto radialThickness = radius / 20.0f;
	const auto expectedYMin = -51.0f;
	const auto expectedYMax = 76.0f;

	auto [verts, uvs, yMin, yMax] = model.CalcGrainGeometry(buffer,
		1u,
		4u,
		0ul,
		-1.0f,
		1.0f,
		radius);

	ASSERT_EQ(72u, verts.size());
	ASSERT_EQ(48u, uvs.size());
	EXPECT_FLOAT_EQ(expectedYMin, yMin);
	EXPECT_FLOAT_EQ(expectedYMax, yMax);

	EXPECT_FLOAT_EQ(0.0f, verts[0]);
	EXPECT_FLOAT_EQ(-1.0f, verts[1]);
	EXPECT_FLOAT_EQ(radius + radialThickness, verts[2]);
	EXPECT_FLOAT_EQ(radius + radialThickness, verts[3]);
	EXPECT_FLOAT_EQ(expectedYMax, verts[4]);
	EXPECT_NEAR(0.0f, verts[5], 0.0001f);
	EXPECT_FLOAT_EQ(0.0f, uvs[0]);
	EXPECT_FLOAT_EQ(0.5f, uvs[1]);
	EXPECT_FLOAT_EQ(0.25f, uvs[2]);
}

TEST(LoopModelMesh, UpdateModelUsesOffsetSamplesAndMaintainsGrainContinuity)
{
	auto model = TestLoopModel();
	const auto offset = constants::MaxLoopFadeSamps;
	const auto loopLength = constants::GrainSamps * 2u;
	auto buffer = MakeBuffer(offset + loopLength);

	buffer[0] = -1.0f;
	buffer[1] = 1.0f;
	buffer[offset + 0] = -0.1f;
	buffer[offset + 1] = 0.2f;
	buffer[offset + constants::GrainSamps + 0] = -0.3f;
	buffer[offset + constants::GrainSamps + 1] = 0.4f;

	model.UpdateModel(buffer, loopLength, offset, 100.0f);

	ASSERT_EQ(144u, model.BackVerts().size());
	ASSERT_EQ(96u, model.BackUvs().size());

	const auto& verts = model.BackVerts();
	EXPECT_FLOAT_EQ(-11.0f, verts[72 + 1]);
	EXPECT_FLOAT_EQ(21.0f, verts[72 + 7]);
	EXPECT_FLOAT_EQ(41.0f, verts[72 + 4]);
	EXPECT_FLOAT_EQ(-31.0f, verts[72 + 13]);
}

TEST(LoopModelMesh, UpdateModelBuildsTrailingPartialGrain)
{
	auto model = TestLoopModel();
	const auto loopLength = constants::GrainSamps + 50u;
	auto buffer = MakeBuffer(loopLength);

	buffer[0] = -0.2f;
	buffer[1] = 0.1f;
	buffer[constants::GrainSamps + 0] = -0.4f;
	buffer[constants::GrainSamps + 1] = 0.3f;

	model.UpdateModel(buffer, loopLength, 0ul, 100.0f);

	ASSERT_EQ(144u, model.BackVerts().size());
	ASSERT_EQ(96u, model.BackUvs().size());

	const auto& verts = model.BackVerts();
	EXPECT_FLOAT_EQ(-21.0f, verts[72 + 1]);
	EXPECT_FLOAT_EQ(11.0f, verts[72 + 7]);
	EXPECT_FLOAT_EQ(31.0f, verts[72 + 4]);
	EXPECT_FLOAT_EQ(-41.0f, verts[72 + 13]);
}
