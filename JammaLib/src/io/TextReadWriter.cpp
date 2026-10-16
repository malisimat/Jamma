#include "../utils/StringUtils.h"
#include "TextReadWriter.h"

using namespace io;

std::optional<std::tuple<std::string, unsigned int, unsigned int>>
	TextReadWriter::_Read(const std::wstring& fileName, unsigned int maxSamps) const
{
	std::ifstream stream;
	stream.open(fileName);
	
	if (!stream.is_open())
	{
		const auto error = errno;
		std::cerr << "[ERROR][file] read-open path='" << utils::EncodeUtf8(fileName) << "' errno=" << error << "; file unavailable" << std::endl;
		return std::nullopt;
	}

	std::string str((std::istreambuf_iterator<char>(stream)),
		(std::istreambuf_iterator<char>()));

	return std::make_tuple(std::move(str), (unsigned int)str.size(), 0);
}

bool TextReadWriter::_Write(std::wstring fileName,
	const std::string& data,
	unsigned int numVals,
	unsigned int sampleRate) const
{
	std::ofstream stream;
	stream.open(fileName);

	if (!stream.is_open())
	{
		const auto error = errno;
		std::cerr << "[ERROR][file] write-open path='" << utils::EncodeUtf8(fileName) << "' errno=" << error << std::endl;
		return false;
	}

	stream << data;
	stream.close();
	if (stream.fail())
	{
		const auto error = errno;
		std::cerr << "[ERROR][file] write/close path='" << utils::EncodeUtf8(fileName) << "' errno=" << error << std::endl;
		return false;
	}
	return true;
}
