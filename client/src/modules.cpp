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
#include <math.h>
#include <cwchar>
#include <wchar.h>
#include <stdlib.h>

#include "config.h"
#include "gui.h"
#include "gui/catalog.h"
#include "input.h"
#include "log.h"
#include "sdk.h"

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

// 模块是否真的该干活：开关打开 + 面板/HUD 编辑模式收起
bool Active(int fi) {
    if (fi < 0) return false;
    if (!cfg::ModuleOn(fi)) return false;
    // 用 WantsMouse 而不是 Visible：HUD 布局模式下也不该继续合成点击，
    // 否则拖元素的时候 AC/NoThrowDelay 会在背后一直往游戏里灌鼠标事件
    if (gui::WantsMouse()) return false;
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

// ── Aimbot ──────────────────────────────────────────────────────
//
// 视角不在 Actor 上：实测 LocalPlayer 前 24KB 里没有任何 float 跟着视角变，
// 把 clientinstance / level / actorsmgr / options / localplayer 五个根对象全差分，
// 也只有 ClientInstance 在动。所以瞄准只能走游戏自己的摄像机：
//   ClientInstance + 0x4E0  行主序 4×4 旋转矩阵，列 = right / up / -forward
//   ClientInstance + 0x520  Projection，p00 在 +0x00、p11 在 +0x14
//   ClientInstance + 0x560  相机位置（与 LocalPlayer.mPos 完全相等，
//                           说明 mPos 本来就是眼睛位置，不要再加眼高）
//
// 角度 -> 鼠标计数：实测 look 300（MOUSEEVENTF_MOVE 的 mickey）让 yaw 走
// +79.7°，即 0.2655°/计数。这个系数跟着游戏内灵敏度走，换灵敏度要重新标。
// 系统指针加速不影响它 —— 游戏用 Raw Input 读原始 mickey，不走加速曲线。
//
// 为什么走闭环而不是算完一次转到位：
//   1. 一次到位是标准的「瞬移甩枪」指纹，回放一眼能看出来；
//   2. 系数一漂（换灵敏度、DPI、动态 FOV），开环就越偏越多；
//   3. 闭环每 tick 重读摄像机，取整误差下一 tick 自己纠回来。
constexpr float kDegPerCount = 0.2655f;
constexpr float kAimMaxStepDeg = 2.5f;    // speed=1 时每 tick 的最大转角
constexpr int   kAimMinIntervalMs = 15;   // 最快 ~66Hz，和渲染帧率解耦
constexpr int   kAimPickMs = 200;         // 重挑目标的间隔
constexpr int   kAimClickWindowMs = 280;  // onclick 的持续窗口

ULONGLONG g_aim_next = 0;
float g_aim_frac_x = 0.0f;    // 上一 tick 没发出去的亚计数余量
float g_aim_frac_y = 0.0f;
bool g_aim_prev_left = false;
ULONGLONG g_aim_click_until = 0;
bool g_aim_warned_weapon = false;
uintptr_t g_aim_target = 0;
ULONGLONG g_aim_pick_at = 0;

float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// 归到 (-180, 180]
float Wrap180(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d <= -180.0f) d += 360.0f;
    return d;
}

