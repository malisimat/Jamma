#pragma once

#include "engine/Station.h"

inline void AddTestRigTrigger(const std::shared_ptr<engine::Station>& station,
	std::shared_ptr<engine::Trigger> trigger)
{
	auto current = station->TriggerMembershipSnapshot();
	auto membership = current ? *current : engine::Station::TriggerMembership{};
	trigger->SetReceiver(station);
	membership.push_back(std::move(trigger));
	station->PublishTriggerMembership(
		std::make_shared<const engine::Station::TriggerMembership>(std::move(membership)));
}
