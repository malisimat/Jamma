#include "LoopGridEditor.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "glm/ext.hpp"
#include "../graphics/LoopGridProjection.h"
#include "MidiNote.h"
#include "MidiQuantisation.h"

using namespace midi;

LoopGridEditor::LoopGridEditor(Host host, utils::Size2d size) :
	_host(std::move(host)),
	_size(size)
{
	const auto buttonPos = _ButtonPosition(size);

	auto buttonParams = gui::GuiButtonParams::PanelButton(190u);
	buttonParams.Text = "Edit loop (E)";
	buttonParams.Position = buttonPos;
	_button = std::make_shared<gui::GuiButton>(buttonParams);
	_button->Init();

	auto feedbackParams = gui::GuiLabelParams::PanelHeader("", 330u);
	feedbackParams.Position = { std::max(0, buttonPos.X - 340), buttonPos.Y + 6 };
	_feedback = std::make_shared<gui::GuiLabel>(feedbackParams);
	_feedback->Init();

	auto modeParams = gui::GuiLabelParams::PanelHeader("", 260u);
	modeParams.Position = { std::max(0, buttonPos.X - 340), buttonPos.Y - 20 };
	_modeLabel = std::make_shared<gui::GuiLabel>(modeParams);
	_modeLabel->Init();

	auto channelParams = gui::GuiNumericInputParams::PanelInput(88u);
	channelParams.Size = {88u, 56u};
	channelParams.Padding = 8u;
	channelParams.TintColor = {0.12f, 0.45f, 0.60f};
	channelParams.Min = 1;
	channelParams.Max = 16;
	channelParams.Step = 0.1;
	channelParams.Decimals = 0;
	channelParams.InitValue = 1;
	_channelInput = std::make_shared<gui::GuiNumericInput>(channelParams);
	_channelInput->Init();
	_channelLabel = std::make_shared<gui::GuiLabel>(gui::GuiLabelParams::PanelHeader("CHANNEL", 88u));
	_channelLabel->Init();
	_Layout();

	for (auto& tick : _timeTicks)
	{
		tick = std::make_shared<gui::GuiLabel>(gui::GuiLabelParams::PanelHeader("", 56u));
		tick->Init();
	}
	for (auto& tick : _pitchTicks)
	{
		tick = std::make_shared<gui::GuiLabel>(gui::GuiLabelParams::PanelHeader("", 56u));
		tick->Init();
	}
}

void LoopGridEditor::InitResources(resources::ResourceLib& resourceLib, bool forceInit)
{
	_button->InitResources(resourceLib, forceInit);
	_feedback->InitResources(resourceLib, forceInit);
	_modeLabel->InitResources(resourceLib, forceInit);
	_channelInput->InitResources(resourceLib, forceInit);
	_channelLabel->InitResources(resourceLib, forceInit);
	for (auto& tick : _timeTicks) tick->InitResources(resourceLib, forceInit);
	for (auto& tick : _pitchTicks) tick->InitResources(resourceLib, forceInit);
}

void LoopGridEditor::ReleaseResources()
{
	CancelInput();
	_button->ReleaseResources();
	_feedback->ReleaseResources();
	_modeLabel->ReleaseResources();
	_channelInput->ReleaseResources();
	_channelLabel->ReleaseResources();
	for (auto& tick : _timeTicks) tick->ReleaseResources();
	for (auto& tick : _pitchTicks) tick->ReleaseResources();
}

void LoopGridEditor::SetSize(utils::Size2d size)
{
	_size = size;
	if (IsOpen())
	{
		_CancelGesture();
		_EndOrbit();
		_PositionCamera();
	}
	_Layout();
}

bool LoopGridEditor::IsReady() const noexcept
{
	return State::Active == _state && !_host.Camera.IsTransitioning();
}

actions::ActionResult LoopGridEditor::_Eaten()
{
	actions::ActionResult eaten;
	eaten.IsEaten = true;
	return eaten;
}

utils::Position2d LoopGridEditor::_ButtonPosition(utils::Size2d size)
{
	return { std::max(0, static_cast<int>(size.Width) - 202),
		std::max(0, static_cast<int>(size.Height) - 42) };
}

float LoopGridEditor::_MidiRadius(std::uint32_t lengthSamps) noexcept
{
	return static_cast<float>(std::clamp(
		70.0 * std::log(static_cast<double>(lengthSamps)) - 600.0, 50.0, 400.0));
}

void LoopGridEditor::_Layout()
{
	const auto pos = _ButtonPosition(_size);
	_button->SetPosition(pos);
	_feedback->SetPosition({ std::max(0, pos.X - 340), pos.Y + 6 });
	_modeLabel->SetPosition({ std::max(0, pos.X - 340), pos.Y - 20 });

}

void LoopGridEditor::_SetFeedback(const std::string& message)
{
	if (message != _feedbackText)
	{
		_feedbackText = message;
		_feedback->SetString(message);
	}
}

void LoopGridEditor::_ResetTarget()
{
	_ClearIdleHover(true);
	_station.reset();
	_take.reset();
	_audioLoop.reset();
	_midiLoop.reset();
}

std::shared_ptr<MidiLoop> LoopGridEditor::_InitialMidiLoop(const engine::LoopTake& take)
{
	std::shared_ptr<MidiLoop> first;
	// Wiring order determines the default; picker order must not hide recorded events.
	for (const auto& loop : take.GetMidiLoops())
	{
		if (!loop || !loop->Model()) continue;
		if (!first) first = loop;
		if (loop->EventCount() > 0u) return loop;
	}
	return first;
}

bool LoopGridEditor::FindCandidate(std::shared_ptr<engine::LoopTake>& take,
	std::shared_ptr<engine::Loop>& audioLoop,
	std::shared_ptr<MidiLoop>& midiLoop) const
{
	const auto hovered = _host.Hovered ? _host.Hovered() : nullptr;
	std::scoped_lock lock(_host.SceneMutex);
	unsigned int fallbackCount = 0u;
	for (const auto& station : _host.Stations)
	{
		for (const auto& candidateTake : station->GetLoopTakes())
		{
			if (!candidateTake || !candidateTake->IsSelected())
				continue;
			for (const auto& candidate : candidateTake->GetLoops())
			{
				if (!candidate)
					continue;
				if (candidate->IsSelected() && hovered
					&& (hovered == candidate || hovered->Parent() == candidate))
				{
					take = candidateTake;
					audioLoop = candidate;
					midiLoop.reset();
					return true;
				}
				if (candidate->IsSelected())
				{
					++fallbackCount;
					take = candidateTake;
					audioLoop = candidate;
					midiLoop.reset();
				}
			}
			bool countedMidiForTake = false;
			for (const auto& candidate : candidateTake->GetMidiLoops())
			{
				if (!candidate)
					continue;
				if (candidate->Model() && candidate->Model()->IsSelected() && hovered
					&& (hovered == candidate->Model() || hovered->Parent() == candidate->Model()))
				{
					take = candidateTake;
					audioLoop.reset();
					midiLoop = _InitialMidiLoop(*candidateTake);
					return true;
				}
				if (!countedMidiForTake && candidate->Model() && candidate->Model()->IsSelected())
				{
					// All channel models share one visible MIDI loop at this depth.
					countedMidiForTake = true;
					++fallbackCount;
					take = candidateTake;
					audioLoop.reset();
					midiLoop = _InitialMidiLoop(*candidateTake);
				}
			}
		}
	}
	return fallbackCount == 1u;
}

