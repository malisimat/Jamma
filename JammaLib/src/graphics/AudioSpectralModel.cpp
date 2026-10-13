#include "AudioSpectralModel.h"
#include "GlDeleteQueue.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <limits>
#include <stdexcept>
#include <utility>

using namespace graphics;

bool AudioSpectralDisplay::IsValid() const noexcept
{
	if (!SourceIdentity || SourceGeneration == 0u || Revision == 0u
		|| TimeSegments == 0u || TimeSegments > audio::AudioSpectralConfig::MaxTimeSegments
		|| FrequencySegments == 0u || FrequencySegments > audio::AudioSpectralConfig::MaxFrequencySegments
		|| !(MinimumFrequencyHz > 0.0f) || !std::isfinite(MinimumFrequencyHz)
		|| !(NyquistHz > MinimumFrequencyHz) || !std::isfinite(NyquistHz)
		|| Heights.size() != static_cast<std::size_t>(TimeSegments + 1u) * (FrequencySegments + 1u))
		return false;
	for (const auto height : Heights)
		if (!std::isfinite(height) || height < 0.0f || height > 1.0f) return false;
	for (unsigned int row = 0u; row <= FrequencySegments; ++row)
		if (Heights[row * (TimeSegments + 1u)] != Heights[row * (TimeSegments + 1u) + TimeSegments])
			return false;
	return true;
}

gui::GuiModelParams AudioSpectralModel::_ModelParams(gui::GuiModelParams params)
{
	params.ModelTextures = { "levels" };
	params.ModelShaders = { "audio_spectral" };
	return params;
}

AudioSpectralModel::AudioSpectralModel(gui::GuiModelParams params,
	unsigned int timeSegments, unsigned int frequencySegments) :
	GuiModel(_ModelParams(std::move(params))),
	_timeSegments(timeSegments), _frequencySegments(frequencySegments)
{
	if (_timeSegments == 0u || _timeSegments > audio::AudioSpectralConfig::MaxTimeSegments
		|| _frequencySegments == 0u || _frequencySegments > audio::AudioSpectralConfig::MaxFrequencySegments)
		throw std::invalid_argument("Spectral surface topology exceeds its fixed limits");
	_grid.reserve(static_cast<std::size_t>(_timeSegments + 1u) * (_frequencySegments + 1u));
	_indices.reserve(static_cast<std::size_t>(_timeSegments) * _frequencySegments * 6u);
	for (unsigned int row = 0u; row <= _frequencySegments; ++row)
		for (unsigned int column = 0u; column <= _timeSegments; ++column)
			_grid.emplace_back(static_cast<float>(column) / _timeSegments,
				static_cast<float>(row) / _frequencySegments);
	for (unsigned int row = 0u; row < _frequencySegments; ++row)
		for (unsigned int column = 0u; column < _timeSegments; ++column)
		{
			const auto a = row * (_timeSegments + 1u) + column;
			const auto b = a + _timeSegments + 1u;
			_indices.insert(_indices.end(), { a, a + 1u, b, a + 1u, b + 1u, b });
		}
}

AudioSpectralModel::~AudioSpectralModel()
{
	_ReleaseResources();
}

float AudioSpectralModel::_CompressMagnitude(double magnitude) noexcept
{
	if (!(magnitude > 0.0)) return 0.0f;
	const auto db = 20.0 * std::log10(magnitude / audio::AudioSpectralConfig::DisplayReferenceMagnitude);
	return static_cast<float>(std::clamp(
		(db - audio::AudioSpectralConfig::DisplayFloorDb)
			/ (audio::AudioSpectralConfig::DisplayCeilingDb - audio::AudioSpectralConfig::DisplayFloorDb),
		0.0, 1.0));
}

