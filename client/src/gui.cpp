// gui.cpp - ClickGui 覆盖层
//
// 几何与配色全部照搬 Python 原版 ClickGui.py 的 nilore 皮肤：
//   NILORE_PANEL_W=120  HEAD_H=20  ROW_H=20  GAP=6  ROW_INSET=1  PANEL_A=0.94
//   BG #171717  HEAD #131313  ROW #202020  ROW_HOVER #272727  ACCENT #2DE8CA
// 模块目录与参数表来自自动生成的 catalog.h（源头是原版 FEATURE_DEFS 等）。
#include "gui.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>

#include <string>
#include <vector>

#include "config.h"
#include "gui/catalog.h"
#include "gui/theme.h"
#include "input.h"
#include "log.h"
#include "render.h"

namespace xiaoyu {
namespace gui {
namespace {

using render::Align;
using render::ALIGN_CENTER;
using render::ALIGN_LEFT;
using render::ALIGN_RIGHT;

// ── 原版常量（单位是「GUI 单位」，乘 S 得到像素）────────────────
const float kPanelW = 120.0f;
const float kHeadH = 20.0f;
const float kRowH = 20.0f;
const float kGap = 6.0f;
const float kInset = 1.0f;
const float kPad = 12.0f;
const float kRadius = 6.0f;
const float kShadow = 5.0f;
const float kParamH = 18.0f;
const float kScrollbarW = 2.0f;

struct Panel {
    int group = 0;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool collapsed = false;
    bool has_pos = false;
    float scroll = 0.0f;
    float content_h = 0.0f;
    float max_body = 0.0f;
    int row_count = 0;
};

std::vector<Panel> g_panels;
std::vector<char> g_expanded;      // 按 feature 下标
std::vector<float> g_scroll_saved; // 未用，预留

// 折叠/滚动必须放在 panel 外：g_panels 每帧重建，
// 状态存在 Panel 里会在下一帧被 LayoutPanels 清掉。
bool g_collapsed[GROUP_COUNT] = {false};
float g_scroll[GROUP_COUNT] = {0.0f};

bool g_visible = false;
float g_s = 2.0f;

// 交互
int g_drag_panel = -1;
float g_drag_off_x = 0.0f;
float g_drag_off_y = 0.0f;
bool g_drag_moved = false;
int g_drag_slider = -1;
int g_drag_scrollbar = -1;
float g_drag_scroll_start = 0.0f;
float g_drag_mouse_start = 0.0f;
int g_bind_target = -1;   // 正在等按键的 feature 下标

// FPS 统计
ULONGLONG g_last_tick = 0;
float g_fps = 0.0f;
float g_frame_ms = 0.0f;

// CPS 统计
std::vector<ULONGLONG> g_click_times;

bool AnyPanelDragging() { return g_drag_panel >= 0 || g_drag_slider >= 0 ||
                                 g_drag_scrollbar >= 0; }

const wchar_t* FeatName(int i) {
    // 原版 CHINESE_FEATURES 里有个别模块没有中文名（比如 WalkVoid 的 zh 是 nullptr），
    // 直接返回 nullptr 会让 render::Text 早退，那一行就成了空白 —— 这里回退到英文名。
    const wchar_t* zh = kFeatures[i].zh;
    if (cfg::Chinese() && zh != nullptr) return zh;
    return kFeatures[i].en;
}

const wchar_t* GroupName(int g) {
    return cfg::Chinese() ? kGroups[g].zh : kGroups[g].en;
}

float ParamDefault(int pi) {
    const ParamInfo& p = kParams[pi];
    if (p.kind != PK_SLIDER) return 0.0f;
    float mid = (p.vmin + p.vmax) * 0.5f;
    if (p.vstep > 0.0f) {
        float steps = floorf((mid - p.vmin) / p.vstep + 0.5f);
        mid = p.vmin + steps * p.vstep;
    }
    return mid;
}

float ParamGet(int pi) { return cfg::ParamValue(pi, ParamDefault(pi)); }

// 一个模块行 + 展开后的参数行，一共多高
float FeatureHeight(int fi) {
    float h = kRowH;
    if (!g_expanded[fi]) return h;
    const FeatureInfo& f = kFeatures[fi];
    h += 4.0f + f.paramCount * kParamH;
    return h;
}

float GroupContentHeight(int gi) {
    float h = 0.0f;
    for (int i = 0; i < kFeatureCount; ++i) {
        if (kFeatures[i].group != gi) continue;
        h += FeatureHeight(i);
    }
    return h;
}

int CountGroupFeatures(int gi) {
    int n = 0;
    for (int i = 0; i < kFeatureCount; ++i) {
        if (kFeatures[i].group == gi) ++n;
    }
    return n;
}

// ── 布局：原版 _createNiloreLayout 的移植 ────────────────────────
void LayoutPanels() {
    g_s = cfg::Scale();
    const float pad = kPad * g_s;
    const float gap = kGap * g_s;
    const float colW = kPanelW * g_s;
    const float headH = kHeadH * g_s;
    const float rowH = kRowH * g_s;
    const float viewW = (float)render::Width();
    const float viewH = (float)render::Height();

    g_panels.clear();
    int cats = 0;
    for (int g = 0; g < GROUP_COUNT; ++g) {
        if (CountGroupFeatures(g) > 0) ++cats;
    }
    if (cats == 0) return;

    float availW = viewW - pad * 2.0f;
    if (availW < colW) availW = colW;
    int per_row = (int)((availW + gap) / (colW + gap));
    if (per_row < 1) per_row = 1;
    if (per_row > cats) per_row = cats;

    int row_count = (cats + per_row - 1) / per_row;
    if (row_count > 1) {
        int even = (cats + row_count - 1) / row_count;
        if (even < per_row) {
            per_row = even;
            row_count = (cats + per_row - 1) / per_row;
        }
    }

    float limit_body = viewH * 0.64f;
    float availH = viewH - pad * 2.0f - gap * (row_count - 1);
    float budget = (availH - row_count * (headH + 6.0f * g_s)) / (float)row_count;
    float max_body = limit_body < budget ? limit_body : budget;
    if (max_body < 0.0f) max_body = 0.0f;

    // 先按分类建 panel 并算各自高度
    std::vector<int> order;
    std::vector<float> heights;
    for (int g = 0; g < GROUP_COUNT; ++g) {
        if (CountGroupFeatures(g) == 0) continue;
        Panel p;
        p.group = g;
        p.w = colW;
        p.row_count = CountGroupFeatures(g);
        p.max_body = max_body;
        p.content_h = GroupContentHeight(g) * g_s;
        p.collapsed = g_collapsed[g];
        p.scroll = g_scroll[g];
        g_panels.push_back(p);
        order.push_back(g);
    }

    for (size_t i = 0; i < g_panels.size(); ++i) {
        Panel& p = g_panels[i];
        float body = p.content_h < max_body ? p.content_h : max_body;
        if (p.collapsed) body = 0.0f;
        p.h = headH + body + (p.collapsed ? 0.0f : 6.0f * g_s);
        heights.push_back(p.h);
    }

    // 行内最大高度决定行间距
    std::vector<float> row_tops;
    std::vector<int> row_counts;
    std::vector<int> row_starts;
    float y = pad;
    int idx = 0;
    const int total = (int)g_panels.size();
    while (idx < total) {
        int cnt = per_row < (total - idx) ? per_row : (total - idx);
        float maxh = 0.0f;
        for (int k = 0; k < cnt; ++k) {
            if (heights[idx + k] > maxh) maxh = heights[idx + k];
        }
        row_tops.push_back(y);
        row_counts.push_back(cnt);
        row_starts.push_back(idx);
        y += maxh + gap;
        idx += cnt;
    }
    float total_h = y - gap + pad;
    float offset_y = (viewH - total_h) * 0.5f;
    if (offset_y < 0.0f) offset_y = 0.0f;

    // 落位：用户拖过的用保存值，否则按算法居中
    for (size_t r = 0; r < row_tops.size(); ++r) {
        int cnt = row_counts[r];
        int start = row_starts[r];
        float row_w = cnt * colW + (cnt - 1) * gap;
        float x0 = (viewW - row_w) * 0.5f;
        if (x0 < pad) x0 = pad;
        float ry = row_tops[r] + offset_y;
        for (int k = 0; k < cnt; ++k) {
            Panel& p = g_panels[start + k];
            p.x = x0 + k * (colW + gap);
            p.y = ry;
            float sx, sy;
            bool col = false;
            if (cfg::PanelPos(p.group, &sx, &sy, &col)) {
                p.x = sx;
                p.y = sy;
                p.has_pos = true;
            }
            // 钳位：栏头至少留 40px 能点到
            float minX = 40.0f - p.w;
            float maxX = viewW - 40.0f;
            float maxY = viewH - headH;
            if (p.x < minX) p.x = minX;
            if (p.x > maxX) p.x = maxX;
            if (p.y < 0.0f) p.y = 0.0f;
            if (p.y > maxY) p.y = maxY;
        }
    }
}

// ── 绘制辅助 ────────────────────────────────────────────────────
void DrawPanelChrome(const Panel& p, bool hover_head) {
    const float s = g_s;
    const D2D1_COLOR_F col_head = hover_head ? theme::kHeadHover : theme::kHeadBg;

    // 阴影
    render::Rect(p.x - kShadow * s, p.y - kShadow * s, p.w + kShadow * 2.0f * s,
                 p.h + kShadow * 2.0f * s, theme::kShadow);

    // 面板
    render::RoundRect(p.x, p.y, p.w, p.h, kRadius * s, theme::kBg);

    // 栏头
    render::PushClip(p.x, p.y, p.w, kHeadH * s + 1.0f);
    render::Rect(p.x, p.y, p.w, kHeadH * s, col_head);
    render::PopClip();

    // 顶部强调线
    render::Rect(p.x, p.y, p.w, (s > 1.5f ? 2.0f : 1.0f), theme::kAccentD);

    // 标题
    float title_size = 11.0f * s;
    render::Text(GroupName(p.group), p.x + 8.0f * s, p.y,
                 p.w - 28.0f * s, kHeadH * s, title_size, theme::kAccent, ALIGN_LEFT,
                 true);

    // 折叠箭头
    const wchar_t* arrow = p.collapsed ? L"\u25B8" : L"\u25BE";
    render::Text(arrow, p.x + p.w - 16.0f * s, p.y, 12.0f * s, kHeadH * s,
                 title_size, theme::kArrow, ALIGN_LEFT, false);
}

// 返回 true 表示命中（用于 hover 高亮）
void DrawScrollbar(const Panel& p) {
    const float s = g_s;
    if (p.collapsed) return;
    float body_h = p.h - kHeadH * s;
    if (p.content_h <= p.max_body + 0.5f) return;

    float bar_w = kScrollbarW * s;
    if (bar_w < 2.0f) bar_w = 2.0f;
    float bx = p.x + p.w - bar_w - 1.0f * s;
    float by = p.y + kHeadH * s + 4.0f * s;
    float bh = body_h - 8.0f * s;
    render::Rect(bx, by, bar_w, bh, theme::kScrollTrack);

    float ratio = p.max_body / p.content_h;
    if (ratio > 1.0f) ratio = 1.0f;
    float thumb_h = bh * ratio;
    if (thumb_h < 12.0f * s) thumb_h = 12.0f * s;
    float max_scroll = p.content_h - p.max_body;
    if (max_scroll < 1.0f) max_scroll = 1.0f;
    float t = p.scroll / max_scroll;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float ty = by + (bh - thumb_h) * t;
    render::RoundRect(bx, ty, bar_w, thumb_h, bar_w * 0.5f, theme::kScrollThumb);
}

// 画一个参数行；interactive 为 true 时只做命中测试
void DrawParamRow(int pi, int fi, float x, float y, float w, bool interactive) {
    const float s = g_s;
    const ParamInfo& p = kParams[pi];
    const float h = kParamH * s;
    const float mx = input::X();
    const float my = input::Y();
    const bool inside = mx >= x && mx <= x + w && my >= y && my <= y + h;

    if (!interactive) {
        float label_size = 9.5f * s;
        render::Text(p.label, x + 12.0f * s, y, w * 0.55f, h, label_size, theme::kDim,
                     ALIGN_LEFT, false);

        switch (p.kind) {
            case PK_SLIDER: {
                float v = ParamGet(pi);
                float range = p.vmax - p.vmin;
                float t = range > 0.0f ? (v - p.vmin) / range : 0.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float track_x = x + 12.0f * s;
                float track_w = w - 24.0f * s;
                float track_y = y + h - 4.0f * s;
                render::Rect(track_x, track_y, track_w, 2.0f * s, theme::kTrack);
                render::Rect(track_x, track_y, track_w * t, 2.0f * s, theme::kAccentD);
                render::RoundRect(track_x + track_w * t - 2.0f * s, track_y - 2.0f * s,
                                  4.0f * s, 6.0f * s, 2.0f * s, theme::kAccent);

                wchar_t buf[64];
                if (p.vstep >= 1.0f) {
                    swprintf_s(buf, L"%.0f", v);
                } else if (p.vstep >= 0.1f) {
                    swprintf_s(buf, L"%.1f", v);
                } else {
                    swprintf_s(buf, L"%.2f", v);
                }
                render::Text(buf, x + w * 0.55f, y, w * 0.45f - 12.0f * s, h, 9.5f * s,
                             inside ? theme::kSub : theme::kDim, ALIGN_RIGHT, false);
                break;
            }
            case PK_TOGGLE: {
                float bw = 22.0f * s;
                float bh = 11.0f * s;
                float bx = x + w - bw - 12.0f * s;
                float by = y + (h - bh) * 0.5f;
                bool on = ParamGet(pi) > 0.5f;
                render::RoundRect(bx, by, bw, bh, bh * 0.5f,
                                  on ? theme::kAccentD : theme::kTrack);
                if (on) {
                    render::RoundRect(bx + bw - bh + 1.0f * s,
                                      by + 1.0f * s, bh - 2.0f * s,
                                      bh - 2.0f * s, (bh - 2.0f * s) * 0.5f,
                                      theme::kAccent);
                } else {
                    render::RoundRect(bx + 1.0f * s, by + 1.0f * s, bh - 2.0f * s,
                                      bh - 2.0f * s, (bh - 2.0f * s) * 0.5f,
                                      theme::kDim);
                }
                break;
            }
            case PK_ENUM: {
                int idx = (int)ParamGet(pi);
                if (idx < 0) idx = 0;
                if (idx >= p.nopts) idx = p.nopts - 1;
                const wchar_t* cur = (p.nopts > 0 && p.options) ? p.options[idx] : L"-";
                render::Text(cur, x + w * 0.5f, y, w * 0.5f - 12.0f * s, h, 9.5f * s,
                             inside ? theme::kAccent : theme::kSub, ALIGN_RIGHT, false);
                break;
            }
            case PK_KEYBIND: {
                int vk = cfg::ModuleBind(fi);
                wchar_t buf[48];
                if (g_bind_target == fi) {
                    wcscpy_s(buf, L"按任意键…");
                } else if (vk == 0) {
                    wcscpy_s(buf, L"未绑定");
                } else {
                    UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR);
                    if (ch >= 32 && ch < 127) {
                        swprintf_s(buf, L"%c", (wchar_t)ch);
                    } else {
                        swprintf_s(buf, L"0x%02X", vk);
                    }
                }
                render::Text(buf, x + w * 0.5f, y, w * 0.5f - 12.0f * s, h, 9.5f * s,
                             g_bind_target == fi ? theme::kAccent
                                                 : (inside ? theme::kSub : theme::kDim),
                             ALIGN_RIGHT, false);
                break;
            }
            default:
                break;
        }
        // 悬停底色
        if (inside) {
            // 画在文字下面会更自然，这里简单地在左侧加一条强调
            render::Rect(x + 3.0f * s, y + 3.0f * s, 2.0f * s, h - 6.0f * s,
                         theme::kAccentDD);
        }
        return;
    }