std::string LoopGridEditor::_UnavailableReason(const std::shared_ptr<engine::LoopTake>& take,
	const std::shared_ptr<engine::Loop>& audioLoop,
	const std::shared_ptr<MidiLoop>& midiLoop) const
{
	if (!take || !take->IsSelected()
		|| (static_cast<bool>(audioLoop) == static_cast<bool>(midiLoop)))
		return "Select one loop to edit";
	const auto state = take->TakeState();
	if (state != engine::LoopTake::STATE_PLAYING && state != engine::LoopTake::STATE_INACTIVE)
		return "Finish recording before editing";
	if (audioLoop)
	{
		if (!audioLoop->IsSelected())
			return "Select the audio loop to edit";
		if (audioLoop->LoopLength() == 0ul)
			return "Loop has no completed audio";
		if (audioLoop->PlayState() == engine::Loop::STATE_RECORDING)
			return "Finish recording before editing";
	}
	else
	{
		if (!midiLoop->Model() || !midiLoop->Model()->IsSelected())
			return "Select the MIDI loop to edit";
		if (midiLoop->CompletedLengthForEditor() == 0u)
			return "Finish recording or set a loop length to edit";
	}
	return {};
}

bool LoopGridEditor::_Validate() const
{
	const auto station = _station.lock();
	const auto take = _take.lock();
	const auto audioLoop = _audioLoop.lock();
	const auto midiLoop = _midiLoop.lock();
	if (!station || !take || (static_cast<bool>(audioLoop) == static_cast<bool>(midiLoop)))
		return false;
	std::scoped_lock lock(_host.SceneMutex);
	const auto& stations = _host.Stations;
	if (std::find(stations.begin(), stations.end(), station) == stations.end())
		return false;
	const auto& takes = station->GetLoopTakes();
	if (std::find(takes.begin(), takes.end(), take) == takes.end())
		return false;
	const auto state = take->TakeState();
	if (state != engine::LoopTake::STATE_PLAYING && state != engine::LoopTake::STATE_INACTIVE)
		return false;
	if (audioLoop)
	{
		const auto& loops = take->GetLoops();
		return audioLoop->LoopLength() > 0ul
			&& audioLoop->PlayState() != engine::Loop::STATE_RECORDING
			&& std::find(loops.begin(), loops.end(), audioLoop) != loops.end();
	}
	const auto& loops = take->GetMidiLoops();
	return midiLoop->CompletedLengthForEditor() > 0u
		&& midiLoop->Model()
		&& std::find(loops.begin(), loops.end(), midiLoop) != loops.end();
}

bool LoopGridEditor::Open(const std::shared_ptr<engine::LoopTake>& take,
	const std::shared_ptr<engine::Loop>& audioLoop,
	const std::shared_ptr<MidiLoop>& midiLoop)
{
	if (State::Closed != _state || !take || !take->IsSelected()
		|| (static_cast<bool>(audioLoop) == static_cast<bool>(midiLoop)))
		return false;
	const auto unavailableReason = _UnavailableReason(take, audioLoop, midiLoop);
	if (!unavailableReason.empty())
	{
		_SetFeedback(unavailableReason);
		return false;
	}
	{
		std::scoped_lock lock(_host.SceneMutex);
		for (const auto& station : _host.Stations)
		{
			const auto& takes = station->GetLoopTakes();
			if (std::find(takes.begin(), takes.end(), take) != takes.end())
			{
				_station = station;
				break;
			}
		}
	}
	_take = take;
	_audioLoop = audioLoop;
	_midiLoop = midiLoop;
	if (!_Validate())
	{
		_ResetTarget();
		_SetFeedback("Loop must be complete and idle to edit");
		return false;
	}
	_returnCamera = _host.Camera.CaptureEditorReturnState();
	_blend = 0.0f;
	_pointerOwned = false;
	_ClearIdleHover(true);
	_EndOrbit();
	_orbitHorizontal = 0.0f;
	_orbitVertical = 0.0f;
	_AcquireRevisionCursor(midiLoop);
	if (midiLoop)
	{
		_FitPitchRange(midiLoop);
		_channelInput->SetValue(_ChannelOf(midiLoop) + 1u);
	}
	_state = State::Opening;
	if (_host.OnOpened)
		_host.OnOpened();
	_host.Camera.SetEditorPerspective(true);
	_PositionCamera();
	_SetFeedback(midiLoop ? "MIDI loop editor" : "Audio loop view");
	return true;
}

void LoopGridEditor::_AcquireRevisionCursor(const std::shared_ptr<MidiLoop>& midiLoop)
{
	_revisionCursors.erase(std::remove_if(_revisionCursors.begin(), _revisionCursors.end(),
		[](const CursorEntry& entry) { return entry.Loop.expired(); }), _revisionCursors.end());
	_revisionCursor.reset();
	if (!midiLoop)
		return;
	for (const auto& entry : _revisionCursors)
		if (entry.Loop.lock() == midiLoop)
		{
			_revisionCursor = entry.Cursor;
			break;
		}
	if (!_revisionCursor)
	{
		_revisionCursor = std::make_shared<actions::MidiEditRevisionCursor>();
		_revisionCursors.push_back({ midiLoop, _revisionCursor });
	}
}

