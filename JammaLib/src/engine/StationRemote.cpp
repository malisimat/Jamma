#include "StationRemote.h"

#include <algorithm>
#include "LoopRemote.h"

using namespace engine;

StationRemote::StationRemote(StationParams params,
	audio::AudioMixerParams mixerParams) :
	Station(params, mixerParams),
	_remoteUserName(params.Name),
	_remoteChannelCount(0u),
	_assignedOutputChannel(0u),
	_isConnectedRemote(false),
	_intervalLengthSamps(constants::DefaultSampleRate),
	_intervalPositionSamps(0u),
	_remoteTake(nullptr),
	_leftLoop(nullptr),
	_rightLoop(nullptr),
	_nameLabel(nullptr)
{
	SetNumBusChannels(2);
	SetModelScale(2.0);

	gui::GuiLabelParams labelParams;
	labelParams.String = _remoteUserName;
	labelParams.Position = { 6, 8 };
	labelParams.ModelPosition = { 6.0f, 8.0f, 0.0f };
	labelParams.Size = { 180, 18 };
	_nameLabel = std::make_shared<gui::GuiLabel>(labelParams);
	_children.push_back(_nameLabel);
}

void StationRemote::EnsureRemoteTake()
{
	if (_remoteTake)
		return;

	LoopTakeParams takeParams;
	takeParams.Id = _name + "-REMOTE-TAKE";
	takeParams.Size = { 100, 100 };
	takeParams.FadeSamps = _fadeSamps;
	audio::MergeMixBehaviourParams takeMerge;
	auto takeMixerParams = LoopTake::GetMixerParams(takeParams.Size, takeMerge);
	_remoteTake = std::make_shared<LoopTake>(takeParams, takeMixerParams);
	_remoteTake->SetRackVisibility(false);

	audio::WireMixBehaviourParams visualOnlyWire;
	visualOnlyWire.Channels = {};
	auto loopMixerParams = Loop::GetMixerParams({ 110, 48 }, visualOnlyWire);

	LoopParams leftParams;
	leftParams.Id = _name + "-REMOTE-L";
	leftParams.TakeId = takeParams.Id;
	leftParams.Wav = leftParams.Id;
	leftParams.Channel = 0;
	leftParams.FadeSamps = _fadeSamps;
	_leftLoop = std::make_shared<LoopRemote>(leftParams, loopMixerParams);
	_leftLoop->SetMeasureLength(_intervalLengthSamps.load());

	LoopParams rightParams = leftParams;
	rightParams.Id = _name + "-REMOTE-R";
	rightParams.Wav = rightParams.Id;
	rightParams.Channel = 1;
	_rightLoop = std::make_shared<LoopRemote>(rightParams, loopMixerParams);
	_rightLoop->SetMeasureLength(_intervalLengthSamps.load());

	_remoteTake->AddLoop(_leftLoop);
	_remoteTake->AddLoop(_rightLoop);
	AddTake(_remoteTake);
	CommitChanges();

	SetRackVisibility(false, false);
}

void StationRemote::SetRemoteUserName(const std::string& userName)
{
	_remoteUserName = userName;
	SetName(userName);

	if (_nameLabel)
		_nameLabel->SetString(_remoteUserName);
}

void StationRemote::SetRemoteChannelCount(unsigned int channelCount)
{
	_remoteChannelCount = channelCount;
}

void StationRemote::SetAssignedOutputChannel(unsigned int outChannelLeft)
{
	_assignedOutputChannel = outChannelLeft;
}

void StationRemote::SetConnectedRemote(bool connected)
{
	_isConnectedRemote = connected;
}

void StationRemote::SetRemoteInterval(unsigned int lengthSamps, unsigned int positionSamps)
{
	const auto safeLength = std::max(1u, lengthSamps);
	_intervalLengthSamps.store(safeLength);
	_intervalPositionSamps.store(positionSamps % safeLength);

	if (_leftLoop)
	{
		_leftLoop->SetMeasureLength(safeLength);
		_leftLoop->SetMeasurePosition(_intervalPositionSamps.load());
	}

	if (_rightLoop)
	{
		_rightLoop->SetMeasureLength(safeLength);
		_rightLoop->SetMeasurePosition(_intervalPositionSamps.load());
	}
}

void StationRemote::IngestStereoBlock(const float* left,
	const float* right,
	unsigned int numSamps)
{
	// Audio callback path: must be real-time safe.
	// EnsureRemoteTake() and SetMeasureLength() are not safe here (allocations).
	// The job thread (_ReconcileRemoteStations) guarantees the take and loops
	// are created and measure length is set before this is called.
	// Early-out if loops are not yet ready.
	if (!left || !right || numSamps == 0u)
		return;

	if (!_leftLoop || !_rightLoop)
		return;

	base::AudioWriteRequest req;
	req.numSamps = numSamps;
	req.stride = 1u;
	req.fadeCurrent = 0.0f;
	req.fadeNew = 1.0f;
	req.source = base::Audible::AUDIOSOURCE_MIXER;

	req.samples = left;
	OnBlockWriteChannel(0u, req, 0);

	req.samples = right;
	OnBlockWriteChannel(1u, req, 0);

	EndMultiWrite(numSamps, true, base::Audible::AUDIOSOURCE_MIXER);

	_leftLoop->IngestSamples(left, numSamps);
	_rightLoop->IngestSamples(right, numSamps);
}
