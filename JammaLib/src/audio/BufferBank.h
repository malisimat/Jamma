#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include "../include/Constants.h"

namespace audio
{
	class BufferBank
	{
	public:
		BufferBank();
		~BufferBank();
		BufferBank(const BufferBank&) = delete;
		BufferBank& operator=(const BufferBank&) = delete;
		BufferBank(BufferBank&& other) noexcept;
		BufferBank& operator=(BufferBank&& other) noexcept;
		void swap(BufferBank& other) noexcept;
		friend void swap(BufferBank& lhs, BufferBank& rhs) noexcept
		{
			lhs.swap(rhs);
		}

	public:
		const float& operator[] (unsigned long index) const;
		float& operator[] (unsigned long index);

		void Init();
		// Audio-callback safe: clamps logical length to current Capacity(), no heap allocation.
		// Length() will never exceed Capacity(). UpdateCapacity() (via Loop::Update()) must be
		// called off-thread to grow capacity and allow SetLength to advance further.
		void SetLength(unsigned long length);
		// Off-thread only: updates logical length and allocates buffer banks as needed.
		// Do NOT call from the audio callback — performs heap allocation.
		void Resize(unsigned long length);
		void UpdateCapacity();
		unsigned long Length() const;
		unsigned long Capacity() const;
		float SubMin(unsigned long i1, unsigned long i2) const;
		float SubMax(unsigned long i1, unsigned long i2) const;
		bool IsBlockContiguous(unsigned long index, unsigned int numSamps) const;
		const float* BlockPtr(unsigned long index) const;

	protected:
		static unsigned int NumBanksToHold(unsigned long length, bool includeCapacityAhead);
	
	public:
		static constexpr unsigned long _BufferBankSize = 1000000ul;
		static constexpr unsigned long _BufferCapacityAhead = 500000ul;
		static constexpr unsigned int _MaxBanks =
			static_cast<unsigned int>(
				(constants::MaxLoopBufferSize + _BufferCapacityAhead + _BufferBankSize - 1ul) /
				_BufferBankSize);

	protected:
		float _dummy;
		std::atomic<unsigned long> _length;
		std::atomic<unsigned int> _numBanks;
		std::array<std::unique_ptr<float[]>, _MaxBanks> _bufferBank;
	};
}
