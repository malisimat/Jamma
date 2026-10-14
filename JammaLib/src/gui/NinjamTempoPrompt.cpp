#include "NinjamTempoPrompt.h"
#include <iomanip>
#include <sstream>
#include <utility>

using namespace gui;
using namespace ninjam;

class NinjamTempoPrompt::ButtonReceiver : public base::ActionReceiver
{
public:
	std::optional<NinjamTempoProposalId> Displayed;
	std::optional<NinjamTempoDecision> Decision;

	actions::ActionResult OnAction(actions::GuiAction action) override
	{
		if (Displayed && action.ElementType == actions::GuiAction::ACTIONELEMENT_BUTTON
			&& (action.Index == AcceptControlIndex || action.Index == RejectControlIndex))
			Decision = NinjamTempoDecision{ *Displayed, action.Index == AcceptControlIndex
				? NinjamTempoChoice::FollowServer : NinjamTempoChoice::StayLocal };
		return actions::ActionResult::NoAction();
	}
};

NinjamTempoPrompt::NinjamTempoPrompt() :
	_popup(std::make_shared<GuiPopup>(GuiPopupParams::PanelDefault())),
	_receiver(std::make_shared<ButtonReceiver>())
{
	_popup->SetTitle("Current server tempo");
	_popup->ConfigureButtons({ { { "Cancel", RejectControlIndex }, { "Follow server", AcceptControlIndex } }, true });
	_popup->SetButtonReceiver(_receiver);
	_popup->Init();
}

void NinjamTempoPrompt::Synchronize(const std::optional<NinjamTempoProposal>& proposal,
	GuiPopupManager& manager, utils::Size2d viewport)
{
	if ((!proposal && !_receiver->Displayed)
		|| (proposal && _receiver->Displayed == proposal->Id)) return;
	_Close(manager); // Replacement is cleanup, not a user rejection.
	if (!proposal) return;
	const auto& change = proposal->Change;
	std::ostringstream bpm;
	bpm << std::fixed << std::setprecision(1) << change.Bpm;
	_popup->SetBodyLines({
		"Tempo: " + bpm.str() + " BPM, " + std::to_string(change.Bpi) + " BPI",
		"Remote master interval: " + std::to_string(change.RemoteMasterIntervalLengthSamps) + " samples",
		"Remote grid step: " + std::to_string(change.RemoteGridStepSamps) + " samples. Apply locally?"
	});
	_popup->FitToViewport(viewport);
	_receiver->Displayed = proposal->Id;
	manager.Open(_popup, _popup);
}

std::optional<NinjamTempoDecision> NinjamTempoPrompt::TakeDecision(GuiPopupManager& manager)
{
	auto decision = std::exchange(_receiver->Decision, std::nullopt);
	if (!decision && _receiver->Displayed && !manager.Contains(_popup))
		decision = NinjamTempoDecision{ *_receiver->Displayed, NinjamTempoChoice::StayLocal };
	if (decision) _Close(manager);
	return decision;
}

void NinjamTempoPrompt::_Close(GuiPopupManager& manager)
{
	manager.CloseOwnedBy(_popup);
	_popup->ClearPointerState();
	_receiver->Displayed.reset();
	_receiver->Decision.reset();
}

void NinjamTempoPrompt::InitResources(resources::ResourceLib& resources, bool forceInit)
{
	_popup->InitResources(resources, forceInit);
}

void NinjamTempoPrompt::ReleaseResources() { _popup->ReleaseResources(); }
void NinjamTempoPrompt::FitToViewport(utils::Size2d viewport) { _popup->FitToViewport(viewport); }
