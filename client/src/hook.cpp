// hook.cpp - 通过 IDXGISwapChain vtable 挂钩 Present
//
// 为什么用 vtable 而不是 inline hook：
//   主程序是 Themida 加壳的，代码段在磁盘上是加密的，按字节特征做 inline hook
//   无从下手；而 vtable 是运行时对象里的函数指针表，改它不需要碰被保护的代码段。
//   Present 在所有 IDXGISwapChainN 版本里都是第 8 项，位置稳定。
#include "hook.h"

#include <d3d11.h>
#include <windows.h>

#include "log.h"

namespace xiaoyu {

// 由 overlay.cpp 实现：Resize 前必须放掉 backbuffer 引用
void OnSwapChainResize();

namespace hook {
namespace {

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain*, UINT, UINT,
                                               const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT,
                                                    DXGI_FORMAT, UINT);

PresentFn g_orig_present = nullptr;
Present1Fn g_orig_present1 = nullptr;
ResizeBuffersFn g_orig_resize = nullptr;

void** g_vtbl = nullptr;
ULONG_PTR g_page_size = 0;
PresentCallback g_cb = nullptr;
IDXGISwapChain* g_active = nullptr;

LRESULT CALLBACK DummyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

// 让 vtable 所在页可写，改完再恢复
bool PatchEntry(int index, void* fn, void** old) {
    if (g_vtbl == nullptr) return false;
    if (old != nullptr) *old = g_vtbl[index];
    DWORD old_protect = 0;
    void** slot = g_vtbl + index;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) {
        LogLine("[hook] VirtualProtect failed err=%lu", (unsigned long)GetLastError());
        return false;
    }
    g_vtbl[index] = fn;
    VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
    return true;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    g_active = sc;
    if (g_cb != nullptr) {
        __try {
            g_cb(sc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // 覆盖层自己崩了不能连累游戏
        }
    }
    if (g_orig_present == nullptr) return E_FAIL;
    return g_orig_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookedPresent1(IDXGISwapChain* sc, UINT sync, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* params) {
    g_active = sc;
    if (g_cb != nullptr) {
        __try {
            g_cb(sc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (g_orig_present1 == nullptr) return E_FAIL;
    return g_orig_present1(sc, sync, flags, params);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w,
                                              UINT h, DXGI_FORMAT fmt, UINT flags) {
    // 必须先释放 backbuffer 的引用，否则 ResizeBuffers 会返回 DXGI_ERROR_INVALID_CALL
    xiaoyu::OnSwapChainResize();
    if (g_orig_resize == nullptr) return E_FAIL;
    return g_orig_resize(sc, count, w, h, fmt, flags);
}

}  // namespace

bool Install(PresentCallback cb) {
    if (g_vtbl != nullptr) {
        g_cb = cb;
        return true;
    }
    g_cb = cb;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DummyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"XiaoyuDummyWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"xiaoyu", WS_OVERLAPPEDWINDOW, 0, 0,
                                256, 256, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        LogLine("[hook] CreateWindow failed err=%lu", (unsigned long)GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 256;
    sd.BufferDesc.Height = 256;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd,
        &sc, &dev, &level, &ctx);
    if (FAILED(hr) || sc == nullptr) {
        LogLine("[hook] D3D11CreateDeviceAndSwapChain failed hr=0x%08lX",
                (unsigned long)hr);
        DestroyWindow(hwnd);
        return false;
    }

    g_vtbl = *reinterpret_cast<void***>(sc);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_page_size = si.dwPageSize;

    void* old_present = nullptr;
    if (!PatchEntry(8, reinterpret_cast<void*>(&HookedPresent), &old_present)) {
        LogLine("[hook] patch Present failed");
        return false;
    }
    g_orig_present = reinterpret_cast<PresentFn>(old_present);

    // Present1 在 IDXGISwapChain1 的 vtable 里是第 22 项；能拿到就一起挂上
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1),
                                     reinterpret_cast<void**>(&sc1))) &&
        sc1 != nullptr) {
        void* old_p1 = nullptr;
        if (PatchEntry(22, reinterpret_cast<void*>(&HookedPresent1), &old_p1)) {
            g_orig_present1 = reinterpret_cast<Present1Fn>(old_p1);
        }
        sc1->Release();
    }

    void* old_resize = nullptr;
    if (PatchEntry(13, reinterpret_cast<void*>(&HookedResizeBuffers), &old_resize)) {
        g_orig_resize = reinterpret_cast<ResizeBuffersFn>(old_resize);
    }

    sc->Release();
    if (ctx != nullptr) ctx->Release();
    if (dev != nullptr) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    LogLine("[hook] installed. Present=%p Present1=%p ResizeBuffers=%p",
            (void*)g_orig_present, (void*)g_orig_present1, (void*)g_orig_resize);
    return true;
}

void Remove() {
    if (g_vtbl == nullptr) return;
    PatchEntry(8, reinterpret_cast<void*>(g_orig_present), nullptr);
    if (g_orig_present1 != nullptr) {
        PatchEntry(22, reinterpret_cast<void*>(g_orig_present1), nullptr);
    }
    if (g_orig_resize != nullptr) {
        PatchEntry(13, reinterpret_cast<void*>(g_orig_resize), nullptr);
    }
    g_vtbl = nullptr;
}

IDXGISwapChain* CurrentSwapChain() { return g_active; }

}  // namespace hook
}  // namespace xiaoyu
