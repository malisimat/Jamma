#pragma once

#include <optional>
#include <memory>
#include "../utils/CommonTypes.h"
#include "../engine/Timer.h"
#include "../audio/AudioDevice.h"
#include "../io/UserConfig.h"

namespace base
{
	class ActionReceiver;

	class Action
	{
	public:
		Action() :
			_actionTime(std::chrono::steady_clock::now()),
			_userConfig(std::nullopt)
		{};

		~Action() {};

	public:
		Time GetActionTime() const { return _actionTime; }
		void SetActionTime(Time time) { _actionTime = time; }

		std::optional<io::UserConfig> GetUserConfig() const { return _userConfig; }
		void SetUserConfig(io::UserConfig cfg) { _userConfig = cfg; }

		std::optional<audio::AudioStreamParams> GetAudioParams() const { return _audioStreamParams; }
		void SetAudioParams(audio::AudioStreamParams params) { _audioStreamParams = params; }

	protected:
		Time _actionTime;
		std::optional<io::UserConfig> _userConfig;
		std::optional< audio::AudioStreamParams> _audioStreamParams;
	};
}
