#pragma once

// Calamansi Juice 2 addition (not present in upstream lemonade-sdk/lemonade):
// a live-updating resource monitor, architecturally modeled on (not copied
// from) the amd-ai-max-dashboard reference project's backend/collectors.py
// and backend/inference_status.py. See docs/calamansi/resources.md and
// MERGING.md.
//
// Distinct from the System State Viewer (system_state.h): that's a cached,
// on-demand snapshot; this is meant to be polled every ~2-5s by the GUI, so
// every collector here is cheap and every subprocess call uses a short
// (2s) timeout so a hung CLI can never hang a poll.

#include <nlohmann/json.hpp>

namespace lemon {

// CPU/memory/GPU/NPU/disk/sensors - meant to be polled every ~2s. Each
// top-level key is independently try/caught so one failing collector never
// blanks the rest of the response (mirrors collectors.py's per-function
// try/except -> {"error": ...} convention).
nlohmann::json get_resources_system();

// Other local inference services detected running alongside this app (LM
// Studio, standalone llama-server, vLLM, FastFlowLM) - deliberately NOT
// this app itself, since that's redundant with the app's own state.
// Shells out more than get_resources_system() (process enumeration +
// per-service HTTP probes), so meant to be polled slower (~4-5s).
nlohmann::json get_resources_inference();

} // namespace lemon
