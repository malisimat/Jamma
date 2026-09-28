#pragma once

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace console
{
	enum class CommandKind { Chat, Help, Connect, Disconnect, Unknown };
	struct ParsedCommand
	{
		CommandKind Kind;
		std::string Verb;
		std::string Arguments;
		int ServerNumber = 0;
	};

	inline ParsedCommand ParseCommand(std::string_view text)
	{
		if (text.empty() || text.front() != '/')
			return { CommandKind::Chat, {}, {} };
		const std::string rest(text.substr(1));
		const auto space = rest.find(' ');
		auto verb = rest.substr(0, space);
		auto args = space == std::string::npos ? std::string{} : rest.substr(space + 1);
		std::transform(verb.begin(), verb.end(), verb.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		while (!args.empty() && args.front() == ' ') args.erase(0, 1);
		if (verb.empty() || verb == "?" || verb == "help")
			return { CommandKind::Help, std::move(verb), std::move(args) };
		if (verb == "c" || verb == "connect")
		{
			int number = 0;
			try { number = std::stoi(args); }
			catch (const std::exception&) {}
			return { CommandKind::Connect, std::move(verb), std::move(args), number };
		}
		if (verb == "d" || verb == "q" || verb == "quit"
			|| verb == "exit" || verb == "disconnect")
			return { CommandKind::Disconnect, std::move(verb), std::move(args) };
		return { CommandKind::Unknown, std::move(verb), std::move(args) };
	}
}
