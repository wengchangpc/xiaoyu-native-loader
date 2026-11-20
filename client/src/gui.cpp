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
#include "modules.h"
#include "render.h"
#include "sdk.h"

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
// 顶部工具条（搜索框 + 配置档按钮）的尺寸，单位同为 GUI 单位
const float kToolH = 22.0f;
const float kSearchW = 150.0f;

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

// 定义在下面（通知那块），但工具条/绑定/HUD 复位都要用，先声明一下。
void PushToast(const wchar_t* text);

std::vector<Panel> g_panels;
std::vector<char> g_expanded;      // 按 feature 下标
std::vector<float> g_scroll_saved; // 未用，预留

// 折叠/滚动必须放在 panel 外：g_panels 每帧重建，
// 状态存在 Panel 里会在下一帧被 LayoutPanels 清掉。
bool g_collapsed[GROUP_COUNT] = {false};
float g_scroll[GROUP_COUNT] = {0.0f};

bool g_visible = false;
float g_s = 2.0f;

// HUD 屏内编辑模式。开着的时候面板是收起的（不然两种交互叠一起），
// 但鼠标仍然要归我们 —— 见 gui::WantsMouse()。
bool g_hud_edit = false;

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

// 绑定完成后 300ms 内不再响应模块快捷键。
// 不这么做的话，刚绑完的那个键在同一次按下里还会走一遍"模块快捷键"循环，
// 于是"点绑定 → 按 K → K 绑上了 → K 顺手把模块也切了一次"，看起来像绑定有 bug。
ULONGLONG g_bind_guard_at = 0;

// 本帧滚轮量。OnFrame 开头从 input 消费一次，面板绘制时读这个缓存。
//
// ★ 以前这里直接调 input::Wheel()，而 input 那边的累积值**从来不清零**：
//   滚一格之后 g_wheel 一直是 1，面板每帧都拿这个 1 再滚一次，
//   于是"滚一下就直接跳到底/跳到顶"。现在改成每帧消费一次。
float g_wheel_frame = 0.0f;

// HUD 拖动状态：g_hud_drag 是正在拖的 feature 下标（-1 = 没在拖），
// 另存一份按下点相对元素左上角的偏移，拖动时元素才不会"跳"到鼠标中心。
int g_hud_drag = -1;
float g_hud_drag_dx = 0.0f;
float g_hud_drag_dy = 0.0f;

// 进/出编辑模式后的一小段静默期。点「HUD 布局」那一下本身就是一次鼠标按下，
// 不挡的话同一帧就会把鼠标底下的元素抓起来开始拖。
ULONGLONG g_hud_edit_guard_at = 0;

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

// 前台窗口是不是本进程的（= 游戏）。
// GetAsyncKeyState 读的是全局键盘状态，在别的窗口打字时它照样为真，
// 所以任何"按某键触发功能"的地方都必须先过这一关，否则绑了字母键的模块
// 会在浏览器里打字时乱开乱关。
bool GameForeground() {
    HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// 虚拟键 -> 人能认的名字。MapVirtualKeyW 只对打得出字符的键有用，
// F1/F12、Shift、方向键这些全是空白或乱码，绑定列表里就成了 "0x70"。
const wchar_t* BindKeyName(int vk) {
    static wchar_t buf[16];
    switch (vk) {
        case 0: return L"\u672A\u7ED1\u5B9A";
        case VK_LBUTTON: return L"\u5DE6\u952E";
        case VK_RBUTTON: return L"\u53F3\u952E";
        case VK_MBUTTON: return L"\u4E2D\u952E";
        case VK_SPACE: return L"Space";
        case VK_TAB: return L"Tab";
        case VK_RETURN: return L"Enter";
        case VK_BACK: return L"Backspace";
        case VK_DELETE: return L"Delete";
        case VK_INSERT: return L"Insert";
        case VK_HOME: return L"Home";
        case VK_END: return L"End";
        case VK_PRIOR: return L"PgUp";
        case VK_NEXT: return L"PgDn";
        case VK_UP: return L"\u4E0A";
        case VK_DOWN: return L"\u4E0B";
        case VK_LEFT: return L"\u5DE6";
        case VK_RIGHT: return L"\u53F3";
        case VK_LSHIFT: return L"LShift";
        case VK_RSHIFT: return L"RShift";
        case VK_LCONTROL: return L"LCtrl";
        case VK_RCONTROL: return L"RCtrl";
        case VK_LMENU: return L"LAlt";
        case VK_RMENU: return L"RAlt";
        case VK_CAPITAL: return L"Caps";
        case VK_OEM_1: return L";";
        case VK_OEM_2: return L"/";
        case VK_OEM_3: return L"`";
        case VK_OEM_4: return L"[";
        case VK_OEM_5: return L"\\";
        case VK_OEM_6: return L"]";
        case VK_OEM_7: return L"'";
        case VK_OEM_PLUS: return L"=";
        case VK_OEM_MINUS: return L"-";
        case VK_OEM_COMMA: return L",";
        case VK_OEM_PERIOD: return L".";
        default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(buf, L"F%d", vk - VK_F1 + 1);
        return buf;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        buf[0] = (wchar_t)vk;
        buf[1] = 0;
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        swprintf_s(buf, L"Num%d", vk - VK_NUMPAD0);
        return buf;
    }
    const UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
    if (ch >= 32 && ch < 127) {
        buf[0] = (wchar_t)ch;
        buf[1] = 0;
        return buf;
    }
    swprintf_s(buf, L"0x%02X", vk);
    return buf;
}

// ★ catalog 里 PK_SLIDER 的第三个浮点数（struct 里叫 vstep）不是步长，
// 是生成器从 ClickGui.py 原样搬过来的第 5 个字段：
//     ('X 位置', 'posx', 0.0, 1.0, 0.471)
// 而 0.471 正好等于 FPS.posx 的出厂值 0.471041713 —— 原版写这个数就是为了
// 让滑块落在模块真实的默认位置上。把它当步长用会同时坏两件事：
//   1) 没调过的滑块显示**量程中点**（posx 变 0.5、颜色变灰），不是原版位置；
//   2) 滑块一跳一大格 —— posx 每格 0.471，等于只剩 0 / 0.471 / 0.942 三档。
// 原版自己也不是拿它当步长的：ClickGui._getDisplayStep() 会按量程重算
// （span<=10 → 0.01），所以这里同样改成"当默认值 + 步长按量程现算"。
//
// 注意这个只影响面板里滑块的显示值。模块实际读参数走 modules.cpp 的
// ParamOf()，那边每个调用点都自带出厂常量，不受这里影响；
// HUD 要精确对齐原版默认值则用下面的 FeatParamUser()。
float ClampToRange(const ParamInfo& p, float v) {
    if (v < p.vmin) v = p.vmin;
    if (v > p.vmax) v = p.vmax;
    return v;
}

float ParamDefault(int pi) {
    const ParamInfo& p = kParams[pi];
    if (p.kind != PK_SLIDER) return 0.0f;
    return ClampToRange(p, p.vstep);
}

// 拖动/显示用的步长。按量程分档就够了，反正原版也没给真步长。
float ParamStep(int pi) {
    const ParamInfo& p = kParams[pi];
    const float range = p.vmax - p.vmin;
    if (range <= 0.0f) return 0.0f;
    if (range <= 1.0f) return 0.01f;
    if (range <= 4.0f) return 0.05f;
    if (range <= 40.0f) return 0.5f;
    return 1.0f;
}

float SnapParam(int pi, float v) {
    const ParamInfo& p = kParams[pi];
    const float step = ParamStep(pi);
    if (step > 0.0f) {
        v = p.vmin + floorf((v - p.vmin) / step + 0.5f) * step;
    }
    return ClampToRange(p, v);
}

float ParamGet(int pi) { return cfg::ParamValue(pi, ParamDefault(pi)); }

// 按属性名找参数下标，找不到返回 -1。
int FeatParamIndex(int fi, const wchar_t* attr) {
    if (fi < 0) return -1;
    const FeatureInfo& f = kFeatures[fi];
    for (int k = 0; k < f.paramCount; ++k) {
        const int pi = f.paramFirst + k;
        if (kParams[pi].attr != nullptr && wcscmp(kParams[pi].attr, attr) == 0) {
            return pi;
        }
    }
    return -1;
}

// 按属性名取某个模块的参数值。HUD 绘制要用（位置/缩放/颜色都在参数里），
// 按下标取太脆 —— catalog 里参数顺序是原版 Python 的字典序。
float FeatParam(int fi, const wchar_t* attr, float def) {
    const int pi = FeatParamIndex(fi, attr);
    if (pi < 0) return def;
    return ParamGet(pi);
}

// 只在"玩家真的调过"时才用参数值，否则用调用方给的出厂默认。
//
// 为什么不直接 ParamGet：catalog 里的 vmin/vmax 是**滑块量程**，
// ParamDefault 取的是量程中点 —— posx 会变成 0.5（HUD 跑到屏幕正中）、
// color_r/g/b 会变成 0.5（青色强调色变成灰）。量程中点不等于原版默认值。
// 原版 Python 里这些字段都是写死的（FPS posx=0.4711、CPS posy=0.9475……），
// 没调过时必须回退到那些数值，HUD 才会停在原来的位置。
float FeatParamUser(int fi, const wchar_t* attr, float def) {
    const int pi = FeatParamIndex(fi, attr);
    if (pi < 0) return def;
    if (!cfg::ParamIsSet(pi)) return def;
    return cfg::ParamValue(pi, def);
}

// HUD 元素的通用外观。原版每个 HUD 模块都带这一组配置项
// （posx/posy/scale/alpha/bg/bg_alpha/color_r/g/b），面板里也给了滑块，
// 之前渲染端坐标全写死，滑块等于白给。
struct HudLook {
    float x, y;  // 左上角像素坐标（ArrayList 的 x 是右边缘，那边单独处理）
    float scale;
    float alpha;
    float r, g, b;
    bool bg;
    float bg_alpha;
};

HudLook ReadHudLook(int fi, float defx, float defy, float defscale,
                    const D2D1_COLOR_F& defcol) {
    HudLook L;
    L.scale = FeatParamUser(fi, L"scale", defscale);
    if (L.scale < 0.3f) L.scale = 0.3f;
    if (L.scale > 4.0f) L.scale = 4.0f;
    L.alpha = FeatParamUser(fi, L"alpha", 1.0f);
    if (L.alpha < 0.05f) L.alpha = 0.05f;
    if (L.alpha > 1.0f) L.alpha = 1.0f;
    L.x = FeatParamUser(fi, L"posx", defx) * (float)render::Width();
    L.y = FeatParamUser(fi, L"posy", defy) * (float)render::Height();
    L.r = FeatParamUser(fi, L"color_r", defcol.r);
    L.g = FeatParamUser(fi, L"color_g", defcol.g);
    L.b = FeatParamUser(fi, L"color_b", defcol.b);
    L.bg = FeatParamUser(fi, L"bg", 0.0f) > 0.5f;
    L.bg_alpha = FeatParamUser(fi, L"bg_alpha", 0.35f);
    if (L.bg_alpha < 0.0f) L.bg_alpha = 0.0f;
    if (L.bg_alpha > 1.0f) L.bg_alpha = 1.0f;
    return L;
}

// 把一块 HUD 钳在屏幕内：posx 拖到 1.0 时不至于整块跑到屏幕外找不回来。
void ClampHudBox(float* x, float* y, float w, float h) {
    const float vw = (float)render::Width();
    const float vh = (float)render::Height();
    if (*x + w > vw) *x = vw - w;
    if (*y + h > vh) *y = vh - h;
    if (*x < 0.0f) *x = 0.0f;
    if (*y < 0.0f) *y = 0.0f;
}

// 紧凑 HSV→RGB（s=0.55, v=1），ArrayList 的 rgb 色彩模式用。
// 不用 D2D 的色相工具是因为这里只要一个纯函数，逐行算色相即可。
D2D1_COLOR_F HueColor(float hue, float alpha) {
    hue = hue - floorf(hue);
    const float h6 = hue * 6.0f;
    const int i = (int)h6;
    const float f = h6 - (float)i;
    const float s = 0.55f;
    const float v = 1.0f;
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    switch (i % 6) {
        case 0: return render::RgbaF(v, t, p, alpha);
        case 1: return render::RgbaF(q, v, p, alpha);
        case 2: return render::RgbaF(p, v, t, alpha);
        case 3: return render::RgbaF(p, q, v, alpha);
        case 4: return render::RgbaF(t, p, v, alpha);
        default: return render::RgbaF(v, p, q, alpha);
    }
}

// ── 搜索过滤 ────────────────────────────────────────────────────
// 116 个模块分 7 栏，找东西全靠滚。这里给个输入框做子串过滤。
// 匹配英文名（原版模块标识就是英文，日志里也是英文，用户能对上）+
// 中文名（原样子串，万一输入法能用）。大小写不敏感。
wchar_t g_search[32] = {0};
int g_search_len = 0;
bool g_search_focus = false;

// 单击的一次性锁：工具栏先处理，面板再看到同一次按下时就不能重复响应，
// 否则点搜索框会顺手把底下那一行的模块开关也切了。
bool g_click_used = false;

wchar_t UpperAscii(wchar_t c) {
    return (c >= L'a' && c <= L'z') ? (wchar_t)(c - L'a' + L'A') : c;
}

bool ContainsCI(const wchar_t* hay, const wchar_t* needle) {
    if (hay == nullptr || needle == nullptr || needle[0] == 0) return true;
    const size_t n = wcslen(needle);
    for (const wchar_t* p = hay; *p != 0; ++p) {
        size_t i = 0;
        while (i < n && p[i] != 0 && UpperAscii(p[i]) == UpperAscii(needle[i])) ++i;
        if (i == n) return true;
    }
    return false;
}

bool FeatMatch(int i) {
    if (g_search_len == 0) return true;
    if (ContainsCI(kFeatures[i].en, g_search)) return true;
    const wchar_t* zh = kFeatures[i].zh;
    return zh != nullptr && wcsstr(zh, g_search) != nullptr;
}

void SearchAppend(wchar_t c) {
    const int cap = (int)(sizeof(g_search) / sizeof(g_search[0])) - 1;
    if (g_search_len >= cap) return;
    g_search[g_search_len++] = c;
    g_search[g_search_len] = 0;
}

void SearchBackspace() {
    if (g_search_len <= 0) return;
    g_search[--g_search_len] = 0;
}

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
        if (!FeatMatch(i)) continue;
        h += FeatureHeight(i);
    }
    return h;
}

