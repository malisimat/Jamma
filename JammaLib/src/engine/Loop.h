#pragma once

#include <string>
#include <memory>
#include "Audible.h"
#include "ActionReceiver.h"
#include "ResourceUser.h"
#include "../resources/WavResource.h"

class LoopParams
{
public:
	std::string Wav;
};

class Loop :
	public Audible, public ResourceUser, public ActionReceiver
{
public:
	Loop(LoopParams loopParams);
	~Loop();

public:

	virtual bool InitResources(ResourceLib& resourceLib) override;
	virtual bool ReleaseResources() override;
	virtual void Play(float* buf, unsigned int numChans, unsigned int numSamps) override;

private:
	unsigned int _index;
	LoopParams _loopParams;
	std::weak_ptr<WavResource> _wav;
};

