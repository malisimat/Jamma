#pragma once

#include <exception>
#include <memory>
#include <string>

namespace base
{
	class Sharable :
		public std::enable_shared_from_this<Sharable>
	{
	public:
		Sharable() {}
		~Sharable() {}

	public:
		std::shared_ptr<Sharable> shared_from_this()
		{
			try
			{
				return std::enable_shared_from_this<Sharable>::shared_from_this();
			}
			catch (const std::bad_weak_ptr&)
			{
				return {};
			}
		}

		virtual std::string ClassName() const {	return "Sharable"; }
	};
}