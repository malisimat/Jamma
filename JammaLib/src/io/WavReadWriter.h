#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <optional>
#include <memory>
#include "FileReadWriter.h"
#include "../utils/StringUtils.h"

namespace io
{
	struct SoundHeader
	{
		char  riff[4];        /* "RIFF"                                  */
		long  flength;        /* file length in bytes                    */
		char  wave[4];        /* "WAVE"                                  */
		char  fmt[4];         /* "fmt "                                  */
		long  block_size;     /* in bytes (generally 16)                 */
		short format_tag;     /* 1=PCM, 257=Mu-Law, 258=A-Law, 259=ADPCM */
		short num_chans;      /* 1=mono, 2=stereo                        */
		long  srate;          /* Sampling rate in samples per second     */
		long  bytes_per_sec;  /* bytes per second                        */
		short bytes_per_samp; /* 2=16-bit mono, 4=16-bit stereo          */
		short bits_per_samp;  /* Number of bits per sample               */
		char  data[4];        /* "data"                                  */
		long  dlength;        /* data length in bytes (filelength - 44)  */
	};

	class WavReadWriter :
		public FileReadWriter<std::vector<float>>
	{
	protected:
		std::optional<std::tuple<std::vector<float>, unsigned int, unsigned int>>
			_Read(const std::wstring& fileName, unsigned int maxVals) const;

		bool _Write(std::wstring fileName,
			const std::vector<float>& buffer,
			unsigned int numSamps,
			unsigned int sampleRate) const;

	protected:
		static FILE* OpenSoundIn(char* fileName, struct SoundHeader* hdr);
		static FILE* OpenSoundOut(char* fileName, struct SoundHeader* hdr);
		static void FillHeader(struct SoundHeader& hdr);

		static void FloatToChar(float f, char* c);
		static float CharToFloat(char* c);
	};
}