void LoopGridEditor::_FitPitchRange(const std::shared_ptr<MidiLoop>& midiLoop)
{
	const auto model = midiLoop->Model();
	MidiLoop::EditState source;
	if (!model || !midiLoop->SnapshotForEdit(source))
		return;
	std::vector<MidiEvent> displayed(source.EventCount);
	if (source.Quantisation.Enabled)
		MidiQuantisation::BuildQuantisedPlaybackEvents(source.Events.data(),
			source.EventCount, source.LoopLengthSamps, source.Quantisation,
			source.QuantisationTransportStartSamps, displayed.data());
	else std::copy_n(source.Events.begin(), source.EventCount, displayed.begin());
	const auto notes = MidiNote::ExtractSpans(displayed.data(),
		displayed.size(), source.LoopLengthSamps);
	int low = 127, high = 0;
	for (const auto& note : notes)
	{
		low = std::min(low, static_cast<int>(note.Note));
		high = std::max(high, static_cast<int>(note.Note));
	}
	const auto rows = notes.empty() ? 24 : std::clamp(
		((high - low + 7 + 11) / 12) * 12, 24, 128);
	int bottom = 24; // C1 is the default when the notes fit with headroom.
	if (!notes.empty())
	{
		const auto headroom = (rows + 3) / 4;
		const auto lowestDefault = std::min(24, low);
		const auto bottomForHeadroom = high + headroom + 1 - rows;
		bottom = std::min(low, std::max(lowestDefault, bottomForHeadroom));
	}
	bottom = std::clamp(bottom, 0, 128 - rows);
	model->SetEditorPitchRange(bottom, rows);
}

bool LoopGridEditor::SelectMidiChannel(unsigned int channel)
{
	const auto take = _take.lock();
	const auto previous = _midiLoop.lock();
	if (!IsOpen() || !take || !previous || channel < 1u || channel > 16u)
		return false;
	std::shared_ptr<MidiLoop> next;
	{
		std::scoped_lock lock(_host.SceneMutex);
		const auto& loops = take->GetMidiLoops();
		const auto& channels = take->MidiLoopChannels();
		for (std::size_t i = 0; i < loops.size() && i < channels.size(); ++i)
			if (channels[i] == channel - 1u && loops[i] && loops[i]->Model()
				&& loops[i]->CompletedLengthForEditor() > 0u)
			{
				next = loops[i];
				break;
			}
	}
	if (!next)
	{
		_channelInput->SetValue(_ChannelOf(previous) + 1u);
		_SetFeedback("No completed loop on MIDI channel " + std::to_string(channel));
		return false;
	}
	_channelInput->SetValue(channel);
	if (next == previous) return true;
	_CancelGesture();
	_EndOrbit();
	_ClearIdleHover(true);
	if (const auto model = previous->Model())
	{
		model->SetEditorMorph(0.0f);
		model->SetEditorActive(false);
		model->SetEditorHover(-1.0f, -1);
	}
	_midiLoop = next;
	_AcquireRevisionCursor(next);
	_FitPitchRange(next);
	_PositionCamera();
	_state = State::Opening;
	_SetFeedback("Editing MIDI channel " + std::to_string(channel));
	return true;
}

void LoopGridEditor::Close()
{
	if (!IsOpen())
		return;
	_CancelGesture();
	_channelInput->ClearFocus();
	_channelDragging = false;
	_ClearIdleHover(true);
	_EndOrbit();
	_revisionCursor.reset();
	_buttonPressed = false;
	_state = State::Closing;
	_host.Camera.RestoreEditorReturnState(_returnCamera);
	_SetFeedback("");
}

glm::mat4 LoopGridEditor::_ModelMatrix() const
{
	auto matrix = glm::mat4(1.0f);
	const auto station = _station.lock();
	const auto take = _take.lock();
	const auto midiLoop = _midiLoop.lock();
	const auto audioLoop = _audioLoop.lock();
	if (!station || !take || (!midiLoop && !audioLoop)) return matrix;
	const auto stationPos = station->ModelPosition();
	const auto takePos = take->ModelPosition();
	matrix = glm::translate(matrix, glm::vec3(stationPos.X, stationPos.Y, stationPos.Z));
	matrix = glm::scale(matrix, glm::vec3(station->ModelScale()));
	matrix = glm::translate(matrix, glm::vec3(takePos.X, takePos.Y, takePos.Z));
	matrix = glm::scale(matrix, glm::vec3(take->ModelScale()));
	if (midiLoop)
	{
		const auto model = midiLoop->Model();
		if (!model) return matrix;
		const auto pos = model->ModelPosition();
		matrix = glm::translate(matrix, glm::vec3(pos.X, pos.Y, pos.Z));
		return glm::scale(matrix, glm::vec3(model->ModelScale()));
	}
	const auto pos = audioLoop->ModelPosition();
	matrix = glm::translate(matrix, glm::vec3(pos.X, pos.Y, pos.Z));
	return glm::scale(matrix, glm::vec3(audioLoop->ModelScale()));
}

glm::mat4 LoopGridEditor::_ViewProjection() const
{
	const auto aspect = _size.Height > 0u
		? static_cast<float>(_size.Width) / _size.Height : 1.0f;
	return _host.ViewProjection(aspect);
}

void LoopGridEditor::_PositionCamera()
{
	const auto matrix = _ModelMatrix();
	const auto centre = glm::vec3(matrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	const auto worldScale = glm::length(glm::vec3(matrix * glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)));
	float radius = 50.0f;
	if (const auto midiLoop = _midiLoop.lock())
	{
		const auto length = midiLoop->CompletedLengthForEditor();
		if (length)
			radius = _MidiRadius(length);
	}
	else if (const auto audioLoop = _audioLoop.lock())
		radius = static_cast<float>(engine::Loop::CalcDrawRadius(audioLoop->LoopLength()));
	const auto aspect = _size.Height > 0u
		? static_cast<float>(_size.Width) / _size.Height : 1.0f;
	const auto distance = graphics::LoopGridProjection::CameraDistance(radius,
		worldScale, aspect);
	_orbitCentre = centre;
	_orbitRadius = distance;
	_host.Camera.SetViewTarget(graphics::Camera::View::TopDown, _OrbitPose());
}

glm::vec3 LoopGridEditor::ProbeEyeLocal() const
{
	const auto eye = _host.Camera.CurrentPose().Eye;
	return glm::vec3(glm::inverse(_ModelMatrix())
		* glm::vec4(eye.X, eye.Y, eye.Z, 1.0f));
}

graphics::Camera::Pose LoopGridEditor::_OrbitPose() const noexcept
{
	// Screen X and Y rotations both respond from the exact face-on pose.
	const auto offset = _orbitRadius * glm::vec3(
		std::sin(_orbitHorizontal) * std::cos(_orbitVertical),
		std::cos(_orbitHorizontal) * std::cos(_orbitVertical),
		std::sin(_orbitVertical));
	graphics::Camera::Pose pose;
	pose.Eye = { _orbitCentre.x + offset.x, _orbitCentre.y + offset.y,
		_orbitCentre.z + offset.z };
	pose.Forward = { -offset.x, -offset.y, -offset.z };
	pose.Up = { 0.0f, 0.0f, -1.0f };
	return pose;
}

void LoopGridEditor::_BeginOrbit(utils::Position2d pointer) noexcept
{
	_orbitDragging = true;
	_orbitPointerAnchor = pointer;
	_orbitAnchorHorizontal = _orbitHorizontal;
	_orbitAnchorVertical = _orbitVertical;
}

