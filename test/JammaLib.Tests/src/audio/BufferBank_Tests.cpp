
#include "gtest/gtest.h"
#include "audio/BufferBank.h"

using audio::BufferBank;

class BufferBankSource
{
public:
	BufferBankSource(unsigned int numSamps)
	{
		Samples = std::vector<float>(numSamps);

		for (auto i = 0u; i < numSamps; i++)
		{
			Samples[i] = ((rand() % 2000) - 1000) / 1001.0f;
		}
	}

public:
	void Fill(BufferBank& bank)
	{
		auto numSamps = (unsigned int)Samples.size();
		bank.SetLength(numSamps);

		for (auto i = 0u; i < numSamps; i++)
		{
			bank[i] = Samples[i];
		}
	}

	bool Matches(const BufferBank& bank)
	{
		auto numSamps = (unsigned int)Samples.size();

		if (bank.Length() != numSamps)
			return false;

		for (auto i = 0u; i < numSamps; i++)
		{
			if (bank[i] != Samples[i])
				return false;
		}

		return true;
	}

private:
	std::vector<float> Samples;
};

TEST(BufferBank, Resizes) {
	BufferBank bank;
	auto initCapacity = bank.Capacity();

	BufferBankSource source(initCapacity + 10);

	source.Fill(bank);

	auto finalCapacity = bank.Capacity();

	ASSERT_TRUE(finalCapacity > initCapacity);
}

TEST(BufferBank, StoresCorrectValues) {
	auto recordSamps = 100;

	BufferBank bank;
	BufferBankSource source(recordSamps);

	source.Fill(bank);
		
	ASSERT_TRUE(source.Matches(bank));
}

TEST(BufferBank, StoresCorrectValuesDuringResize) {
	BufferBank bank;
	auto initCapacity = bank.Capacity();

	BufferBankSource source(initCapacity + 10);

	source.Fill(bank);

	ASSERT_TRUE(source.Matches(bank));
}
