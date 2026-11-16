// render.cpp - Direct2D 覆盖层（分层窗口 + UpdateLayeredWindow）
//
// 为什么用 D2D：
//   1. 中文文字直接用 DirectWrite，不用做字形图集；
//   2. 圆角矩形 / 抗锯齿原生支持，跟原版面板的视觉一致。
//
// 为什么从 DirectComposition 换回分层窗口：
//   DComp 那一版所有 HRESULT 都返回成功、日志也打了 "ready"，但屏幕上
//   什么都没有 —— 合成层是否真的被 DWM 接受，从进程内部看不出来，盲猜
//   成本太高。分层窗口 + UpdateLayeredWindow 是覆盖层领域用了十几年的
//   老路子，行为完全确定：DIB 里是什么像素，屏幕上就是什么像素，
//   而且 WS_EX_TRANSPARENT 的点击穿透也是这套机制里久经验证的。
//
//   代价是每帧要把 DIB 提交一次（1215x702x4 ≈ 3.4MB），面板收起时
//   按 20fps 算约 68MB/s，现代机器扛得住。
//
// 绘制路径：
//   CreateDIBSection(32bpp, top-down, 预乘 alpha)
//     -> ID2D1DCRenderTarget::BindDC(内存 DC)
//     -> BeginDraw / 画 / EndDraw
//     -> UpdateLayeredWindow(ULW_ALPHA)
//
// 对外 API 保持不变，gui.cpp 一行都不用改。
#include "render.h"

#include <d2d1helper.h>
#include <wrl/client.h>

#include <map>

#include "log.h"

using Microsoft::WRL::ComPtr;