void LoopGridEditor::_UpdateOrbit(utils::Position2d pointer) noexcept
{
	if (!_orbitDragging) return;
	constexpr float radiansPerPixel = 0.004f;
	constexpr float maxAngle = 1.05f;
	const auto delta = pointer - _orbitPointerAnchor;
	_orbitHorizontal = std::clamp(_orbitAnchorHorizontal
		+ static_cast<float>(delta.X) * radiansPerPixel, -maxAngle, maxAngle);
	// App pixels rise upward; moving up should reveal the grid from its top edge (-Z).
	_orbitVertical = std::clamp(_orbitAnchorVertical
		- static_cast<float>(delta.Y) * radiansPerPixel, -maxAngle, maxAngle);
	_host.Camera.SetEditorPose(_OrbitPose());
}

void LoopGridEditor::_EndOrbit() noexcept
{
	_orbitDragging = false;
}

std::optional<MidiGridGesture::Point> LoopGridEditor::_PointAt(
	utils::Position2d pixel, bool clampToGrid) const
{
	const auto loop = _midiLoop.lock();
	if (!loop || !loop->Model()) return std::nullopt;
	const auto length = loop->CompletedLengthForEditor();
	const auto width = static_cast<int>(_size.Width);
	const auto height = static_cast<int>(_size.Height);
	if (!length || width <= 0 || height <= 0) return std::nullopt;
	const auto local = graphics::LoopGridProjection::UnprojectToLocalPlane(_ViewProjection(),
		_ModelMatrix(), pixel, width, height, 2.0f);
	if (!local) return std::nullopt;
	const auto radius = _MidiRadius(length);
	const auto u = static_cast<double>(local->x / (2.0f * radius) + 0.5f);
	const auto v = static_cast<double>(0.5f - local->z / (1.56f * radius));
	if (!clampToGrid && (u < 0.0 || u >= 1.0 || v < 0.0 || v >= 1.0))
		return std::nullopt;
	const auto boundedU = std::clamp(u, 0.0, std::nextafter(1.0, 0.0));
	const auto boundedV = std::clamp(v, 0.0, std::nextafter(1.0, 0.0));
	const auto model = loop->Model();
	const auto pitch = std::clamp(model->EditorBottomPitch()
		+ static_cast<int>(boundedV * model->EditorVisibleRows()), 0, 127);
	const auto sample = static_cast<std::uint32_t>((static_cast<std::uint64_t>(
		LoopGridGeometry::SampleAtU(boundedU, length)) + model->EditorTimeOrigin()) % length);
	return MidiGridGesture::Point{ sample,
		static_cast<std::uint8_t>(pitch), model->EditorTimeOrigin()
			? LoopGridGeometry::SampleU(sample, length) : boundedU, boundedV };
}

std::uint8_t LoopGridEditor::_ChannelOf(const std::shared_ptr<MidiLoop>& loop) const
{
	std::uint8_t channel = 0u;
	if (auto take = _take.lock())
	{
		const auto& loops = take->GetMidiLoops();
		const auto& channels = take->MidiLoopChannels();
		const auto it = std::find(loops.begin(), loops.end(), loop);
		if (it != loops.end() && static_cast<std::size_t>(it - loops.begin()) < channels.size())
			channel = static_cast<std::uint8_t>(channels[it - loops.begin()]);
	}
	return channel;
}

double LoopGridEditor::_PixelsPerSample(std::uint32_t lengthSamps) const
{
	const auto radius = _MidiRadius(lengthSamps);
	const auto viewProjection = _ViewProjection();
	const auto model = _ModelMatrix();
	const auto width = static_cast<int>(_size.Width);
	const auto height = static_cast<int>(_size.Height);
	const auto left = graphics::LoopGridProjection::Project(viewProjection, model,
		{ -radius, 2.0f, 0.0f }, width, height);
	const auto right = graphics::LoopGridProjection::Project(viewProjection, model,
		{ radius, 2.0f, 0.0f }, width, height);
	return left && right && lengthSamps
		? static_cast<double>(std::abs(right->X - left->X)) / lengthSamps : 0.0;
}

void LoopGridEditor::_HandleWheel(const actions::TouchAction& action)
{
	if (auto loop = _midiLoop.lock())
		if (auto model = loop->Model())
		{
			if (base::Action::MODIFIER_SHIFT & action.Modifiers)
			{
				const auto rows = std::clamp(model->EditorVisibleRows()
					+ (action.Value > 0 ? -12 : 12), 12, 128);
				model->SetEditorPitchRange(model->EditorBottomPitch(), rows);
			}
			else model->SetEditorPitchRange(model->EditorBottomPitch()
				+ (action.Value > 0 ? 3 : -3), model->EditorVisibleRows());
			_CancelGesture();
			_SetFeedback("Wheel scrolls pitch; Shift+wheel zooms");
		}
}

void LoopGridEditor::_BeginGesture(const actions::TouchAction& action)
{
	const auto point = _PointAt(action.Position, false);
	const auto loop = _midiLoop.lock();
	MidiLoop::EditState source;
	if (!point || !loop || !loop->SnapshotForEdit(source))
		return;
	_gesture = std::make_unique<MidiGridGesture>();
	if (_gesture->Begin(source, *point, _ChannelOf(loop), _PixelsPerSample(source.LoopLengthSamps)))
	{
		_pointerOwned = true;
		_pointerButton = 0;
		_previewDirty = true;
		_UpdateHover(*point);
	}
	else
	{
		_CancelGesture();
		_SetFeedback("Cannot create or map this MIDI edit");
	}
}

