#include "BufferBank.h"

using namespace audio;

BufferBank::BufferBank() :
	_length(0ul)
{
	Init();
}

BufferBank::~BufferBank()
{
}

void BufferBank::Init()
{
	_length = 0;
	_bufferBank.clear();

	UpdateCapacity();
}

const float& audio::BufferBank::operator[](unsigned long index) const
{
	if (index < Capacity())
	{
		auto bank = index / _BufferBankSize;
		auto offset = index % _BufferBankSize;

		return _bufferBank[bank][offset];
	}

	return _dummy;
}

float& audio::BufferBank::operator[](unsigned long index)
{
	if (index < Capacity())
	{
		auto bank = index / _BufferBankSize;
		auto offset = index % _BufferBankSize;

		return _bufferBank[bank][offset];
	}

	return _dummy;
}

void BufferBank::SetLength(unsigned long length)
{
	auto capacity = Capacity();
	_length = length < capacity ? length : capacity;
}

void BufferBank::Resize(unsigned long length)
{
	_length = length;
	UpdateCapacity();
}

void BufferBank::UpdateCapacity()
{
	int numBanks = NumBanksToHold(_length, true);

	if (numBanks > _bufferBank.size())
	{
		while (numBanks > _bufferBank.size())
			_bufferBank.push_back(std::vector<float>(_BufferBankSize, 0.0f));
	}
	else if (numBanks < _bufferBank.size())
	{
		while (numBanks < _bufferBank.size())
			_bufferBank.pop_back();
	}
}

unsigned long BufferBank::Length() const
{
	return _length;
}

unsigned long BufferBank::Capacity() const
{
	return _BufferBankSize * (unsigned long)_bufferBank.size();
}

float BufferBank::SubMin(unsigned long i1, unsigned long i2) const
{
	auto length = Length();

	if (0 == length)
		return 0.0f;

	i1 = i1 > 0 ? i1 : 0;
	i1 = i1 < length ? i1 : length;
	i2 = i2 < length ? i2 : length;

	if (i2 <= i1)
		return 0.0f;

	auto curMin = (*this)[i1];
	for (auto i = i1; i < i2; i++)
	{
		if ((*this)[i] < curMin)
			curMin = (*this)[i];
	}

	return curMin;
}


float BufferBank::SubMax(unsigned long i1, unsigned long i2) const
{
	auto length = Length();

	if (0 == length)
		return 0.0f;

	i1 = i1 > 0 ? i1 : 0;
	i1 = i1 < length ? i1 : length;
	i2 = i2 < length ? i2 : length;

	if (i2 <= i1)
		return 0.0f;

	auto curMax = (*this)[i1];
	for (auto i = i1; i < i2; i++)
	{
		if ((*this)[i]> curMax)
			curMax = (*this)[i];
	}

	return curMax;
}

unsigned int BufferBank::NumBanksToHold(unsigned long length, bool includeCapacityAhead)
{
	if (includeCapacityAhead)
		length += _BufferCapacityAhead;

	auto numBanks = 1ul + (length / (unsigned long)_BufferBankSize);
	if ((length % (unsigned long)_BufferBankSize) == 0)
		return numBanks > 1 ? numBanks - 1 : 1u;

	return numBanks;
}

bool BufferBank::IsBlockContiguous(unsigned long index, unsigned int numSamps) const
{
	if (numSamps == 0)
		return true;

	if (index + numSamps > Capacity())
		return false;

	auto startBank = index / _BufferBankSize;
	auto endBank = (index + numSamps - 1) / _BufferBankSize;

	return startBank == endBank;
}

const float* BufferBank::BlockPtr(unsigned long index) const
{
	if (index >= Capacity())
		return nullptr;

	auto bank = index / _BufferBankSize;
	auto offset = index % _BufferBankSize;

	return &_bufferBank[bank][offset];
}
