// render.cpp - Direct2D 覆盖层（自带 D3D11 设备 + DirectComposition 合成交换链）
//
// 为什么用 D2D：
//   1. 中文文字直接用 DirectWrite，不用做字形图集；
//   2. 圆角矩形 / 抗锯齿原生支持，跟原版面板的视觉一致。
//
// 为什么用 DirectComposition 而不是普通窗口 + UpdateLayeredWindow：
//   DComp 走的是 DWM 的合成层，每帧只是把交换链内容交给合成器，不需要
//   把整个 DIB 拷一遍（UpdateLayeredWindow 在 1080p 下每帧要拷 8MB）。
//   而且 DComp 的 visual 是 GPU 合成的，帧率和游戏一致，不会有撕裂感。
//
// 合成交换链的硬性要求（错了就 CreateSwapChainForComposition 失败）：
//   - swapEffect 只能是 FLIP_SEQUENTIAL 或 FLIP_DISCARD（不能用 DISCARD）
//   - alphaMode 用 PREMULTIPLIED，否则透明区域会是黑的
//   - 窗口必须带 WS_EX_NOREDIRECTIONBITMAP，否则窗口本身会刷一层背景色
#include "render.h"

#include <d2d1helper.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <map>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace xiaoyu {
namespace render {
namespace {

ComPtr<ID3D11Device> g_d3d;
ComPtr<ID3D11DeviceContext> g_d3d_ctx;
ComPtr<IDXGISwapChain1> g_swap;
ComPtr<IDCompositionDevice> g_comp;
ComPtr<IDCompositionTarget> g_comp_target;
ComPtr<IDCompositionVisual> g_comp_visual;

ComPtr<ID2D1Factory1> g_factory;
ComPtr<ID2D1Device> g_d2d_device;
ComPtr<ID2D1DeviceContext> g_ctx;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<ID2D1SolidColorBrush> g_brush;
ComPtr<ID2D1Bitmap1> g_target;

HWND g_hwnd = nullptr;
int g_width = 0;
int g_height = 0;
int g_clip_depth = 0;
float g_dpi = 96.0f;

// (size*10, bold, align) -> format
std::map<int, IDWriteTextFormat*> g_formats;

IDWriteTextFormat* GetFormat(float size, bool bold, int align) {
    int key = ((int)(size * 10.0f) << 8) | ((bold ? 1 : 0) << 4) | (align & 0xF);
    auto it = g_formats.find(key);
    if (it != g_formats.end()) return it->second;

    wchar_t family[64] = L"Microsoft YaHei";
    // 系统没有雅黑就退回
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\msyh.ttc") == INVALID_FILE_ATTRIBUTES) {
        wcscpy_s(family, L"Microsoft Sans Serif");
    }

    IDWriteTextFormat* fmt = nullptr;
    HRESULT hr = g_dwrite->CreateTextFormat(
        family, nullptr,
        bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        size, L"zh-cn", &fmt);
    if (FAILED(hr) || fmt == nullptr) {
        hr = g_dwrite->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &fmt);
        if (FAILED(hr) || fmt == nullptr) return nullptr;
    }
    fmt->SetTextAlignment(align == ALIGN_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER
                          : align == ALIGN_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                 : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_formats[key] = fmt;
    return fmt;
}

// 建（或重建）合成交换链。Resize 也走这里。
bool CreateSwapChain(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    // 先把旧的放掉，否则 ResizeBuffers 会因为还有引用而失败
    if (g_ctx != nullptr) g_ctx->SetTarget(nullptr);
    g_target.Reset();

    if (g_swap != nullptr && g_width == w && g_height == h) {
        return true;  // 尺寸没变，不用重建
    }

    if (g_swap != nullptr) {
        HRESULT hr = g_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            LogLine("[d2d] ResizeBuffers failed hr=0x%08lX, 整个重建", (unsigned long)hr);
            g_swap.Reset();
        }
    }

