#pragma once

namespace io
{
	class NetworkSession
	{
	public:
		NetworkSession() noexcept;
		~NetworkSession() noexcept;

		NetworkSession(const NetworkSession&) = delete;
		NetworkSession& operator=(const NetworkSession&) = delete;
		NetworkSession(NetworkSession&&) = delete;
		NetworkSession& operator=(NetworkSession&&) = delete;

		bool IsInitialised() const noexcept;

	private:
		bool _initialised = false;
	};
}