bool LoopGridEditor::_BeginSpecialGesture(const actions::TouchAction& action)
{
	const auto point = _PointAt(action.Position, false);
	const auto loop = _midiLoop.lock();
	MidiLoop::EditState source;
	if (!point || !loop || !loop->Model() || !loop->SnapshotForEdit(source))
		return false;
	const auto targets = MidiGridTargets::Build(source);
	const auto model = loop->Model();
	bool displayMatches = model->NoteInstanceCount() == targets.Notes.size();
	for (std::size_t i = 0; displayMatches && i < targets.Notes.size(); ++i)
	{
		const auto& note = targets.Notes[i];
		const auto& on = source.Events[note.On];
		displayMatches = model->EditorNoteMatches(i, MidiNote{note.Start, note.End - note.Start,
			on.Channel(), note.Pitch, on.data2, on.flags}, source.LoopLengthSamps);
	}
	if (!displayMatches)
	{
		_SetFeedback("Note display is refreshing; try again");
		return true; // Never capture a source identity behind a stale displayed instance.
	}
	const auto target = targets.Resolve(point->Sample, point->Pitch, nullptr);
	const bool velocity = action.Index == 2;
	if (!velocity && !target.NoteIndex) return true; // Ctrl is reserved for moving notes.
	if (target.NoteIndex)
	{
		_gesture = std::make_unique<MidiGridGesture>();
		const auto accepted = velocity ? _gesture->BeginVelocity(source, *point)
			: _gesture->BeginSnappedMove(source, *point);
		if (!accepted)
		{
			_CancelGesture();
			_SetFeedback(targets.Notes[*target.NoteIndex].Ambiguous
				? "Ambiguous note: edit rejected"
				: "Enable a time grid, or use plain-left free move");
			return true; // Never fall through to paint or view on a note.
		}
		_gestureModelGeneration = loop->Model()->EditorModelGeneration();
	}
	else
	{
		_pitchView = true;
		_pitchViewPointerAnchor = action.Position;
		_pitchViewGesture.Begin(loop->Model()->EditorBottomPitch(),
			loop->Model()->EditorVisibleRows(), point->V);
	}
	_pointerOwned = true;
	_pointerButton = action.Index;
	_relativePointer = velocity && !_pitchView;
	_ClearIdleHover(false);
	if (_relativePointer && (!_host.BeginRelativePointer
		|| !_host.BeginRelativePointer(_pointerButton, action.Position)))
	{
		_CancelGesture();
		_SetFeedback("Pointer anchoring failed: gesture cancelled");
		return true;
	}
	_previewDirty = true;
	_UpdateHeldTarget();
	if (_pitchView) _SetFeedback("Drag up/down to pan pitch; left/right to zoom");
	return true;
}

void LoopGridEditor::_UpdateHeldTarget()
{
	const auto loop = _midiLoop.lock();
	const auto model = loop ? loop->Model() : nullptr;
	if (!model) return;
	if (!_gesture || !_gesture->CapturedNoteIndex() || _gesture->Rejected())
	{
		model->SetEditorHover(-1.0f, -1);
		model->ClearEditorHeld();
		return;
	}
	_hoverRevision = _gesture->Before().Revision;
	model->SetEditorHover(-1.0f, -1);
	model->SetEditorHeld(static_cast<int>(*_gesture->CapturedNoteIndex()),
		_gesture->Mode() == MidiGridGesture::Kind::Velocity ? _gesture->ProposedVelocity() : -1);
	if (_gesture->Mode() == MidiGridGesture::Kind::Velocity)
		_SetFeedback("Velocity " + std::to_string(_gesture->ProposedVelocity()));
	else if (_gesture->Mode() == MidiGridGesture::Kind::SnappedMove && !_gesture->Preview().empty())
	{
		const auto& preview = _gesture->Preview().front();
		_SetFeedback("Move: pitch " + std::to_string(preview.Pitch)
			+ ", sample " + std::to_string(preview.Start));
	}
}

void LoopGridEditor::_EndGesture(const actions::TouchAction& action)
{
	const auto wasRelative = _relativePointer;
	bool validDrop = true;
	if (_gesture && !wasRelative)
	{
		const auto snappedMove = _gesture->Mode() == MidiGridGesture::Kind::SnappedMove;
		const auto point = _PointAt(action.Position, !snappedMove);
		validDrop = !snappedMove || point.has_value();
		if (point) _gesture->Update(*point);
	}
	if (validDrop && _gesture && _gesture->Dirty() && !_gesture->Rejected()) _PublishGesture();
	else if (_gesture && _gesture->Rejected()) _SetFeedback("Edit rejected; source unchanged");
	_CancelGesture();
	_ClearIdleHover(false);
	// Relative teardown sends a fresh absolute position from the window adapter.
	if (!wasRelative)
	{
		_idleHoverPointer = action.Position;
		if (auto point = _PointAt(action.Position, false)) _UpdateHover(*point);
	}
}

void LoopGridEditor::_PublishGesture()
{
	const auto loop = _midiLoop.lock();
	const auto take = _take.lock();
	if (!loop || !take)
		return;
	std::uint64_t acceptedRevision = 0u;
	if (!take->PublishMidiEdit(loop, _gesture->Working(), &acceptedRevision))
	{
		_SetFeedback("Edit changed or cannot be represented");
		return;
	}
	if (!_revisionCursor)
		_revisionCursor = std::make_shared<actions::MidiEditRevisionCursor>();
	_host.Undo.Add(std::make_shared<actions::MidiLoopEditUndo>(take, loop,
		_gesture->Before(), _gesture->Working(), acceptedRevision, _revisionCursor));
	_SetFeedback("MIDI edit applied (Ctrl+Z to undo)");
}

void LoopGridEditor::_CancelGesture()
{
	const auto relative = _relativePointer;
	const auto button = _pointerButton;
	_pointerOwned = false;
	_pointerButton = -1;
	_relativePointer = false;
	_pitchView = false;
	_previewDirty = false;
	_hoverRevision = 0u;
	if (_gesture) _gesture->Cancel();
	_gesture.reset();
	if (auto loop = _midiLoop.lock())
		if (auto model = loop->Model())
		{
			model->SetEditorHover(-1.0f, -1);
			model->SetEditorPreview({}, loop->CompletedLengthForEditor());
			model->ClearEditorHeld();
		}
	if (relative && _host.EndRelativePointer) _host.EndRelativePointer(button);
}

void LoopGridEditor::_ClearIdleHover(bool clearCache)
{
	_idleHoverPointer.reset();
	if (clearCache)
	{
		_idleHoverCacheValid = false;
		_idleHoverCacheRevision = 0u;
		_idleHoverCacheLength = 0u;
		_idleHoverCacheQuantisation = {};
		_idleHoverCacheTransportStart = 0u;
		_idleHoverTargets = {};
		_idleHoverGrid.reset();
	}
}

void LoopGridEditor::_UpdateIdleHover()
{
	if (!_idleHoverPointer || !IsOpen() || _pointerOwned || _orbitDragging)
		return;
	const auto point = _PointAt(*_idleHoverPointer, false);
	if (!point)
	{
		if (auto loop = _midiLoop.lock())
			if (auto model = loop->Model()) model->SetEditorHover(-1.0f, -1);
		return;
	}
	_UpdateHover(*point);
}

