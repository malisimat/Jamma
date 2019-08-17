#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include <filesystem>

namespace io
{
	template <typename T>
	class FileReadWriter
	{
	public:
		void CreateDirectory(const std::wstring& fileName)
		{
			std::filesystem::path p(fileName);
			std::filesystem::path dir = p.parent_path();
			std::filesystem::create_directories(dir);
		}

		std::optional<std::tuple<T, unsigned int, unsigned int>>
			Read(const std::wstring& fileName, unsigned int maxVals) const
		{
			return std::move(_Read(fileName, maxVals));
		}

		bool Write(std::wstring fileName,
			T data,
			unsigned int numVals,
			unsigned int sampleRate) const
		{
			CreateDirectory(fileName);

			return _Write(fileName,
				data,
				numVals,
				sampleRate);
		}

	protected:
		virtual std::optional<std::tuple<T, unsigned int, unsigned int>>
			_Read(const std::wstring& fileName, unsigned int maxVals) const = 0;

		virtual bool _Write(std::wstring fileName,
			T data,
			unsigned int numVals,
			unsigned int sampleRate) const = 0;
	};
}