// 面板缩放。原版 nilore 是 120 单位宽的设计，单位乘缩放得到像素。
// 以前默认写死 2.0，7 个分类在 1231x741 的窗口里要摆成 4 列 2 行，
// 高度还得吃满 64% —— 打开就是"一屏幕面板"。现在默认跟随窗口尺寸，
// 以 1040x660 为基准取两方向里较小的比例，并在 0.85~1.7 之间夹住。
float AutoScale() {
    const float w = (float)render::Width();
    const float h = (float)render::Height();
    if (w < 100.0f || h < 100.0f) return 1.0f;
    float sw = w / 1040.0f;
    float sh = h / 660.0f;
    float s = sw < sh ? sw : sh;
    if (s < 0.85f) s = 0.85f;
    if (s > 1.7f) s = 1.7f;
    return s;
}

// 列宽按内容算：中文模块名比英文长，固定 120 单位会把名字和右侧箭头挤在一起。
// 结果缓存起来，只在语言切换时重算 —— 每帧 Measure 116 条文本的开销不值得。
float g_group_w[GROUP_COUNT] = {0.0f};
bool g_group_w_ready = false;
bool g_group_w_zh = false;

void RebuildGroupWidths() {
    if (g_group_w_ready && g_group_w_zh == cfg::Chinese()) return;

    // DirectWrite 还没起来时 Measure 全是 0，这时候不能缓存
    float probe = render::Measure(GroupName(0), 11.0f, true);
    if (probe <= 0.0f) return;

    for (int g = 0; g < GROUP_COUNT; ++g) {
        float need = kPanelW;
        for (int i = 0; i < kFeatureCount; ++i) {
            if (kFeatures[i].group != g) continue;
            // 8 左内边距 + 名字 + 20 给右侧展开箭头
            float rw = 8.0f + render::Measure(FeatName(i), 10.5f, false) + 20.0f;
            if (rw > need) need = rw;
            const FeatureInfo& f = kFeatures[i];
            for (int k = 0; k < f.paramCount; ++k) {
                const ParamInfo& pi = kParams[f.paramFirst + k];
                // 参数行：12 左 + 标签 + 48 给右侧的数值 / 开关 / 枚举
                float pw = 12.0f + render::Measure(pi.label, 9.5f, false) + 48.0f;
                if (pw > need) need = pw;
            }
        }
        // 标题行：8 左 + 标题 + 22（折叠箭头 + 右边距）
        float hw = 8.0f + render::Measure(GroupName(g), 11.0f, true) + 22.0f;
        if (hw > need) need = hw;

        if (need < kPanelW) need = kPanelW;
        if (need > 200.0f) need = 200.0f;   // 太宽反而难看，超了就让名字自己截
        g_group_w[g] = need;
    }
    g_group_w_ready = true;
    g_group_w_zh = cfg::Chinese();
    float wmin = 1e9f, wmax = 0.0f;
    for (int g = 0; g < GROUP_COUNT; ++g) {
        if (g_group_w[g] <= 0.0f) continue;
        if (g_group_w[g] < wmin) wmin = g_group_w[g];
        if (g_group_w[g] > wmax) wmax = g_group_w[g];
    }
    LogLine("[gui] 列宽自适应：%.0f ~ %.0f 单位，缩放 %.2f", wmin, wmax, g_s);
}

int CountGroupFeatures(int gi) {
    int n = 0;
    for (int i = 0; i < kFeatureCount; ++i) {
        if (kFeatures[i].group != gi) continue;
        if (!FeatMatch(i)) continue;
        ++n;
    }
    return n;
}

