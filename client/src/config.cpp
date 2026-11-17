// config.cpp
#include "config.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <map>
#include <string>
#include <vector>

#include "gui/catalog.h"
#include "log.h"

namespace xiaoyu {
namespace cfg {
namespace {

const wchar_t* kPathDefault = L"C:\\Xiaoyu\\native_config.txt";

// 当前生效的配置文件路径。原来是编译期常量，加配置档之后必须是可变的：
// 切档 = 保存旧路径 → 重置成默认值 → 从新路径读。
wchar_t g_path[MAX_PATH] = L"C:\\Xiaoyu\\native_config.txt";
int g_profile = 0;

std::vector<char> g_on;
std::vector<int> g_bind;
std::vector<float> g_val;
std::vector<char> g_has;

struct PanelConf {
    bool has = false;
    float x = 0.0f;
    float y = 0.0f;
    bool collapsed = false;
};
PanelConf g_panels[GROUP_COUNT];

float g_scale = 0.0f;   // 0 = 自动：按窗口大小算缩放，见 gui.cpp 的 AutoScale()
bool g_chinese = true;
bool g_block_input = true;
bool g_auto_unlock = false;
bool g_take_focus = false;   // 抢前台会让基岩版弹出自己的 Esc 菜单，默认关
int g_unlock_key = 0xA4;  // VK_LMENU

bool g_dirty = false;
ULONGLONG g_last_change = 0;

std::map<std::wstring, int> g_name_to_index;
bool g_index_ready = false;

void BuildIndex() {
    if (g_index_ready) return;
    for (int i = 0; i < kFeatureCount; ++i) {
        g_name_to_index[kFeatures[i].en] = i;
    }
    g_index_ready = true;
}

void EnsureArrays() {
    if ((int)g_on.size() != kFeatureCount) {
        g_on.assign(kFeatureCount, 0);
        g_bind.assign(kFeatureCount, 0);
    }
    if ((int)g_val.size() != kParamCount) {
        g_val.assign(kParamCount, 0.0f);
        g_has.assign(kParamCount, 0);
    }
}

int FindFeature(const std::wstring& name) {
    BuildIndex();
    auto it = g_name_to_index.find(name);
    return it == g_name_to_index.end() ? -1 : it->second;
}

int FindParam(int feature, const std::wstring& attr) {
    if (feature < 0 || feature >= kFeatureCount) return -1;
    const FeatureInfo& f = kFeatures[feature];
    for (int i = 0; i < f.paramCount; ++i) {
        int pi = f.paramFirst + i;
        if (attr == kParams[pi].attr) return pi;
    }
    return -1;
}

std::wstring Trim(const std::wstring& s) {
    // Save 写出来的文件带 UTF-8 BOM，读回来第一个字符是 U+FEFF。
    // 它不属于 isspace 那套，会挂在 key 最前面，导致 key[0] == L'm' 之类的判断全部落空。
    // 这里按码位显式跳过，而不是塞进 find_first_not_of 的字符集里（宽字面量里的
    // \uFEFF 转义在 MSVC 下行为不够确定）。
    size_t start = 0;
    if (!s.empty() && s[0] == 0xFEFF) start = 1;

    size_t a = s.find_first_not_of(L" \t\r\n", start);
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    if (b < a) return L"";
    return s.substr(a, b - a + 1);
}

void SplitOnce(const std::wstring& s, wchar_t sep, std::wstring* a, std::wstring* b) {
    size_t p = s.find(sep);
    if (p == std::wstring::npos) {
        *a = s;
        b->clear();
        return;
    }
    *a = s.substr(0, p);
    *b = s.substr(p + 1);
}

}  // namespace

void Load() {
    EnsureArrays();
    BuildIndex();

    FILE* f = _wfopen(g_path, L"r, ccs=UTF-8");
    if (f == nullptr) {
        LogLine("[cfg] no config yet (%ls)", g_path);
        return;
    }

    wchar_t line[1024];
    int n = 0;
    bool saw_focus = false;   // focus= 只有新版本才会写
    while (fgetws(line, 1024, f) != nullptr) {
        std::wstring s = Trim(line);
        if (s.empty() || s[0] == L'#') continue;

        std::wstring key, value;
        SplitOnce(s, L'=', &key, &value);
        key = Trim(key);
        value = Trim(value);
        if (key.empty()) continue;
        ++n;

        if (key == L"scale") {
            // 0 表示"自动"（按窗口大小算）。老版本写过 scale=2.000，
            // 那个值会一直把 GUI 撑得很大，这里允许用户明确写 0 退回自动。
            float v = (float)_wtof(value.c_str());
            if (v == 0.0f) {
                g_scale = 0.0f;
            } else if (v >= 0.8f && v <= 6.0f) {
                g_scale = v;
            }
            continue;
        }
        if (key == L"chinese") {
            g_chinese = (_wtoi(value.c_str()) != 0);
            continue;
        }
        if (key == L"blockinput") {
            g_block_input = (_wtoi(value.c_str()) != 0);
            continue;
        }
        if (key == L"focus") {
            // 抢前台这条路已经废弃：基岩版失焦会弹自己的 Esc 菜单，
            // 和 GUI 抢置顶，关掉 GUI 后游戏还停在菜单里。
            // 旧配置里写着 focus=1 也一律按关处理。
            g_take_focus = false;
            saw_focus = true;
            continue;
        }
        if (key == L"unlock") {
            g_auto_unlock = (_wtoi(value.c_str()) != 0);
            continue;
        }
        if (key == L"unlockkey") {
            int vk = _wtoi(value.c_str());
            if (vk > 0 && vk < 256) g_unlock_key = vk;
            continue;
        }

        if (key.size() > 2 && key[0] == L'm' && key[1] == L'.') {
            int idx = FindFeature(key.substr(2));
            if (idx >= 0) g_on[idx] = (_wtoi(value.c_str()) != 0) ? 1 : 0;
            continue;
        }
        if (key.size() > 2 && key[0] == L'b' && key[1] == L'.') {
            int idx = FindFeature(key.substr(2));
            if (idx >= 0) g_bind[idx] = _wtoi(value.c_str());
            continue;
        }
        if (key.size() > 2 && key[0] == L'p' && key[1] == L'.') {
            std::wstring rest = key.substr(2);
            std::wstring fname, attr;
            SplitOnce(rest, L'.', &fname, &attr);
            int fi = FindFeature(fname);
            int pi = FindParam(fi, attr);
            if (pi >= 0) {
                g_val[pi] = (float)_wtof(value.c_str());
                g_has[pi] = 1;
            }
            continue;
        }
        if (key.size() > 6 && key.compare(0, 6, L"panel.") == 0) {
            std::wstring gname = key.substr(6);
            int gi = -1;
            for (int i = 0; i < GROUP_COUNT; ++i) {
                if (gname == kGroups[i].en) {
                    gi = i;
                    break;
                }
            }
            if (gi < 0) continue;
            float x = 0.0f, y = 0.0f;
            int col = 0;
            if (swscanf_s(value.c_str(), L"%f,%f,%d", &x, &y, &col) >= 2) {
                g_panels[gi].has = true;
                g_panels[gi].x = x;
                g_panels[gi].y = y;
                g_panels[gi].collapsed = (col != 0);
            }
            continue;
        }
    }
    fclose(f);

    // 旧版配置里 scale=2.000 是当年写死的默认值，unlock=1 是后来被证伪的
    // "合成 Alt" 方案。这两项如果原样读回来，升级后界面还是铺满屏幕、
    // 关闭面板还是会把游戏卡在光标模式里。老配置只认一次，之后就正常保存了。
    if (!saw_focus) {
        if (g_scale > 0.0f) {
            LogLine("[cfg] 旧版配置 scale=%.2f 已重置为自动", g_scale);
        }
        g_scale = 0.0f;
        g_auto_unlock = false;
        g_take_focus = false;
    }

    LogLine("[cfg] loaded %d lines, scale=%.2f chinese=%d", n, g_scale,
            g_chinese ? 1 : 0);
}

void Save() {
    EnsureArrays();

    FILE* f = _wfopen(g_path, L"w, ccs=UTF-8");
    if (f == nullptr) {
        LogLine("[cfg] Save failed: cannot open %ls", g_path);
        return;
    }

    fwprintf(f, L"# xiaoyu native config\n");
    fwprintf(f, L"scale=%.3f\n", g_scale);
    fwprintf(f, L"chinese=%d\n", g_chinese ? 1 : 0);
    fwprintf(f, L"blockinput=%d\n", g_block_input ? 1 : 0);
    fwprintf(f, L"focus=%d\n", g_take_focus ? 1 : 0);
    fwprintf(f, L"unlock=%d\n", g_auto_unlock ? 1 : 0);
    fwprintf(f, L"unlockkey=%d\n", g_unlock_key);

    for (int i = 0; i < kFeatureCount; ++i) {
        if (g_on[i]) fwprintf(f, L"m.%ls=1\n", kFeatures[i].en);
        if (g_bind[i] != 0) fwprintf(f, L"b.%ls=%d\n", kFeatures[i].en, g_bind[i]);
    }
    for (int i = 0; i < kParamCount; ++i) {
        if (!g_has[i]) continue;
        int fi = -1;
        for (int k = 0; k < kFeatureCount; ++k) {
            const FeatureInfo& fe = kFeatures[k];
            if (i >= fe.paramFirst && i < fe.paramFirst + fe.paramCount) {
                fi = k;
                break;
            }
        }
        if (fi < 0) continue;
        fwprintf(f, L"p.%ls.%ls=%.4f\n", kFeatures[fi].en, kParams[i].attr, g_val[i]);
    }
    for (int gi = 0; gi < GROUP_COUNT; ++gi) {
        if (!g_panels[gi].has) continue;
        fwprintf(f, L"panel.%ls=%.2f,%.2f,%d\n", kGroups[gi].en, g_panels[gi].x,
                 g_panels[gi].y, g_panels[gi].collapsed ? 1 : 0);
    }
    fclose(f);
    g_dirty = false;
    LogLine("[cfg] saved");
}

void MarkDirty() {
    g_dirty = true;
    g_last_change = GetTickCount64();
}

void Tick() {
    if (!g_dirty) return;
    if (GetTickCount64() - g_last_change < 1500) return;
    Save();
}

float Scale() { return g_scale; }
void SetScale(float s) {
    if (s < 0.8f) s = 0.8f;
    if (s > 6.0f) s = 6.0f;
    g_scale = s;
    MarkDirty();
}

bool Chinese() { return g_chinese; }
void SetChinese(bool v) {
    g_chinese = v;
    MarkDirty();
}

bool ModuleOn(int index) {
    EnsureArrays();
    if (index < 0 || index >= kFeatureCount) return false;
    return g_on[index] != 0;
}

void SetModuleOn(int index, bool on) {
    EnsureArrays();
    if (index < 0 || index >= kFeatureCount) return;
    g_on[index] = on ? 1 : 0;
    MarkDirty();
}

int ModuleBind(int index) {
    EnsureArrays();
    if (index < 0 || index >= kFeatureCount) return 0;
    return g_bind[index];
}

void SetModuleBind(int index, int vk) {
    EnsureArrays();
    if (index < 0 || index >= kFeatureCount) return;
    g_bind[index] = vk;
    MarkDirty();
}

float ParamValue(int index, float def) {
    EnsureArrays();
    if (index < 0 || index >= kParamCount) return def;
    if (!g_has[index]) {
        g_val[index] = def;
        return def;
    }
    return g_val[index];
}

void SetParamValue(int index, float v) {
    EnsureArrays();
    if (index < 0 || index >= kParamCount) return;
    g_val[index] = v;
    g_has[index] = 1;
    MarkDirty();
}

bool PanelPos(int group, float* x, float* y, bool* collapsed) {
    if (group < 0 || group >= GROUP_COUNT) return false;
    if (!g_panels[group].has) return false;
    if (x) *x = g_panels[group].x;
    if (y) *y = g_panels[group].y;
    if (collapsed) *collapsed = g_panels[group].collapsed;
    return true;
}

void SetPanelPos(int group, float x, float y, bool collapsed) {
    if (group < 0 || group >= GROUP_COUNT) return;
    g_panels[group].has = true;
    g_panels[group].x = x;
    g_panels[group].y = y;
    g_panels[group].collapsed = collapsed;
    MarkDirty();
}

bool ParamIsSet(int index) {
    if (index < 0 || index >= (int)g_has.size()) return false;
    return g_has[index] != 0;
}

void ClearParam(int index) {
    if (index < 0 || index >= (int)g_has.size()) return;
    if (g_has[index] == 0) return;
    // 把"用户显式设过"的标记摘掉就够了：值本身没人读（ParamValue 会回落到
    // ParamDefault），Save() 也只写 g_has 为真的项，下次落盘这一行就消失了。
    g_has[index] = 0;
    g_val[index] = 0.0f;
    MarkDirty();
}

bool HudEnabled(int feature_index) { return ModuleOn(feature_index); }

void SetHudEnabled(int feature_index, bool on) { SetModuleOn(feature_index, on); }

bool BlockInput() { return g_block_input; }
void SetBlockInput(bool v) {
    g_block_input = v;
    MarkDirty();
}

bool TakeFocus() { return g_take_focus; }
void SetTakeFocus(bool v) {
    if (g_take_focus == v) return;
    g_take_focus = v;
    MarkDirty();
}

bool AutoUnlock() { return g_auto_unlock; }
void SetAutoUnlock(bool v) {
    g_auto_unlock = v;
    MarkDirty();
}

int UnlockKey() { return g_unlock_key; }
void SetUnlockKey(int vk) {
    if (vk > 0 && vk < 256) {
        g_unlock_key = vk;
        MarkDirty();
    }
}

// ── 配置档 ──────────────────────────────────────────────────────

namespace {

void ProfilePath(int slot, wchar_t* out, size_t cap) {
    if (slot <= 0) {
        wcsncpy_s(out, cap, kPathDefault, _TRUNCATE);
        return;
    }
    swprintf_s(out, cap, L"C:\\Xiaoyu\\native_config_%d.txt", slot);
}

// 切档前必须把内存状态清回默认值。
// 只做"读新文件"是不够的：新档文件可能还不存在，Load() 会直接返回，
// 上一个档的开关/参数就全留在内存里，看起来像"切了档但没变"。
void ResetDefaults() {
    EnsureArrays();
    g_on.assign(kFeatureCount, 0);
    g_bind.assign(kFeatureCount, 0);
    g_val.assign(kParamCount, 0.0f);
    g_has.assign(kParamCount, 0);
    for (int i = 0; i < GROUP_COUNT; ++i) {
        g_panels[i].has = false;
        g_panels[i].x = 0.0f;
        g_panels[i].y = 0.0f;
        g_panels[i].collapsed = false;
    }
    g_scale = 0.0f;          // 自动
    g_chinese = true;
    g_block_input = true;
    g_auto_unlock = false;
    g_take_focus = false;
    g_unlock_key = 0xA4;     // VK_LMENU
    g_dirty = false;
    g_last_change = 0;
}

}  // namespace

int CurrentProfile() { return g_profile; }

void SwitchProfile(int slot) {
    if (slot < 0) slot = 0;
    if (slot > 3) slot = 3;
    if (slot == g_profile) return;

    // 先把当前档的改动落盘，否则切走就全丢了
    Save();
    const int from = g_profile;
    ResetDefaults();
    ProfilePath(slot, g_path, MAX_PATH);
    g_profile = slot;
    Load();
    LogLine("[cfg] 配置档 %d -> %d（%ls）", from, slot, g_path);
}

}  // namespace cfg
}  // namespace xiaoyu
