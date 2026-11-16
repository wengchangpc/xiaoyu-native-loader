// render.cpp - Direct2D 覆盖层
//
// 为什么用 D2D 而不是自己写顶点渲染器：
//   1. 中文文字直接用 DirectWrite，不用做字形图集；
//   2. 圆角矩形 / 抗锯齿原生支持，跟原版面板的视觉一致；
//   3. 直接画在交换链 backbuffer 上，不需要额外 RT。
#include "render.h"

#include <d2d1helper.h>
#include <wrl/client.h>

#include <map>
#include <string>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace xiaoyu {
namespace render {
namespace {

ComPtr<ID2D1Factory1> g_factory;
ComPtr<ID2D1Device> g_d2d_device;
ComPtr<ID2D1DeviceContext> g_ctx;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<ID2D1SolidColorBrush> g_brush;
ComPtr<ID2D1Bitmap1> g_target;

IDXGISwapChain* g_sc = nullptr;
DXGI_FORMAT g_format = DXGI_FORMAT_B8G8R8A8_UNORM;
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
    // 系统没有雅黑就退回等线 / 宋体
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
        // 再退一步：不指定字体族
        hr = g_dwrite->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-cn", &fmt);
        if (FAILED(hr)) return nullptr;
    }
    fmt->SetTextAlignment(align == ALIGN_CENTER ? DWRITE_TEXT_ALIGNMENT_CENTER
                          : align == ALIGN_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                 : DWRITE_TEXT_ALIGNMENT_LEADING);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_formats[key] = fmt;
    return fmt;
}

bool CreateTarget(IDXGISwapChain* sc) {
    if (sc == nullptr || g_ctx == nullptr) return false;

    IDXGISurface* surf = nullptr;
    HRESULT hr = sc->GetBuffer(0, __uuidof(IDXGISurface),
                               reinterpret_cast<void**>(&surf));
    if (FAILED(hr) || surf == nullptr) {
        LogLine("[d2d] GetBuffer failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    ComPtr<IDXGISurface> surface;
    surface.Attach(surf);

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(g_format, D2D1_ALPHA_MODE_PREMULTIPLIED),
        g_dpi, g_dpi);

    ComPtr<ID2D1Bitmap1> bmp;
    hr = g_ctx->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bmp);
    if (FAILED(hr) || bmp == nullptr) {
        LogLine("[d2d] CreateBitmapFromDxgiSurface failed hr=0x%08lX fmt=%d",
                (unsigned long)hr, (int)g_format);
        return false;
    }
    g_target = bmp;
    g_ctx->SetTarget(g_target.Get());
    g_ctx->SetDpi(g_dpi, g_dpi);
    return true;
}

void DropTarget() {
    if (g_ctx != nullptr) g_ctx->SetTarget(nullptr);
    g_target.Reset();
}

}  // namespace

bool Init(IDXGISwapChain* sc) {
    if (sc == nullptr) return false;
    if (g_ctx != nullptr) return true;

    DXGI_SWAP_CHAIN_DESC desc = {};
    sc->GetDesc(&desc);
    g_format = desc.BufferDesc.Format;
    g_width = (int)desc.BufferDesc.Width;
    g_height = (int)desc.BufferDesc.Height;
    g_dpi = 96.0f;

    // 有些游戏用 R8G8B8A8，D2D 也吃；只有 BGRA/RGBA 这两类能直接当目标
    if (g_format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        g_format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        g_format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB &&
        g_format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        LogLine("[d2d] unsupported backbuffer format %d, forcing BGRA", (int)g_format);
        g_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    ComPtr<ID3D11Device> device;
    HRESULT hr = S_OK;
    {
        ID3D11Device* raw = nullptr;
        hr = sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&raw));
        if (FAILED(hr) || raw == nullptr) {
            LogLine("[d2d] GetDevice failed hr=0x%08lX", (unsigned long)hr);
            return false;
        }
        device.Attach(raw);
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = device.As(&dxgi_device);
    if (FAILED(hr)) {
        LogLine("[d2d] QI IDXGIDevice failed");
        return false;
    }

    D2D1_FACTORY_OPTIONS opts = {};
    {
        ID2D1Factory1* raw = nullptr;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               __uuidof(ID2D1Factory1), &opts,
                               reinterpret_cast<void**>(&raw));
        if (FAILED(hr) || raw == nullptr) {
            LogLine("[d2d] D2D1CreateFactory failed hr=0x%08lX", (unsigned long)hr);
            return false;
        }
        g_factory.Attach(raw);
    }

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

    g_sc = sc;
    if (!CreateTarget(sc)) {
        // 目标建不出来就先不失败：有可能 backbuffer 还没就绪，下一帧再试
        LogLine("[d2d] first CreateTarget failed, will retry");
    }
    LogLine("[d2d] ready fmt=%d size=%dx%d", (int)g_format, g_width, g_height);
    return true;
}