    if (g_swap == nullptr) {
        ComPtr<IDXGIDevice> dxgi_device;
        if (FAILED(g_d3d.As(&dxgi_device))) {
            LogLine("[d2d] QI IDXGIDevice failed");
            return false;
        }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgi_device->GetAdapter(&adapter))) {
            LogLine("[d2d] GetAdapter failed");
            return false;
        }
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                      reinterpret_cast<void**>(factory.GetAddressOf())))) {
            LogLine("[d2d] GetParent(IDXGIFactory2) failed");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = (UINT)w;
        sd.Height = (UINT)h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.Stereo = FALSE;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        // 合成交换链只接受 flip 模型；DISCARD 会直接 DXGI_ERROR_INVALID_CALL
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        // 预乘 alpha：D2D 的输出就是这个格式，透明区域才能真透明
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        sd.Flags = 0;

        IDXGISwapChain1* raw = nullptr;
        HRESULT hr = factory->CreateSwapChainForComposition(g_d3d.Get(), &sd, nullptr, &raw);
        if (FAILED(hr) || raw == nullptr) {
            LogLine("[d2d] CreateSwapChainForComposition failed hr=0x%08lX (%dx%d)",
                    (unsigned long)hr, w, h);
            return false;
        }
        g_swap.Attach(raw);

        // 视觉树只在第一次建的时候搭好；之后 Resize 只重建 buffer
        if (g_comp == nullptr) {
            hr = DCompositionCreateDevice(dxgi_device.Get(), __uuidof(IDCompositionDevice),
                                          reinterpret_cast<void**>(g_comp.GetAddressOf()));
            if (FAILED(hr) || g_comp == nullptr) {
                LogLine("[dcomp] CreateDevice failed hr=0x%08lX", (unsigned long)hr);
                return false;
            }
            hr = g_comp->CreateTargetForHwnd(g_hwnd, TRUE, &g_comp_target);
            if (FAILED(hr) || g_comp_target == nullptr) {
                LogLine("[dcomp] CreateTargetForHwnd failed hr=0x%08lX", (unsigned long)hr);
                return false;
            }
            hr = g_comp->CreateVisual(&g_comp_visual);
            if (FAILED(hr) || g_comp_visual == nullptr) {
                LogLine("[dcomp] CreateVisual failed hr=0x%08lX", (unsigned long)hr);
                return false;
            }
            hr = g_comp_visual->SetContent(g_swap.Get());
            if (FAILED(hr)) {
                LogLine("[dcomp] SetContent failed hr=0x%08lX", (unsigned long)hr);
                return false;
            }
            hr = g_comp_target->SetRoot(g_comp_visual.Get());
            if (FAILED(hr)) {
                LogLine("[dcomp] SetRoot failed hr=0x%08lX", (unsigned long)hr);
                return false;
            }
            g_comp->Commit();
        } else {
            // 交换链重建了，visual 的内容要重新指向
            g_comp_visual->SetContent(g_swap.Get());
            g_comp->Commit();
        }
    }

    g_width = w;
    g_height = h;
    return true;
}

