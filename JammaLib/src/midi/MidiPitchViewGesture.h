#pragma once
#include <algorithm>
#include <cmath>
namespace midi
{
	// Physical deltas are accumulated independently. Up pans higher; right
	// increases rows. Pan uses 12 relative device units/row and zoom uses 8 units/row.
	// Zoom preserves the pitch at the anchored vertical fraction.
	class MidiPitchViewGesture
	{
	public:
		void Begin(int bottom, int rows, double anchorFraction) noexcept
		{
			_startRows = _rows = std::clamp(rows, 12, 128);
			_startBottom = _bottom = std::clamp(bottom, 0, 128 - _rows);
			_anchor = std::clamp(anchorFraction, 0.0, 1.0); _dx = _dy = 0.0;
		}
		void Update(double dx, double dy) noexcept
		{
			if (!std::isfinite(dx) || !std::isfinite(dy)) return;
			_dx = std::clamp(_dx + dx, -1000000.0, 1000000.0);
			_dy = std::clamp(_dy + dy, -1000000.0, 1000000.0);
			_rows = std::clamp(_startRows + static_cast<int>(std::clamp(std::trunc(_dx / 8.0), -128.0, 128.0)), 12, 128);
			_bottom = std::clamp(_startBottom + static_cast<int>(std::round((_startRows - _rows) * _anchor))
				+ static_cast<int>(std::clamp(std::trunc(_dy / 12.0), -128.0, 128.0)), 0, 128 - _rows);
		}
		int Bottom() const noexcept { return _bottom; }
		int Rows() const noexcept { return _rows; }
	private:
		int _startBottom = 0, _bottom = 0, _startRows = 12, _rows = 12;
		double _anchor = 0.0, _dx = 0.0, _dy = 0.0;
	};
}
