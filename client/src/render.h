// render.h - Direct2D + DirectWrite 覆盖层渲染（自带交换链）
//
// 演进说明：
//   最初这层是画到游戏 DXGI 交换链的 backbuffer 上的。但实测这个 NetEase
//   客户端走的是 OpenGL（窗口类名 OGLES，exe 只导入 OPENGL32.dll），
//   进程里根本没有 DXGI 交换链，Present 钩子永远不触发。
//
//   所以改成自带 D3D11 设备 + DirectComposition 合成交换链，画到我们自己的
//   透明置顶窗口上。对外 API 和之前一致，gui.cpp 不需要任何改动。
#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <dwrite.h>

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

// 建 D3D11 设备、合成交换链、DirectComposition 视觉树、D2D 上下文。
// w/h 是覆盖窗口客户区尺寸。
bool Init(HWND hwnd, int w, int h);
void Shutdown();

// 窗口尺寸变化：重建交换链（backbuffer 尺寸必须和窗口一致）
void Resize(int w, int h);

// 每帧：拿 backbuffer 当 D2D 目标、清成全透明、BeginDraw
bool BeginFrame();
void EndFrame();

// 下一帧做一次像素自检并写日志（数非透明像素 + 包围盒），用于远程排障
void RequestSelfCheck();

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