namespace xiaoyu {
namespace render {
namespace {

ComPtr<ID2D1Factory1> g_factory;
ComPtr<ID2D1DCRenderTarget> g_rt;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<ID2D1SolidColorBrush> g_brush;

HWND g_hwnd = nullptr;
HDC g_screen_dc = nullptr;   // 桌面 DC，UpdateLayeredWindow 要用
HDC g_mem_dc = nullptr;      // 内存 DC，选入 DIB
HBITMAP g_dib = nullptr;
HBITMAP g_old_bitmap = nullptr;
void* g_bits = nullptr;      // DIB 像素，直接可读，用于自检

int g_width = 0;
int g_height = 0;
int g_clip_depth = 0;
float g_dpi = 96.0f;
bool g_frame_checked = false;

// (size*10, bold, align) -> format
std::map<int, IDWriteTextFormat*> g_formats;

IDWriteTextFormat* GetFormat(float size, bool bold, int align) {
    int key = ((int)(size * 10.0f) << 8) | ((bold ? 1 : 0) << 4) | (align & 0xF);
    auto it = g_formats.find(key);
    if (it != g_formats.end()) return it->second;

    wchar_t family[64] = L"Microsoft YaHei";
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

void ReleaseSurface() {
    if (g_mem_dc != nullptr && g_old_bitmap != nullptr) {
        SelectObject(g_mem_dc, g_old_bitmap);
        g_old_bitmap = nullptr;
    }
    if (g_dib != nullptr) {
        DeleteObject(g_dib);
        g_dib = nullptr;
    }
    g_bits = nullptr;
    if (g_mem_dc != nullptr) {
        DeleteDC(g_mem_dc);
        g_mem_dc = nullptr;
    }
}

bool CreateSurface(int w, int h) {
    ReleaseSurface();
    if (g_screen_dc == nullptr) {
        g_screen_dc = GetDC(nullptr);
        if (g_screen_dc == nullptr) {
            LogLine("[d2d] GetDC(nullptr) failed");
            return false;
        }
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   // 负值 = top-down，方便按行序号读像素
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g_dib = CreateDIBSection(g_screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (g_dib == nullptr || bits == nullptr) {
        LogLine("[d2d] CreateDIBSection failed err=%lu", (unsigned long)GetLastError());
        return false;
    }
    g_bits = bits;

    g_mem_dc = CreateCompatibleDC(g_screen_dc);
    if (g_mem_dc == nullptr) {
        LogLine("[d2d] CreateCompatibleDC failed err=%lu", (unsigned long)GetLastError());
        return false;
    }
    g_old_bitmap = (HBITMAP)SelectObject(g_mem_dc, g_dib);

    g_width = w;
    g_height = h;
    return true;
}

}  // namespace

bool Init(HWND hwnd, int w, int h) {
    if (hwnd == nullptr || w <= 0 || h <= 0) return false;
    if (g_rt != nullptr) return true;
    g_hwnd = hwnd;

    if (!CreateSurface(w, h)) return false;

    D2D1_FACTORY_OPTIONS opts = {};
    ID2D1Factory1* raw_factory = nullptr;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                   &opts, reinterpret_cast<void**>(&raw_factory));
    if (FAILED(hr) || raw_factory == nullptr) {
        LogLine("[d2d] D2D1CreateFactory failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    g_factory.Attach(raw_factory);

    // DC 渲染目标的像素格式必须和 DIB 一致，alpha 用预乘
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        g_dpi, g_dpi);

    hr = g_factory->CreateDCRenderTarget(&props, &g_rt);
    if (FAILED(hr) || g_rt == nullptr) {
        LogLine("[d2d] CreateDCRenderTarget failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    g_rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    hr = g_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_brush);
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

    LogLine("[d2d] ready %dx%d fmt=BGRA(premul) mode=layered-window", g_width, g_height);
    return true;
}

void Shutdown() {
    g_brush.Reset();
    g_rt.Reset();
    g_factory.Reset();
    g_dwrite.Reset();
    for (auto& kv : g_formats) {
        if (kv.second != nullptr) kv.second->Release();
    }
    g_formats.clear();
    ReleaseSurface();
    if (g_screen_dc != nullptr) {
        ReleaseDC(nullptr, g_screen_dc);
        g_screen_dc = nullptr;
    }
    g_hwnd = nullptr;
    g_width = g_height = 0;
}

void Resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (w == g_width && h == g_height) return;
    if (!CreateSurface(w, h)) {
        LogLine("[d2d] Resize 到 %dx%d 失败", w, h);
        return;
    }
    g_frame_checked = false;
    LogLine("[d2d] resized to %dx%d", w, h);
}

bool BeginFrame() {
    if (g_rt == nullptr || g_mem_dc == nullptr || g_brush == nullptr) return false;

    RECT rc = {0, 0, g_width, g_height};
    HRESULT hr = g_rt->BindDC(g_mem_dc, &rc);
    if (FAILED(hr)) {
        LogLine("[d2d] BindDC failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }

    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
    // 清成全透明：分层窗口按 alpha 把游戏画面露出来
    g_rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    g_clip_depth = 0;
    return true;
}

void EndFrame() {
    if (g_rt == nullptr) return;
    while (g_clip_depth > 0) {
        g_rt->PopAxisAlignedClip();
        --g_clip_depth;
    }
    HRESULT hr = g_rt->EndDraw();
    if (FAILED(hr)) {
        LogLine("[d2d] EndDraw failed hr=0x%08lX", (unsigned long)hr);
        return;
    }

    // 首帧做一次自检：数非透明像素，并算出它们的包围盒。
    // 这条日志能直接回答三件事：
    //   - 0 个非透明像素        → gui 根本没画东西
    //   - 有像素但包围盒在屏幕外 → 布局坐标算错了
    //   - 有像素且位置合理      → 画对了，那就是窗口没显示出去
    if (!g_frame_checked && g_bits != nullptr) {
        g_frame_checked = true;
        const unsigned char* px = (const unsigned char*)g_bits;
        int opaque = 0;
        int minx = g_width, miny = g_height, maxx = -1, maxy = -1;
        for (int y = 0; y < g_height; ++y) {
            const unsigned char* row = px + (size_t)y * g_width * 4;
            for (int x = 0; x < g_width; ++x) {
                if (row[x * 4 + 3] != 0) {
                    ++opaque;
                    if (x < minx) minx = x;
                    if (x > maxx) maxx = x;
                    if (y < miny) miny = y;
                    if (y > maxy) maxy = y;
                }
            }
        }
        if (opaque == 0) {
            LogLine("[d2d] 首帧自检：0 个非透明像素（%dx%d），gui 没画出任何东西",
                    g_width, g_height);
        } else {
            LogLine("[d2d] 首帧自检：%d 个非透明像素，包围盒 (%d,%d)-(%d,%d)",
                    opaque, minx, miny, maxx, maxy);
        }
    }

    if (g_hwnd == nullptr || g_screen_dc == nullptr) return;

    RECT wr = {0, 0, 0, 0};
    if (!GetWindowRect(g_hwnd, &wr)) return;

    POINT dst = {wr.left, wr.top};
    SIZE size = {g_width, g_height};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;   // 用 DIB 自己的 alpha 通道

    if (!UpdateLayeredWindow(g_hwnd, g_screen_dc, &dst, &size, g_mem_dc, &src, 0, &blend,
                             ULW_ALPHA)) {
        // 只在第一次报错，避免每帧刷屏
        static bool reported = false;
        if (!reported) {
            reported = true;
            LogLine("[d2d] UpdateLayeredWindow failed err=%lu",
                    (unsigned long)GetLastError());
        }
    }
}

int Width() { return g_width; }
int Height() { return g_height; }

void RequestSelfCheck() { g_frame_checked = false; }

void Rect(float x, float y, float w, float h, const D2D1_COLOR_F& c) {
    // g_brush 为空时直接 return：ComPtr 的 operator-> 不做判空，空指针会直接 AV
    if (g_rt == nullptr || g_brush == nullptr || w <= 0.0f || h <= 0.0f) return;
    g_brush->SetColor(c);
    g_rt->FillRectangle(D2D1::RectF(x, y, x + w, y + h), g_brush.Get());
}

void RoundRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& c) {
    if (g_rt == nullptr || g_brush == nullptr || w <= 0.0f || h <= 0.0f) return;
    float rr = r;
    if (rr > w * 0.5f) rr = w * 0.5f;
    if (rr > h * 0.5f) rr = h * 0.5f;
    g_brush->SetColor(c);
    g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), rr, rr),
                               g_brush.Get());
}

void Line(float x1, float y1, float x2, float y2, float width, const D2D1_COLOR_F& c) {
    if (g_rt == nullptr || g_brush == nullptr) return;
    g_brush->SetColor(c);
    g_rt->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), g_brush.Get(), width);
}

void RoundRectStroke(float x, float y, float w, float h, float r, float t,
                     const D2D1_COLOR_F& c) {
    if (g_rt == nullptr || g_brush == nullptr || w <= 0.0f || h <= 0.0f || t <= 0.0f) return;
    // 线宽是沿着路径居中展开的，所以路径本身要向内收 t/2，否则外侧半条线会
    // 溢出面板轮廓，在透明背景上糊出一圈毛边。
    const float inset = t * 0.5f;
    const float inner_w = w - t;
    const float inner_h = h - t;
    if (inner_w <= 0.0f || inner_h <= 0.0f) return;
    float rr = r - inset;
    if (rr < 0.0f) rr = 0.0f;
    if (rr > inner_w * 0.5f) rr = inner_w * 0.5f;
    if (rr > inner_h * 0.5f) rr = inner_h * 0.5f;
    g_brush->SetColor(c);
    g_rt->DrawRoundedRectangle(
        D2D1::RoundedRect(
            D2D1::RectF(x + inset, y + inset, x + w - inset, y + h - inset), rr, rr),
        g_brush.Get(), t);
}

void Polygon(const D2D1_POINT_2F* pts, int count, const D2D1_COLOR_F& c) {
    if (g_rt == nullptr || g_brush == nullptr || g_factory == nullptr) return;
    if (pts == nullptr || count < 3) return;

    ComPtr<ID2D1PathGeometry> geo;
    if (FAILED(g_factory->CreatePathGeometry(&geo)) || geo == nullptr) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink)) || sink == nullptr) return;

    sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(pts + 1, (UINT32)(count - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return;   // Close 失败时几何不能用，别再往下画

    g_brush->SetColor(c);
    g_rt->FillGeometry(geo.Get(), g_brush.Get());
}

void Text(const wchar_t* s, float x, float y, float w, float h, float size,
          const D2D1_COLOR_F& c, int align, bool bold) {
    if (g_rt == nullptr || g_brush == nullptr || s == nullptr || *s == L'\0') return;
    IDWriteTextFormat* fmt = GetFormat(size, bold, align);
    if (fmt == nullptr) return;
    g_brush->SetColor(c);
    g_rt->DrawTextW(s, (UINT32)wcslen(s), fmt, D2D1::RectF(x, y, x + w, y + h),
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
    if (g_rt == nullptr) return;
    g_rt->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h),
                              D2D1_ANTIALIAS_MODE_ALIASED);
    ++g_clip_depth;
}

void PopClip() {
    if (g_rt == nullptr || g_clip_depth <= 0) return;
    g_rt->PopAxisAlignedClip();
    --g_clip_depth;
}

}  // namespace render
}  // namespace xiaoyu
