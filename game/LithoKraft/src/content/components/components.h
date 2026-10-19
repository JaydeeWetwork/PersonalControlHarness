// Private to lithokraft_content: registration hooks for the built-in block behaviours.
#pragma once
#include <functional>
#include <memory>
#include <string_view>
#include "lithokraft/content/component.h"

namespace lithokraft::components {

using Registrar = std::function<void(std::string_view name, std::unique_ptr<BlockBehavior>)>;

void registerPassive(const Registrar& add);  // wire, torch, lamp, color lamp (readout only)
void registerLogic(const Registrar& add);    // repeater, thresholder, redirector, pulsor, memory cell, fast latch
void registerInputs(const Registrar& add);   // switch, button, keyboard
void registerSound(const Registrar& add);    // tone block
void registerCertificateTable(const Registrar& add);  // certificate table (describe-only seam; signing mechanic game-side, B7)
void registerEtchOR(const Registrar& add);            // EtchOR (describe-only seam; etch flow game-side, B6)

}  // namespace lithokraft::components
