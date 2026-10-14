#pragma once

#include <memory>
#include <optional>
#include "GuiPopup.h"
#include "GuiPopupManager.h"
#include "../ninjam/NinjamTempoProposal.h"

namespace gui
{
	// UI-thread owned. Consumes proposal values, never network or timing services.
	class NinjamTempoPrompt
	{
	public:
		NinjamTempoPrompt();
		void Synchronize(const std::optional<ninjam::NinjamTempoProposal>& proposal,
			GuiPopupManager& manager, utils::Size2d viewport);
		std::optional<ninjam::NinjamTempoDecision> TakeDecision(GuiPopupManager& manager);
		void InitResources(resources::ResourceLib& resources, bool forceInit);
		void ReleaseResources();
		void FitToViewport(utils::Size2d viewport);

	private:
		void _Close(GuiPopupManager& manager);
		class ButtonReceiver;
		static constexpr unsigned int AcceptControlIndex = 7101u;
		static constexpr unsigned int RejectControlIndex = 7102u;
		std::shared_ptr<GuiPopup> _popup;
		std::shared_ptr<ButtonReceiver> _receiver;
	};
}
