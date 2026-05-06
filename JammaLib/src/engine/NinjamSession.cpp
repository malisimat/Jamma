#include "NinjamSession.h"

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <thread>
#include <unordered_set>
#include "../utils/StringUtils.h"

using namespace engine;

namespace
{
	using PublicServerInfo = NinjamSession::PublicServerInfo;

	struct StaticNinjamServer
	{
		const char* Name;
		const char* Host;
	};

	constexpr StaticNinjamServer kStaticServerList[] = {
		{ "ninjamer.com #1",          "ninjamer.com:2049"                  },
		{ "ninjamer.com #2",          "ninjamer.com:2050"                  },
		{ "ninjamer.com #3",          "ninjamer.com:2051"                  },
		{ "ninjamer.com #4",          "ninjamer.com:2052"                  },
		{ "ninbot.com #1",            "ninbot.com:2049"                    },
		{ "ninbot.com #2",            "ninbot.com:2050"                    },
		{ "ninbot.com #3",            "ninbot.com:2051"                    },
		{ "ninbot.com #4",            "ninbot.com:2052"                    },
		{ "ninbot.com #5",            "ninbot.com:2053"                    },
		{ "ninbot.com #6",            "ninbot.com:2054"                    },
		{ "mutantlab.com",            "mutantlab.com:2049"                 },
		{ "musicorner",               "musicorner.redirectme.net:2050"     },
		{ "ninbot.rootsociety.nl",    "ninbot.rootsociety.nl:8001"         },
		{ "getaroom (NA)",            "getaroom-na.ninjam.com:2049"        },
		{ "getaroom (EU)",            "getaroom-eu.ninjam.com:2049"        },
		{ "ninjam.com",               "ninjam.com:2049"                    },
		{ "autosong.ninjam.com",      "autosong.ninjam.com:2049"           },
	};

	constexpr auto kServerListFetchTtl = std::chrono::seconds(30);
	constexpr int kServerListFetchTimeoutMs = 6500;

	std::mutex gPublicServerListMutex;
	std::vector<PublicServerInfo> gPublicServerListCache;
	std::chrono::steady_clock::time_point gPublicServerListLastFetch{};
	std::atomic_bool gPublicServerListFetchInFlight{ false };
	bool gPublicServerListHasLiveData = false;

	std::vector<PublicServerInfo> BuildStaticServerList()
	{
		std::vector<PublicServerInfo> servers;
		servers.reserve(std::size(kStaticServerList));
		for (const auto& server : kStaticServerList)
			servers.push_back({ server.Name, server.Host });
		return servers;
	}

	struct ScopedWinHttpHandle
	{
		HINTERNET Handle = nullptr;

		ScopedWinHttpHandle() = default;
		explicit ScopedWinHttpHandle(HINTERNET handle) : Handle(handle) {}
		~ScopedWinHttpHandle()
		{
			if (Handle)
				WinHttpCloseHandle(Handle);
		}

		ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
		ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;
		ScopedWinHttpHandle(ScopedWinHttpHandle&& other) noexcept : Handle(other.Handle)
		{
			other.Handle = nullptr;
		}
		ScopedWinHttpHandle& operator=(ScopedWinHttpHandle&& other) noexcept
		{
			if (this != &other)
			{
				if (Handle)
					WinHttpCloseHandle(Handle);
				Handle = other.Handle;
				other.Handle = nullptr;
			}
			return *this;
		}

		operator HINTERNET() const { return Handle; }
	};

	std::optional<std::string> FetchAutosongServerListHtml()
	{
		ScopedWinHttpHandle session(WinHttpOpen(L"Jamma/1.0",
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS,
			0));
		if (!session)
			return std::nullopt;

		WinHttpSetTimeouts(session,
			kServerListFetchTimeoutMs,
			kServerListFetchTimeoutMs,
			kServerListFetchTimeoutMs,
			kServerListFetchTimeoutMs);

		ScopedWinHttpHandle connect(WinHttpConnect(session,
			L"autosong.ninjam.com",
			INTERNET_DEFAULT_HTTP_PORT,
			0));
		if (!connect)
			return std::nullopt;

		ScopedWinHttpHandle request(WinHttpOpenRequest(connect,
			L"GET",
			L"/server-list.php",
			nullptr,
			WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES,
			0));
		if (!request)
			return std::nullopt;

		if (!WinHttpSendRequest(request,
			WINHTTP_NO_ADDITIONAL_HEADERS,
			0,
			WINHTTP_NO_REQUEST_DATA,
			0,
			0,
			0))
		{
			return std::nullopt;
		}

		if (!WinHttpReceiveResponse(request, nullptr))
			return std::nullopt;

		DWORD statusCode = 0;
		DWORD statusCodeSize = sizeof(statusCode);
		if (!WinHttpQueryHeaders(request,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,
			&statusCode,
			&statusCodeSize,
			WINHTTP_NO_HEADER_INDEX)
			|| statusCode != HTTP_STATUS_OK)
		{
			return std::nullopt;
		}

		std::string body;
		for (;;)
		{
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request, &available))
				return std::nullopt;

