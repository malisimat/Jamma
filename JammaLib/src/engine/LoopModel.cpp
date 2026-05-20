#include "LoopModel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace engine;
using base::DrawContext;
using base::Drawable;
using base::DrawableParams;
using graphics::GlDrawContext;
using audio::BufferBank;
using actions::ActionResult;
using gui::GuiModel;
using utils::Size2d;

const Size2d LoopModel::_LedGap = { 6, 6 };
const float LoopModel::_MinHeight = 1.0f;
const float LoopModel::_RadialThicknessFrac = 1.0f / 20.0f;
const float LoopModel::_HeightScale = 100.0f;
const float LoopModel::_UnitMeshRadius = 100.0f;
const unsigned int LoopModel::_WaveformSegments = 2048u;
const unsigned int LoopModel::_WaveformPboCount = 2u;

LoopModel::LoopModel(LoopModelParams params) :
	GuiModel(params),
	_loopIndexFrac(0),
	_modelState(STATE_RECORDING),
	_waveformRadius(_UnitMeshRadius),
	_waveformColorMultiplier(0.5f / (_HeightScale + _MinHeight)),
	_hasWaveformData(false),
	_waveformNeedsUpload(false),
	_waveformTexture(0u),
	_waveformPbos{ 0u, 0u },
	_waveformWritePboIndex(0u),
	_waveformDecimated(_WaveformSegments, glm::vec2(0.0f, 0.0f))
{
	auto [fixedVerts, fixedUvs] = BuildFixedGeometry(_WaveformSegments, _UnitMeshRadius);
	SetGeometry(std::move(fixedVerts), std::move(fixedUvs));
}

LoopModel::~LoopModel()
{
}

void LoopModel::Draw3d(DrawContext& ctx,
	unsigned int numInstances,
	base::DrawPass pass)
{
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);

	glCtx.PushMvp(glm::rotate(glm::mat4(1.0), (float)(constants::TWOPI * (_loopIndexFrac + 0.0)), glm::vec3(0.0f, 1.0f, 0.0f)));

	unsigned int id;
	std::vector<unsigned int> idVec;

	switch (_modelState)
	{
	case STATE_PICKING:
		idVec = GlobalId();
		idVec.resize(3);
		for (auto& idPart : idVec)
			idPart += 1;
		id = utils::VecToId(idVec);

		glCtx.SetUniform("ObjectId", id);
		break;
	case STATE_HIGHLIGHTING:
		glCtx.SetUniform("Highlight", _isSelected ? 1.0f : 0.0f);
		break;
	default:
		glCtx.SetUniform("LoopState", (unsigned int)_modelState);
		glCtx.SetUniform("LoopHover", _isPicking3d ? 1.0f : 0.0f);
		break;
	}

	if (_waveformNeedsUpload)
		UploadWaveformTexture();

	auto modelTexture = GetTexture();
	auto modelShader = GetShader();

	auto texture = modelTexture.lock();
	auto shader = modelShader.lock();

	if (!shader || 0u == _vertexArray)
	{
		glCtx.PopMvp();
		return;
	}

	glCtx.SetUniform("TextureSampler", 0u);
	glCtx.SetUniform("WaveformSampler", 1u);
	glCtx.SetUniform("WaveformRadius", _waveformRadius);
	glCtx.SetUniform("WaveformHeightScale", _HeightScale);
	glCtx.SetUniform("WaveformMinHeight", _MinHeight);
	glCtx.SetUniform("WaveformColorMultiplier", _waveformColorMultiplier);

	glUseProgram(shader->GetId());
	shader->SetUniforms(dynamic_cast<GlDrawContext&>(ctx));

	glBindVertexArray(_vertexArray);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture ? texture->GetId() : 0u);

	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_1D, _waveformTexture);

	if (numInstances > 1)
		glDrawArraysInstanced(GL_TRIANGLES, 0, _numTris * 3, numInstances);
	else
		glDrawArrays(GL_TRIANGLES, 0, _numTris * 3);

	glBindTexture(GL_TEXTURE_1D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindVertexArray(0);
	glUseProgram(0);

	glCtx.PopMvp();
}