void LoopGridEditor::_UpdateHover(MidiGridGesture::Point point)
{
	const auto loop = _midiLoop.lock();
	const auto model = loop ? loop->Model() : nullptr;
	if (!model) return;
	if (_pointerOwned)
	{
		model->SetEditorHover(-1.0f, -1);
		return;
	}
	MidiGridTargets::Target target;
	std::uint32_t length = 0u;
	bool gridResolved = false;
	if (_gesture)
	{
		if (_gesture->Rejected())
		{
			model->SetEditorHover(-1.0f, -1);
			return;
		}
		target = _gesture->TargetAt(point);
		length = _gesture->Before().LoopLengthSamps;
		gridResolved = _gesture->Grid().has_value();
		_hoverRevision = _gesture->Before().Revision;
	}
	else
	{
		MidiLoop::EditState source;
		if (!loop->SnapshotForEdit(source))
		{
			model->SetEditorHover(-1.0f, -1);
			return;
		}
		if (!_idleHoverCacheValid || _idleHoverCacheRevision != source.Revision
			|| _idleHoverCacheLength != source.LoopLengthSamps
			|| _idleHoverCacheQuantisation != source.Quantisation
			|| _idleHoverCacheTransportStart != source.QuantisationTransportStartSamps)
		{
			_idleHoverTargets = MidiGridTargets::Build(source);
			_idleHoverGrid = LoopGridGeometry::Resolve(source.LoopLengthSamps,
				source.Quantisation, source.QuantisationTransportStartSamps);
			_idleHoverCacheRevision = source.Revision;
			_idleHoverCacheLength = source.LoopLengthSamps;
			_idleHoverCacheQuantisation = source.Quantisation;
			_idleHoverCacheTransportStart = source.QuantisationTransportStartSamps;
			_idleHoverCacheValid = true;
		}
		target = _idleHoverTargets.Resolve(point.Sample, point.Pitch,
			_idleHoverGrid ? &*_idleHoverGrid : nullptr);
		length = source.LoopLengthSamps;
		gridResolved = _idleHoverGrid.has_value();
		_hoverRevision = source.Revision;
	}
	if (!length) return;
	// Free timing has no cell to highlight, so empty space keeps the pointer glow.
	if (!target.NoteIndex && !gridResolved)
	{
		model->SetEditorHover(static_cast<float>(point.U), point.Pitch);
		return;
	}
	model->SetEditorTarget(static_cast<float>(target.Start) / length,
		static_cast<float>(target.End) / length, target.Pitch,
		target.NoteIndex ? static_cast<int>(*target.NoteIndex) : -1);
}

void LoopGridEditor::_UpdatePreview()
{
	const auto loop = _midiLoop.lock();
	if (!loop || !loop->Model() || !_gesture) return;
	std::vector<graphics::MidiModel::EditorPreviewSpan> spans;
	spans.reserve(_gesture->Preview().size());
	for (const auto& preview : _gesture->Preview())
		spans.push_back({ preview.Start, preview.End, preview.Pitch, preview.Fill,
			_gesture->Mode() == MidiGridGesture::Kind::SnappedMove });
	loop->Model()->SetEditorPreview(std::move(spans),
		_gesture->Before().LoopLengthSamps);
}

void LoopGridEditor::_CheckGesture()
{
	if (!_gesture || !_pointerOwned) return;
	const auto loop = _midiLoop.lock();
	const auto take = _take.lock();
	MidiLoop::EditState current;
	if (!IsReady() || !_Validate()
		|| !loop || !take || !loop->SnapshotForEdit(current)
		|| !_gesture->MatchesPublished(current)
		|| (_gesture->CapturedNoteIndex() && loop->Model()
			&& _gestureModelGeneration != loop->Model()->EditorModelGeneration())
		|| current.Quantisation != take->ResolvedMidiQuantisation()
		|| current.QuantisationTransportStartSamps != take->MidiQuantisationTransportStartSamps())
	{
		_CancelGesture();
		_SetFeedback("Edit cancelled: loop or grid changed");
	}
}

void LoopGridEditor::CancelInput()
{
	_CancelGesture();
	_EndOrbit();
	_ClearIdleHover(true);
	_buttonPressed = false;
	_channelDragging = false;
	_channelInput->ClearFocus();
}

void LoopGridEditor::Tick(float deltaSeconds)
{
	if (IsOpen() && !_Validate())
		Close();
	if (_pointerOwned)
		_CheckGesture();
	if (State::Closed == _state)
		return;
	const auto step = std::max(0.0f, deltaSeconds) / 0.4f;
	if (State::Closing == _state)
	{
		_blend = std::max(0.0f, _blend - step);
		if (_blend == 0.0f && !_host.Camera.IsTransitioning())
		{
			if (auto loop = _audioLoop.lock())
				if (auto model = loop->Model())
				{
					model->SetEditorMorph(0.0f);
					model->SetEditorActive(false);
				}
			if (auto loop = _midiLoop.lock())
				if (auto model = loop->Model())
				{
					model->SetEditorMorph(0.0f);
					model->SetEditorActive(false);
					model->SetEditorHover(-1.0f, -1);
				}
			_host.Camera.SetEditorPerspective(false);
			_state = State::Closed;
			_ResetTarget();
		}
		return;
	}
	_blend = std::min(1.0f, _blend + step);
	if (_blend == 1.0f && !_host.Camera.IsTransitioning())
		_state = State::Active;
	if (_previewDirty)
	{
		_UpdatePreview();
		_previewDirty = false;
	}
	_UpdateIdleHover();
}

