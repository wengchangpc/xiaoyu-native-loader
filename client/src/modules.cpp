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
#include <wchar.h>
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
// 判定"玩家还按着左键吗"不能用 GetAsyncKeyState：我们每注入一次 LEFTDOWN
// 就把自己的判定刷了一次，玩家松手之后连点会永远停不下来。
// 真正的物理状态由 input.cpp 的 WH_MOUSE_LL 钩子记录（它用
// MSLLHOOKSTRUCT.flags 里的 LLMHF_INJECTED 把注入事件过滤掉）。
// 钩子的开关也归 input 管，这里只负责提需求。

// ── 成对注入 ────────────────────────────────────────────────────
// 一次 SendInput 里把「松开 + 按下」一起发出去，中间不留空窗。
// 游戏收到的是两次独立的按键事件，于是"按住不放"变成"又按了一下"。
void SendKeyUpDown(WORD vk) {
    const WORD sc = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[0].ki.wScan = sc;
    in[0].ki.dwFlags = KEYEVENTF_KEYUP;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.wScan = sc;
    SendInput(2, in, sizeof(INPUT));
}

// 鼠标键的成对注入，语义和 SendKeyUpDown 一样：让游戏以为是"又点了一下"。
void SendMouseUpDown(DWORD up_flag, DWORD down_flag) {
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = up_flag;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = down_flag;
    SendInput(2, in, sizeof(INPUT));
}

// 连发间隔加抖动。
// 固定 50ms 一发的节奏在服务端看是完美的等距脉冲，等于自己报"我是脚本"。
// 反作弊优先，所以所有连发类动作都走这里：基准 ±15% 的随机。
ULONGLONG JitterMs(int base_ms, int percent) {
    if (base_ms <= 0) return 0;
    const int span = base_ms * percent / 100;
    if (span <= 0) return (ULONGLONG)base_ms;
    const int d = (rand() % (span * 2 + 1)) - span;
    int v = base_ms + d;
    if (v < 1) v = 1;
    return (ULONGLONG)v;
}

// 逐字符按 Unicode 注入。中文没法用虚拟键码表示，只能走 KEYEVENTF_UNICODE，
// 让系统在目标线程里合成 WM_CHAR。基岩版的聊天输入框吃的是 WM_CHAR，
// 所以这条路能打出中文（用 VkKeyScan 走键盘布局就会被转成问号）。
void TypeUnicode(const wchar_t* s) {
    for (const wchar_t* p = s; *p != 0; ++p) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = (WORD)*p;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

// ── 各模块状态 ──────────────────────────────────────────────────
bool g_sprint_held = false;
ULONGLONG g_ac_next_click = 0;
ULONGLONG g_afk_next = 0;
ULONGLONG g_afk_release_at = 0;
bool g_afk_walking = false;
ULONGLONG g_fish_next = 0;
int g_fish_phase = 0;   // 0=还没抛竿 1=等鱼 2=收杆后冷却

// NoJumpDelay
ULONGLONG g_jump_next = 0;

// NoThrowDelay
ULONGLONG g_throw_next = 0;

// Spammer：0=等下一轮 1=已按 T 等聊天框 2=已打字等回车
ULONGLONG g_spam_next = 0;
int g_spam_phase = 0;
ULONGLONG g_spam_step_at = 0;

// Spin
ULONGLONG g_spin_next = 0;
int g_spin_dir = 1;
ULONGLONG g_spin_flip_at = 0;

// AttackSprintReset
bool g_prev_phys_left = false;
ULONGLONG g_sprint_suppress_until = 0;
bool g_asr_warned = false;

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
    const bool held = input::PhysLeftDown();
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
    bool want = Active(fi) && input::KeyDown((int)'W');
    // AttackSprintReset 打中目标后要把疾跑掐掉几个 tick，
    // 这时候就算 W 还按着也不能立刻把它按回去，否则重置等于没做。
    if (want && GetTickCount64() < g_sprint_suppress_until) want = false;
    if (want && !g_sprint_held) {
        HoldKey(VK_LCONTROL);
        g_sprint_held = true;
    } else if (!want && g_sprint_held) {
        UnholdKey(VK_LCONTROL);
        g_sprint_held = false;
    }
}