double LoopModel::LoopIndexFrac() const
{
	return _loopIndexFrac;
}

void LoopModel::SetLoopIndexFrac(double frac)
{
	_loopIndexFrac = frac;
}

void LoopModel::SetLoopState(LoopModelState state)
{
	_modelState = state;
}

unsigned int LoopModel::TotalNumLeds(unsigned int vuHeight,
	unsigned int ledHeight)
{
	if ((vuHeight % ledHeight) == 0)
		return vuHeight / ledHeight;

	return (vuHeight / ledHeight) + 1;
}

unsigned int LoopModel::CurrentNumLeds(unsigned int vuHeight,
	unsigned int ledHeight,
	double value)
{
	auto numLeds = TotalNumLeds(vuHeight, ledHeight);

	return (unsigned int)std::ceil(value * numLeds);
}

std::weak_ptr<resources::ShaderResource> LoopModel::GetShader()
{
	unsigned int shaderIndex = 0u;
	
	switch (_modelState)
	{
	case STATE_PICKING:
		shaderIndex = 1u;
		break;
	case STATE_HIGHLIGHTING:
		shaderIndex = 2u;
		break;
	}

	if (_modelShaders.size() > shaderIndex)
		return _modelShaders.at(shaderIndex);

	return std::weak_ptr<resources::ShaderResource>();
}

void LoopModel::_InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	GuiModel::_InitResources(resourceLib, forceInit);

	if (0u == _waveformTexture)
		glGenTextures(1, &_waveformTexture);

	glBindTexture(GL_TEXTURE_1D, _waveformTexture);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexImage1D(GL_TEXTURE_1D,
		0,
		GL_RG16F,
		(GLsizei)_WaveformSegments,
		0,
		GL_RG,
		GL_FLOAT,
		_waveformDecimated.data());
	glBindTexture(GL_TEXTURE_1D, 0);

	if ((0u == _waveformPbos[0]) && (0u == _waveformPbos[1]))
		glGenBuffers(_WaveformPboCount, _waveformPbos);

	auto waveformBytes = static_cast<GLsizeiptr>(_WaveformSegments * sizeof(glm::vec2));
	for (auto pbo = 0u; pbo < _WaveformPboCount; pbo++)
	{
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _waveformPbos[pbo]);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, waveformBytes, nullptr, GL_STREAM_DRAW);
	}
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

	_waveformNeedsUpload = true;
}

void LoopModel::_ReleaseResources()
{
	if (0u != _waveformTexture)
	{
		glDeleteTextures(1, &_waveformTexture);
		_waveformTexture = 0u;
	}

	glDeleteBuffers(_WaveformPboCount, _waveformPbos);
	_waveformPbos[0] = 0u;
	_waveformPbos[1] = 0u;

	GuiModel::_ReleaseResources();
}

void LoopModel::UpdateModel(const BufferBank& buffer,
	unsigned long loopLength,
	unsigned long offset,
	float radius)
{
	UpdateModel(buffer, loopLength, loopLength, offset, radius);
}

void LoopModel::UpdateModel(const BufferBank& buffer,
	unsigned long sourceLoopLength,
	unsigned long displayLoopLength,
	unsigned long offset,
	float radius)
{
	auto availableSamples = buffer.Length() > offset ? buffer.Length() - offset : 0ul;
	auto clampedLength = std::min(sourceLoopLength, availableSamples);

	_waveformRadius = std::max(radius, 1.0f);
	_hasWaveformData = (displayLoopLength > 0ul) && (clampedLength > 0ul);

	if (!_hasWaveformData)
	{
		std::fill(_waveformDecimated.begin(), _waveformDecimated.end(), glm::vec2(0.0f, 0.0f));
		_waveformColorMultiplier = 0.5f / (_HeightScale + _MinHeight);
		_waveformNeedsUpload = true;
		return;
	}

	DecimateWaveformInto(buffer,
		offset,
		clampedLength,
		_waveformDecimated);

	auto maxPeakLevel = 0.0f;
	for (const auto& minMax : _waveformDecimated)
	{
		auto maxAbs = std::max(std::fabs(minMax.x), std::fabs(minMax.y));
		if (maxAbs > maxPeakLevel)
			maxPeakLevel = maxAbs;
	}

	const auto peakDenominator = (_HeightScale * std::max(maxPeakLevel, 0.0001f)) + _MinHeight;
	_waveformColorMultiplier = peakDenominator > 0.0f ? (0.5f / peakDenominator) : 0.5f;

	_waveformNeedsUpload = true;
}