			if (available == 0)
				break;

			const auto offset = body.size();
			body.resize(offset + available);
			DWORD read = 0;
			if (!WinHttpReadData(request, body.data() + offset, available, &read))
				return std::nullopt;

			body.resize(offset + read);
		}

		return body;
	}

	std::vector<PublicServerInfo> ParseAutosongServerList(const std::string& html)
	{
		std::string flat = html;
		for (auto& ch : flat)
		{
			if (ch == '\r' || ch == '\n' || ch == '\t')
				ch = ' ';
		}

		const std::regex blockRegex(R"(([A-Za-z0-9._-]+:\d+)\s*(.*?)<ul>(.*?)</ul>)",
			std::regex_constants::icase);
		const std::regex tempoRegex(R"((\d+(?:\.\d+)?)\s*BPM\s*/\s*(\d+))",
			std::regex_constants::icase);
		const std::regex userRatioRegex(R"((\d+)\s*/\s*(\d+)\s*Users:)",
			std::regex_constants::icase);
		const std::regex usersRegex(R"((\d+)\s*Users:)", std::regex_constants::icase);

		std::vector<PublicServerInfo> parsed;
		for (std::sregex_iterator it(flat.begin(), flat.end(), blockRegex), end; it != end; ++it)
		{
			PublicServerInfo server;
			server.Host = utils::Trim((*it)[1].str());

			auto meta = utils::CollapseWhitespace(utils::HtmlToText((*it)[2].str(), false));
			const auto queryPos = meta.find("-- query time");
			if (queryPos != std::string::npos)
				meta = utils::Trim(meta.substr(0, queryPos));
			server.Description = meta;

			const auto bodyText = utils::HtmlToText((*it)[3].str(), true);
			for (auto line : utils::SplitLines(bodyText))
			{
				line = utils::CollapseWhitespace(std::move(line));
				if (line.empty())
					continue;

				if (line.rfind("Topic:", 0) == 0)
				{
					server.Topic = utils::Trim(line.substr(6));
					continue;
				}

				if (line.rfind("Server down:", 0) == 0)
				{
					server.Status = line;
					server.IsReachable = false;
					server.HasLiveData = true;
					continue;
				}

				std::smatch match;
				if (std::regex_search(line, match, tempoRegex))
				{
					server.Bpm = std::stof(match[1].str());
					server.Bpi = std::stoi(match[2].str());
					server.HasLiveData = true;
					continue;
				}

				if (std::regex_search(line, match, userRatioRegex))
				{
					server.ActiveUsers = std::stoi(match[1].str());
					server.Capacity = std::stoi(match[2].str());
					server.HasLiveData = true;
					continue;
				}

				if (std::regex_search(line, match, usersRegex))
				{
					server.ActiveUsers = std::stoi(match[1].str());
					server.HasLiveData = true;
				}
			}

			if (!server.Topic.empty())
				server.HasLiveData = true;

			if (!server.Host.empty())
				parsed.push_back(std::move(server));
		}

		return parsed;
	}

	std::vector<PublicServerInfo> MergeServerLists(const std::vector<PublicServerInfo>& fetched)
	{
		auto merged = BuildStaticServerList();
		std::unordered_set<std::string> seenHosts;

		for (const auto& server : merged)
			seenHosts.insert(utils::ToLower(server.Host));

		for (auto& server : merged)
		{
			const auto key = utils::ToLower(server.Host);
			auto match = std::find_if(fetched.begin(), fetched.end(), [&](const PublicServerInfo& candidate) {
				return utils::ToLower(candidate.Host) == key;
			});
			if (match == fetched.end())
				continue;

			server.Description = match->Description;
			server.Topic = match->Topic;
			server.Status = match->Status;
			server.ActiveUsers = match->ActiveUsers;
			server.Capacity = match->Capacity;
			server.Bpi = match->Bpi;
			server.Bpm = match->Bpm;
			server.HasLiveData = match->HasLiveData;
			server.IsReachable = match->IsReachable;
		}

		for (const auto& fetchedServer : fetched)
		{
			const auto key = utils::ToLower(fetchedServer.Host);
			if (seenHosts.find(key) != seenHosts.end())
				continue;

			auto extra = fetchedServer;
			if (extra.Name.empty())
				extra.Name = extra.Host;
			merged.push_back(std::move(extra));
		}

		return merged;
	}
}

NinjamSession::~NinjamSession()
{
	Stop();
}

void NinjamSession::Start(const io::JamFile::NinjamConfig& config)
{
	Stop();

	if (config.Host.empty() || config.User.empty())
		return;

	_connection = std::make_unique<io::NinjamConnection>(
		config.Host, config.User, config.Pass, config.WorkDir);

	_connection->SetAudioFormat(
		_audioSampleRate, _audioBlockSize,
		_audioNumInputChannels, _audioNumOutputChannels);

	std::cout << "[NINJAM] Auto-connect enabled from JAM config" << std::endl;
	std::cout << "[NINJAM] Type a message and press Enter to chat" << std::endl;

	_connection->Connect();
}