    // ── 命中测试 ──
    if (input::Pressed(0)) {
        switch (p.kind) {
            case PK_SLIDER:
                if (inside) {
                    g_drag_slider = pi;
                    // 立刻把滑块跳到点击位置
                    float track_x = x + 12.0f * s;
                    float track_w = w - 24.0f * s;
                    float t = (mx - track_x) / track_w;
                    if (t < 0.0f) t = 0.0f;
                    if (t > 1.0f) t = 1.0f;
                    float v = p.vmin + t * (p.vmax - p.vmin);
                    if (p.vstep > 0.0f) {
                        float steps = floorf((v - p.vmin) / p.vstep + 0.5f);
                        v = p.vmin + steps * p.vstep;
                        if (v > p.vmax) v = p.vmax;
                    }
                    cfg::SetParamValue(pi, v);
                }
                break;
            case PK_TOGGLE:
                if (inside) {
                    cfg::SetParamValue(pi, ParamGet(pi) > 0.5f ? 0.0f : 1.0f);
                }
                break;
            case PK_ENUM:
                if (inside && p.nopts > 0) {
                    int idx = (int)ParamGet(pi);
                    idx = (idx + 1) % p.nopts;
                    cfg::SetParamValue(pi, (float)idx);
                }
                break;
            case PK_KEYBIND:
                if (inside) g_bind_target = fi;
                break;
            default:
                break;
        }
    }