std::vector<glm::vec2> LoopModel::DecimateWaveform(const BufferBank& buffer, unsigned long offset, unsigned long length, unsigned int numSegments)
{
	std::vector<glm::vec2> result(numSegments, glm::vec2(0.0f, 0.0f));
	DecimateWaveformInto(buffer, offset, length, result);
	return result;
}

void LoopModel::DecimateWaveformInto(const BufferBank& buffer,
	unsigned long offset,
	unsigned long length,
	std::vector<glm::vec2>& outSegments)
{
	if (outSegments.empty())
		return;

	std::fill(outSegments.begin(), outSegments.end(), glm::vec2(0.0f, 0.0f));

	if ((0ul == length) || (offset >= buffer.Length()))
		return;

	auto availableSamples = buffer.Length() - offset;
	auto clampedLength = std::min(length, availableSamples);
	if (0ul == clampedLength)
		return;

	auto segmentCount = static_cast<unsigned long>(outSegments.size());
	for (auto segment = 0ul; segment < segmentCount; segment++)
	{
		auto segmentStart = (segment * clampedLength) / segmentCount;
		auto segmentEnd = ((segment + 1ul) * clampedLength) / segmentCount;
		if (segmentEnd <= segmentStart)
		{
			segmentStart = std::min(segmentStart, clampedLength - 1ul);
			segmentEnd = segmentStart + 1ul;
		}

		auto maxPositive = 0.0f;
		auto maxNegativeAbs = 0.0f;

		for (auto sampleIndex = segmentStart; sampleIndex < segmentEnd; sampleIndex++)
		{
			auto sample = buffer[offset + sampleIndex];
			if (sample >= 0.0f)
				maxPositive = std::max(maxPositive, sample);
			else
				maxNegativeAbs = std::max(maxNegativeAbs, std::fabs(sample));
		}

		outSegments[segment] = glm::vec2(-maxNegativeAbs, maxPositive);
	}
}