void Shutdown() {
    DropTarget();
    for (auto& kv : g_formats) {
        if (kv.second != nullptr) kv.second->Release();
    }
    g_formats.clear();
    g_brush.Reset();
    g_ctx.Reset();
    g_d2d_device.Reset();
    g_factory.Reset();
    g_dwrite.Reset();
    g_sc = nullptr;
}

void OnResize() {
    DropTarget();
    LogLine("[d2d] OnResize -- target dropped");
}

bool BeginFrame(IDXGISwapChain* sc) {
    if (sc == nullptr) return false;
    if (g_ctx == nullptr) {
        if (!Init(sc)) return false;
    }
    if (g_target == nullptr) {
        // 换过交换链或刚 resize 完，重建目标
        if (sc != g_sc) {
            g_sc = sc;
            DXGI_SWAP_CHAIN_DESC desc = {};
            sc->GetDesc(&desc);
            g_width = (int)desc.BufferDesc.Width;
            g_height = (int)desc.BufferDesc.Height;
        }
        if (!CreateTarget(sc)) return false;
    }

    IDXGISurface* raw_surface = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(IDXGISurface),
                                reinterpret_cast<void**>(&raw_surface))) &&
        raw_surface != nullptr) {
        ComPtr<IDXGISurface> surface;
        surface.Attach(raw_surface);
        // 尺寸变化时重建
        DXGI_SURFACE_DESC sd = {};
        surface->GetDesc(&sd);
        if ((int)sd.Width != g_width || (int)sd.Height != g_height) {
            g_width = (int)sd.Width;
            g_height = (int)sd.Height;
            DropTarget();
            if (!CreateTarget(sc)) return false;
        }
    }

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
        LogLine("[d2d] D2DERR_RECREATE_TARGET -- dropping target");
        DropTarget();
    }
}

int Width() { return g_width; }
int Height() { return g_height; }

void Rect(float x, float y, float w, float h, const D2D1_COLOR_F& c) {
    if (g_ctx == nullptr || w <= 0.0f || h <= 0.0f) return;
    g_brush->SetColor(c);
    g_ctx->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_brush.Get());
}

void RoundRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& c) {
    if (g_ctx == nullptr || w <= 0.0f || h <= 0.0f) return;
    float rr = r;
    if (rr > w * 0.5f) rr = w * 0.5f;
    if (rr > h * 0.5f) rr = h * 0.5f;
    g_brush->SetColor(c);
    g_ctx->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rr, rr),
                                g_brush.Get());
}

void Line(float x1, float y1, float x2, float y2, float width, const D2D1_COLOR_F& c) {
    if (g_ctx == nullptr) return;
    g_brush->SetColor(c);
    g_ctx->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush.Get(), width);
}

void Text(const wchar_t* s, float x, float y, float w, float h, float size,
          const D2D1_COLOR_F& c, int align, bool bold) {
    if (g_ctx == nullptr || s == nullptr || *s == L'\0') return;
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