    if (g_drag_slider == pi && p.kind == PK_SLIDER) {
        // 鼠标按住时持续跟随
        float track_x = x + 12.0f * s;
        float track_w = w - 24.0f * s;
        float t = (mx - track_x) / track_w;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        float v = p.vmin + t * (p.vmax - p.vmin);
        if (p.vstep > 0.0f) {
            float steps = floorf((v - p.vmin) / p.vstep + 0.5f);
            v = p.vmin + steps * p.vstep;
            if (v > p.vmax) v = p.vmax;
        }
        cfg::SetParamValue(pi, v);
    }
}

void DrawFeatureRow(int fi, const Panel& p, float row_x, float& row_y, bool interactive) {
    const float s = g_s;
    const float w = p.w - kInset * 2.0f * s;
    const float h = kRowH * s;
    const float mx = input::X();
    const float my = input::Y();
    const float y = row_y - p.scroll;
    const bool on = cfg::ModuleOn(fi);
    const bool inside = mx >= row_x && mx <= row_x + w && my >= y && my <= y + h &&
                        my >= p.y + kHeadH * s && my <= p.y + p.h;

    if (!interactive) {
        if (on) {
            render::Rect(row_x, y, w, h, theme::kRow);
        } else if (inside) {
            render::Rect(row_x, y, w, h, theme::kRowHover);
        }

        const FeatureInfo& f = kFeatures[fi];
        float label_size = 10.5f * s;
        render::Text(FeatName(fi), row_x + 8.0f * s, y, w - 28.0f * s, h, label_size,
                     on ? theme::kAccent : theme::kOff, ALIGN_LEFT, false);

        if (f.paramCount > 0) {
            const wchar_t* ar = g_expanded[fi] ? L"\u25BE" : L"\u25B8";
            render::Text(ar, row_x + w - 18.0f * s, y, 14.0f * s, h, label_size,
                         theme::kArrow, ALIGN_LEFT, false);
        }
    } else {
        if (input::Pressed(0) && inside && g_drag_panel < 0) {
            // 右半边点箭头区 = 展开，其他位置 = 切开关
            float arrow_x = row_x + w - 20.0f * s;
            const FeatureInfo& f = kFeatures[fi];
            if (f.paramCount > 0 && mx >= arrow_x) {
                g_expanded[fi] = g_expanded[fi] ? 0 : 1;
            } else if (f.action) {
                LogLineW((std::wstring(L"[action] ") + f.en).c_str());
            } else {
                cfg::SetModuleOn(fi, !on);
            }
        }
    }

    row_y += h;

    // 参数区
    if (g_expanded[fi]) {
        const FeatureInfo& f = kFeatures[fi];
        row_y += 4.0f * s;
        for (int k = 0; k < f.paramCount; ++k) {
            int pi = f.paramFirst + k;
            DrawParamRow(pi, fi, row_x, row_y - p.scroll, w, interactive);
            row_y += kParamH * s;
        }
    }
}

void DrawPanel(Panel& p, bool interactive) {
    const float s = g_s;
    const float headH = kHeadH * s;
    const float mx = input::X();
    const float my = input::Y();

    const bool head_inside = mx >= p.x && mx <= p.x + p.w && my >= p.y &&
                             my <= p.y + headH;
    const bool body_inside = mx >= p.x && mx <= p.x + p.w && my >= p.y + headH &&
                             my <= p.y + p.h;

    if (!interactive) {
        DrawPanelChrome(p, head_inside || g_drag_panel == p.group);
    } else {
        // 栏头：按下开始拖，松开若没怎么动就当作折叠
        if (input::Pressed(0) && head_inside && g_drag_panel < 0) {
            g_drag_panel = p.group;
            g_drag_off_x = mx - p.x;
            g_drag_off_y = my - p.y;
            g_drag_moved = false;
        }
        if (g_drag_panel == p.group) {
            if (input::Down(0)) {
                float nx = mx - g_drag_off_x;
                float ny = my - g_drag_off_y;
                if (fabsf(nx - p.x) > 1.0f || fabsf(ny - p.y) > 1.0f) g_drag_moved = true;
                p.x = nx;
                p.y = ny;
            }
            if (input::Released(0)) {
                g_drag_panel = -1;
                if (g_drag_moved) {
                    cfg::SetPanelPos(p.group, p.x, p.y, p.collapsed);
                } else {
                    p.collapsed = !p.collapsed;
                    g_collapsed[p.group] = p.collapsed;
                    cfg::SetPanelPos(p.group, p.x, p.y, p.collapsed);
                    float body = p.collapsed ? 0.0f
                                             : (p.content_h < p.max_body ? p.content_h
                                                                         : p.max_body);
                    p.h = headH + body + (p.collapsed ? 0.0f : 6.0f * s);
                }
            }
        }

        // 滚轮
        if (body_inside && !p.collapsed && input::Wheel() != 0.0f) {
            float max_scroll = p.content_h - p.max_body;
            if (max_scroll > 0.0f) {
                p.scroll -= input::Wheel() * 24.0f * s;
                if (p.scroll < 0.0f) p.scroll = 0.0f;
                if (p.scroll > max_scroll) p.scroll = max_scroll;
                g_scroll[p.group] = p.scroll;
            }
        }
        if (p.collapsed) {
            p.scroll = 0.0f;
            g_scroll[p.group] = 0.0f;
        }
    }

    if (p.collapsed) return;

    // 栏体
    float body_y = p.y + headH;
    float body_h = p.h - headH;
    if (!interactive) {
        render::RoundRect(p.x, body_y, p.w, body_h, 0.0f, theme::kBody);
        DrawScrollbar(p);
    }

    if (!interactive) render::PushClip(p.x, body_y, p.w, body_h);

    float row_y = body_y + 3.0f * s;
    for (int i = 0; i < kFeatureCount; ++i) {
        if (kFeatures[i].group != p.group) continue;
        DrawFeatureRow(i, p, p.x + kInset * s, row_y, interactive);
    }

    if (!interactive) render::PopClip();
}

// ── HUD ─────────────────────────────────────────────────────────
int FindFeatureByName(const wchar_t* name) {
    for (int i = 0; i < kFeatureCount; ++i) {
        if (wcscmp(kFeatures[i].en, name) == 0) return i;
    }
    return -1;
}

int Cps() {
    ULONGLONG now = GetTickCount64();
    int n = 0;
    for (size_t i = 0; i < g_click_times.size(); ++i) {
        if (now - g_click_times[i] <= 1000) ++n;
    }
    return n;
}

void DrawHud() {
    const float s = g_s;
    const float viewW = (float)render::Width();
    float y = 8.0f * s;

    // FPS
    int fps_idx = FindFeatureByName(L"FPS");
    if (fps_idx >= 0 && cfg::ModuleOn(fps_idx)) {
        wchar_t buf[64];
        swprintf_s(buf, L"FPS \u00B7 %.0f", g_fps);
        render::Text(buf, 8.0f * s, y, 200.0f * s, 14.0f * s, 10.0f * s, theme::kAccent,
                     ALIGN_LEFT, true);
        y += 14.0f * s;
    }

    // CPS
    int cps_idx = FindFeatureByName(L"CPS");
    if (cps_idx >= 0 && cfg::ModuleOn(cps_idx)) {
        wchar_t buf[64];
        swprintf_s(buf, L"CPS \u00B7 %d", Cps());
        render::Text(buf, 8.0f * s, y, 200.0f * s, 14.0f * s, 10.0f * s, theme::kAccent,
                     ALIGN_LEFT, true);
        y += 14.0f * s;
    }

    // ArrayList：右上角列出已开启功能
    int arr_idx = FindFeatureByName(L"ArrayList");
    if (arr_idx >= 0 && cfg::ModuleOn(arr_idx)) {
        float ly = 8.0f * s;
        for (int i = 0; i < kFeatureCount; ++i) {
            if (!cfg::ModuleOn(i)) continue;
            if (i == arr_idx) continue;
            const wchar_t* name = FeatName(i);
            float tw = render::Measure(name, 10.0f * s, false);
            float box_w = tw + 12.0f * s;
            float bx = viewW - box_w - 6.0f * s;
            render::Rect(bx, ly, box_w, 14.0f * s, theme::kRow);
            render::Rect(bx + box_w - 2.0f * s, ly, 2.0f * s, 14.0f * s, theme::kAccent);
            render::Text(name, bx, ly, box_w - 6.0f * s, 14.0f * s, 10.0f * s,
                         theme::kAccent, ALIGN_RIGHT, false);
            ly += 15.0f * s;
        }
    }

    // Keystrokes：左下角 WASD + 鼠标
    int ks_idx = FindFeatureByName(L"Keystrokes");
    if (ks_idx >= 0 && cfg::ModuleOn(ks_idx)) {
        float box = 18.0f * s;
        float gap = 2.0f * s;
        float bx = 10.0f * s;
        float by = (float)render::Height() - 3.0f * (box + gap) - 10.0f * s;

        struct KeyBox { const wchar_t* label; int vk; float dx, dy; float w, h; };
        KeyBox boxes[] = {
            {L"W", 'W', 1.0f, 0.0f, 1.0f, 1.0f},
            {L"A", 'A', 0.0f, 1.0f, 1.0f, 1.0f},
            {L"S", 'S', 1.0f, 1.0f, 1.0f, 1.0f},
            {L"D", 'D', 2.0f, 1.0f, 1.0f, 1.0f},
            {L"\u5DE6\u952E", VK_LBUTTON, 0.0f, 2.0f, 1.5f, 1.0f},
            {L"\u53F3\u952E", VK_RBUTTON, 1.5f, 2.0f, 1.5f, 1.0f},
        };
        for (size_t i = 0; i < sizeof(boxes) / sizeof(boxes[0]); ++i) {
            bool down = input::KeyDown(boxes[i].vk) ||
                        (boxes[i].vk == VK_LBUTTON && input::Down(0)) ||
                        (boxes[i].vk == VK_RBUTTON && input::Down(1));
            float x = bx + boxes[i].dx * (box + gap);
            float yy = by + boxes[i].dy * (box + gap);
            float w = boxes[i].w * box + (boxes[i].w - 1.0f) * gap;
            render::RoundRect(x, yy, w, box, 3.0f * s,
                              down ? theme::kAccent : theme::kRow);
            render::Text(boxes[i].label, x, yy, w, box, 9.0f * s,
                         down ? theme::kHeadBg : theme::kOff, ALIGN_CENTER, false);
        }
    }
}

void HandleHotkeys() {
    // GUI 打开/关闭
    if (input::HotkeyPressed()) {
        SetVisible(!g_visible);
    }

    // 缩放
    if (input::KeyDown(VK_CONTROL)) {
        if (input::KeyPressed(VK_OEM_4)) cfg::SetScale(cfg::Scale() - 0.25f);
        if (input::KeyPressed(VK_OEM_6)) cfg::SetScale(cfg::Scale() + 0.25f);
    }

    // 绑定捕获
    if (g_bind_target >= 0) {
        int vk = input::TakePressedKey();
        if (vk != 0) {
            if (vk == VK_ESCAPE) {
                cfg::SetModuleBind(g_bind_target, 0);
            } else {
                cfg::SetModuleBind(g_bind_target, vk);
            }
            g_bind_target = -1;
        }
    }

    // 模块快捷键
    for (int i = 0; i < kFeatureCount; ++i) {
        int vk = cfg::ModuleBind(i);
        if (vk == 0) continue;
        if (input::KeyPressed(vk) && i != FindFeatureByName(L"ClickGui")) {
            if (!kFeatures[i].action) cfg::SetModuleOn(i, !cfg::ModuleOn(i));
        }
    }
}

void ReleaseDrags() {
    if (!input::Down(0)) {
        if (g_drag_slider >= 0) {
            g_drag_slider = -1;
        }
        if (g_drag_scrollbar >= 0) g_drag_scrollbar = -1;
    }
}

}  // namespace

void Init() {
    cfg::Load();
    g_expanded.assign(kFeatureCount, 0);
    g_scroll_saved.assign(kFeatureCount, 0.0f);

    // 折叠状态从配置恢复一次，之后由 g_collapsed 持有
    for (int g = 0; g < GROUP_COUNT; ++g) {
        float x = 0.0f, y = 0.0f;
        bool col = false;
        g_collapsed[g] = false;
        g_scroll[g] = 0.0f;
        if (cfg::PanelPos(g, &x, &y, &col)) g_collapsed[g] = col;
    }

    g_s = cfg::Scale();
    g_last_tick = GetTickCount64();
    LogLine("[gui] init, %d features, %d params, scale=%.2f", kFeatureCount, kParamCount,
            cfg::Scale());
}

void Shutdown() {
    cfg::Save();
}

void Toggle() { SetVisible(!g_visible); }

bool Visible() { return g_visible; }

void SetVisible(bool v) {
    if (g_visible == v) return;
    g_visible = v;
    input::SetCapture(v && cfg::BlockInput());
    if (v) {
        // 打开时把鼠标从游戏手里放出来
        ClipCursor(nullptr);
        int guard = 0;
        while (ShowCursor(TRUE) < 0 && guard++ < 64) {
        }
    } else {
        g_bind_target = -1;
        g_drag_panel = -1;
        g_drag_slider = -1;
        int guard = 0;
        while (ShowCursor(FALSE) >= 0 && guard++ < 64) {
        }
    }
    LogLine("[gui] visible=%d", v ? 1 : 0);
}

void OnFrame() {
    // FPS
    ULONGLONG now = GetTickCount64();
    if (g_last_tick != 0) {
        float dt = (float)(now - g_last_tick);
        g_frame_ms = g_frame_ms * 0.9f + dt * 0.1f;
        if (g_frame_ms > 0.01f) g_fps = 1000.0f / g_frame_ms;
    }
    g_last_tick = now;

    input::NewFrame();
    cfg::Tick();

    // 点击计数（CPS）
    if (input::Pressed(0)) {
        g_click_times.push_back(now);
        if (g_click_times.size() > 64) g_click_times.erase(g_click_times.begin());
    }

    HandleHotkeys();
    ReleaseDrags();

    if (g_visible) {
        // 每帧把鼠标钳制解除一次：游戏会不停重新 ClipCursor
        ClipCursor(nullptr);
        g_s = cfg::Scale();
        LayoutPanels();

        for (size_t i = 0; i < g_panels.size(); ++i) {
            DrawPanel(g_panels[i], true);
        }
        for (size_t i = 0; i < g_panels.size(); ++i) {
            DrawPanel(g_panels[i], false);
        }

        // 鼠标指针（游戏自己把光标藏了，这里补一个）
        float mx = input::X();
        float my = input::Y();
        render::RoundRect(mx, my, 2.0f, 12.0f, 1.0f, theme::kAccent);
        render::RoundRect(mx - 3.0f, my, 8.0f, 3.0f, 1.5f, theme::kAccent);
    }

    DrawHud();
}

}  // namespace gui
}  // namespace xiaoyu
