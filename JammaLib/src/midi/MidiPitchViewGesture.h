#pragma once
#include <algorithm>
#include <cmath>
namespace midi
{
	// Horizontal pixel displacement zooms; the current projected grid fraction
	// keeps the pitch captured on press beneath the cursor, within row/range limits.
	class MidiPitchViewGesture
	{
	public:
		void Begin(int bottom, int rows, double anchorFraction) noexcept
		{
			_startRows = _rows = std::clamp(rows, 12, 128);
			_bottom = std::clamp(bottom, 0, 128 - _rows);
			_anchorPitch = _bottom + std::clamp(anchorFraction, 0.0, 1.0) * _rows;
		}
		void Update(double horizontalPixels, double pointerFraction) noexcept
		{
			if (!std::isfinite(horizontalPixels) || !std::isfinite(pointerFraction)) return;
			_rows = std::clamp(_startRows + static_cast<int>(std::clamp(
				std::trunc(horizontalPixels / 8.0), -128.0, 128.0)), 12, 128);
			_bottom = static_cast<int>(std::round(std::clamp(
				_anchorPitch - pointerFraction * _rows, 0.0, static_cast<double>(128 - _rows))));
		}
		int Bottom() const noexcept { return _bottom; }
		int Rows() const noexcept { return _rows; }
	private:
		int _bottom = 0, _startRows = 12, _rows = 12;
		double _anchorPitch = 0.0;
	};
}