std::shared_ptr<const AudioSpectralDisplay> AudioSpectralModel::BuildDisplay(
	const audio::AudioSpectralAnalysis& analysis, const audio::AudioSpectralGainField* gains,
	const audio::AudioSpectralEnvelope* envelope, std::uint64_t revision,
	unsigned int timeSegments, unsigned int frequencySegments, std::string& reason, std::stop_token stop)
{
	reason.clear();
	if (!analysis.Source || !analysis.Source->Identity || analysis.Source->Generation == 0u
		|| !analysis.Transform.IsValid() || !analysis.Source->Layout.IsValid()
		|| revision == 0u || timeSegments == 0u || frequencySegments == 0u
		|| timeSegments > audio::AudioSpectralConfig::MaxTimeSegments
		|| frequencySegments > audio::AudioSpectralConfig::MaxFrequencySegments
		|| analysis.Geometry.FrameCount == 0u
		|| analysis.Geometry.BinCount != analysis.Transform.WindowSamps / 2u + 1u
		|| !(analysis.WindowSum > 0.0) || !std::isfinite(analysis.WindowSum)
		|| analysis.Coefficients.size() / analysis.Geometry.BinCount != analysis.Geometry.FrameCount
		|| analysis.Coefficients.size() % analysis.Geometry.BinCount != 0u
		|| (gains && (gains->FrameCount() != analysis.Geometry.FrameCount
			|| gains->BinCount() != analysis.Geometry.BinCount))
		|| (envelope && envelope->Length() != analysis.Source->Layout.LogicalLength))
	{
		reason = "Spectral display source, dimensions or revision are invalid";
		return nullptr;
	}
	const auto estimate = audio::AudioSpectralDsp::EstimateWorkingMemory(
		analysis.Source->Layout, analysis.Transform, reason);
	if (!estimate || analysis.Geometry.FirstFrameCentre != estimate->Geometry.FirstFrameCentre
		|| analysis.Geometry.FrameCount != estimate->Geometry.FrameCount)
	{
		if (reason.empty()) reason = "Spectral display frame geometry is invalid";
		return nullptr;
	}
	try
	{
		auto display = std::make_shared<AudioSpectralDisplay>();
		display->SourceIdentity = analysis.Source->Identity;
		display->SourceGeneration = analysis.Source->Generation;
		display->Revision = revision;
		display->TimeSegments = timeSegments;
		display->FrequencySegments = frequencySegments;
		const auto sampleRate = static_cast<double>(analysis.Source->Layout.SampleRate);
		const auto nyquist = sampleRate * 0.5;
		const auto lower = (std::min)(audio::AudioSpectralConfig::DisplayMinimumFrequencyHz, nyquist * 0.5);
		display->MinimumFrequencyHz = static_cast<float>(lower);
		display->NyquistHz = static_cast<float>(nyquist);
		display->Heights.resize(static_cast<std::size_t>(timeSegments + 1u) * (frequencySegments + 1u));
        // Worker-owned bounded scratch: row mapping is fixed for this display.
        // Cached corner heights avoid repeating FFT-bin magnitude/gain/log work
        // when several time vertices fall between the same source frame pair.
        struct RowSample
        {
            unsigned int FirstBin, NextBin;
            double BinWeight;
            float H00 = 0.0f, H01 = 0.0f, H10 = 0.0f, H11 = 0.0f;
        };
        std::vector<RowSample> rows;
        rows.reserve(frequencySegments + 1u);
        for (unsigned int row = 0u; row <= frequencySegments; ++row)
        {
            const auto hz = row == 0u ? 0.0 : lower * std::pow(nyquist / lower,
                static_cast<double>(row) / frequencySegments);
            const auto binPosition = hz * analysis.Transform.WindowSamps / sampleRate;
            const auto firstBin = (std::min)(static_cast<unsigned int>(binPosition), analysis.Geometry.BinCount - 1u);
            rows.push_back({firstBin, (std::min)(firstBin + 1u, analysis.Geometry.BinCount - 1u),
                std::clamp(binPosition - firstBin, 0.0, 1.0)});
        }
        auto cachedFirst = analysis.Geometry.FrameCount;
        auto cachedNext = analysis.Geometry.FrameCount;
        auto cachedEnvelope = -1.0f;
		for (unsigned int column = 0u; column < timeSegments; ++column)
		{
			if (stop.stop_requested()) { reason = "Spectral display cancelled"; return nullptr; }
			const auto bodySample = static_cast<double>(column) * analysis.Source->Layout.LogicalLength / timeSegments;
			const auto framePosition = (bodySample - analysis.Geometry.FirstFrameCentre) / analysis.Transform.HopSamps;
			const auto firstFrame = (std::min)(static_cast<std::size_t>(framePosition), analysis.Geometry.FrameCount - 1u);
			const auto nextFrame = (std::min)(firstFrame + 1u, analysis.Geometry.FrameCount - 1u);
			const auto frameWeight = std::clamp(framePosition - firstFrame, 0.0, 1.0);
			const auto envelopeGain = envelope ? envelope->GainAt(static_cast<unsigned long>(bodySample)) : 1.0f;
			for (unsigned int row = 0u; row <= frequencySegments; ++row)
			{
                auto& sample = rows[row];
				const auto height = [&](std::size_t frame, unsigned int bin)
				{
					const auto magnitude = analysis.Magnitude(frame, bin)
						* (gains ? gains->GainLinear(frame, bin) : 1.0) * envelopeGain;
					return !std::isfinite(magnitude) || magnitude < 0.0
						? std::numeric_limits<float>::quiet_NaN() : _CompressMagnitude(magnitude);
				};
                if (cachedFirst != firstFrame || cachedNext != nextFrame || cachedEnvelope != envelopeGain)
                {
                    sample.H00 = height(firstFrame, sample.FirstBin);
                    sample.H01 = height(firstFrame, sample.NextBin);
                    sample.H10 = height(nextFrame, sample.FirstBin);
                    sample.H11 = height(nextFrame, sample.NextBin);
                }
                // Preserve height-domain interpolation and per-corner envelope clamp.
                const auto first = std::lerp(sample.H00, sample.H01, sample.BinWeight);
                const auto next = std::lerp(sample.H10, sample.H11, sample.BinWeight);
                const auto value = std::lerp(first, next, frameWeight);
				if (!std::isfinite(value) || value < 0.0)
				{
					reason = "Spectral display contains invalid magnitudes or heights";
					return nullptr;
				}
				display->Heights[row * (timeSegments + 1u) + column] = static_cast<float>(value);
			}
            cachedFirst = firstFrame; cachedNext = nextFrame; cachedEnvelope = envelopeGain;
		}
		// Duplicate the wrapped time endpoint, rather than dropping the last mesh edge.
		for (unsigned int row = 0u; row <= frequencySegments; ++row)
			display->Heights[row * (timeSegments + 1u) + timeSegments] = display->Heights[row * (timeSegments + 1u)];
		if (!display->IsValid()) { reason = "Spectral display frequency range is unavailable"; return nullptr; }
		return display;
	}
	catch (const std::bad_alloc&) { reason = "Not enough memory for the spectral display"; }
	return nullptr;
}

