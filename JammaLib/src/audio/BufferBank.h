#pragma once

#include <vector>

namespace audio
{
	class BufferBank
	{
	public:
		BufferBank();
		~BufferBank();

	public:
		const float& operator[] (unsigned long index) const;
		float& operator[] (unsigned long index);

		void Init();
		// Audio-callback safe: clamps logical length to current Capacity(), no heap allocation.
		// Length() will never exceed Capacity(). UpdateCapacity() (via Loop::Update()) must be
		// called off-thread to grow capacity and allow SetLength to advance further.
		void SetLength(unsigned long length);
		// Off-thread only: updates logical length and allocates/frees buffer banks as needed.
		// Do NOT call from the audio callback — performs heap allocation.
		void Resize(unsigned long length);
		void UpdateCapacity();
		unsigned long Length() const;
		unsigned long Capacity() const;
		float SubMin(unsigned long i1, unsigned long i2) const;
		float SubMax(unsigned long i1, unsigned long i2) const;

	protected:
		static unsigned int NumBanksToHold(unsigned long length, bool includeCapacityAhead);
	
	public:
		static const unsigned int _BufferBankSize = 1000000u;
		static const unsigned int _BufferCapacityAhead = 500000u;

	protected:
		float _dummy;
		unsigned int _length;
		std::vector<std::vector<float>> _bufferBank;
	};
}