void LoopGridEditor::UpdateUi(const glm::mat4& viewProjection)
{
	_channelInput->SetVisible(false);
	_channelLabel->SetVisible(false);
	if (IsOpen())
	{
		std::string mode = "Audio: view only";
		if (auto loop = _midiLoop.lock())
		{
			MidiLoop::EditState state;
			if (loop->SnapshotForEdit(state))
				mode = LoopGridGeometry::Resolve(state.LoopLengthSamps,
					state.Quantisation, state.QuantisationTransportStartSamps)
					? "Paint: click notes to remove; drag empty cells to add" : "Free timing: drag notes or edges";
		}
		_modeLabel->SetString(mode);

		const auto width = static_cast<int>(_size.Width);
		const auto height = static_cast<int>(_size.Height);
		if (_station.lock() && _take.lock())
		{
			float radius = 100.0f;
			std::shared_ptr<graphics::MidiModel> midiModel;
			if (auto midiLoop = _midiLoop.lock())
			{
				const auto length = midiLoop->CompletedLengthForEditor();
				if (length > 0u)
					radius = _MidiRadius(length);
				midiModel = midiLoop->Model();
			}
			else if (auto audioLoop = _audioLoop.lock())
				radius = static_cast<float>(engine::Loop::CalcDrawRadius(audioLoop->LoopLength()));
			const auto modelMatrix = _ModelMatrix();
			if (midiModel)
			{
				const auto anchor = graphics::LoopGridProjection::SideControlPosition(
					viewProjection, modelMatrix, radius, width, height, {88u, 84u});
				if (anchor)
				{
					_channelInput->SetVisible(true);
					_channelLabel->SetVisible(true);
					_channelInput->SetPosition(*anchor);
					_channelLabel->SetPosition({anchor->X, anchor->Y + 62});
					if (!_channelInput->HasFocus() && !_channelDragging)
						_channelInput->SetValue(_ChannelOf(_midiLoop.lock()) + 1u);
				}
			}

			static constexpr const char* timeNames[] = { "0", "1/4", "1/2", "3/4", "END" };
			for (std::size_t i = 0u; i < _timeTicks.size(); ++i)
			{
				const auto u = static_cast<float>(i) / 4.0f;
				const auto point = graphics::LoopGridProjection::Project(viewProjection, modelMatrix,
					{ (u - 0.5f) * radius * 2.0f, 0.0f, radius * 0.88f }, width, height);
				_timeTicks[i]->SetString(point ? timeNames[i] : "");
				if (point)
					_timeTicks[i]->SetPosition({ point->X - 14, point->Y - 24 });
			}
			for (auto& tick : _pitchTicks) tick->SetString("");
			if (midiModel)
			{
				const auto bottom = midiModel->EditorBottomPitch();
				const auto rows = midiModel->EditorVisibleRows();
				for (std::size_t octave = 0u; octave < _pitchTicks.size(); ++octave)
				{
					const auto pitch = static_cast<int>(octave * 12u);
					if (pitch < bottom || pitch > bottom + rows) continue;
					const auto v = static_cast<float>(pitch - bottom) / rows;
					const auto point = graphics::LoopGridProjection::Project(viewProjection, modelMatrix,
						{ -radius * 1.12f, 0.0f, -(v * 2.0f - 1.0f) * radius * 0.78f }, width, height);
					if (!point) continue;
					_pitchTicks[octave]->SetString("C" + std::to_string(static_cast<int>(octave) - 1));
					_pitchTicks[octave]->SetPosition({ point->X - 12, point->Y - 10 });
				}
			}
		}
	}
	const auto showsClose = IsOpen();
	if (!showsClose && State::Closed == _state)
	{
		std::shared_ptr<engine::LoopTake> take;
		std::shared_ptr<engine::Loop> audioLoop;
		std::shared_ptr<MidiLoop> midiLoop;
		const auto hasCandidate = FindCandidate(take, audioLoop, midiLoop);
		const auto reason = hasCandidate
			? _UnavailableReason(take, audioLoop, midiLoop)
			: std::string("Select and hover one loop to edit");
		_button->SetEnabled(hasCandidate && reason.empty());
		_SetFeedback(reason);
	}
	else
		_button->SetEnabled(true);
	if (showsClose != _buttonShowsClose)
	{
		_button->SetText(showsClose ? "Close editor (Esc)" : "Edit loop (E)");
		_buttonShowsClose = showsClose;
	}
	_button->SetVisible(State::Closing != _state);
}

void LoopGridEditor::ApplyToModels()
{
	if (auto audioLoop = _audioLoop.lock())
		if (auto model = audioLoop->Model())
		{
			model->SetEditorMorph(_blend);
			model->SetEditorActive(true);
		}
	if (auto midiLoop = _midiLoop.lock())
		if (auto model = midiLoop->Model())
		{
			model->SetEditorMorph(_blend);
			model->SetEditorActive(true);
			if (const auto take = _take.lock())
			{
				const auto length = midiLoop->CompletedLengthForEditor();
				model->SetEditorPlayFrac(length == 0u ? 0.0f
					: static_cast<float>(take->MidiPlayIndex() % length) / static_cast<float>(length));
				MidiLoop::EditState published;
				if (midiLoop->SnapshotForEdit(published))
				{
					if (_hoverRevision != published.Revision) model->SetEditorHover(-1.0f, -1);
					model->UpdateEditorGrid(published.LoopLengthSamps,
						published.Quantisation,
						published.QuantisationTransportStartSamps);
				}
			}
		}
}

void LoopGridEditor::Draw(base::DrawContext& ctx)
{
	_feedback->Draw(ctx);
	if (IsOpen())
		_modeLabel->Draw(ctx);
	if (IsOpen() && !_midiLoop.expired())
	{
		_channelLabel->Draw(ctx);
		_channelInput->Draw(ctx);
	}
	_button->Draw(ctx);
	if (IsOpen() && _blend > 0.72f)
	{
		for (auto& tick : _timeTicks) tick->Draw(ctx);
		if (!_midiLoop.expired())
			for (auto& tick : _pitchTicks) tick->Draw(ctx);
	}
}

bool LoopGridEditor::_HandleButton(actions::TouchAction action)
{
	if (State::Closing == _state)
		return false;
	const auto inside = _button->HitTest(_button->GlobalToLocal(action.Position));
	if (actions::TouchAction::TOUCH_DOWN == action.State && 0 == action.Index
		&& action.Touch == actions::TouchAction::TOUCH_MOUSE && inside)
	{
		_buttonPressed = true;
		_button->OnAction(_button->GlobalToLocal(action));
		return true;
	}
	if (actions::TouchAction::TOUCH_UP == action.State && _buttonPressed
		&& action.Touch == actions::TouchAction::TOUCH_MOUSE && action.Index == 0)
	{
		_buttonPressed = false;
		_button->OnAction(_button->GlobalToLocal(action));
		if (inside)
		{
			if (IsOpen())
				Close();
			else
			{
				std::shared_ptr<engine::LoopTake> take;
				std::shared_ptr<engine::Loop> audioLoop;
				std::shared_ptr<MidiLoop> midiLoop;
				if (_button->IsEnabled() && FindCandidate(take, audioLoop, midiLoop))
					Open(take, audioLoop, midiLoop);
			}
		}
		return true;
	}
	return false;
}