// AntiAFK：对齐原版 misc/antiafk.py —— 每 interval 秒给一次
// 「小幅位移 + 随机转视角」，让服务器看到有人在操作。
// 原版是直接 SetMotion + SetRot，我们只能合成输入：鼠标相对移动 = 转视角，
// 短按 W = 位移。位移时间压到 90ms，避免挂机时自己走进岩浆。
void TickAntiAfk() {
    const int fi = FindFeature(L"AntiAFK");
    if (!Active(fi)) {
        g_afk_next = 0;
        return;
    }
    float interval = ParamOf(fi, L"interval", 5.0f);
    if (interval < 1.0f) interval = 1.0f;

    const ULONGLONG now = GetTickCount64();
    if (g_afk_next == 0) {
        g_afk_next = now + (ULONGLONG)(interval * 1000.0f);
        return;
    }
    if (now < g_afk_next) return;
    g_afk_next = now + (ULONGLONG)(interval * 1000.0f);

    // 视角：±30° 偏航、±15° 俯仰，和原版 random.uniform 的范围一致
    const int dx = (rand() % 61) - 30;
    const int dy = (rand() % 31) - 15;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));

    // 位移：短按 W
    HoldKey((WORD)'W');
    g_afk_release_at = now + 90;
    g_afk_walking = true;
}

