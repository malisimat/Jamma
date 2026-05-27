#include "BufferBank.h"

#include <algorithm>

using namespace audio;

BufferBank::BufferBank() :
	_dummy(0.0f),
	_length(0ul),
	_numBanks(0u)
{
	Init();
}

BufferBank::~BufferBank()
{
}

BufferBank::BufferBank(BufferBank&& other) noexcept :
	_dummy(other._dummy),
	_length(0ul),
	_numBanks(0u)
{
	swap(other);
}

BufferBank& BufferBank::operator=(BufferBank&& other) noexcept
{
	if (this != &other)
		swap(other);

	return *this;
}

void BufferBank::swap(BufferBank& other) noexcept
{
	std::swap(_dummy, other._dummy);

	auto length = _length.load(std::memory_order_relaxed);
	_length.store(other._length.load(std::memory_order_relaxed), std::memory_order_relaxed);
	other._length.store(length, std::memory_order_relaxed);

	auto numBanks = _numBanks.load(std::memory_order_relaxed);
	_numBanks.store(other._numBanks.load(std::memory_order_relaxed), std::memory_order_relaxed);
	other._numBanks.store(numBanks, std::memory_order_relaxed);

	for (auto i = 0u; i < _MaxBanks; ++i)
		std::swap(_bufferBank[i], other._bufferBank[i]);
}

void BufferBank::Init()
{
	_length.store(0ul, std::memory_order_relaxed);
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
	_length.store(length < capacity ? length : capacity, std::memory_order_relaxed);
}

void BufferBank::Resize(unsigned long length)
{
	_length.store(length, std::memory_order_relaxed);
	UpdateCapacity();
}

void BufferBank::UpdateCapacity()
{
	auto numBanks = static_cast<unsigned int>(NumBanksToHold(Length(), true));
	numBanks = std::min(numBanks, _MaxBanks);

	auto currentBanks = _numBanks.load(std::memory_order_acquire);
	while (currentBanks < numBanks)
	{
		if (!_bufferBank[currentBanks])
			_bufferBank[currentBanks] = std::make_unique<float[]>(_BufferBankSize);

		std::fill_n(_bufferBank[currentBanks].get(), _BufferBankSize, 0.0f);
		++currentBanks;
		_numBanks.store(currentBanks, std::memory_order_release);
	}
}

unsigned long BufferBank::Length() const
{
	return _length.load(std::memory_order_relaxed);
}

unsigned long BufferBank::Capacity() const
{
	return _BufferBankSize * static_cast<unsigned long>(_numBanks.load(std::memory_order_acquire));
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