// SendInput 只喂前台窗口。后台发出去既改不了游戏视角，又会把系统光标甩走。
bool GameForeground() {
    const HWND fg = GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void SendMouseMove(LONG dx, LONG dy) {
    if (dx == 0 && dy == 0) return;
    INPUT in = {};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

// 挑目标：aim_range 内、fov 锥内、离相机最近的那个。
// 比距离平方省一个 sqrt；角度筛选用单位向量点积对 cos(fov/2) 比。
bool PickAimTarget(const sdk::GameCamera& cam, float range, float fov_deg,
                   float y_off, sdk::Vec3* out, uintptr_t* out_actor) {
    uintptr_t list[512];
    const int n = sdk::ActorSetSnapshot(list, 512);
    if (n <= 0) return false;

    const uintptr_t lp = sdk::LocalPlayer();
    const float half_cos = cosf(Clampf(fov_deg, 1.0f, 360.0f) * 0.5f * 0.01745329f);
    float r = (range > 0.0f) ? range : 1.0f;
    float best_d2 = r * r;

    bool found = false;
    for (int i = 0; i < n; ++i) {
        if (list[i] == lp) continue;
        sdk::Vec3 p;
        if (!mem::Read(list[i] + sdk::kOffPos, &p, sizeof(p))) continue;
        const float dx = p.x - cam.eye.x;
        const float dy = p.y + y_off - cam.eye.y;
        const float dz = p.z - cam.eye.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < 0.04f || d2 > best_d2) continue;
        const float inv = 1.0f / sqrtf(d2);
        const float dot = (dx * cam.fx + dy * cam.fy + dz * cam.fz) * inv;
        if (dot < half_cos) continue;   // 在 fov 锥外
        best_d2 = d2;
        out->x = p.x;
        out->y = p.y + y_off;
        out->z = p.z;
        *out_actor = list[i];
        found = true;
    }
    return found;
}

void TickAimbot() {
    const int fi = FindFeature(L"Aimbot");
    if (!Active(fi)) {
        g_aim_next = 0;
        g_aim_frac_x = 0.0f;
        g_aim_frac_y = 0.0f;
        g_aim_target = 0;
        g_aim_pick_at = 0;
        g_aim_prev_left = false;
        g_aim_click_until = 0;
        g_aim_warned_weapon = false;
        return;
    }

    // ── 触发条件 ──────────────────────────────────────────────
    // onclick / onhold / bind_key 全没配 = 常开（原版也是这个语义）。
    // onclick 不只看按下沿，还给一个短窗口：游戏里攻击是按下瞬间判定，
    // 但视角要跨几 tick 才收敛，只瞄一帧等于没瞄。
    const bool on_click = ParamOf(fi, L"onclick", 0.0f) > 0.5f;
    const bool on_hold = ParamOf(fi, L"onhold", 0.0f) > 0.5f;
    const int bind = cfg::ModuleBind(fi);

    const bool left = input::PhysLeftDown();
    if (on_click) {
        if (left && !g_aim_prev_left) g_aim_click_until = GetTickCount64() + kAimClickWindowMs;
    } else {
        g_aim_click_until = 0;
    }
    g_aim_prev_left = left;

    const ULONGLONG now = GetTickCount64();
    bool want;
    if (on_click || on_hold || bind != 0) {
        want = false;
        if (on_click && now < g_aim_click_until) want = true;
        if (on_hold && left) want = true;
        if (bind != 0 && input::KeyDown(bind)) want = true;
    } else {
        want = true;
    }
    if (!want) {
        g_aim_next = 0;
        g_aim_frac_x = 0.0f;
        g_aim_frac_y = 0.0f;
        return;
    }

    // onweapon 要走「手持物品」链路（BlockSource -> 玩家背包 -> 选中槽），
    // 这条链还没打通。别假装实现了：只提示一次，按常开跑。
    if (ParamOf(fi, L"onweapon", 0.0f) > 0.5f) {
        if (!g_aim_warned_weapon) {
            g_aim_warned_weapon = true;
            LogLine("[mod] Aimbot 的「仅持武器」还没有手持物品链路，当前按常开处理");
        }
    } else {
        g_aim_warned_weapon = false;
    }

    if (!GameForeground()) { g_aim_next = 0; return; }
    if (now < g_aim_next) return;
    g_aim_next = now + kAimMinIntervalMs;

    // ── 摄像机 ────────────────────────────────────────────────
    const uintptr_t lp = sdk::LocalPlayer();
    if (lp == 0) return;
    const uintptr_t ci = sdk::ClientInstanceOf(lp);
    if (ci == 0) return;
    sdk::GameCamera cam;
    if (!sdk::ReadGameCamera(ci, &cam)) return;

    // ── 目标：每 kAimPickMs 重挑一次，中间只重读选中那个（省 RPM） ──
    const float range = ParamOf(fi, L"aim_range", 20.0f);
    const float fov = ParamOf(fi, L"fov", 90.0f);
    const float y_off = ParamOf(fi, L"y_offset", 0.0f);

    sdk::Vec3 tgt;
    bool have = false;
    if (g_aim_target != 0 && now < g_aim_pick_at) {
        sdk::Vec3 p;
        if (mem::Read(g_aim_target + sdk::kOffPos, &p, sizeof(p))) {
            tgt.x = p.x;
            tgt.y = p.y + y_off;
            tgt.z = p.z;
            have = true;
        }
    }
    if (!have) {
        g_aim_target = 0;
        sdk::ActorSetStart();
        have = PickAimTarget(cam, range, fov, y_off, &tgt, &g_aim_target);
        g_aim_pick_at = now + kAimPickMs;
        if (!have) { g_aim_target = 0; return; }
    }

    // ── 还差多少角度 ──────────────────────────────────────────
    // forward 约定（和 probe33 用「按住 W 的位移方向」量到的一致）：
    //   forward = (-sin(yaw), -sin(pitch), cos(yaw))
    //   yaw   = atan2(-fx, fz)        向右转 yaw 增大
    //   pitch = -asin(fy)             向下看 pitch 增大（MC 约定）
    const float dx = tgt.x - cam.eye.x;
    const float dy = tgt.y - cam.eye.y;
    const float dz = tgt.z - cam.eye.z;
    const float horiz = sqrtf(dx * dx + dz * dz);
    if (horiz < 1e-3f) return;

    const float yaw_cur = atan2f(-cam.fx, cam.fz) * 57.29578f;
    const float yaw_tgt = atan2f(-dx, dz) * 57.29578f;
    const float d_yaw = Wrap180(yaw_tgt - yaw_cur);

    const float pitch_cur = -asinf(Clampf(cam.fy, -1.0f, 1.0f)) * 57.29578f;
    const float pitch_tgt = -atan2f(dy, horiz) * 57.29578f;
    float d_pitch = pitch_tgt - pitch_cur;

    if (ParamOf(fi, L"lock_y", 0.0f) > 0.5f) d_pitch = 0.0f;

    // ── 本 tick 走多少 ────────────────────────────────────────
    // static：固定步长，直给；
    // dynamic：按剩余角度比例缩，快到了自动减速，收尾不会左右冲。
    const bool static_mode = ParamOf(fi, L"mode", 0.0f) > 0.5f;
    const float spd = Clampf(ParamOf(fi, L"speed", 3.0f), 0.1f, 10.0f);
    const float spdy = Clampf(ParamOf(fi, L"y_speed", 3.0f), 0.1f, 10.0f);
    const float step_x = kAimMaxStepDeg * spd;
    const float step_y = kAimMaxStepDeg * spdy;

    float move_yaw, move_pitch;
    if (static_mode) {
        move_yaw = Clampf(d_yaw, -step_x, step_x);
        move_pitch = Clampf(d_pitch, -step_y, step_y);
    } else {
        move_yaw = Clampf(d_yaw * 0.45f, -step_x, step_x);
        move_pitch = Clampf(d_pitch * 0.45f, -step_y, step_y);
    }
    // 死区：小于 0.15° 就别动，免得贴在目标上抖
    if (fabsf(move_yaw) < 0.15f) move_yaw = 0.0f;
    if (fabsf(move_pitch) < 0.15f) move_pitch = 0.0f;
    if (move_yaw == 0.0f && move_pitch == 0.0f) return;

    // ── 角度 -> 计数，余量累积 ────────────────────────────────
    // 不累积的话每 tick 都会因为取整丢一点，60Hz 下累积误差肉眼可见。
    g_aim_frac_x += move_yaw / kDegPerCount;
    g_aim_frac_y += move_pitch / kDegPerCount;
    LONG mx = (LONG)g_aim_frac_x;
    LONG my = (LONG)g_aim_frac_y;
    g_aim_frac_x -= (float)mx;
    g_aim_frac_y -= (float)my;
    if (mx > 3000) mx = 3000;
    if (mx < -3000) mx = -3000;
    if (my > 3000) my = 3000;
    if (my < -3000) my = -3000;
    SendMouseMove(mx, my);
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
    // 读游戏内存实现的（esp.cpp / sdk.h 的摄像机链路）。链路：
    //   LocalPlayer 静态全局 -> BlockSource -> Level -> ActorManager
    //   -> ECS registry -> ActorOwnerComponent / RotationComponent
    // ESP 用整条链路；CoordDisplay 只用 LocalPlayer -> mPos，所以它同时也是
    // 最便宜的一个"链路是否还活着"的探针。
    // Aimbot 用的是 ClientInstance 那条：静态 kClientInstance -> 4×4 旋转矩阵
    // + Projection + CameraPos，再来回合成鼠标位移。不走 mRot（Actor 上没这个字段）。
    static const wchar_t* kMemory[] = {L"ESP", L"CoordDisplay", L"Aimbot"};
    for (size_t i = 0; i < sizeof(kMemory) / sizeof(kMemory[0]); ++i) {
        if (wcscmp(kMemory[i], english_name) == 0) return true;
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
    g_aim_next = 0;
    g_aim_frac_x = 0.0f;
    g_aim_frac_y = 0.0f;
    g_aim_target = 0;
    g_aim_pick_at = 0;
    g_aim_click_until = 0;
    g_aim_prev_left = false;
}

void Tick() {
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        srand((unsigned)GetTickCount64());
    }
    // 面板或 HUD 布局模式一打开就立刻停手：我们的合成点击会激活游戏窗口，
    // 前台被抢回去之后面板就点不动了；布局模式下还会跟拖拽抢鼠标。
    if (gui::WantsMouse()) {
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
        const int aim = FindFeature(L"Aimbot");
        const bool need_mouse = (ac >= 0 && cfg::ModuleOn(ac)) ||
                                (asr >= 0 && cfg::ModuleOn(asr)) ||
                                (ntd >= 0 && cfg::ModuleOn(ntd)) ||
                                (aim >= 0 && cfg::ModuleOn(aim));
        input::SetPhysicalWanted(need_mouse);
    }
    {
        const int njd = FindFeature(L"NoJumpDelay");
        input::SetPhysicalKeyWanted(njd >= 0 && cfg::ModuleOn(njd));
    }
    TickAutoClicker();
    TickAttackSprintReset();
    TickAimbot();
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