std::optional<actions::ActionResult> LoopGridEditor::OnAction(actions::TouchAction action)
{
	_CheckGesture();
	const auto isMouse = action.Touch == actions::TouchAction::TOUCH_MOUSE;
	if (_pointerOwned)
	{
		if (isMouse && actions::TouchAction::TOUCH_UP == action.State && action.Index == _pointerButton)
			_EndGesture(action);
		return _Eaten(); // Another button, wheel, modifier or HUD cannot switch a held gesture.
	}
	if (IsOpen() && !_midiLoop.expired() && isMouse && action.Index == 0)
	{
		const auto inside = _channelInput->IsVisible() && _channelInput->HitTest(_channelInput->GlobalToLocal(action.Position));
		if (inside || _channelDragging)
		{
			if (action.State == actions::TouchAction::TOUCH_DOWN)
			{
				_channelInput->RequestFocus();
				_channelDragging = true;
			}
			_channelInput->OnAction(_channelInput->GlobalToLocal(action));
			if (action.State == actions::TouchAction::TOUCH_UP) _channelDragging = false;
			return _Eaten();
		}
		if (action.State == actions::TouchAction::TOUCH_DOWN && _channelInput->HasFocus())
		{
			actions::KeyAction commit;
			commit.KeyChar = 13u;
			commit.KeyActionType = actions::KeyAction::KEY_DOWN;
			_channelInput->OnAction(commit);
			_channelInput->ClearFocus();
			SelectMidiChannel(static_cast<unsigned int>(std::lround(_channelInput->Value())));
			return _Eaten();
		}
	}
	if (_HandleButton(action)) return _Eaten();
	if (!IsEngaged()) return std::nullopt;
	if (_orbitDragging)
	{
		if (isMouse && actions::TouchAction::TOUCH_UP == action.State && action.Index == _pointerButton)
		{
			_EndOrbit();
			_pointerButton = -1;
			_idleHoverPointer = action.Position;
		}
		return _Eaten();
	}
	if (actions::TouchAction::TOUCH_DOWN == action.State && 4 == action.Index && isMouse)
		_HandleWheel(action);
	else if (actions::TouchAction::TOUCH_DOWN == action.State && isMouse
		&& (action.Index == 0 || action.Index == 2) && IsReady() && _Validate())
	{
		const auto control = (base::Action::MODIFIER_CTRL & action.Modifiers) != 0;
		if ((action.Index == 2 || control) && _BeginSpecialGesture(action)) return _Eaten();
		_ClearIdleHover(false);
		if (action.Index == 0 && !control) _BeginGesture(action);
		if (!_pointerOwned)
		{
			if (auto loop = _midiLoop.lock())
				if (auto model = loop->Model()) model->SetEditorHover(-1.0f, -1);
			_pointerButton = action.Index;
			_BeginOrbit(action.Position);
		}
	}
	return _Eaten();
}

std::optional<actions::ActionResult> LoopGridEditor::OnAction(actions::TouchMoveAction action)
{
	if (!IsEngaged()) return std::nullopt;
	_CheckGesture();
	if (_channelDragging)
	{
		_channelInput->OnAction(_channelInput->GlobalToLocal(action));
		if (0u == (action.MouseButtonsDown & 1u)) _channelDragging = false;
		SelectMidiChannel(static_cast<unsigned int>(std::lround(_channelInput->Value())));
		return _Eaten();
	}
	if (0u == (action.MouseButtonsDown & 1u)) _buttonPressed = false;
	if (_pointerOwned && (action.Touch != actions::TouchAction::TOUCH_MOUSE
		|| 0u == (action.MouseButtonsDown & (1u << _pointerButton))))
	{
		_ClearIdleHover(false);
		_CancelGesture();
		_SetFeedback("Gesture cancelled: pointer anchoring or capture lost");
	}
	else if (_pointerOwned)
	{
		if (_relativePointer && !action.IsRelative) return _Eaten();
		if (_pitchView)
		{
			const auto local = graphics::LoopGridProjection::UnprojectToLocalPlane(_ViewProjection(),
				_ModelMatrix(), action.Position, static_cast<int>(_size.Width),
				static_cast<int>(_size.Height), 2.0f);
			const auto loop = _midiLoop.lock();
			if (!local || !loop) return _Eaten();
			const auto fraction = 0.5 - local->z / (1.56 * _MidiRadius(loop->CompletedLengthForEditor()));
			_pitchViewGesture.Update(action.Position.X - _pitchViewPointerAnchor.X, fraction);
			if (auto loop = _midiLoop.lock())
				if (auto model = loop->Model())
				{
					model->SetEditorPitchRange(_pitchViewGesture.Bottom(), _pitchViewGesture.Rows());
					_SetFeedback("Pitch view " + std::to_string(_pitchViewGesture.Bottom())
						+ "–" + std::to_string(_pitchViewGesture.Bottom() + _pitchViewGesture.Rows() - 1));
				}
		}
		else if (_gesture)
		{
			if (_relativePointer) _gesture->UpdateRelative(action.RelativeDelta.Y);
			else if (auto point = _PointAt(action.Position, true)) _gesture->Update(*point);
			_previewDirty = true;
			if (_gesture->CapturedNoteIndex()) _UpdateHeldTarget();
			else if (auto point = _PointAt(action.Position, true)) _UpdateHover(*point);
			if (_gesture->Rejected())
			{
				_CancelGesture();
				_SetFeedback("Cannot represent this move; source unchanged");
			}
		}
	}
	else if (_orbitDragging)
	{
		if (action.Touch == actions::TouchAction::TOUCH_MOUSE
			&& (action.MouseButtonsDown & (1u << _pointerButton)) != 0u)
			_UpdateOrbit(action.Position);
		else { _EndOrbit(); _pointerButton = -1; }
	}
	else if (!action.IsRelative) _idleHoverPointer = action.Position;
	return _Eaten();
}

std::optional<actions::ActionResult> LoopGridEditor::OnAction(const actions::KeyAction& action)
{
	if (!IsEngaged())
		return std::nullopt;
	if (_channelInput->HasFocus() && action.KeyChar != 27u)
	{
		_channelInput->OnAction(action);
		if (action.KeyChar == 13u && action.KeyActionType == actions::KeyAction::KEY_DOWN)
		{
			_channelInput->ClearFocus();
			SelectMidiChannel(static_cast<unsigned int>(std::lround(_channelInput->Value())));
		}
		return _Eaten();
	}
	if (27u == action.KeyChar && actions::KeyAction::KEY_UP == action.KeyActionType)
		Close();
	if (90u == action.KeyChar && actions::KeyAction::KEY_UP == action.KeyActionType
		&& (base::Action::MODIFIER_CTRL & action.Modifiers))
	{
		_CancelGesture();
		const bool redo = (base::Action::MODIFIER_SHIFT & action.Modifiers) != 0;
		const auto accepted = redo ? _host.Undo.Redo() : _host.Undo.Undo();
		_SetFeedback(accepted ? (redo ? "MIDI edit redone" : "MIDI edit undone")
			: "No applicable MIDI edit");
	}
	return _Eaten();
}

std::optional<actions::ActionResult> LoopGridEditor::TryOpenFromKey(const actions::KeyAction& action)
{
	if (!((69u == action.KeyChar) && actions::KeyAction::KEY_UP == action.KeyActionType
		&& base::Action::MODIFIER_NONE == action.Modifiers))
		return std::nullopt;
	std::shared_ptr<engine::LoopTake> take;
	std::shared_ptr<engine::Loop> audioLoop;
	std::shared_ptr<MidiLoop> midiLoop;
	if (FindCandidate(take, audioLoop, midiLoop))
		Open(take, audioLoop, midiLoop);
	else
		_SetFeedback("Select and hover one loop to edit");
	return _Eaten();
}
