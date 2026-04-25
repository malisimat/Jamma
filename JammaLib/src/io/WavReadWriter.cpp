#include "WavReadWriter.h"
#include <algorithm>

using namespace io;

std::optional<std::tuple<std::vector<float>, unsigned int, unsigned int>>
	WavReadWriter::_Read(const std::wstring& fileName, unsigned int maxVals) const
{
	struct SoundHeader info;
	struct SoundHeader* pinfo = &info;

	// The variable maxSamps represents the maximum
	// number of 32 bit floats the array can contain.
	if (maxVals < 1)
		return std::nullopt;

	auto fileNameStr = utils::EncodeUtf8(fileName);
	auto cname = new char[fileNameStr.size() + 1];
	strcpy_s(cname, fileNameStr.size() + 1, fileNameStr.c_str());

	auto fid = OpenSoundIn(cname, pinfo);

	delete[] cname;

	if (fid == NULL)
		return {};

	size_t numSampsLoaded = maxVals;

	// Only load the reported number of samples
	// (but prevent loading more than the array can hold)
	if (info.dlength < (long)(maxVals * 2))
		numSampsLoaded = info.dlength / 2; // Two 16 bit integers per sample (32 bit float)

	if (numSampsLoaded < 1)
	{
		numSampsLoaded = 0;
		fclose(fid);

		return {};
	}

	auto bufferIn = new char[numSampsLoaded * 2];  // Two bytes per wav file sample (16 bit integer)
	auto bytesRead = fread(bufferIn, 1, numSampsLoaded * 2, fid);

	if ((bytesRead / 2) < numSampsLoaded)
		numSampsLoaded = bytesRead / 2;  // Two bytes per wav file sample (16 bit integer)

	fclose(fid);

	std::vector<float> buffer(numSampsLoaded);

	for (auto i=0U; i<numSampsLoaded; i++)
	{
		buffer[i] = CharToFloat(bufferIn + (i * 2));
	}

	delete[] bufferIn;

	return std::make_tuple(std::move(buffer), (unsigned int)numSampsLoaded, info.srate);
}

bool WavReadWriter::_Write(std::wstring fileName,
	const std::vector<float>& data,
	unsigned int numVals,
	unsigned int sampleRate) const
{
	char* cname;
	char* bufferOut;
	struct SoundHeader info;
	struct SoundHeader* pinfo = &info;
	FILE* fid;

	auto fileNameStr = utils::EncodeUtf8(fileName);
	cname = new char[fileNameStr.size() + 1];
	strcpy_s(cname, fileNameStr.size() + 1, fileNameStr.c_str());

	if (numVals < 1)
	{
		delete[] cname;
		return false;
	}

	info.srate = sampleRate;
	info.dlength = numVals * 2;
	info.bits_per_samp = 16;

	FillHeader(info);

	fid = OpenSoundOut(cname, pinfo);

	delete[] cname;

	if (fid == NULL)
		return false;

	bufferOut = new char[numVals * 2]; // Two bytes per wav file sample (16 bit integer)

	for (unsigned int i = 0; i < numVals; i++)
	{
		FloatToChar(data[i], bufferOut + (i * 2));
	}

	fwrite(bufferOut, 1, numVals * 2, fid);

	fclose(fid);
	delete[] bufferOut;

	return true;
}

FILE* WavReadWriter::OpenSoundIn(char *fileName, struct SoundHeader *hdr)
{
	FILE *myfile;

	// Check file name for .wav extension
	if (fileName[strlen(fileName) - 1] == 'v')
	{
		myfile = _fsopen(fileName, "rb", _SH_DENYNO);

		if (myfile == NULL)
		{
			std::cout << "Open (Read) wav file error\n";
		}
		else
			fread(hdr, 1, 44, myfile);
	}
	else
	{
		std::cout << "You must specify a valid .wav file name\n";
		myfile = 0;
	}

	return myfile;
}

FILE *WavReadWriter::OpenSoundOut(char *fileName, struct SoundHeader *hdr)
{
	FILE *myfile;
	errno_t err;

	// Check file name for .wav extension
	if (fileName[strlen(fileName) - 1] == 'v')
	{
		err = fopen_s(&myfile, fileName, "wb");

		if (err != 0)
		{
			std::cout << "Open (Write) wav file error\n";
			myfile = 0;
		}
		else
			fwrite(hdr, 1, 44, myfile);
	}
	else
	{
		std::cout << "You must specify a valid .wav file name\n";
		myfile = 0;
	}

	return myfile;
}

void WavReadWriter::FillHeader(struct SoundHeader &hdr)
{
	struct SoundHeader myhdr = { "RIF", 88244, "WAV", "fmt", 16, 1, 1,
		44100, 88200, 2, 16, "dat", 88200 };

	myhdr.srate = hdr.srate;
	myhdr.bits_per_samp = hdr.bits_per_samp;
	myhdr.bytes_per_samp = (hdr.bits_per_samp / 8);
	myhdr.dlength = hdr.dlength;
	myhdr.flength = hdr.dlength + 44;
	myhdr.bytes_per_sec = hdr.srate * myhdr.bytes_per_samp;
	myhdr.riff[3] = 'F';
	myhdr.wave[3] = 'E';
	myhdr.fmt[3] = ' ';
	myhdr.data[3] = 'a';
	hdr = myhdr;
}

void WavReadWriter::FloatToChar(float f, char* c)
{
	auto clamped = std::max(-1.0f, std::min(1.0f, f));
	int tempint = (int)(clamped * 32767.0f);

	*c = (char)(tempint & 0xFF);
	*(c + 1) = (char)((tempint >> 8) & 0xFF);
}

float WavReadWriter::CharToFloat(char* c)
{
	short tempshort = (short)(((((short) *(c + 1)) << 8) & 0xFF00) | (((short)*c) & 0x00FF));

	return (((float)tempshort) / 32768.0f);
}