bool AudioSpectralModel::SetDisplay(std::shared_ptr<const AudioSpectralDisplay> display, std::string& reason)
{
	reason.clear();
	if (!display || !display->IsValid() || display->TimeSegments != _timeSegments
		|| display->FrequencySegments != _frequencySegments)
	{
		reason = "Spectral display does not match the fixed surface topology";
		return false;
	}
	std::lock_guard lock(_surfaceMutex);
	if (_pendingDisplay && _pendingDisplay->SourceIdentity == display->SourceIdentity
		&& (_pendingDisplay->SourceGeneration > display->SourceGeneration
			|| (_pendingDisplay->SourceGeneration == display->SourceGeneration
				&& _pendingDisplay->Revision >= display->Revision)))
	{
		reason = "Spectral display revision is obsolete";
		return false;
	}
	_pendingDisplay = std::move(display);
	return true;
}

void AudioSpectralModel::SetEditorMorph(float morph)
{
	std::lock_guard lock(_surfaceMutex);
	_morph = std::isfinite(morph) ? std::clamp(morph, 0.0f, 1.0f) : 0.0f;
}

void AudioSpectralModel::SetRadius(float radius)
{
	std::lock_guard lock(_surfaceMutex);
	if (std::isfinite(radius) && radius > 0.0f) _radius = radius;
}

void AudioSpectralModel::SetPlayFraction(double fraction)
{
	std::lock_guard lock(_surfaceMutex);
	if (!std::isfinite(fraction)) return;
	fraction -= std::floor(fraction);
	_playFraction = static_cast<float>(fraction);
}

AudioSpectralModel::Surface AudioSpectralModel::DisplayedSurface() const
{
	std::lock_guard lock(_surfaceMutex);
	return _displayedSurface;
}

std::shared_ptr<const AudioSpectralDisplay> AudioSpectralModel::Displayed() const
{
	std::lock_guard lock(_surfaceMutex);
	return _displayedSurface.Display;
}

bool AudioSpectralModel::IsAvailable() const
{
	std::lock_guard lock(_surfaceMutex);
	return _uploadedDisplay && _unavailableReason.empty();
}

std::string AudioSpectralModel::UnavailableReason() const
{
	std::lock_guard lock(_surfaceMutex);
	return _unavailableReason;
}

void AudioSpectralModel::_Fail(std::string reason)
{
	std::lock_guard lock(_surfaceMutex);
	_unavailableReason = std::move(reason);
	_displayedSurface.Display.reset();
}

