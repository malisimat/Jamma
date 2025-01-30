#pragma once

namespace utils
{
	struct Position2d
	{
		int X;
		int Y;

		Position2d operator+(const Position2d& a) const
		{
			return { X + a.X, Y + a.Y };
		}

		Position2d operator-(const Position2d& a) const
		{
			return { X - a.X, Y - a.Y };
		}

		Position2d operator*(const Position2d& a) const
		{
			return { X * a.X, Y * a.Y };
		}

		Position2d& operator+=(const Position2d& a)
		{
			X += a.X;
			Y += a.Y;
			return *this;
		}

		Position2d& operator-=(const Position2d& a)
		{
			X -= a.X;
			Y -= a.Y;
			return *this;
		}

		Position2d& operator*=(const Position2d& a)
		{
			X *= a.X;
			Y *= a.Y;
			return *this;
		}

		bool operator==(const Position2d& a) const
		{
			return (a.X == X && a.Y == Y);
		}

		static Position2d Clamp(Position2d v, Position2d vMin, Position2d vMax)
		{
			return Position2d{
				v.X < vMin.X ? vMin.X : v.X > vMax.X ? vMax.X : v.X,
				v.Y < vMin.Y ? vMin.Y : v.Y > vMax.Y ? vMax.Y : v.Y
			};
		}
	};

	struct Position3d
	{
		float X;
		float Y;
		float Z;

		Position3d operator+(const Position3d& a) const
		{
			return { X + a.X, Y + a.Y, Z + a.Z };
		}

		Position3d operator-(const Position3d& a) const
		{
			return { X - a.X, Y - a.Y, Z - a.Z };
		}

		Position3d operator*(const Position3d& a) const
		{
			return { X * a.X, Y * a.Y, Z * a.Z };
		}

		Position3d& operator+=(const Position3d& a)
		{
			X += a.X;
			Y += a.Y;
			Z += a.Z;
			return *this;
		}

		Position3d& operator-=(const Position3d& a)
		{
			X -= a.X;
			Y -= a.Y;
			Z -= a.Z;
			return *this;
		}

		Position3d& operator*=(const Position3d& a)
		{
			X *= a.X;
			Y *= a.Y;
			Z *= a.Z;
			return *this;
		}

		bool operator==(const Position3d& a) const
		{
			return (a.X == X && a.Y == Y && a.Z == Z);
		}

		static Position3d Clamp(Position3d v, Position3d vMin, Position3d vMax)
		{
			return Position3d{
				v.X < vMin.X ? vMin.X : v.X > vMax.X ? vMax.X : v.X,
				v.Y < vMin.Y ? vMin.Y : v.Y > vMax.Y ? vMax.Y : v.Y,
				v.Z < vMin.Z ? vMin.Z : v.Z > vMax.Z ? vMax.Z : v.Z
			};
		}
	};

	struct Size2d
	{
		unsigned int Width;
		unsigned int Height;

		Size2d operator+(const Size2d& a) const
		{
			return { Width + a.Width, Height + a.Height };
		}

		Size2d operator-(const Size2d& a) const
		{
			auto w = Width >= a.Width ? (Width - a.Width) : 0;
			auto h = Height >= a.Height ? (Height - a.Height) : 0;

			return { w, h };
		}

		Size2d operator*(const Size2d& a) const
		{
			return { Width * a.Width, Height * a.Height };
		}

		Size2d& operator+=(const Size2d& a)
		{
			Width += a.Width;
			Height += a.Height;
			return *this;
		}

		Size2d& operator-=(const Size2d& a)
		{
			Width = Width <= a.Width ? (Width - a.Width) : 0;
			Height = Height <= a.Height ? (Height - a.Height) : 0;
			return *this;
		}

		Size2d& operator*=(const Size2d& a)
		{
			Width *= a.Width;
			Height *= a.Height;
			return *this;
		}

		bool operator==(const Size2d& a) const
		{
			return (a.Width == Width && a.Height == Height);
		}

		static Size2d Clamp(Size2d v, Size2d vMin, Size2d vMax)
		{
			auto newSize = v;

			if (v.Width < vMin.Width)
				newSize.Width = vMin.Width;
			if (v.Width > vMax.Width)
				newSize.Width = vMax.Width;
			if (v.Height < vMin.Height)
				newSize.Height = vMin.Height;
			if (v.Height > vMax.Height)
				newSize.Height = vMax.Height;

			return newSize;
		}

		static bool RectTest(Size2d rect, Position2d pos)
		{
			if (pos.X > 0 && pos.X < (int)rect.Width)
				return (pos.Y > 0 && pos.Y < (int)rect.Height);

			return false;
		}
	};
}
