#include "Socket.h"

#include "WDL/jnetlib/util.h"

namespace io
{
	WinsockSession::WinsockSession() :
		_started(JNL::open_socketlib() == 0)
	{
	}

	WinsockSession::~WinsockSession()
	{
		if (_started)
			JNL::close_socketlib();
	}

	bool WinsockSession::IsStarted() const noexcept
	{
		return _started;
	}
}