// 结束 AntiAFK 那次短按（放在 Tick 里统一收尾，避免 Sleep 卡住渲染）
void TickAntiAfkRelease() {
    if (!g_afk_walking) return;
    if (GetTickCount64() < g_afk_release_at) return;
    UnholdKey((WORD)'W');
    g_afk_walking = false;
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

// NoJumpDelay / AutoJumpSpam 共用：「按住空格时反复顶一次跳」。
// 原版是 on_tick 里每 tick 调一次 clientApi.SimulateJump()，这里没有引擎 API，
// 只能把「按住」拆成「松开+按下」重新发一遍，游戏就会当成又按了一下。
// 触发判据用物理按键状态（WH_KEYBOARD_LL），不能用 GetAsyncKeyState ——
// 我们自己注入的 DOWN 会把它写脏，玩家松手后空格会被永久按住。
void TickJumpSpam() {
    const int fi = FindFeature(L"NoJumpDelay");
    if (!Active(fi) || !input::PhysSpaceDown()) {
        g_jump_next = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_jump_next == 0) g_jump_next = now;
    if (now < g_jump_next) return;
    g_jump_next = now + JitterMs(50, 15);   // 20Hz 基准，带抖动
    SendKeyUpDown(VK_SPACE);
}

// NoThrowDelay（无投掷延迟）：对齐原版 world/nothrowdelay.py 的意图 ——
// 去掉连续投掷（珍珠/雪球/鸡蛋）之间的冷却。
// 原版是改引擎里的 use 冷却，我们只能逐个"再按一次右键"把冷却顶掉：
// 玩家按住右键期间，按 20Hz 把右键拆成「松开 + 按下」重新发一遍。
// 判"物理上按着右键"同样走 WH_MOUSE_LL，理由和 AC 判左键一样。
void TickNoThrowDelay() {
    const int fi = FindFeature(L"NoThrowDelay");
    if (!Active(fi) || !input::PhysRightDown()) {
        g_throw_next = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_throw_next == 0) g_throw_next = now;
    if (now < g_throw_next) return;
    g_throw_next = now + JitterMs(50, 15);
    SendMouseUpDown(MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_RIGHTDOWN);
}

// Spammer：对齐原版 misc/spammer.py —— 每 delay 秒发一条「文本 + 随机后缀」。
// 原版直接调 _voice_trans.sendChatMsg，我们走界面：T 开聊天框 → 打字 → 回车。
// 默认文本沿用原版，random_len=0 时不加后缀。
void TickSpammer() {
    const int fi = FindFeature(L"Spammer");
    if (!Active(fi)) {
        g_spam_phase = 0;
        g_spam_next = 0;
        return;
    }
    float delay = ParamOf(fi, L"delay_seconds", 5.0f);
    if (delay < 0.1f) delay = 0.1f;
    int rnd_len = (int)ParamOf(fi, L"random_len", 4.0f);
    if (rnd_len < 0) rnd_len = 0;
    if (rnd_len > 64) rnd_len = 64;

    const ULONGLONG now = GetTickCount64();
    if (g_spam_phase == 0) {
        if (g_spam_next == 0) {
            g_spam_next = now + (ULONGLONG)(delay * 1000.0f);
            return;
        }
        if (now < g_spam_next) return;
        TapKey('T');                       // 打开聊天框
        g_spam_phase = 1;
        g_spam_step_at = now + 160;        // 等界面出来
        return;
    }
    if (now < g_spam_step_at) return;

    if (g_spam_phase == 1) {
        wchar_t msg[160];
        // 原版默认文本。写死是因为 GUI 里没有文本框，只有延迟/后缀长度两个参数。
        wcscpy_s(msg, L"Xiaoyu入侵服务器");
        if (rnd_len > 0) {
            static const wchar_t kAlnum[] =
                L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            const int base = (int)wcslen(msg);
            const int room = (int)(sizeof(msg) / sizeof(msg[0])) - base - 2;
            if (room > 2) {
                int n = rnd_len < room - 1 ? rnd_len : room - 1;
                msg[base] = L' ';
                for (int i = 0; i < n; ++i) {
                    msg[base + 1 + i] = kAlnum[rand() % 62];
                }
                msg[base + 1 + n] = 0;
            }
        }
        TypeUnicode(msg);
        g_spam_phase = 2;
        g_spam_step_at = now + 140;        // 等输入法把字吃进去
        return;
    }

    // phase 2：回车发出去
    TapKey(VK_RETURN);
    g_spam_phase = 0;
    g_spam_next = now + (ULONGLONG)(delay * 1000.0f);
    LogLine("[mod] Spammer 已发送（间隔 %.1f 秒）", delay);
}

// Spin：对齐原版 combat/spin.py 的「持续自转」。
// 差别要说清楚：原版是**静默**转（DepartCamera + LockLocalPlayerRot，自己画面不动，
// 只有服务端/别人看到你在转）。那需要改玩家的实际旋转，必须走内存。
// 这里只能合成鼠标相对位移，等于真的把镜头转起来 —— 自己画面会跟着转。
// speed 是「每 tick 转多少度」，按 1 鼠标计数 ≈ 0.25 度估，灵敏度不同会有偏差。
void TickSpin() {
    const int fi = FindFeature(L"Spin");
    if (!Active(fi)) {
        g_spin_next = 0;
        g_spin_flip_at = 0;
        return;
    }
    float speed = ParamOf(fi, L"speed", 37.3f);
    if (speed < 1.0f) speed = 1.0f;
    if (speed > 60.0f) speed = 60.0f;
    float jitter = ParamOf(fi, L"jitter", 0.15f);
    if (jitter < 0.0f) jitter = 0.0f;
    if (jitter > 10.0f) jitter = 10.0f;

    const ULONGLONG now = GetTickCount64();
    if (g_spin_next == 0) g_spin_next = now;
    if (now < g_spin_next) return;
    g_spin_next = now + 50;   // 20Hz

    // flip：每 3 秒掉一次头，纯匀速转太容易被看出是机器
    if (ParamOf(fi, L"flip", 0.0f) > 0.5f) {
        if (g_spin_flip_at == 0) g_spin_flip_at = now + 3000;
        if (now >= g_spin_flip_at) {
            g_spin_flip_at = now + 3000;
            g_spin_dir = -g_spin_dir;
        }
    }

    const float wobble = 1.0f + jitter * 0.1f * ((float)(rand() % 1000) / 1000.0f - 0.5f);
    int dx = (int)(speed * 4.0f * wobble * (float)g_spin_dir);
    if (dx > -1 && dx < 1) dx = g_spin_dir >= 0 ? 1 : -1;

    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// AttackSprintReset：对齐原版 combat/attacksprintreset.py ——
// 命中那一刻取消疾跑，并保持 4 个 tick（原版 _suppress_ticks = 4）。
// 我们没有「命中」事件，用物理左键按下当攻击信号（注入的点击不算）。
// 能掐掉的只有**我们自己**按住的那个 Ctrl；如果玩家是自己按着 Ctrl 疾跑，
// 这里再注入一次 Ctrl 松开会和他打架，所以那种情况不动手，只记一次日志。
void TickAttackSprintReset() {
    const int fi = FindFeature(L"AttackSprintReset");
    const bool on = Active(fi);
    const bool down = input::PhysLeftDown();
    const bool pressed = down && !g_prev_phys_left;
    g_prev_phys_left = down;

    if (!on) {
        if (g_sprint_suppress_until != 0) g_sprint_suppress_until = 0;
        g_asr_warned = false;
        return;
    }
    if (!pressed) return;

    if (g_sprint_held) {
        g_sprint_suppress_until = GetTickCount64() + 200;   // 4 tick
        UnholdKey(VK_LCONTROL);
        g_sprint_held = false;
    } else if (!g_asr_warned) {
        g_asr_warned = true;
        LogLine("[mod] AttackSprintReset 需要「自动疾跑」同时开启才能掐断疾跑");
    }
}

}  // namespace

bool Implemented(const wchar_t* english_name) {
    if (english_name == nullptr) return false;
    // 纯 GUI / HUD 类：本来就不需要游戏交互
    // 注意 HUD / TitleRender 不在这里：HUD 是关掉原版界面元素、TitleRender 是
    // 把称号画在实体头顶，两者都要读游戏状态，现在做不到，别谎报成已实现。
    static const wchar_t* kGuiOnly[] = {
        L"ClickGui", L"ConfigSave", L"FPS",      L"CPS",
        L"ArrayList", L"Keystrokes", L"Notification", L"Debug",
    };
    for (size_t i = 0; i < sizeof(kGuiOnly) / sizeof(kGuiOnly[0]); ++i) {
        if (wcscmp(kGuiOnly[i], english_name) == 0) return true;
    }
    // 靠输入合成实现的
    static const wchar_t* kSynth[] = {
        L"AC", L"Sprint", L"AntiAFK", L"AutoFish",
        L"NoJumpDelay", L"NoThrowDelay", L"Spammer", L"Spin", L"AttackSprintReset",
    };
    for (size_t i = 0; i < sizeof(kSynth) / sizeof(kSynth[0]); ++i) {
        if (wcscmp(kSynth[i], english_name) == 0) return true;
    }
    return false;
}

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
    g_afk_walking = false;
    g_jump_next = 0;
    g_throw_next = 0;
    g_spam_phase = 0;
    g_spam_next = 0;
    g_spin_next = 0;
    g_sprint_suppress_until = 0;
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
        input::SetPhysicalWanted(false);   // 面板打开时把钩子摘掉，别让输入多绕一圈
        input::SetPhysicalKeyWanted(false);
        ReleaseAll();
        return;
    }
    // 只有真正需要"物理按键状态"的模块开着时才挂钩子，其余时间不给输入加任何额外环节
    {
        const int ac = FindFeature(L"AC");
        const int asr = FindFeature(L"AttackSprintReset");
        const int ntd = FindFeature(L"NoThrowDelay");
        const bool need_mouse = (ac >= 0 && cfg::ModuleOn(ac)) ||
                                (asr >= 0 && cfg::ModuleOn(asr)) ||
                                (ntd >= 0 && cfg::ModuleOn(ntd));
        input::SetPhysicalWanted(need_mouse);
    }
    {
        const int njd = FindFeature(L"NoJumpDelay");
        input::SetPhysicalKeyWanted(njd >= 0 && cfg::ModuleOn(njd));
    }
    TickAutoClicker();
    TickAttackSprintReset();
    TickSprint();
    TickAntiAfk();
    TickAntiAfkRelease();
    TickAutoFish();
    TickJumpSpam();
    TickNoThrowDelay();
    TickSpammer();
    TickSpin();
}

}  // namespace modules
}  // namespace xiaoyu