glm::vec3 AudioSpectralModel::LocalVertex(const Surface& surface,
	unsigned int timeVertex, unsigned int frequencyVertex) noexcept
{
	if (!surface.Display || timeVertex > surface.Display->TimeSegments
		|| frequencyVertex > surface.Display->FrequencySegments) return {};
	const auto& display = *surface.Display;
	const auto u = static_cast<float>(timeVertex) / display.TimeSegments;
	const auto v = static_cast<float>(frequencyVertex) / display.FrequencySegments;
	const auto height = display.Heights[frequencyVertex * (display.TimeSegments + 1u) + timeVertex];
	const auto angle = static_cast<float>(constants::TWOPI) * u;
	const auto ringRadius = surface.Radius * (1.0f + (v - 0.5f) * 0.10f);
	const auto ring = glm::vec3(std::sin(angle) * ringRadius, SurfaceBaseY, std::cos(angle) * ringRadius);
	const auto rectangle = glm::vec3((u - 0.5f) * surface.Radius * 2.0f,
		SurfaceBaseY + height * surface.Radius * HeightRadiusScale,
		-(v * 2.0f - 1.0f) * surface.Radius * DepthRadiusScale);
	const auto point = glm::mix(ring, rectangle, surface.Morph);
	const auto phase = 1.0f - surface.PlayFraction;
	const auto rotation = static_cast<float>(constants::TWOPI) * phase * (1.0f - surface.Morph);
	return { std::cos(rotation) * point.x + std::sin(rotation) * point.z, point.y,
		-std::sin(rotation) * point.x + std::cos(rotation) * point.z };
}

void AudioSpectralModel::_InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	if (!HasCurrentGlContext()) return;
	if (!InitTextures(resourceLib) || !InitShaders(resourceLib))
	{
		_Fail("Spectral shader or levels palette is unavailable");
		return;
	}
	if (_vertexArray != 0u && _heightTexture != 0u && !forceInit) return;
	_ReleaseResources();
	GLint maxTextureSize = 0, vertexTextures = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
	glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &vertexTextures);
	if (vertexTextures < 1 || maxTextureSize < static_cast<GLint>(_timeSegments + 1u)
		|| maxTextureSize < static_cast<GLint>(_frequencySegments + 1u))
	{
		_Fail("GPU cannot sample the configured spectral height texture");
		return;
	}
	glGenVertexArrays(1, &_vertexArray);
	glGenBuffers(1, &_gridBuffer);
	glGenBuffers(1, &_indexBuffer);
	glBindVertexArray(_vertexArray);
	glBindBuffer(GL_ARRAY_BUFFER, _gridBuffer);
	glBufferData(GL_ARRAY_BUFFER, _grid.size() * sizeof(glm::vec2), _grid.data(), GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(glm::vec2), nullptr);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _indexBuffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, _indices.size() * sizeof(unsigned int), _indices.data(), GL_STATIC_DRAW);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glGenTextures(1, &_heightTexture);
	GLint unpackBuffer = 0;
	glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, _heightTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, _timeSegments + 1u, _frequencySegments + 1u,
		0, GL_RED, GL_FLOAT, nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
	if (_vertexArray == 0u || _heightTexture == 0u || glGetError() != GL_NO_ERROR)
	{
		_ReleaseResources();
		_Fail("Unable to allocate the spectral mesh or R32F texture");
	}
}

void AudioSpectralModel::_ReleaseResources()
{
	GlDeleteQueue::DeleteTextures(1, &_heightTexture);
	GlDeleteQueue::DeleteBuffers(1, &_gridBuffer);
	GlDeleteQueue::DeleteBuffers(1, &_indexBuffer);
	GlDeleteQueue::DeleteVertexArrays(1, &_vertexArray);
	_heightTexture = _gridBuffer = _indexBuffer = _vertexArray = 0u;
	std::lock_guard lock(_surfaceMutex);
	_uploadedDisplay.reset();
	_displayedSurface.Display.reset();
}

bool AudioSpectralModel::_UploadDisplay()
{
	std::shared_ptr<const AudioSpectralDisplay> pending, uploaded;
	{
		std::lock_guard lock(_surfaceMutex);
		pending = _pendingDisplay;
		uploaded = _uploadedDisplay;
	}
	if (!pending || _heightTexture == 0u) return false;
	if (pending == uploaded) return true;
	unsigned int left = _timeSegments + 1u, right = 0u, bottom = _frequencySegments + 1u, top = 0u;
	for (unsigned int row = 0u; row <= _frequencySegments; ++row)
		for (unsigned int column = 0u; column <= _timeSegments; ++column)
		{
			const auto index = row * (_timeSegments + 1u) + column;
			if (uploaded && pending->Heights[index] == uploaded->Heights[index]) continue;
			left = (std::min)(left, column); right = (std::max)(right, column);
			bottom = (std::min)(bottom, row); top = (std::max)(top, row);
		}
	if (left <= right && bottom <= top)
	{
		GLint alignment = 0, rowLength = 0, unpackBuffer = 0, skipPixels = 0, skipRows = 0;
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
		glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, _timeSegments + 1u);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, _heightTexture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, left, bottom, right - left + 1u, top - bottom + 1u,
			GL_RED, GL_FLOAT, pending->Heights.data() + bottom * (_timeSegments + 1u) + left);
		const auto error = glGetError();
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
		if (error != GL_NO_ERROR)
		{
			// Upload may have partially changed the texture: no CPU mirror is now valid.
			std::lock_guard lock(_surfaceMutex);
			_uploadedDisplay.reset();
			_displayedSurface.Display.reset();
			_unavailableReason = "Unable to upload the spectral height texture";
			return false;
		}
	}
	std::lock_guard lock(_surfaceMutex);
	_uploadedDisplay = std::move(pending);
	_unavailableReason.clear();
	return true;
}