// ── 布局：原版 _createNiloreLayout 的移植 ────────────────────────
void LayoutPanels() {
    // 配置里 scale=0 表示自动，按窗口大小算；写了具体值就用用户的
    float sc = cfg::Scale();
    if (sc <= 0.0f) sc = AutoScale();
    g_s = sc;
    RebuildGroupWidths();
    const float pad = kPad * g_s;
    const float gap = kGap * g_s;
    const float headH = kHeadH * g_s;
    // 估算每行能放几个时用最宽的那一列，宁可多折一行也不让列挤出去
    float colW = kPanelW * g_s;
    for (int g = 0; g < GROUP_COUNT; ++g) {
        float cw = g_group_w[g] * g_s;
        if (cw > colW) colW = cw;
    }
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

    // 单栏内容上限 52% 的高度，整体（含多行）只用可用高度的 82% ——
    // 这样打开面板时四周一定留有游戏的画面，不会看起来像全屏菜单。
    // 顶部要给工具条（搜索框 + 配置档）留出位置，否则居中之后会压在它上面
    const float tool_h = (kToolH + kGap) * g_s;
    float limit_body = viewH * 0.52f;
    float availH = (viewH - pad * 2.0f - tool_h) * 0.82f - gap * (row_count - 1);
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
        p.w = g_group_w[g] * g_s;
        p.row_count = CountGroupFeatures(g);
        p.max_body = max_body;
        p.content_h = GroupContentHeight(g) * g_s;
        p.collapsed = g_collapsed[g];
        p.scroll = g_scroll[g];
        // 内容高度会随参数展开/收起变化，旧的 scroll 可能已经越界，
        // 越界时拖动条会停在半空、列表底部出现空白，看着像"滚动坏了"。
        float max_scroll = p.content_h - max_body;
        if (max_scroll < 0.0f) max_scroll = 0.0f;
        if (p.scroll > max_scroll) p.scroll = max_scroll;
        if (p.scroll < 0.0f) p.scroll = 0.0f;
        g_scroll[g] = p.scroll;
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
    if (offset_y < tool_h) offset_y = tool_h;

    // 落位：用户拖过的用保存值，否则按算法居中
    for (size_t r = 0; r < row_tops.size(); ++r) {
        int cnt = row_counts[r];
        int start = row_starts[r];
        float row_w = (float)(cnt - 1) * gap;
        for (int k = 0; k < cnt; ++k) row_w += g_panels[start + k].w;
        float x0 = (viewW - row_w) * 0.5f;
        if (x0 < pad) x0 = pad;
        float ry = row_tops[r] + offset_y;
        float cx = x0;
        for (int k = 0; k < cnt; ++k) {
            Panel& p = g_panels[start + k];
            p.x = cx;
            p.y = ry;
            cx += p.w + gap;
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
    const float r = kRadius * s;
    const float head_h = kHeadH * s;
    const D2D1_COLOR_F col_head = hover_head ? theme::kHeadHover : theme::kHeadBg;

    // 柔和阴影：由外向内叠三层半透明圆角矩形。
    // 原来画的是一个硬边纯黑矩形（kShadow 0.30），在游戏画面上看起来
    // 就是个套在面板外面的黑框，非常脏。三层叠加才有渐变感。
    for (int i = 3; i >= 1; --i) {
        float d = kShadow * s * (float)i / 3.0f;
        float a = 0.05f * (float)(4 - i);   // 外 0.05 内 0.15
        render::RoundRect(p.x - d, p.y - d + 1.5f * s, p.w + d * 2.0f, p.h + d * 2.0f,
                          r + d, render::RgbaF(0.0f, 0.0f, 0.0f, a));
    }

    // 面板整体一次画完：四个角自然都是圆的，栏体不再需要单独铺底色
    render::RoundRect(p.x, p.y, p.w, p.h, r, theme::kBody);

    // 栏头：先按圆角画，再用矩形把下半部分的圆角填平。
    // 这样上面两角跟着面板圆角走，下面和栏体无缝接上 ——
    // 之前直接用 PushClip + 直角矩形，顶部两角会把面板的圆角盖成直角。
    render::RoundRect(p.x, p.y, p.w, head_h, r, col_head);
    render::Rect(p.x, p.y + head_h * 0.5f, p.w, head_h * 0.5f, col_head);

    // 顶部强调线：两端各让出一点，避免直角戳出圆角轮廓
    render::Rect(p.x + r * 0.75f, p.y, p.w - r * 1.5f,
                 (s > 1.5f ? 1.6f : 1.0f) * s, theme::kAccentD);

    // 标题
    float title_size = 11.0f * s;
    render::Text(GroupName(p.group), p.x + 8.0f * s, p.y,
                 p.w - 28.0f * s, kHeadH * s, title_size, theme::kAccent, ALIGN_LEFT,
                 true);

    // 折叠箭头
    const wchar_t* arrow = p.collapsed ? L"\u25B8" : L"\u25BE";
    render::Text(arrow, p.x + p.w - 16.0f * s, p.y, 12.0f * s, kHeadH * s,
                 title_size, theme::kArrow, ALIGN_LEFT, false);

    // 外描边放最后画：先画会被栏头底色盖掉顶边那一截
    render::RoundRectStroke(p.x, p.y, p.w, p.h, r, 1.0f, theme::kRim);
}

// 滚动条几何。原来滚动条是"只画不响应"的 —— g_drag_scrollbar 从来没被赋值，
// 所以拖着没反应、滚轮又因为窗口没焦点收不到消息，等于完全滚不动。
struct BarGeom {
    bool valid = false;
    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
    float thumb_h = 0.0f;
    float max_scroll = 0.0f;
};

BarGeom ScrollbarGeom(const Panel& p) {
    BarGeom g;
    if (p.collapsed) return g;
    if (p.content_h <= p.max_body + 0.5f) return g;

    const float s = g_s;
    float view_h = (p.h - kHeadH * s) - 6.0f * s;
    if (view_h < 12.0f * s) return g;

    g.bw = kScrollbarW * s;
    if (g.bw < 4.0f) g.bw = 4.0f;
    g.bx = p.x + p.w - g.bw - 2.0f * s;
    g.by = p.y + kHeadH * s + 2.0f * s;
    g.bh = view_h - 4.0f * s;
    if (g.bh < 8.0f) return g;

    float ratio = p.max_body / p.content_h;
    if (ratio > 1.0f) ratio = 1.0f;
    g.thumb_h = g.bh * ratio;
    float min_thumb = 14.0f * s;
    if (g.thumb_h < min_thumb) g.thumb_h = min_thumb;
    if (g.thumb_h > g.bh) g.thumb_h = g.bh;

    g.max_scroll = p.content_h - p.max_body;
    if (g.max_scroll < 1.0f) g.max_scroll = 1.0f;
    g.valid = true;
    return g;
}

void DrawScrollbar(const Panel& p, bool hot) {
    BarGeom g = ScrollbarGeom(p);
    if (!g.valid) return;
    float t = p.scroll / g.max_scroll;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float ty = g.by + (g.bh - g.thumb_h) * t;
    render::RoundRect(g.bx, g.by, g.bw, g.bh, g.bw * 0.5f, theme::kScrollTrack);
    render::RoundRect(g.bx, ty, g.bw, g.thumb_h, g.bw * 0.5f,
                      hot ? theme::kScrollThumbHot : theme::kScrollThumb);
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
                // 小数位数跟着真实步长走，别再拿 catalog 里那个"默认值"当步长判断
                const float dstep = ParamStep(pi);
                if (dstep >= 1.0f) {
                    swprintf_s(buf, L"%.0f", v);
                } else if (dstep >= 0.1f) {
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
                const int vk = cfg::ModuleBind(fi);
                wchar_t buf[64];
                if (g_bind_target == fi) {
                    wcscpy_s(buf, cfg::Chinese()
                                      ? L"\u6309\u952E\u2026 (Del \u53D6\u6D88)"
                                      : L"press key (Del = none)");
                } else {
                    wcscpy_s(buf, BindKeyName(vk));
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
    // 右键点任意一条参数 = 只把这一条恢复默认（模块整体复位见模块行的右键）
    if (input::Pressed(1) && inside) {
        if (p.kind == PK_KEYBIND) {
            cfg::SetModuleBind(fi, 0);
            if (g_bind_target == fi) g_bind_target = -1;
        } else {
            cfg::ClearParam(pi);
        }
        wchar_t msg[96];
        swprintf_s(msg, L"%ls \u00B7 \u5DF2\u6062\u590D\u9ED8\u8BA4", p.label);
        PushToast(msg);
        return;
    }

    if (input::Pressed(0) && !g_click_used) {
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
                    cfg::SetParamValue(pi, SnapParam(pi, p.vmin + t * (p.vmax - p.vmin)));
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
                // 再点一次同一行 = 取消这次捕获（不用去按 Delete）
                if (inside) g_bind_target = (g_bind_target == fi) ? -1 : fi;
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
        cfg::SetParamValue(pi, SnapParam(pi, p.vmin + t * (p.vmax - p.vmin)));
    }
}

// ── 顶部工具条：搜索框 + 配置档 ──────────────────────────────────
void ToolbarRect(float* x, float* y, float* w, float* h) {
    *x = kPad * g_s;
    *y = kPad * g_s;
    *w = kSearchW * g_s;
    *h = kToolH * g_s;
}

void ProfileRect(int slot, float* x, float* y, float* w, float* h) {
    float bx, by, bw, bh;
    ToolbarRect(&bx, &by, &bw, &bh);
    const float bwid = 20.0f * g_s;
    const float gap = 4.0f * g_s;
    *x = bx + bw + kGap * g_s + (float)(slot - 1) * (bwid + gap);
    *y = by;
    *w = bwid;
    *h = bh;
}

bool InRect(float mx, float my, float x, float y, float w, float h) {
    return mx >= x && mx <= x + w && my >= y && my <= y + h;
}

// 工具条最右边那个「HUD 布局」按钮：点一下收起面板、进屏内拖拽模式。
void EditBtnRect(float* x, float* y, float* w, float* h) {
    float px, py, pw, ph;
    ProfileRect(3, &px, &py, &pw, &ph);
    *x = px + pw + 8.0f * g_s;
    *y = py;
    *w = 46.0f * g_s;
    *h = ph;
}

void DrawToolbar(bool interactive) {
    const float s = g_s;
    float bx, by, bw, bh;
    ToolbarRect(&bx, &by, &bw, &bh);
    const float mx = input::X();
    const float my = input::Y();

    float px[4], py[4], pw[4], ph[4];
    for (int i = 1; i <= 3; ++i) ProfileRect(i, &px[i], &py[i], &pw[i], &ph[i]);
    float ex, ey, ew, eh;
    EditBtnRect(&ex, &ey, &ew, &eh);

    if (interactive) {
        if (input::Pressed(0) && !g_click_used) {
            for (int i = 1; i <= 3; ++i) {
                if (!InRect(mx, my, px[i], py[i], pw[i], ph[i])) continue;
                cfg::SwitchProfile(i);
                g_search_focus = false;
                g_click_used = true;
                {
                    wchar_t msg[64];
                    swprintf_s(msg, L"\u914D\u7F6E\u6863 %d", i);
                    PushToast(msg);
                }
                return;
            }
            if (InRect(mx, my, ex, ey, ew, eh)) {
                // 进 HUD 拖拽模式：先开编辑态再收面板，
                // 顺序反了会有一帧 WantsMouse() 变假，覆盖层把鼠标还给游戏。
                SetHudEdit(true);
                SetVisible(false);
                g_click_used = true;
                PushToast(cfg::Chinese() ? L"\u8FDB\u5165 HUD \u5E03\u5C40\u6A21\u5F0F"
                                         : L"HUD layout mode");
                return;
            }
            if (InRect(mx, my, bx, by, bw, bh)) {
                g_search_focus = true;
                g_click_used = true;   // 抢掉这次点击，别再落到下面的面板上
            } else {
                g_search_focus = false;
            }
        }
        if (!g_search_focus) return;
        // 有焦点就把本帧按下、能转成字符的键吃进来
        for (int vk = 1; vk < 256; ++vk) {
            if (!input::KeyPressed(vk)) continue;
            if (vk == VK_BACK) { SearchBackspace(); continue; }
            if (vk == VK_RETURN || vk == VK_ESCAPE) { g_search_focus = false; continue; }
            const UINT ch = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
            if (ch >= 0x20 && ch < 0x7F) SearchAppend((wchar_t)ch);
        }
        return;
    }

    // ── 绘制 ──
    render::RoundRect(bx, by, bw, bh, 4.0f * s,
                      g_search_focus ? theme::kRowHover : theme::kRow);
    if (g_search_focus) {
        render::RoundRectStroke(bx, by, bw, bh, 4.0f * s, 1.0f, theme::kAccent);
    }
    if (g_search_len == 0) {
        render::Text(cfg::Chinese() ? L"\u641C\u7D22\u6A21\u5757\u2026" : L"Search...",
                     bx + 6.0f * s, by, bw - 12.0f * s, bh, 10.0f * s,
                     theme::kDim, ALIGN_LEFT, false);
    } else {
        render::Text(g_search, bx + 6.0f * s, by, bw - 12.0f * s, bh, 10.0f * s,
                     theme::kOff, ALIGN_LEFT, false);
    }
    if (g_search_focus) {
        // 光标：跟着已输入文本的宽度走
        const float tw = render::Measure(g_search, 10.0f * s, false);
        render::Rect(bx + 7.0f * s + tw, by + 5.0f * s, 1.5f * s, bh - 10.0f * s,
                     theme::kAccent);
    }

    // 配置档按钮：左键直接切档（切之前会自动保存当前档）
    const int cur = cfg::CurrentProfile();
    for (int i = 1; i <= 3; ++i) {
        const bool hot = InRect(mx, my, px[i], py[i], pw[i], ph[i]);
        const bool act = (cur == i);
        render::RoundRect(px[i], py[i], pw[i], ph[i], 3.0f * s,
                          act ? theme::kAccent : (hot ? theme::kRowHover : theme::kRow));
        wchar_t lb[2] = {(wchar_t)(L'0' + i), 0};
        render::Text(lb, px[i], py[i], pw[i], ph[i], 9.5f * s,
                     act ? theme::kHeadBg : theme::kOff, ALIGN_CENTER, false);
    }

    // HUD 布局按钮：点一下收起面板，直接在画面上拖 HUD 元素摆位置
    const bool ehot = InRect(mx, my, ex, ey, ew, eh);
    render::RoundRect(ex, ey, ew, eh, 3.0f * s, ehot ? theme::kRowHover : theme::kRow);
    render::Text(cfg::Chinese() ? L"HUD \u5E03\u5C40" : L"HUD layout", ex, ey, ew, eh,
                 9.5f * s, ehot ? theme::kAccent : theme::kOff, ALIGN_CENTER, false);
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
        // 上下各让出 1 个单位：原来行是满贴的，整栏看起来就是一块死黑，
        // 加缝隙之后每一行才有独立感（原版 ROW_INSET=1 也是这个意思）。
        const float iy = kInset * s;
        const float rh = h - iy * 2.0f;
        if (on) {
            render::RoundRect(row_x, y + iy, w, rh, 2.0f * s, theme::kRow);
        } else if (inside) {
            render::RoundRect(row_x, y + iy, w, rh, 2.0f * s, theme::kRowHover);
        }

        const FeatureInfo& f = kFeatures[fi];
        const float label_size = 10.5f * s;

        // 绑了键的模块在行尾挂一个小键帽，不用展开参数就能看见绑的是什么。
        // 之前只有展开到「功能快捷键」那一行才知道，绑多了根本记不住。
        const int bvk = (fi == g_bind_target) ? 0 : cfg::ModuleBind(fi);
        float name_w = w - 28.0f * s;
        if (bvk != 0) {
            const wchar_t* kn = BindKeyName(bvk);
            const float ks = 8.5f * s;
            const float kw = render::Measure(kn, ks, false) + 9.0f * s;
            const float kh = 12.0f * s;
            const float kx = row_x + w - 20.0f * s - kw;
            render::RoundRect(kx, y + (h - kh) * 0.5f, kw, kh, 2.0f * s, theme::kRowHover);
            render::Text(kn, kx, y, kw, h, ks, theme::kDim, ALIGN_CENTER, false);
            name_w = kx - (row_x + 8.0f * s) - 4.0f * s;
            if (name_w < 10.0f * s) name_w = 10.0f * s;
        }

        render::Text(FeatName(fi), row_x + 8.0f * s, y, name_w, h, label_size,
                     on ? theme::kAccent : theme::kOff, ALIGN_LEFT, false);

        if (f.paramCount > 0) {
            const wchar_t* ar = g_expanded[fi] ? L"\u25BE" : L"\u25B8";
            render::Text(ar, row_x + w - 18.0f * s, y, 14.0f * s, h, label_size,
                         theme::kArrow, ALIGN_LEFT, false);
        }
    } else {
        if (input::Pressed(0) && !g_click_used && inside && g_drag_panel < 0) {
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
            // ★ 必须抢掉这次按下。以前这里没置位，一次点击会继续往下
            //   传给后面的面板/控件：两个面板一重叠，点一下会同时切两个模块，
            //   或者切完开关接着又被滚动条吃掉。
            g_click_used = true;
        }
        // 右键点模块行 = 把这个模块的所有参数和快捷键恢复出厂。
        // 调参数调到不知道动了哪个的时候，比逐个滑块往回拖快得多。
        if (input::Pressed(1) && !g_click_used && inside && g_drag_panel < 0) {
            const FeatureInfo& f = kFeatures[fi];
            for (int k = 0; k < f.paramCount; ++k) cfg::ClearParam(f.paramFirst + k);
            cfg::SetModuleBind(fi, 0);
            if (g_bind_target == fi) g_bind_target = -1;
            wchar_t msg[96];
            swprintf_s(msg, L"%ls \u00B7 \u5DF2\u6062\u590D\u51FA\u5382", FeatName(fi));
            PushToast(msg);
            LogLine("[gui] reset feature %ls", f.en);
            g_click_used = true;
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

    // ★ 这里曾经写成 if (!interactive) 画 chrome —— 和 DrawFeatureRow 的语义正好相反。
    //   DrawFeatureRow / DrawParamRow 是 interactive=true 时"画"、false 时"处理点击"，
    //   于是两轮遍历变成：第一轮画出所有行视觉（但没有面板底色），
    //   第二轮才铺底色 —— 铺底把第一轮画的东西整块盖掉，面板就只剩底色和标题。
    //   现在统一成：interactive=true 这一轮 = 画（先铺 chrome 再画行），
    //   interactive=false 这一轮 = 纯输入处理，一个像素都不画。
    if (interactive) {
        DrawPanelChrome(p, head_inside || g_drag_panel == p.group);
    } else {
        // 栏头：按下开始拖，松开若没怎么动就当作折叠
        if (input::Pressed(0) && !g_click_used && head_inside && g_drag_panel < 0) {
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

        // 滚轮（Raw Input + WM_MOUSEWHEEL，去重后每帧一个值，见 g_wheel_frame）
        if (body_inside && !p.collapsed && g_wheel_frame != 0.0f) {
            float max_scroll = p.content_h - p.max_body;
            if (max_scroll > 0.0f) {
                p.scroll -= g_wheel_frame * 28.0f * s;   // 原版 _niloreScrollAt 的步长
                if (p.scroll < 0.0f) p.scroll = 0.0f;
                if (p.scroll > max_scroll) p.scroll = max_scroll;
                g_scroll[p.group] = p.scroll;
            }
        }

        // 滚动条拖动 / 点轨道跳转
        {
            BarGeom bar = ScrollbarGeom(p);
            if (bar.valid && g_drag_scrollbar < 0 && g_drag_panel < 0) {
                // 命中区比视觉宽度左右各宽 3px，细条也好点
                float pad_x = 3.0f * s;
                bool in_track = mx >= bar.bx - pad_x && mx <= bar.bx + bar.bw + pad_x &&
                                my >= bar.by && my <= bar.by + bar.bh;
                if (input::Pressed(0) && !g_click_used && in_track) {
                    float t = p.scroll / bar.max_scroll;
                    float thumb_y = bar.by + (bar.bh - bar.thumb_h) * t;
                    bool on_thumb = my >= thumb_y && my <= thumb_y + bar.thumb_h;
                    g_drag_scrollbar = p.group;
                    g_drag_mouse_start = my;
                    if (!on_thumb) {
                        // 点在轨道空白处：滑块中心跳到鼠标位置
                        float span = bar.bh - bar.thumb_h;
                        float nt = span > 1.0f ? (my - bar.by - bar.thumb_h * 0.5f) / span
                                               : 0.0f;
                        if (nt < 0.0f) nt = 0.0f;
                        if (nt > 1.0f) nt = 1.0f;
                        p.scroll = nt * bar.max_scroll;
                        g_scroll[p.group] = p.scroll;
                    }
                    g_drag_scroll_start = p.scroll;
                }
            }
            if (g_drag_scrollbar == p.group) {
                if (input::Down(0)) {
                    BarGeom bg = ScrollbarGeom(p);
                    if (bg.valid) {
                        float span = bg.bh - bg.thumb_h;
                        if (span < 1.0f) span = 1.0f;
                        float ns = g_drag_scroll_start +
                                   (my - g_drag_mouse_start) / span * bg.max_scroll;
                        if (ns < 0.0f) ns = 0.0f;
                        if (ns > bg.max_scroll) ns = bg.max_scroll;
                        p.scroll = ns;
                        g_scroll[p.group] = ns;
                    }
                }
                if (input::Released(0)) g_drag_scrollbar = -1;
            }
        }

        if (p.collapsed) {
            p.scroll = 0.0f;
            g_scroll[p.group] = 0.0f;
        }
    }

    if (p.collapsed) return;

    // 栏体。面板底色已经由 DrawPanelChrome 整体铺好，这里只画内容和滚动条。
    // view_h 要扣掉底部 6 个单位的留白，否则滚动到底时行会盖住面板的圆角。
    float body_y = p.y + headH;
    float body_h = p.h - headH;
    float view_h = body_h - 6.0f * s;
    if (view_h < 0.0f) view_h = 0.0f;

    if (interactive) {
        BarGeom bar = ScrollbarGeom(p);
        bool hot = bar.valid && mx >= bar.bx - 3.0f * s && mx <= bar.bx + bar.bw + 3.0f * s &&
                   my >= bar.by && my <= bar.by + bar.bh;
        DrawScrollbar(p, hot || g_drag_scrollbar == p.group);
        render::PushClip(p.x, body_y, p.w, view_h);
    }

    float row_y = body_y + 3.0f * s;
    for (int i = 0; i < kFeatureCount; ++i) {
        if (kFeatures[i].group != p.group) continue;
        if (!FeatMatch(i)) continue;   // 搜索过滤：不匹配的行既不画也不占高度
        DrawFeatureRow(i, p, p.x + kInset * s, row_y, interactive);
    }

    if (interactive) render::PopClip();
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

// ── 模块开关通知 ────────────────────────────────────────────────
// 面板收起时看不到谁被打开了，切模块的时候在屏幕右侧弹一条，
// 2 秒后自己淡出。开关变化靠每帧对比上一次的快照，不需要模块自己去调。
struct NotifyItem {
    std::wstring text;
    bool on = false;
    ULONGLONG at = 0;
};

std::vector<NotifyItem> g_notifies;
std::vector<char> g_prev_on;
bool g_notify_was_on = false;

// 系统提示（绑定成功 / HUD 复位 / 切配置档 …）。
// 单独一条链，不能塞进 g_notifies —— 那个列表在 Notification 模块关掉时
// 会被整体清空，而"我刚按了 Delete 取消绑定"这类反馈跟那个模块没关系。
std::vector<NotifyItem> g_toasts;

void PushToast(const wchar_t* text) {
    if (text == nullptr) return;
    NotifyItem n;
    n.text = text;
    n.on = true;
    n.at = GetTickCount64();
    g_toasts.push_back(n);
    if (g_toasts.size() > 4) g_toasts.erase(g_toasts.begin());
}

void PruneToasts() {
    const ULONGLONG now = GetTickCount64();
    while (!g_toasts.empty() && now - g_toasts.front().at > 2400) {
        g_toasts.erase(g_toasts.begin());
    }
}

void TickNotifications() {
    PruneToasts();   // 系统提示跟 Notification 模块无关，先处理掉

    const int idx = FindFeatureByName(L"Notification");
    const bool enabled = (idx >= 0) && cfg::ModuleOn(idx);
    if (!enabled) {
        g_notifies.clear();
        g_notify_was_on = false;
        return;
    }

    const ULONGLONG now = GetTickCount64();

    // 刚打开通知功能时先同步一次快照，否则会把当时所有开着的模块全播一遍
    if (!g_notify_was_on) {
        g_notify_was_on = true;
        g_prev_on.assign(kFeatureCount, 0);
        for (int i = 0; i < kFeatureCount; ++i) g_prev_on[i] = cfg::ModuleOn(i) ? 1 : 0;
        g_notifies.clear();
        return;
    }

    if ((int)g_prev_on.size() != kFeatureCount) {
        g_prev_on.assign(kFeatureCount, 0);
        for (int i = 0; i < kFeatureCount; ++i) g_prev_on[i] = cfg::ModuleOn(i) ? 1 : 0;
        return;
    }

    for (int i = 0; i < kFeatureCount; ++i) {
        const char cur = cfg::ModuleOn(i) ? 1 : 0;
        if (cur == g_prev_on[i]) continue;
        g_prev_on[i] = cur;
        NotifyItem n;
        n.text = FeatName(i);
        if (!modules::Implemented(kFeatures[i].en)) {
            n.text += L"  ·  未实现";
        }
        n.on = (cur != 0);
        n.at = now;
        g_notifies.push_back(n);
        if (g_notifies.size() > 6) g_notifies.erase(g_notifies.begin());
    }

    while (!g_notifies.empty() && now - g_notifies.front().at > 2400) {
        g_notifies.erase(g_notifies.begin());
    }
}

void DrawNotifyList(const std::vector<NotifyItem>& list, float& y) {
    if (list.empty()) return;
    const float s = g_s;
    const ULONGLONG now = GetTickCount64();

    for (size_t i = 0; i < list.size(); ++i) {
        const NotifyItem& n = list[i];
        const ULONGLONG age = now - n.at;
        if (age > 2400) continue;

        float a = 1.0f;
        if (age < 120) a = (float)age / 120.0f;
        else if (age > 2000) a = 1.0f - (float)(age - 2000) / 400.0f;
        if (a < 0.0f) a = 0.0f;

        const float slide = (1.0f - a) * 24.0f * s;
        const float tw = render::Measure(n.text.c_str(), 10.0f * s, false);
        const float w = tw + 28.0f * s;
        const float hgt = 18.0f * s;
        const float x = (float)render::Width() - w - 10.0f * s + slide;

        render::RoundRect(x, y, w, hgt, 3.0f * s,
                          render::RgbaF(0.06f, 0.06f, 0.06f, 0.90f * a));
        render::Rect(x, y, 2.0f * s, hgt,
                     render::WithAlpha(n.on ? theme::kAccent : theme::kOff, a));
        render::Text(n.text.c_str(), x + 8.0f * s, y, w - 10.0f * s, hgt, 10.0f * s,
                     render::WithAlpha(n.on ? theme::kAccent : theme::kSub, a),
                     ALIGN_LEFT, false);
        y += hgt + 3.0f * s;
    }
}

void DrawNotifications() {
    if (g_notifies.empty() && g_toasts.empty()) return;
    float y = (float)render::Height() * 0.42f;
    // 系统提示排在前面：它是对刚刚那次操作的直接反馈
    DrawNotifyList(g_toasts, y);
    DrawNotifyList(g_notifies, y);
}

// ── HUD 屏内编辑 ────────────────────────────────────────────────
// 原版有个独立的 HudEditScreen：进编辑模式后每个 HUD 元素外面套一圈亮框，
// 直接拖着摆。这里做同一件事，写回去的还是模块自己的 posx/posy。
//
// 做法：DrawHud 每画完一个元素就把它的矩形登记进 g_hud_hits，
// 全部登记完之后再统一跑一次 HudEditInput。这样命中盒和画面严丝合缝，
// 不会出现"框在这、字在那"的错位。
struct HudHit {
    int fi = -1;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool right_anchor = false;   // ArrayList 的 posx 是右边缘，不是左边缘
};

const int kHudHitMax = 8;
HudHit g_hud_hits[kHudHitMax];
int g_hud_hit_count = 0;

void SetFeatParam(int fi, const wchar_t* attr, float v) {
    const int pi = FeatParamIndex(fi, attr);
    if (pi >= 0) cfg::SetParamValue(pi, v);
}

int HudHitAt(float mx, float my) {
    for (int i = g_hud_hit_count - 1; i >= 0; --i) {
        const HudHit& t = g_hud_hits[i];
        if (mx >= t.x - 4.0f && mx <= t.x + t.w + 4.0f && my >= t.y - 4.0f &&
            my <= t.y + t.h + 4.0f) {
            return i;
        }
    }
    return -1;
}

void HudEditBox(int fi, float x, float y, float w, float h, bool right_anchor) {
    if (fi < 0) return;
    if (g_hud_hit_count < kHudHitMax) {
        HudHit& t = g_hud_hits[g_hud_hit_count++];
        t.fi = fi;
        t.x = x;
        t.y = y;
        t.w = w;
        t.h = h;
        t.right_anchor = right_anchor;
    }
    if (!g_hud_edit) return;

    const float s = g_s;
    const bool hot = (g_hud_drag >= 0) && (g_hud_drag < g_hud_hit_count) &&
                     (g_hud_hits[g_hud_drag].fi == fi);
    render::RoundRectStroke(x - 3.0f * s, y - 3.0f * s, w + 6.0f * s, h + 6.0f * s,
                            3.0f * s, hot ? 2.0f : 1.0f,
                            hot ? theme::kAccent : render::RgbaF(1.0f, 1.0f, 1.0f, 0.60f));
    if (!hot) {
        // 名字挂在框的左上角，密密麻麻一堆框的时候能分清谁是谁
        const wchar_t* name = FeatName(fi);
        const float ts = 8.5f * s;
        const float tw = render::Measure(name, ts, false) + 8.0f * s;
        render::Rect(x - 3.0f * s, y - 3.0f * s - 11.0f * s, tw, 11.0f * s,
                     render::RgbaF(0.06f, 0.06f, 0.06f, 0.85f));
        render::Text(name, x - 3.0f * s, y - 3.0f * s - 11.0f * s, tw, 11.0f * s, ts,
                     theme::kSub, ALIGN_CENTER, false);
    }
}

void HudEditInput() {
    if (!g_hud_edit) {
        g_hud_drag = -1;
        return;
    }
    const float mx = input::X();
    const float my = input::Y();
    const float vw = (float)render::Width();
    const float vh = (float)render::Height();
    if (vw < 1.0f || vh < 1.0f) return;
    if (GetTickCount64() < g_hud_edit_guard_at) return;

    // 右键 = 这个元素的位置恢复出厂值（posx/posy 都删掉，回落到原版数值）
    if (input::Pressed(1)) {
        const int i = HudHitAt(mx, my);
        if (i >= 0) {
            const int fi = g_hud_hits[i].fi;
            cfg::ClearParam(FeatParamIndex(fi, L"posx"));
            cfg::ClearParam(FeatParamIndex(fi, L"posy"));
            wchar_t msg[96];
            swprintf_s(msg, L"%ls \u00B7 \u4F4D\u7F6E\u5DF2\u590D\u4F4D", FeatName(fi));
            PushToast(msg);
        }
        g_hud_drag = -1;
        return;
    }

    if (g_hud_drag < 0 && input::Pressed(0)) {
        const int i = HudHitAt(mx, my);
        if (i >= 0) {
            g_hud_drag = i;
            g_hud_drag_dx = mx - g_hud_hits[i].x;
            g_hud_drag_dy = my - g_hud_hits[i].y;
        }
    }

    if (g_hud_drag < 0) return;
    if (!input::Down(0)) {
        g_hud_drag = -1;
        return;
    }
    if (g_hud_drag >= g_hud_hit_count) {
        g_hud_drag = -1;
        return;
    }

    const HudHit& t = g_hud_hits[g_hud_drag];
    const float nx = mx - g_hud_drag_dx;
    const float ny = my - g_hud_drag_dy;

    // 吸附：默认 0.5% 一格，按住 Shift 细到 0.1%。
    // 不做吸附的话位置会是 0.4713xxx 这种随机小数，想手动对齐两个元素基本不可能。
    const float q = input::KeyDown(VK_SHIFT) ? 0.001f : 0.005f;
    float fx = (t.right_anchor ? (nx + t.w) : nx) / vw;
    float fy = ny / vh;
    fx = floorf(fx / q + 0.5f) * q;
    fy = floorf(fy / q + 0.5f) * q;
    if (fx < 0.0f) fx = 0.0f;
    if (fx > 1.0f) fx = 1.0f;
    if (fy < 0.0f) fy = 0.0f;
    if (fy > 1.0f) fy = 1.0f;

    SetFeatParam(t.fi, L"posx", fx);
    SetFeatParam(t.fi, L"posy", fy);
}

void DrawHudEditHint() {
    if (!g_hud_edit) return;
    const float s = g_s;
    const wchar_t* tip =
        cfg::Chinese()
            ? L"HUD \u5E03\u5C40 \u00B7 \u62D6\u52A8\u6446\u653E \u00B7 \u53F3\u952E\u590D\u4F4D"
              L" \u00B7 \u518D\u6309\u4E00\u6B21\u70ED\u952E\u9000\u51FA"
            : L"HUD layout \u00B7 drag to place \u00B7 right-click to reset \u00B7 "
              L"hotkey again to exit";
    const float ts = 10.0f * s;
    const float tw = render::Measure(tip, ts, false) + 24.0f * s;
    const float th = 22.0f * s;
    const float x = ((float)render::Width() - tw) * 0.5f;
    const float y = 10.0f * s;
    render::RoundRect(x, y, tw, th, 4.0f * s, render::RgbaF(0.06f, 0.06f, 0.06f, 0.88f));
    render::Rect(x, y, 2.0f * s, th, theme::kAccent);
    render::Text(tip, x, y, tw, th, ts, theme::kAccent, ALIGN_CENTER, false);

    // 正在拖的时候把当前坐标也报出来，吸附到 0.5% 才看得出效果
    if (g_hud_drag >= 0 && g_hud_drag < g_hud_hit_count) {
        const int fi = g_hud_hits[g_hud_drag].fi;
        wchar_t sub[128];
        swprintf_s(sub, L"%ls   X %.1f%%   Y %.1f%%", FeatName(fi),
                   FeatParamUser(fi, L"posx", 0.0f) * 100.0f,
                   FeatParamUser(fi, L"posy", 0.0f) * 100.0f);
        const float sw = render::Measure(sub, ts, false) + 24.0f * s;
        const float sx = ((float)render::Width() - sw) * 0.5f;
        const float sy = y + th + 4.0f * s;
        render::RoundRect(sx, sy, sw, th, 4.0f * s,
                          render::RgbaF(0.06f, 0.06f, 0.06f, 0.88f));
        render::Text(sub, sx, sy, sw, th, ts, theme::kOff, ALIGN_CENTER, false);
    }
}

void DrawHud() {
    // 面板里的鼠标指针一律用系统光标，不自绘。
    //
    // 这里原来有一个 DrawSoftwareCursor()，会画一个箭头形状的多边形。它带来两个问题：
    //   1) 系统光标可见时会和它叠成"两个光标一起动"；
    //   2) 形状和大小跟真实光标不一致，点起来总觉得对不上。
    // 现在改成：面板打开时由覆盖层把系统光标"推"成可见（见 overlay.cpp 的
    // SetSystemCursor），面板里直接使用真实光标。这样位置天然一致、手感也一致。
    g_hud_hit_count = 0;
    const float s = g_s;

    // FPS：位置/缩放/透明度/颜色/背景全读模块自己的参数，没调过就用原版出厂值
    // （Python 里 FPS.posx=0.47104、posy=0.01130，就是左上角偏中间那条）。
    int fps_idx = FindFeatureByName(L"FPS");
    if (fps_idx >= 0 && cfg::ModuleOn(fps_idx)) {
        const HudLook L = ReadHudLook(fps_idx, 0.47104f, 0.01130f, 1.0f, theme::kAccent);
        wchar_t buf[64];
        swprintf_s(buf, L"FPS \u00B7 %.0f", g_fps);
        const float ts = 10.0f * s * L.scale;
        const float tw = render::Measure(buf, ts, true) + 4.0f * s;
        const float th = ts * 1.4f;
        float bx = L.x;
        float by = L.y;
        if (!g_hud_edit) ClampHudBox(&bx, &by, tw, th);   // 编辑中不钳，否则拖到边上会粘住
        if (L.bg) {
            render::RoundRect(bx - 2.0f * s, by, tw + 4.0f * s, th, 2.0f * s,
                              render::RgbaF(0.0f, 0.0f, 0.0f, L.bg_alpha * L.alpha));
        }
        render::Text(buf, bx, by, tw, th, ts, render::RgbaF(L.r, L.g, L.b, L.alpha),
                     ALIGN_LEFT, true);
        HudEditBox(fps_idx, bx, by, tw, th, false);
    }

    // CPS：同上（Python 里 CPS.posx=0.67708、posy=0.94753，屏幕底部偏右）。
    int cps_idx = FindFeatureByName(L"CPS");
    if (cps_idx >= 0 && cfg::ModuleOn(cps_idx)) {
        const HudLook L = ReadHudLook(cps_idx, 0.67708f, 0.94753f, 1.0f, theme::kAccent);
        wchar_t buf[64];
        swprintf_s(buf, L"CPS \u00B7 %d", Cps());
        const float ts = 10.0f * s * L.scale;
        const float tw = render::Measure(buf, ts, true) + 4.0f * s;
        const float th = ts * 1.4f;
        float bx = L.x;
        float by = L.y;
        if (!g_hud_edit) ClampHudBox(&bx, &by, tw, th);   // 编辑中不钳，否则拖到边上会粘住
        if (L.bg) {
            render::RoundRect(bx - 2.0f * s, by, tw + 4.0f * s, th, 2.0f * s,
                              render::RgbaF(0.0f, 0.0f, 0.0f, L.bg_alpha * L.alpha));
        }
        render::Text(buf, bx, by, tw, th, ts, render::RgbaF(L.r, L.g, L.b, L.alpha),
                     ALIGN_LEFT, true);
        HudEditBox(cps_idx, bx, by, tw, th, false);
    }

    // CoordDisplay：玩家坐标。
    //
    // 数据走 sdk 那条已经跑通的链路（LocalPlayer -> mPos），和 ESP 同一个来源。
    // 它同时也是"链路还活着吗"最直观的指示：数字一直在跳就说明读到了，
    // 一直是 0 / 不动就说明链路断了 —— 比翻日志快。
    // 原版 Python（module/visual/coorddisplay.py）默认 posx=0.01 posy=0.10，
    // 文本格式 "X=%.1f Y=%.1f Z=%.1f"。
    int cd_idx = FindFeatureByName(L"CoordDisplay");
    if (cd_idx >= 0 && cfg::ModuleOn(cd_idx)) {
        const uintptr_t lp = sdk::LocalPlayer();
        sdk::Vec3 pos = {0.0f, 0.0f, 0.0f};
        if (lp != 0 && sdk::LocalPlayerPos(lp, &pos)) {
            const HudLook L = ReadHudLook(cd_idx, 0.01f, 0.10f, 1.0f, theme::kAccent);
            wchar_t buf[96];
            swprintf_s(buf, L"X=%.1f Y=%.1f Z=%.1f", pos.x, pos.y, pos.z);
            const float ts = 10.0f * s * L.scale;
            const float tw = render::Measure(buf, ts, true) + 4.0f * s;
            const float th = ts * 1.4f;
            float bx = L.x;
            float by = L.y;
            if (!g_hud_edit) ClampHudBox(&bx, &by, tw, th);
            if (L.bg) {
                render::RoundRect(bx - 2.0f * s, by, tw + 4.0f * s, th, 2.0f * s,
                                  render::RgbaF(0.0f, 0.0f, 0.0f, L.bg_alpha * L.alpha));
            }
            render::Text(buf, bx, by, tw, th, ts,
                         render::RgbaF(L.r, L.g, L.b, L.alpha), ALIGN_LEFT, true);
            HudEditBox(cd_idx, bx, by, tw, th, false);
        }
    }

    // ArrayList：已开启功能列表。
    //
    // ★ 原版 posx 是**列表右边缘**，不是左边缘 —— ArrayListScreen.py：
    //     right_edge = screen_w * px;  bg_x = right_edge - bg_w
    //   posy 是列表顶部。默认 0.98 / 0.02，贴着右上角。所以这里不能当左边缘用，
    //   否则滑块拖到 0.98 时整块列表会被推到屏幕右边外面去。
    int arr_idx = FindFeatureByName(L"ArrayList");
    if (arr_idx >= 0 && cfg::ModuleOn(arr_idx)) {
        const HudLook L = ReadHudLook(arr_idx, 0.98f, 0.02f, 1.0f, theme::kAccent);
        const bool fill = FeatParamUser(arr_idx, L"fill", 1.0f) > 0.5f;
        const bool border = FeatParamUser(arr_idx, L"border", 1.0f) > 0.5f;
        // color_mode：kOpt4 = { rgb, custom }，0 = 按行循环色相（原版默认），1 = 统一用 color_r/g/b
        const int mode = (int)FeatParamUser(arr_idx, L"color_mode", 0.0f);

        const float ts = 10.0f * s * L.scale;
        const float row_h = 14.0f * s * L.scale;
        const float stride = row_h + 1.0f * s * L.scale;
        const float vh = (float)render::Height();
        float ry = L.y;
        int row = 0;
        // 整块列表的包围盒（编辑模式拖拽要用）：右边界固定是 posx，
        // 左边界取所有行里最靠左的那条，宽度自然就是最长的那一行。
        float ux0 = L.x;
        float uy1 = L.y;
        for (int i = 0; i < kFeatureCount; ++i) {
            if (!cfg::ModuleOn(i)) continue;
            if (i == arr_idx) continue;
            if (ry + row_h > vh) break;  // 列表放不下就截断，别画到屏幕外
            const wchar_t* name = FeatName(i);
            const float tw = render::Measure(name, ts, false);
            const float box_w = tw + 12.0f * s * L.scale;
            float bx = L.x - box_w;
            if (bx < 0.0f) bx = 0.0f;
            if (bx < ux0) ux0 = bx;

            D2D1_COLOR_F col;
            if (mode == 0) {
                // 彩虹渐变：起始色相 0.55（青绿，和主题色同族），每行推进一点
                col = HueColor(0.55f + (float)row * 0.035f, L.alpha);
            } else {
                col = render::RgbaF(L.r, L.g, L.b, L.alpha);
            }
            if (fill || border) {
                // 原版：border 用 0.45、fill 用 0.40，都是纯黑底
                const float ba = (fill ? 0.40f : 0.45f) * L.alpha;
                render::Rect(bx, ry, box_w, row_h, render::RgbaF(0.0f, 0.0f, 0.0f, ba));
            }
            render::Rect(bx + box_w - 2.0f * s, ry, 2.0f * s, row_h, col);
            render::Text(name, bx, ry, box_w - 6.0f * s, row_h, ts, col, ALIGN_RIGHT, false);
            ry += stride;
            uy1 = ry - (stride - row_h);
            ++row;
        }
        if (row > 0) {
            HudEditBox(arr_idx, ux0, L.y, L.x - ux0, uy1 - L.y, true);
        }
    }

    // Keystrokes：按键显示。位置/缩放/颜色都读模块自己的参数（原版就是这么做的），
    // 之前固定画在左下角，滑块怎么拖都不动，等于参数白给。
    // 默认值取原版 keystrokes.py 的出厂值：posx=0.25677, posy=0.775, scale=0.75,
    // alpha=0.55, bg_alpha=0.3 —— 用 FeatParamUser 而不是 FeatParam，
    // 否则没调过的滑块会给出量程中点（posx 变 0.5 → 按键块跑到屏幕正中）。
    int ks_idx = FindFeatureByName(L"Keystrokes");
    if (ks_idx >= 0 && cfg::ModuleOn(ks_idx)) {
        float ksc = FeatParamUser(ks_idx, L"scale", 0.75f);
        if (ksc < 0.4f) ksc = 0.4f;
        if (ksc > 2.5f) ksc = 2.5f;
        float alpha = FeatParamUser(ks_idx, L"alpha", 0.55f);
        if (alpha < 0.1f) alpha = 0.1f;
        if (alpha > 1.0f) alpha = 1.0f;
        const float cr = FeatParamUser(ks_idx, L"color_r", 1.0f);
        const float cg = FeatParamUser(ks_idx, L"color_g", 1.0f);
        const float cb = FeatParamUser(ks_idx, L"color_b", 1.0f);

        const float box = 16.0f * s * ksc;
        const float gap = 2.0f * s * ksc;
        const float unit = box + gap;
        const float pad = 4.0f * s;
        const int rows = 5;
        const float totalW = 3.0f * box + 2.0f * gap;
        const float totalH = (float)rows * box + (float)(rows - 1) * gap;

        const float viewW2 = (float)render::Width();
        const float viewH2 = (float)render::Height();
        float bx = FeatParamUser(ks_idx, L"posx", 0.25677f) * viewW2;
        float by = FeatParamUser(ks_idx, L"posy", 0.775f) * viewH2;
        // 钳在屏幕里：用户把位置拖到边上时别让按键块跑出去。
        // 编辑模式下不钳 —— 钳了会让拖拽算出的 px 和实际绘制位置打架，表现是"拖到边上粘住"。
        if (!g_hud_edit) {
            if (bx > viewW2 - totalW - pad) bx = viewW2 - totalW - pad;
            if (by > viewH2 - totalH - pad) by = viewH2 - totalH - pad;
            if (bx < pad) bx = pad;
            if (by < pad) by = pad;
        }

        if (FeatParamUser(ks_idx, L"bg", 0.0f) > 0.5f) {
            float ba = FeatParamUser(ks_idx, L"bg_alpha", 0.3f);
            if (ba < 0.0f) ba = 0.0f;
            if (ba > 1.0f) ba = 1.0f;
            render::RoundRect(bx - pad, by - pad, totalW + pad * 2.0f, totalH + pad * 2.0f,
                              5.0f * s, render::RgbaF(0.0f, 0.0f, 0.0f, ba * alpha));
        }

        const D2D1_COLOR_F idle = render::RgbaF(1.0f, 1.0f, 1.0f, 0.10f * alpha);
        const D2D1_COLOR_F hot = render::RgbaF(cr, cg, cb, 0.90f * alpha);
        const D2D1_COLOR_F idle_txt = render::RgbaF(1.0f, 1.0f, 1.0f, 0.72f * alpha);

        // dx/dy 是"第几列/第几行"，w 是宽度占几列
        struct KeyBox { const wchar_t* label; int vk; float dx, dy, w; };
        const KeyBox boxes[] = {
            {L"W", 'W', 1.0f, 0.0f, 1.0f},
            {L"A", 'A', 0.0f, 1.0f, 1.0f},
            {L"S", 'S', 1.0f, 1.0f, 1.0f},
            {L"D", 'D', 2.0f, 1.0f, 1.0f},
            {L"\u5DE6\u952E", VK_LBUTTON, 0.0f, 2.0f, 1.5f},
            {L"\u53F3\u952E", VK_RBUTTON, 1.5f, 2.0f, 1.5f},
            {L"Space", VK_SPACE, 0.0f, 3.0f, 3.0f},
            {L"Shift", VK_LSHIFT, 0.0f, 4.0f, 1.5f},
            {L"Ctrl", VK_LCONTROL, 1.5f, 4.0f, 1.5f},
        };
        for (size_t i = 0; i < sizeof(boxes) / sizeof(boxes[0]); ++i) {
            bool down = input::KeyDown(boxes[i].vk) ||
                        (boxes[i].vk == VK_LBUTTON && input::Down(0)) ||
                        (boxes[i].vk == VK_RBUTTON && input::Down(1));
            float x = bx + boxes[i].dx * unit;
            float yy = by + boxes[i].dy * unit;
            float w = boxes[i].w * box + (boxes[i].w - 1.0f) * gap;
            render::RoundRect(x, yy, w, box, 3.0f * s, down ? hot : idle);
            render::Text(boxes[i].label, x, yy, w, box, 9.0f * s * ksc,
                         down ? theme::kHeadBg : idle_txt, ALIGN_CENTER, false);
        }
        HudEditBox(ks_idx, bx, by, totalW, totalH, false);
    }

    // 拖动/复位按这个帧登记到的盒子来判定，所以必须在所有元素画完之后跑
    HudEditInput();
    DrawHudEditHint();
    DrawNotifications();
}

// 热键防抖：抢焦点/交还焦点的瞬间，GetAsyncKeyState 会短暂读到 0 再读到按下，
// 于是一次右 Shift 被算成两次边沿 —— 表现就是"按一下没反应，按两下才关掉"，
// 或者"窗口恢复了但 GUI 又打开了"。这里给一个 280ms 的静默期挡掉第二次。
ULONGLONG g_toggle_guard_at = 0;

void HandleHotkeys() {
    // GUI 打开/关闭
    if (input::HotkeyPressed()) {
        ULONGLONG now = GetTickCount64();
        if (now >= g_toggle_guard_at) {
            g_toggle_guard_at = now + 280;
            if (g_hud_edit) {
                // 编辑模式下的热键是"退出"，不是开面板：否则会一边拖 HUD
                // 一边把面板盖上来，两套交互叠在一起。
                SetHudEdit(false);
                PushToast(cfg::Chinese() ? L"\u5DF2\u9000\u51FA HUD \u5E03\u5C40"
                                         : L"HUD layout off");
            } else {
                SetVisible(!g_visible);
            }
        }
    }

    // 缩放：自动模式下 g_s 是算出来的，不能拿 cfg::Scale()（0）去加减
    if (input::KeyDown(VK_CONTROL)) {
        float base = g_s;
        if (base < 0.85f) base = 0.85f;
        if (input::KeyPressed(VK_OEM_4)) {
            float v = base - 0.15f;
            if (v < 0.85f) v = 0.85f;
            cfg::SetScale(v);
        }
        if (input::KeyPressed(VK_OEM_6)) {
            float v = base + 0.15f;
            if (v > 2.6f) v = 2.6f;
            cfg::SetScale(v);
        }
    }

    // ── 绑定捕获 ────────────────────────────────────────────────
    // 点参数行里的「功能快捷键」进入，按任意键完成，Delete / Esc 取消。
    // 之前这里有两个真 bug：
    //   1) input::NewFrame 把 ESC 从捕获队列里过滤掉了，所以
    //      "if (vk == VK_ESCAPE) 取消绑定" 是永远进不去的死分支；
    //   2) 绑完没有 return，同一次按下的键继续走到下面的模块快捷键循环，
    //      于是"绑好 K" 的瞬间模块还被顺手切了一次。
    if (g_bind_target >= 0) {
        const int vk = input::TakePressedKey();
        if (vk != 0) {
            const int fi = g_bind_target;
            wchar_t msg[96];
            if (vk == VK_DELETE || vk == VK_ESCAPE) {
                cfg::SetModuleBind(fi, 0);
                swprintf_s(msg, L"%ls \u00B7 \u5DF2\u53D6\u6D88\u7ED1\u5B9A", FeatName(fi));
                LogLine("[gui] bind cleared: %ls", kFeatures[fi].en);
            } else {
                // 同一个键已经绑给别人时先摘掉旧的，否则按一下会同时切两个模块，
                // 而且用户根本看不出来是哪一个在捣鬼。
                for (int i = 0; i < kFeatureCount; ++i) {
                    if (i != fi && cfg::ModuleBind(i) == vk) cfg::SetModuleBind(i, 0);
                }
                cfg::SetModuleBind(fi, vk);
                swprintf_s(msg, L"%ls \u00B7 \u7ED1\u5B9A %ls", FeatName(fi), BindKeyName(vk));
                LogLine("[gui] bind %ls -> vk=0x%02X", kFeatures[fi].en, vk);
            }
            PushToast(msg);
            g_bind_target = -1;
            g_bind_guard_at = GetTickCount64() + 300;
        }
        // 等键期间不要再往下走，否则这次按下的键会被当模块快捷键消费掉
        return;
    }

    // 模块快捷键。
    // 搜索框有焦点时全部屏蔽 —— 否则在框里打一个字母，绑了那个键的模块就被切了。
    if (g_search_focus) return;
    // 游戏不在前台时不响应：GetAsyncKeyState 是全局的，在别的窗口打字
    // （尤其绑了字母键的模块）会把功能一路开关，看着像自己乱跳。
    if (!GameForeground()) return;
    if (GetTickCount64() < g_bind_guard_at) return;
    const int clickgui_idx = FindFeatureByName(L"ClickGui");
    for (int i = 0; i < kFeatureCount; ++i) {
        const int vk = cfg::ModuleBind(i);
        if (vk == 0) continue;
        if (i == clickgui_idx) continue;
        if (!input::KeyPressed(vk)) continue;
        // action 类模块（AutoFish 之类）没有开关语义，别绑了就以为能触发
        if (!kFeatures[i].action) cfg::SetModuleOn(i, !cfg::ModuleOn(i));
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

// 对外只留一个开关入口。编辑模式开着时按热键应该是"退出布局"，
// 不是把面板盖到布局界面上。
void Toggle() {
    if (g_hud_edit) {
        SetHudEdit(false);
        return;
    }
    SetVisible(!g_visible);
}

bool Visible() { return g_visible; }

void SetVisible(bool v) {
    if (g_visible == v) return;
    g_visible = v;
    // 这里以前会 while (ShowCursor(...)) 把进程的显示计数往两头推。
    // 这个 DLL 注入在游戏进程里，显示计数和游戏是共用的 —— 推一次之后
    // 游戏自己那一次 ShowCursor 根本拉不回来，光标就长期处于错误状态。
    // 现在完全不碰显示计数：面板打开时由 overlay 的 SetSystemCursor 统一管。
    if (!v) {
        g_bind_target = -1;
        g_drag_panel = -1;
        g_drag_slider = -1;
        g_drag_scrollbar = -1;
        g_search_focus = false;   // 收起面板就别再吃键盘了
        g_click_used = false;
    }
    LogLine("[gui] visible=%d", v ? 1 : 0);
}

bool WantsMouse() { return g_visible || g_hud_edit; }

bool HudEdit() { return g_hud_edit; }

void SetHudEdit(bool v) {
    if (g_hud_edit == v) return;
    g_hud_edit = v;
    g_hud_edit_guard_at = GetTickCount64() + 180;
    if (v) {
        // 进编辑模式就把面板侧的交互状态清干净，免得收起面板后还留着
        // 半截拖动/等按键的状态
        g_bind_target = -1;
        g_search_focus = false;
        g_drag_panel = -1;
        g_drag_slider = -1;
        g_drag_scrollbar = -1;
    } else {
        g_hud_drag = -1;
    }
    LogLine("[gui] hud_edit=%d", v ? 1 : 0);
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
    // 滚轮每帧只取一次并清零，面板绘制时读这个值（否则会一直拿旧值往下滚）
    g_wheel_frame = input::ConsumeWheel();
    cfg::Tick();

    // 缩放每帧统一算一次：面板收起时 DrawHud 也要用，不能只在面板分支里算
    {
        float sc = cfg::Scale();
        g_s = (sc > 0.0f) ? sc : AutoScale();
    }

    // 点击计数（CPS）
    if (input::Pressed(0)) {
        g_click_times.push_back(now);
        if (g_click_times.size() > 64) g_click_times.erase(g_click_times.begin());
    }

    HandleHotkeys();
    ReleaseDrags();

    // 单击一次性锁每帧清一次：工具栏先抢，面板后看
    g_click_used = false;

    if (g_visible) {
        // 注意：这里不再每帧调 ClipCursor(nullptr)。
        // 游戏每帧都会自己重新裁剪光标，我们跟着解一次只是互相打架，
        // 而且会让"是否已解锁"的检测永远为假。解锁交给 overlay.cpp
        // 里合成的那个按键（cfg::UnlockKey）。
        LayoutPanels();

        // 工具条先处理点击，抢到就置 g_click_used，面板那边不会再响应同一次按下
        DrawToolbar(true);

        for (size_t i = 0; i < g_panels.size(); ++i) {
            DrawPanel(g_panels[i], true);
        }
        // ★ 输入轮要**倒序**：绘制是正序，index 大的后画 = 视觉上在上面，
        //   所以点击也必须让 index 大的先抢。以前两轮都正序，重叠区域上
        //   被压在底下的面板先响应，点了上面那块结果动的是下面那个。
        //   这一轮现在不画任何像素，倒序不会影响 z 序。
        for (size_t i = g_panels.size(); i-- > 0;) {
            DrawPanel(g_panels[i], false);
        }
        // 工具条最后画，保证压在面板上面
        DrawToolbar(false);

        // 不画任何指针：面板里直接用系统光标（由 overlay.cpp 保证可见）。
    }

    DrawHud();

    // 模块运行时：自动连点 / 疾跑 / 防挂机 / 钓鱼。
    // 放在最后是因为它要读本帧的按键状态，而 input::NewFrame() 在帧首。
    TickNotifications();
    modules::Tick();
}

}  // namespace gui
}  // namespace xiaoyu