std::tuple<std::vector<float>, std::vector<float>> LoopModel::BuildFixedGeometry(unsigned int numSegments, float radius)
{
	std::vector<float> verts;
	std::vector<float> uvs;

	if (numSegments == 0u)
		return std::make_tuple(verts, uvs);

	auto radialThickness = radius * _RadialThicknessFrac;
	auto lastYMin = -_MinHeight;
	auto lastYMax = _MinHeight;

	for (auto grain = 1u; grain <= numSegments; grain++)
	{
		auto angle1 = ((float)constants::TWOPI) * ((float)(grain - 1) / (float)numSegments);
		auto angle2 = ((float)constants::TWOPI) * ((float)grain / (float)numSegments);

		auto xInner1 = sin(angle1) * (radius - radialThickness);
		auto xInner2 = sin(angle2) * (radius - radialThickness);
		auto xOuter1 = sin(angle1) * (radius + radialThickness);
		auto xOuter2 = sin(angle2) * (radius + radialThickness);
		auto yMin = -_MinHeight;
		auto yMax = _MinHeight;
		auto zInner1 = cos(angle1) * (radius - radialThickness);
		auto zInner2 = cos(angle2) * (radius - radialThickness);
		auto zOuter1 = cos(angle1) * (radius + radialThickness);
		auto zOuter2 = cos(angle2) * (radius + radialThickness);

		auto u1 = (float)(grain - 1u) / (float)numSegments;
		auto u2 = (float)grain / (float)numSegments;

		// Front
		verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
		verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
		verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
		uvs.push_back(u1); uvs.push_back(0.0f);
		uvs.push_back(u2); uvs.push_back(1.0f);
		uvs.push_back(u1); uvs.push_back(1.0f);

		verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
		verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
		verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
		uvs.push_back(u1); uvs.push_back(0.0f);
		uvs.push_back(u2); uvs.push_back(0.0f);
		uvs.push_back(u2); uvs.push_back(1.0f);

		// Top
		verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
		verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
		verts.push_back(xInner1); verts.push_back(lastYMax); verts.push_back(zInner1);
		uvs.push_back(u1); uvs.push_back(1.0f);
		uvs.push_back(u2); uvs.push_back(1.0f);
		uvs.push_back(u1); uvs.push_back(1.0f);

		verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
		verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
		verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
		uvs.push_back(u1); uvs.push_back(1.0f);
		uvs.push_back(u2); uvs.push_back(1.0f);
		uvs.push_back(u2); uvs.push_back(1.0f);

		// Back
		verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
		verts.push_back(xInner1); verts.push_back(lastYMax); verts.push_back(zInner1);
		verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
		uvs.push_back(u1); uvs.push_back(0.0f);
		uvs.push_back(u1); uvs.push_back(1.0f);
		uvs.push_back(u2); uvs.push_back(1.0f);

		verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
		verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
		verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
		uvs.push_back(u1); uvs.push_back(0.0f);
		uvs.push_back(u2); uvs.push_back(1.0f);
		uvs.push_back(u2); uvs.push_back(0.0f);

		// Bottom
		verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
		verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
		verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
		uvs.push_back(u1); uvs.push_back(0.0f);
		uvs.push_back(u1); uvs.push_back(0.0f);
		uvs.push_back(u2); uvs.push_back(0.0f);

		verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
		verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
		verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
		uvs.push_back(u1); uvs.push_back(0.0f);
		uvs.push_back(u2); uvs.push_back(0.0f);
		uvs.push_back(u2); uvs.push_back(0.0f);

		lastYMin = yMin;
		lastYMax = yMax;
	}

	return std::make_tuple(verts, uvs);
}

void LoopModel::UploadWaveformTexture()
{
	if ((0u == _waveformTexture) || _waveformDecimated.empty())
		return;

	auto waveformBytes = static_cast<GLsizeiptr>(_waveformDecimated.size() * sizeof(glm::vec2));
	if ((0u != _waveformPbos[0]) && (0u != _waveformPbos[1]))
	{
		auto pboIndex = _waveformWritePboIndex % _WaveformPboCount;
		auto writePbo = _waveformPbos[pboIndex];

		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, writePbo);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, waveformBytes, nullptr, GL_STREAM_DRAW);

		void* pboMem = glMapBufferRange(GL_PIXEL_UNPACK_BUFFER,
			0,
			waveformBytes,
			GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT);
		if (nullptr != pboMem)
		{
			std::memcpy(pboMem, _waveformDecimated.data(), (size_t)waveformBytes);
			glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
		}

		glBindTexture(GL_TEXTURE_1D, _waveformTexture);
		glTexSubImage1D(GL_TEXTURE_1D,
			0,
			0,
			(GLsizei)_waveformDecimated.size(),
			GL_RG,
			GL_FLOAT,
			0);
		glBindTexture(GL_TEXTURE_1D, 0);

		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		_waveformWritePboIndex = (_waveformWritePboIndex + 1u) % _WaveformPboCount;
	}
	else
	{
		glBindTexture(GL_TEXTURE_1D, _waveformTexture);
		glTexSubImage1D(GL_TEXTURE_1D,
			0,
			0,
			(GLsizei)_waveformDecimated.size(),
			GL_RG,
			GL_FLOAT,
			_waveformDecimated.data());
		glBindTexture(GL_TEXTURE_1D, 0);
	}

	_waveformNeedsUpload = false;
}

