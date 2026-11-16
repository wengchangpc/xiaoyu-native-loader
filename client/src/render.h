// render.h - Direct2D + DirectWrite 覆盖层渲染
#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi.h>

namespace xiaoyu {
namespace render {

enum Align { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

inline D2D1_COLOR_F Rgba(int r, int g, int b, int a) {
    D2D1_COLOR_F c;
    c.r = r / 255.0f;
    c.g = g / 255.0f;
    c.b = b / 255.0f;
    c.a = a / 255.0f;
    return c;
}

inline D2D1_COLOR_F RgbaF(float r, float g, float b, float a) {
    D2D1_COLOR_F c;
    c.r = r;
    c.g = g;
    c.b = b;
    c.a = a;
    return c;
}

inline D2D1_COLOR_F WithAlpha(const D2D1_COLOR_F& c, float a) {
    D2D1_COLOR_F o = c;
    o.a = a;
    return o;
}

bool Init(IDXGISwapChain* sc);
void Shutdown();

// 交换链被 Resize 时调用：丢掉旧的 backbuffer 目标
void OnResize();

bool BeginFrame(IDXGISwapChain* sc);
void EndFrame();

int Width();
int Height();

void Rect(float x, float y, float w, float h, const D2D1_COLOR_F& c);
void RoundRect(float x, float y, float w, float h, float r, const D2D1_COLOR_F& c);
void Line(float x1, float y1, float x2, float y2, float width, const D2D1_COLOR_F& c);
void Text(const wchar_t* s, float x, float y, float w, float h, float size,
          const D2D1_COLOR_F& c, int align = ALIGN_LEFT, bool bold = false);
float Measure(const wchar_t* s, float size, bool bold = false);

void PushClip(float x, float y, float w, float h);
void PopClip();

}  // namespace render
}  // namespace xiaoyu
