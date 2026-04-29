#pragma once

#include <iostream>
#include <string>
#include <optional>
#include <fstream>
#include <sstream>
#include "FileReadWriter.h"

namespace io
{
	class TextReadWriter :
		public FileReadWriter<std::string>
	{
	protected:
		std::optional<std::tuple<std::string, unsigned int, unsigned int>>
			_Read(const std::wstring& fileName, unsigned int maxVals) const;

		bool _Write(std::wstring fileName,
			const std::string& data,
			unsigned int numVals,
			unsigned int sampleRate) const;
	};
}
