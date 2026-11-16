// modules.cpp - 用合成输入实现的模块
//
// 设计原则：
//   1. 只在「面板没打开」时动手。面板打开意味着前台在我们这边，
//      这时候再合成点击/按键会把前台又抢回游戏，面板立刻失焦。
//   2. 所有按下的键都记在 g_held 里，任何异常路径（关面板、模块被关、
//      退出）都要能松回去，绝不留下一个卡住的 Ctrl 或鼠标左键。
//   3. 每个模块的生效都写一行日志，方便远程确认到底有没有触发。
#include "modules.h"

#include <windows.h>
#include <cwchar>
#include <stdlib.h>

#include "config.h"
#include "gui.h"
#include "gui/catalog.h"
#include "input.h"
#include "log.h"

namespace xiaoyu {
namespace modules {
namespace {

// ── 目录查询 ────────────────────────────────────────────────────
int FindFeature(const wchar_t* en) {
    for (int i = 0; i < kFeatureCount; ++i) {
        if (kFeatures[i].en != nullptr && wcscmp(kFeatures[i].en, en) == 0) return i;
    }
    return -1;
}

// 按 attr 取参数值。catalog 里参数顺序是原版 Python 的字典序，
// 直接按下标取值太脆，用属性名匹配。
float ParamOf(int fi, const wchar_t* attr, float def) {
    if (fi < 0) return def;
    const FeatureInfo& f = kFeatures[fi];
    for (int k = 0; k < f.paramCount; ++k) {
        const int pi = f.paramFirst + k;
        if (kParams[pi].attr != nullptr && wcscmp(kParams[pi].attr, attr) == 0) {
            return cfg::ParamValue(pi, def);
        }
    }
    return def;
}

// 模块是否真的该干活：开关打开 + 面板收起
bool Active(int fi) {
    if (fi < 0) return false;
    if (!cfg::ModuleOn(fi)) return false;
    if (gui::Visible()) return false;
    return true;
}

// ── 合成输入 ────────────────────────────────────────────────────
const int kMaxHeld = 8;
WORD g_held[kMaxHeld] = {0};
int g_held_n = 0;
bool g_mouse_down = false;

void HoldKey(WORD vk) {
    if (g_held_n >= kMaxHeld) return;
    for (int i = 0; i < g_held_n; ++i) {
        if (g_held[i] == vk) return;
    }
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    SendInput(1, &in, sizeof(in));
    g_held[g_held_n++] = vk;
}

void UnholdKey(WORD vk) {
    int at = -1;
    for (int i = 0; i < g_held_n; ++i) {
        if (g_held[i] == vk) { at = i; break; }
    }
    if (at < 0) return;
    INPUT in = {};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
    for (int i = at; i + 1 < g_held_n; ++i) g_held[i] = g_held[i + 1];
    --g_held_n;
}

void TapKey(WORD vk) {
    HoldKey(vk);
    UnholdKey(vk);
}

void MouseDown() {
    if (g_mouse_down) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &in, sizeof(in));
    g_mouse_down = true;
}

void MouseUp() {
    if (!g_mouse_down) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
    g_mouse_down = false;
}

void TapMouse() {
    MouseDown();
    MouseUp();
}

// ── 物理鼠标按键 ────────────────────────────────────────────────
// 为什么非要挂钩子：SendInput 注入的 LEFTUP 会把 GetAsyncKeyState 一起改掉，
// 于是"玩家还按着吗"这个判断会失真 —— 更糟的是我们每注入一次 DOWN 又刷新了
// 自己的判定，连点就永远停不下来。
//
// WH_MOUSE_LL 的 MSLLHOOKSTRUCT.flags 里有 LLMHF_INJECTED，能明确区分
// 物理事件和注入事件。只认物理事件，问题就没了。钩子只在 AC 打开期间挂着。
HHOOK g_mouse_hook = nullptr;
volatile LONG g_phys_left = 0;

LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (ms != nullptr && (ms->flags & LLMHF_INJECTED) == 0) {
            if (wp == WM_LBUTTONDOWN) g_phys_left = 1;
            else if (wp == WM_LBUTTONUP) g_phys_left = 0;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void EnsureMouseHook(bool on) {
    if (on && g_mouse_hook == nullptr) {
        g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc,
                                         GetModuleHandleW(nullptr), 0);
        if (g_mouse_hook == nullptr) {
            LogLine("[mod] 装鼠标钩子失败 err=%lu，自动连点将不可用",
                    (unsigned long)GetLastError());
        }
    } else if (!on && g_mouse_hook != nullptr) {
        UnhookWindowsHookEx(g_mouse_hook);
        g_mouse_hook = nullptr;
        g_phys_left = 0;
    }
}

// ── 各模块状态 ──────────────────────────────────────────────────
bool g_sprint_held = false;
ULONGLONG g_ac_next_click = 0;
ULONGLONG g_afk_next = 0;
ULONGLONG g_fish_next = 0;
int g_fish_phase = 0;   // 0=还没抛竿 1=等鱼 2=收杆后冷却

// AC（自动连点）：玩家按住左键时，按 cps_min~cps_max 的节奏连点。
//
// 难点在于"玩家还按着吗"：我们自己注入的 LEFTUP 会把 GetAsyncKeyState 一起改掉，
// 于是发完一次点击，下一帧就误判成"玩家松开了"，连点立刻停。
// 所以不用瞬时状态，用"最近 160ms 内见过按下"来判定按住 —— 注入的 down 也会刷新它，
// 玩家真正松手时 160ms 后就自然停。
void TickAutoClicker() {
    const int fi = FindFeature(L"AC");
    if (!Active(fi)) {
        if (g_mouse_down) MouseUp();
        g_ac_next_click = 0;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    // 用钩子记录的物理状态，不看 GetAsyncKeyState —— 后者会被我们自己的
    // 注入事件带跑，连点会停不下来
    const bool held = g_phys_left != 0;
    if (!held) {
        if (g_mouse_down) MouseUp();
        g_ac_next_click = 0;
        return;
    }

    float lo = ParamOf(fi, L"cps_min", 10.0f);
    float hi = ParamOf(fi, L"cps_max", 18.0f);
    if (lo < 1.0f) lo = 1.0f;
    if (hi < lo) hi = lo;
    if (hi > 30.0f) hi = 30.0f;

    if (g_ac_next_click == 0) g_ac_next_click = now;
    if (now < g_ac_next_click) return;

    // 每一击都重新摇一次间隔，固定节奏反而不像人
    const float span = hi - lo;
    const float cps = lo + (span > 0.0f ? (float)(rand() % 1000) / 1000.0f * span : 0.0f);
    g_ac_next_click = now + (ULONGLONG)(1000.0f / (cps > 0.5f ? cps : 0.5f));

    MouseDown();
    MouseUp();
}

// Sprint：按住 W 的时候替玩家按住疾跑键。
// 基岩版 Windows 端默认疾跑是左 Ctrl（双击 W 也能触发，但双击要额外松一次
// 真实的 W，会和玩家自己的按键打架）。这里选按住 Ctrl，松 W 就松 Ctrl。
void TickSprint() {
    const int fi = FindFeature(L"Sprint");
    const bool want = Active(fi) && input::KeyDown(VK_W);
    if (want && !g_sprint_held) {
        HoldKey(VK_LCONTROL);
        g_sprint_held = true;
    } else if (!want && g_sprint_held) {
        UnholdKey(VK_LCONTROL);
        g_sprint_held = false;
    }
}

// AntiAFK：每隔 interval 秒跳一下。跳跃是所有动作里最不容易把自己弄死的，
// 把"前进"换成"跳"就是为了不让人掉进岩浆。
void TickAntiAfk() {
    const int fi = FindFeature(L"AntiAFK");
    if (!Active(fi)) {
        g_afk_next = 0;
        return;
    }
    float interval = ParamOf(fi, L"interval", 30.0f);
    if (interval < 1.0f) interval = 1.0f;

    const ULONGLONG now = GetTickCount64();
    if (g_afk_next == 0) {
        g_afk_next = now + (ULONGLONG)(interval * 1000.0f);
        return;
    }
    if (now >= g_afk_next) {
        g_afk_next = now + (ULONGLONG)(interval * 1000.0f);
        TapKey(VK_SPACE);
    }
}

// AutoFish：抛竿 → 等 max_wait → 收杆 → 冷却 recast_delay 后再抛。
// 说明：这里没有"看浮标"的能力（那要么读屏要么读内存），所以是固定节奏，
// 适合挂机池子；真要按鱼上钩收杆，得等内存模块那一阶段。
void TickAutoFish() {
    const int fi = FindFeature(L"AutoFish");
    if (!Active(fi)) {
        if (g_fish_phase != 0 && g_mouse_down) MouseUp();
        g_fish_phase = 0;
        g_fish_next = 0;
        return;
    }
    float wait = ParamOf(fi, L"max_wait", 15.0f);
    if (wait < 3.0f) wait = 3.0f;
    float recast = ParamOf(fi, L"recast_delay", 1.0f);

    const ULONGLONG now = GetTickCount64();
    if (g_fish_next == 0) {
        g_fish_next = now + 500;   // 切到钓鱼模块后先缓半秒再抛
        return;
    }
    if (now < g_fish_next) return;

    if (g_fish_phase == 0) {
        TapMouse();                       // 抛竿
        g_fish_phase = 1;
        g_fish_next = now + (ULONGLONG)(wait * 1000.0f);
        LogLine("[mod] AutoFish 抛竿，%.0f 秒后收杆", wait);
    } else if (g_fish_phase == 1) {
        TapMouse();                       // 收杆
        g_fish_phase = 2;
        g_fish_next = now + (ULONGLONG)((recast < 0.5f ? 0.5f : recast) * 1000.0f);
    } else {
        g_fish_phase = 0;                 // 冷却结束，下一帧重新抛
    }
}

}  // namespace

void ReleaseAll() {
    for (int i = g_held_n - 1; i >= 0; --i) {
        UnholdKey(g_held[i]);
    }
    g_held_n = 0;
    MouseUp();
    g_sprint_held = false;
    g_ac_next_click = 0;
    g_fish_phase = 0;
    g_fish_next = 0;
}

void Tick() {
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        srand((unsigned)GetTickCount64());
    }
    // 面板一打开就立刻停手：我们的合成点击会激活游戏窗口，
    // 前台被抢回去之后面板就点不动了。
    if (gui::Visible()) {
        EnsureMouseHook(false);   // 面板打开时把钩子摘掉，别让输入多绕一圈
        ReleaseAll();
        return;
    }
    // AC 打开时才挂物理鼠标钩子，其余时间不增加任何输入延迟
    {
        const int ac = FindFeature(L"AC");
        EnsureMouseHook(ac >= 0 && cfg::ModuleOn(ac) && !gui::Visible());
    }
    TickAutoClicker();
    TickSprint();
    TickAntiAfk();
    TickAutoFish();
}

}  // namespace modules
}  // namespace xiaoyu
