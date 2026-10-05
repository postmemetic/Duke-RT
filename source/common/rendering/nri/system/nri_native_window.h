#pragma once
#include "NRI.h"
#include "Extensions/NRISwapChain.h"

// SDL owns these handles; NRI's swapchain must be destroyed before the window.
bool I_GetNRIWindow(nri::Window& window);