void AudioSpectralModel::SetBrush(glm::vec2 centre, glm::vec2 radii, int mode)
{
	std::lock_guard lock(_surfaceMutex);
	_brushCentre = centre;
	_brushRadii = radii;
	_brushMode = mode;
}

void AudioSpectralModel::Draw3d(base::DrawContext& ctx, unsigned int numInstances, base::DrawPass pass)
{
	if (pass != base::PASS_SCENE || !HasCurrentGlContext() || _vertexArray == 0u || !_UploadDisplay())
		return; // Surface picking uses the retained displayed heightfield, not a flat picker shader.
	const auto shader = GetShader().lock();
	const auto texture = GetTexture().lock();
	if (!shader || !texture) { _Fail("Spectral shader or levels palette is unavailable"); return; }
	Surface surface;
	float playFraction;
	glm::vec2 brushCentre, brushRadii;
	int brushMode;
	{
		std::lock_guard lock(_surfaceMutex);
		surface = { _uploadedDisplay, _radius, _morph, _playFraction };
		playFraction = _playFraction;
		brushCentre = _brushCentre; brushRadii = _brushRadii; brushMode = _brushMode;
	}
	if (!surface.Display || surface.Morph <= 0.0f) return;
	auto& glCtx = dynamic_cast<GlDrawContext&>(ctx);
	const auto pos = ModelPosition();
	const auto scale = ModelScale();
	glCtx.PushMvp(glm::translate(glm::mat4(1.0f), glm::vec3(pos.X, pos.Y, pos.Z)));
	glCtx.PushMvp(glm::scale(glm::mat4(1.0f), glm::vec3(static_cast<float>(scale))));
	const auto matrices = glCtx.GetUniform("MVP");
	if (matrices && matrices->type() == typeid(std::vector<glm::mat4>))
	{
		for (const auto& matrix : std::any_cast<const std::vector<glm::mat4>&>(*matrices))
			surface.ClipFromLocal *= matrix;
		surface.HasDrawTransform = true;
	}
	glCtx.SetUniform("TextureSampler", 0u);
	glCtx.SetUniform("SpectralHeightSampler", 1u);
	glCtx.SetUniform("SpectralRadius", surface.Radius);
	glCtx.SetUniform("SpectralHeightScale", HeightRadiusScale);
	glCtx.SetUniform("SpectralDepthScale", DepthRadiusScale);
	glCtx.SetUniform("SpectralBaseY", SurfaceBaseY);
	glCtx.SetUniform("EditorMorph", surface.Morph);
	glCtx.SetUniform("EditorPlayFrac", playFraction);
	glCtx.SetUniform("SpectralBrush", glm::vec4(brushCentre, brushRadii));
	glCtx.SetUniform("SpectralBrushMode", brushMode);
	glUseProgram(shader->GetId());
	shader->SetUniforms(glCtx);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture->GetId());
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, _heightTexture);
	glBindVertexArray(_vertexArray);
	const auto wasCulling = glIsEnabled(GL_CULL_FACE);
	glDisable(GL_CULL_FACE); // The camera can orbit below the editable surface.
	if (numInstances > 1u)
		glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(_indices.size()), GL_UNSIGNED_INT, nullptr, numInstances);
	else
		glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(_indices.size()), GL_UNSIGNED_INT, nullptr);
	const auto error = glGetError();
	if (wasCulling) glEnable(GL_CULL_FACE);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	glCtx.PopMvp();
	glCtx.PopMvp();
	if (error != GL_NO_ERROR) { _Fail("Unable to draw the spectral surface"); return; }
	std::lock_guard lock(_surfaceMutex);
	_displayedSurface = std::move(surface);
	_unavailableReason.clear();
}