void NinjamSession::Stop()
{
	if (_connection)
	{
		_connection->Disconnect();
		_connection.reset();
	}
}

void NinjamSession::SendChat(const std::string& msg)
{
	if (_connection && _connection->IsConnected())
	{
		_connection->SendChat(msg);
		std::cout << "[NINJAM] <you> " << msg << std::endl;
	}
	else
	{
		std::cout << "[NINJAM] Not connected - message not sent" << std::endl;
	}
}

bool NinjamSession::IsConnected() const noexcept
{
	return _connection && _connection->IsConnected();
}

std::optional<io::NinjamRemoteSnapshot> NinjamSession::Pump()
{
	if (!_connection)
		return std::nullopt;

	_connection->Pump();

	if (!_connection->IsConnected())
		return std::nullopt;

	return _connection->Snapshot();
}

void NinjamSession::SetAudioFormat(unsigned int sampleRate,
	unsigned int blockSize,
	unsigned int numInputChannels,
	unsigned int numOutputChannels)
{
	_audioSampleRate = sampleRate;
	_audioBlockSize = blockSize;
	_audioNumInputChannels = numInputChannels;
	_audioNumOutputChannels = numOutputChannels;

	if (_connection)
		_connection->SetAudioFormat(sampleRate, blockSize, numInputChannels, numOutputChannels);
}

void NinjamSession::ProcessAudioBlock(const float* interleavedInput,
	unsigned int numFrames,
	unsigned int sampleRate)
{
	if (_connection)
		_connection->ProcessAudioBlock(interleavedInput, numFrames, sampleRate);
}

bool NinjamSession::ConsumeStereoPair(unsigned int outChannelLeft,
	const float*& left,
	const float*& right,
	unsigned int& numFrames) const
{
	if (!_connection)
		return false;

	return _connection->ConsumeStereoPair(outChannelLeft, left, right, numFrames);
}

bool NinjamSession::RequestServerTempo(float bpm, int bpi)
{
	if (!_connection || !_connection->IsConnected())
		return false;

	return _connection->RequestServerTempo(bpm, bpi);
}

NinjamSession::PublicServerDirectorySnapshot NinjamSession::GetPublicServerDirectorySnapshot()
{
	std::scoped_lock lock(gPublicServerListMutex);
	if (gPublicServerListCache.empty())
		gPublicServerListCache = BuildStaticServerList();

	return {
		gPublicServerListCache,
		gPublicServerListFetchInFlight.load(),
		gPublicServerListHasLiveData
	};
}

void NinjamSession::RefreshPublicServerDirectoryAsync(std::function<void()> onComplete)
{
	const auto now = std::chrono::steady_clock::now();
	{
		std::scoped_lock lock(gPublicServerListMutex);
		if (!gPublicServerListCache.empty()
			&& gPublicServerListHasLiveData
			&& ((now - gPublicServerListLastFetch) < kServerListFetchTtl))
		{
			return;
		}
	}

	bool expected = false;
	if (!gPublicServerListFetchInFlight.compare_exchange_strong(expected, true))
		return;

	std::thread([onComplete = std::move(onComplete)]() mutable {
		auto clearInFlight = []() { gPublicServerListFetchInFlight = false; };

		auto html = FetchAutosongServerListHtml();
		if (!html.has_value())
		{
			clearInFlight();
			std::cout << "[NINJAM] Live server refresh timed out; using cached server list" << std::endl;
			return;
		}

		auto parsed = ParseAutosongServerList(html.value());
		if (parsed.empty())
		{
			clearInFlight();
			std::cout << "[NINJAM] Live server refresh returned no parsable entries" << std::endl;
			return;
		}

		{
			std::scoped_lock lock(gPublicServerListMutex);
			gPublicServerListCache = MergeServerLists(parsed);
			gPublicServerListLastFetch = std::chrono::steady_clock::now();
			gPublicServerListHasLiveData = true;
		}

		clearInFlight();
		std::cout << "[NINJAM] Live server metadata refreshed" << std::endl;
		if (onComplete)
			onComplete();
	}).detach();
}

std::string NinjamSession::FormatPublicServerSummary(const PublicServerInfo& server)
{
	std::ostringstream summary;
	if (!server.HasLiveData)
		return {};

	summary << " [";
	bool hasAnyField = false;
	if (!server.IsReachable && !server.Status.empty())
	{
		summary << server.Status;
		hasAnyField = true;
	}
	else
	{
		if (server.ActiveUsers >= 0)
		{
			summary << server.ActiveUsers;
			if (server.Capacity >= 0)
				summary << "/" << server.Capacity;
			summary << " users";
			hasAnyField = true;
		}
		if (server.Bpm > 0.0f)
		{
			if (hasAnyField)
				summary << ", ";
			summary << static_cast<int>(server.Bpm + 0.5f) << " BPM";
			hasAnyField = true;
		}
		if (server.Bpi > 0)
		{
			if (hasAnyField)
				summary << ", ";
			summary << server.Bpi << " BPI";
			hasAnyField = true;
		}
	}

	if (!hasAnyField)
		return {};

	summary << "]";
	return summary.str();
}
