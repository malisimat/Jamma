#pragma once

#include <memory>
#include <string>
#include <iostream>
#include "Resource.h"
#include "../io/WavReadWriter.h"

namespace resources
{
	class WavResource :
		public Resource
	{
	public:
		WavResource(std::string name, std::vector<float> audioBuf, unsigned int numSamps, unsigned int sampleRate);
		~WavResource();

		// Delete the copy constructor/assignment
		WavResource(const WavResource&) = delete;
		WavResource& operator=(const WavResource&) = delete;

		WavResource(WavResource&& other) :
			Resource(other._name),
			_numSamps(other._numSamps),
			_sampleRate(other._sampleRate),
			_audioBuf(std::move(other._audioBuf))
		{
			other._numSamps = 0;
			other._numSamps = 0;
			other._audioBuf = {};

		}

		WavResource& operator=(WavResource&& other)
		{
			if (this != &other)
			{

				Release();
				std::swap(_name, other._name);
				std::swap(_numSamps, other._numSamps);
				std::swap(_sampleRate, other._sampleRate);
				std::swap(_audioBuf, other._audioBuf);
			}

			return *this;
		}

	public:
		virtual Type GetType() const override { return WAV; }

	public:
		unsigned int Length() const;
		const std::vector<float>& Buffer();
		static std::optional<std::tuple<std::vector<float>, unsigned int, unsigned int>> Load(const std::string& wavFile);

	private:
		unsigned int _numSamps;
		unsigned int _sampleRate;
		std::vector<float> _audioBuf;
	};
}
