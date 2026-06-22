#pragma once

#include <atomic>
#include <string>
#include "Sharable.h"
#include "../resources/Resource.h"
#include "../resources/ResourceLib.h"

namespace base
{
	class ResourceUser
	{
	public:
		ResourceUser() :
			_resourcesInitialised(false),
			_resourcesNeedInitialising(false)
		{ }

	public:
		virtual void InitResources(resources::ResourceLib& resourceLib, bool forceInit)
		{
			if (forceInit || _resourcesNeedInitialising.load(std::memory_order_acquire))
			{
				ReleaseResources();

				_InitResources(resourceLib, true);
				_resourcesInitialised = true;
				_resourcesNeedInitialising.store(false, std::memory_order_release);
			}
			else
			{
				if (!_resourcesInitialised)
					_InitResources(resourceLib, false);

				_resourcesInitialised = true;
			}
		};

		void ReleaseResources()
		{
			if (_resourcesInitialised)
				_ReleaseResources();

			_resourcesInitialised = false;
		};

	protected:
		virtual void _InitResources(resources::ResourceLib& resourceLib, bool forceInit) { };
		virtual void _ReleaseResources() { };

		// Written by any thread (e.g. SetGeometry / SetInstanceAttributes from
		// the data/audio side); read by the render thread.  Atomic so the flag
		// flip is always visible without a mutex round-trip.
		std::atomic<bool> _resourcesNeedInitialising;

	private:
		bool _resourcesInitialised;
	};
}