// 把当前 backbuffer 包成 D2D 目标。flip 模型每帧 backbuffer 会换，
// 所以这个必须每帧重做，不能缓存。
bool BindTarget() {
    if (g_swap == nullptr || g_ctx == nullptr) return false;

    IDXGISurface* raw = nullptr;
    HRESULT hr = g_swap->GetBuffer(0, __uuidof(IDXGISurface),
                                   reinterpret_cast<void**>(&raw));
    if (FAILED(hr) || raw == nullptr) {
        LogLine("[d2d] GetBuffer failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    ComPtr<IDXGISurface> surface;
    surface.Attach(raw);

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        g_dpi, g_dpi);

    ComPtr<ID2D1Bitmap1> bmp;
    hr = g_ctx->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bmp);
    if (FAILED(hr) || bmp == nullptr) {
        LogLine("[d2d] CreateBitmapFromDxgiSurface failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    g_target = bmp;
    g_ctx->SetTarget(g_target.Get());
    g_ctx->SetDpi(g_dpi, g_dpi);
    g_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

}  // namespace

bool Init(HWND hwnd, int w, int h) {
    if (hwnd == nullptr) return false;
    if (g_ctx != nullptr) return true;
    g_hwnd = hwnd;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;  // D2D 必须有 BGRA 支持
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION, &g_d3d, &level,
                                   &g_d3d_ctx);
    if (FAILED(hr) || g_d3d == nullptr) {
        // 硬件不行就退 WARP，总比不显示强
        LogLine("[d3d] hardware device failed hr=0x%08lX, 退 WARP", (unsigned long)hr);
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0,
                               D3D11_SDK_VERSION, &g_d3d, &level, &g_d3d_ctx);
        if (FAILED(hr) || g_d3d == nullptr) {
            LogLine("[d3d] WARP device failed hr=0x%08lX", (unsigned long)hr);
            return false;
        }
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(g_d3d.As(&dxgi_device))) {
        LogLine("[d3d] QI IDXGIDevice failed");
        return false;
    }

    if (!CreateSwapChain(w, h)) return false;

    D2D1_FACTORY_OPTIONS opts = {};
    ID2D1Factory1* raw_factory = nullptr;
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                           &opts, reinterpret_cast<void**>(&raw_factory));
    if (FAILED(hr) || raw_factory == nullptr) {
        LogLine("[d2d] D2D1CreateFactory failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    g_factory.Attach(raw_factory);

    hr = g_factory->CreateDevice(dxgi_device.Get(), &g_d2d_device);
    if (FAILED(hr) || g_d2d_device == nullptr) {
        LogLine("[d2d] CreateDevice failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_ctx);
    if (FAILED(hr) || g_ctx == nullptr) {
        LogLine("[d2d] CreateDeviceContext failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    g_ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    hr = g_ctx->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_brush);
    if (FAILED(hr) || g_brush == nullptr) {
        LogLine("[d2d] CreateSolidColorBrush failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(g_dwrite.GetAddressOf()));
    if (FAILED(hr) || g_dwrite == nullptr) {
        LogLine("[dwrite] CreateFactory failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    LogLine("[d2d] ready %dx%d fmt=BGRA(premul) swapchain=composition", g_width, g_height);
    return true;
}

void Shutdown() {
    if (g_ctx != nullptr) g_ctx->SetTarget(nullptr);
    g_target.Reset();
    for (auto& kv : g_formats) {
        if (kv.second != nullptr) kv.second->Release();
    }
    g_formats.clear();
    g_brush.Reset();
    g_ctx.Reset();
    g_d2d_device.Reset();
    g_factory.Reset();
    g_dwrite.Reset();
    g_comp_visual.Reset();
    g_comp_target.Reset();
    g_comp.Reset();
    g_swap.Reset();
    g_d3d_ctx.Reset();
    g_d3d.Reset();
    g_hwnd = nullptr;
    g_width = g_height = 0;
}

void Resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == g_width && h == g_height) return;
    if (!CreateSwapChain(w, h)) {
        LogLine("[d2d] Resize 到 %dx%d 失败", w, h);
    } else {
        LogLine("[d2d] resized to %dx%d", w, h);
    }
}

bool BeginFrame() {
    if (g_ctx == nullptr || g_brush == nullptr) return false;
    if (!BindTarget()) return false;

    // 清成全透明：DComp 会按 alpha 把游戏画面露出来
    g_ctx->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    g_clip_depth = 0;
    g_ctx->BeginDraw();
    g_ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    return true;
}

void EndFrame() {
    if (g_ctx == nullptr) return;
    while (g_clip_depth > 0) {
        g_ctx->PopAxisAlignedClip();
        --g_clip_depth;
    }
    HRESULT hr = g_ctx->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        LogLine("[d2d] D2DERR_RECREATE_TARGET");
        g_ctx->SetTarget(nullptr);
        g_target.Reset();
        return;
    }
    if (g_swap == nullptr) return;

    hr = g_swap->Present(0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LogLine("[d2d] 设备丢失 hr=0x%08lX，下次重建", (unsigned long)hr);
        g_ctx->SetTarget(nullptr);
        g_target.Reset();
        g_swap.Reset();
        g_width = g_height = 0;
    }
}

int Width() { return g_width; }
int Height() { return g_height; }

void Rect(float x, float y, float w, float h, const D2D1_COLOR_F& c) {
    // g_brush 为空时直接 return：ComPtr 的 operator-> 不做判空，空指针会直接 AV
    if (g_ctx == nullptr || g_brush == nullptr || w <= 0.0f || h <= 0.0f) return;
    g_brush->SetColor(c);
    g_ctx->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_brush.Get());
}

void RoundRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& c) {
    if (g_ctx == nullptr || g_brush == nullptr || w <= 0.0f || h <= 0.0f) return;
    float rr = r;
    if (rr > w * 0.5f) rr = w * 0.5f;
    if (rr > h * 0.5f) rr = h * 0.5f;
    g_brush->SetColor(c);
    g_ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rr, rr),
                                g_brush.Get());
}

void Line(float x1, float y1, float x2, float y2, float width, const D2D1_COLOR_F& c) {
    if (g_ctx == nullptr || g_brush == nullptr) return;
    g_brush->SetColor(c);
    g_ctx->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush.Get(), width);
}

void Text(const wchar_t* s, float x, float y, float w, float h, float size,
          const D2D1_COLOR_F& c, int align, bool bold) {
    if (g_ctx == nullptr || g_brush == nullptr || s == nullptr || *s == L'\0') return;
    IDWriteTextFormat* fmt = GetFormat(size, bold, align);
    if (fmt == nullptr) return;
    g_brush->SetColor(c);
    g_ctx->DrawTextW(s, (UINT32)wcslen(s), fmt, D2D1::RectF(x, y, x + w, y + h),
                     g_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                     DWRITE_MEASURING_MODE_NATURAL);
}

float Measure(const wchar_t* s, float size, bool bold) {
    if (g_dwrite == nullptr || s == nullptr || *s == L'\0') return 0.0f;
    IDWriteTextFormat* fmt = GetFormat(size, bold, ALIGN_LEFT);
    if (fmt == nullptr) return 0.0f;

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = g_dwrite->CreateTextLayout(s, (UINT32)wcslen(s), fmt, 4096.0f, 100.0f,
                                            &layout);
    if (FAILED(hr) || layout == nullptr) return 0.0f;
    DWRITE_TEXT_METRICS m = {};
    if (FAILED(layout->GetMetrics(&m))) return 0.0f;
    return m.widthIncludingTrailingWhitespace;
}

void PushClip(float x, float y, float w, float h) {
    if (g_ctx == nullptr) return;
    g_ctx->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h),
                               D2D1_ANTIALIAS_MODE_ALIASED);
    ++g_clip_depth;
}

void PopClip() {
    if (g_ctx == nullptr || g_clip_depth <= 0) return;
    g_ctx->PopAxisAlignedClip();
    --g_clip_depth;
}

}  // namespace render
}  // namespace xiaoyu