std::tuple<std::vector<float>, std::vector<float>, float, float>
LoopModel::CalcGrainGeometry(const BufferBank& buffer,
	unsigned int grain,
	unsigned int numGrains,
	unsigned long offset,
	float lastYMin,
	float lastYMax,
	float radius)
{
	auto sourceLoopLength = buffer.Length() > offset ? buffer.Length() - offset : 0ul;
	return CalcGrainGeometry(buffer,
		sourceLoopLength,
		grain,
		numGrains,
		offset,
		lastYMin,
		lastYMax,
		radius);
}

std::tuple<std::vector<float>, std::vector<float>, float, float>
LoopModel::CalcGrainGeometry(const BufferBank& buffer,
	unsigned long sourceLoopLength,
	unsigned int grain,
	unsigned int numGrains,
	unsigned long offset,
	float lastYMin,
	float lastYMax,
	float radius)
{
	if (sourceLoopLength == 0ul || numGrains == 0u)
		return std::make_tuple(std::vector<float>(), std::vector<float>(), lastYMin, lastYMax);

	const float radialThickness = radius * _RadialThicknessFrac;
	const float maxMagnitude = _MinHeight + _HeightScale;

	auto yToUv = [maxMagnitude](float y) {
		auto uv = std::fabs(y) / maxMagnitude;
		return uv > 1.0f ? 1.0f : uv;
	};

	auto angle1 = ((float)constants::TWOPI) * ((float)(grain - 1) / (float)numGrains);
	auto angle2 = ((float)constants::TWOPI) * ((float)grain / (float)numGrains);
	auto sourceStart = static_cast<unsigned long>((static_cast<double>(grain - 1u) * static_cast<double>(sourceLoopLength)) / static_cast<double>(numGrains));
	auto sourceEnd = static_cast<unsigned long>(ceil((static_cast<double>(grain) * static_cast<double>(sourceLoopLength)) / static_cast<double>(numGrains)));
	auto i1 = offset + std::min(sourceStart, sourceLoopLength);
	auto i2 = offset + std::min(std::max(sourceEnd, sourceStart + 1ul), sourceLoopLength);
	auto gMin = buffer.SubMin(i1, i2);
	auto gMax = buffer.SubMax(i1, i2);

	auto xInner1 = sin(angle1) * (radius - radialThickness);
	auto xInner2 = sin(angle2) * (radius - radialThickness);
	auto xOuter1 = sin(angle1) * (radius + radialThickness);
	auto xOuter2 = sin(angle2) * (radius + radialThickness);
	auto yMin = (_HeightScale * gMin) - _MinHeight;
	auto yMax = (_HeightScale * gMax) + _MinHeight;
	auto zInner1 = cos(angle1) * (radius - radialThickness);
	auto zInner2 = cos(angle2) * (radius - radialThickness);
	auto zOuter1 = cos(angle1) * (radius + radialThickness);
	auto zOuter2 = cos(angle2) * (radius + radialThickness);

	std::vector<float> verts;
	std::vector<float> uvs;

	// Front
	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));

	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));

	// Top
	verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	verts.push_back(xInner1); verts.push_back(lastYMax); verts.push_back(zInner1);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));

	verts.push_back(xOuter1); verts.push_back(lastYMax); verts.push_back(zOuter1);
	verts.push_back(xOuter2); verts.push_back(yMax); verts.push_back(zOuter2);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));

	// Back
	verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
	verts.push_back(xInner1); verts.push_back(lastYMax); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));

	verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMax); verts.push_back(zInner2);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMax));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));

	// Bottom
	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xInner1); verts.push_back(lastYMin); verts.push_back(zInner1);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));

	verts.push_back(xOuter1); verts.push_back(lastYMin); verts.push_back(zOuter1);
	verts.push_back(xInner2); verts.push_back(yMin); verts.push_back(zInner2);
	verts.push_back(xOuter2); verts.push_back(yMin); verts.push_back(zOuter2);
	uvs.push_back(angle1 / (float)constants::TWOPI); uvs.push_back(yToUv(lastYMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));
	uvs.push_back(angle2 / (float)constants::TWOPI); uvs.push_back(yToUv(yMin));

	return std::make_tuple(verts, uvs, yMin, yMax);
}
