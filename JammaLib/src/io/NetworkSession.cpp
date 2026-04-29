#include "NetworkSession.h"

#include "WDL/jnetlib/util.h"

namespace io
{
	NetworkSession::NetworkSession() noexcept :
		_initialised(JNL::open_socketlib() == 0)
	{
	}

	NetworkSession::~NetworkSession() noexcept
	{
		if (_initialised)
			JNL::close_socketlib();
	}

	bool NetworkSession::IsInitialised() const noexcept
	{
		return _initialised;
	}
}
