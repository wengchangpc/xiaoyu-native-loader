// hook.h - DXGI Present hook
#pragma once

#include <dxgi.h>

namespace xiaoyu {
namespace hook {

typedef void (*PresentCallback)(IDXGISwapChain* sc);

bool Install(PresentCallback cb);
void Remove();
IDXGISwapChain* CurrentSwapChain();

}  // namespace hook
}  // namespace xiaoyu
