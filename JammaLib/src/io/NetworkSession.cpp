#include "NetworkSession.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace io
{
	NetworkSession::NetworkSession() noexcept :
		_initialised(false)
	{
	#ifdef _WIN32
		WSADATA wsaData{};
		_initialised = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
	#else
		_initialised = true;
	#endif
	}

	NetworkSession::~NetworkSession() noexcept
	{
	#ifdef _WIN32
		if (_initialised)
			WSACleanup();
	#endif
	}

	bool NetworkSession::IsInitialised() const noexcept
	{
		return _initialised;
	}
}
