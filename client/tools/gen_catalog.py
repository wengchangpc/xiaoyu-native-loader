# -*- coding: utf-8 -*-
"""从 Python 客户端的 ClickGui.py 里把模块目录与参数表原样抽出来，生成 C++ 头文件。

为什么用生成而不是手抄：116 个模块、124 条中文翻译、86 组滑块参数、77 组开关参数。
手抄一定漏项或错序，而原版注释明确要求「保持 FEATURE_DEFS 书写顺序，
不要按英文名排序（否则中文面板顺序错乱）」。

输出：src/gui/catalog.h
"""
from __future__ import print_function
import ast
import io
import os
import sys

SRC = r'C:\Users\creative_1\Desktop\源码\XiaoyuScripts\uiScript\ClickGui.py'
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'src', 'gui', 'catalog.h')


def find_assign(tree, name):
    for node in tree.body:
        if isinstance(node, ast.Assign):
            for t in node.targets:
                if isinstance(t, ast.Name) and t.id == name:
                    return node.value
    return None


def cstr(s):
    if s is None:
        return 'nullptr'
    out = []
    for ch in s:
        if ch == '"':
            out.append('\\"')
        elif ch == '\\':
            out.append('\\\\')
        elif ord(ch) < 128:
            out.append(ch)
        else:
            out.append('\\u%04X' % ord(ch))
    return 'L"%s"' % ''.join(out)


def main():
    if not os.path.isfile(SRC):
        # CI 上没有本地 Python 源码，直接用仓库里已提交的 catalog.h
        print('source not found, keeping committed catalog.h: %s' % SRC)
        return 0

    with io.open(SRC, 'r', encoding='utf-8') as f:
        text = f.read()
    tree = ast.parse(text)

    feature_defs = ast.literal_eval(find_assign(tree, 'FEATURE_DEFS'))
    group_order = ast.literal_eval(find_assign(tree, 'GROUP_ORDER'))
    chinese = ast.literal_eval(find_assign(tree, 'CHINESE_FEATURES'))
    cats = ast.literal_eval(find_assign(tree, 'CHINESE_CATEGORIES'))
    fparams = ast.literal_eval(find_assign(tree, 'FEATURE_PARAMS')) or {}
    ftoggles = ast.literal_eval(find_assign(tree, 'FEATURE_TOGGLES')) or {}
    fmodbinds = ast.literal_eval(find_assign(tree, 'FEATURE_MODULE_KEYBINDS')) or {}

    print('features : %d' % len(feature_defs))
    print('groups   : %d %s' % (len(group_order), group_order))
    print('zh names : %d' % len(chinese))
    print('sliders  : %d modules' % len(fparams))
    print('toggles  : %d modules' % len(ftoggles))

    # ---- 先收集枚举选项，给每个选项数组一个编号 ----
    opt_names = []      # [(feature, label, [options])]
    opt_lookup = {}     # (feature, label) -> index
    for fname, items in ftoggles.items():
        for it in items:
            if len(it) > 2 and it[2]:
                opt_lookup[(fname, it[0])] = len(opt_names)
                opt_names.append((fname, it[0], list(it[2])))

    # ---- 展平参数表 ----
    flat = []
    ranges = []
    for name, group, is_action in feature_defs:
        first = len(flat)
        for item in fparams.get(name, []) or []:
            if not isinstance(item, (list, tuple)) or len(item) != 5:
                continue
            label, attr, vmin, vmax, vstep = item
            flat.append(('PK_SLIDER', label, attr, float(vmin), float(vmax),
                         float(vstep), None, 0))
        for item in ftoggles.get(name, []) or []:
            if not isinstance(item, (list, tuple)) or len(item) < 2:
                continue
            label, attr = item[0], item[1]
            opts = item[2] if len(item) > 2 else None
            if opts:
                oi = opt_lookup.get((name, label))
                flat.append(('PK_ENUM', label, attr, 0.0, 0.0, 0.0, oi, len(opts)))
            else:
                flat.append(('PK_TOGGLE', label, attr, 0.0, 0.0, 0.0, None, 0))
        for item in fmodbinds.get(name, []) or []:
            if not isinstance(item, (list, tuple)) or len(item) != 2:
                continue
            flat.append(('PK_KEYBIND', item[0], item[1], 0.0, 0.0, 0.0, None, 0))
        if not fmodbinds.get(name) and not is_action:
            # 原版：每个已注册模块都补一条主快捷键行
            flat.append(('PK_KEYBIND', u'\u529f\u80fd\u5feb\u6377\u952e',
                         'bind_key', 0.0, 0.0, 0.0, None, 0))
        ranges.append((first, len(flat) - first))

    L = []
    w = L.append
    w('// 本文件由 tools/gen_catalog.py 自动生成，不要手改。')
    w('// 数据来源：XiaoyuScripts/uiScript/ClickGui.py')
    w('//   FEATURE_DEFS / GROUP_ORDER / CHINESE_FEATURES / CHINESE_CATEGORIES')
    w('//   FEATURE_PARAMS / FEATURE_TOGGLES / FEATURE_MODULE_KEYBINDS')
    w('#pragma once')
    w('')
    w('namespace xiaoyu {')
    w('')
    w('enum Group : int {')
    for i, g in enumerate(group_order):
        w('    GROUP_%s = %d,' % (g.upper(), i))
    w('    GROUP_COUNT = %d' % len(group_order))
    w('};')
    w('')
    w('struct GroupInfo {')
    w('    const wchar_t* en;')
    w('    const wchar_t* zh;')
    w('};')
    w('')
    w('inline const GroupInfo kGroups[GROUP_COUNT] = {')
    for g in group_order:
        w('    { %s, %s },' % (cstr(g), cstr(cats.get(g, g))))
    w('};')
    w('')
    w('enum ParamKind : int { PK_SLIDER = 0, PK_TOGGLE = 1, PK_ENUM = 2, PK_KEYBIND = 3 };')
    w('')
    w('struct ParamInfo {')
    w('    int  kind;')
    w('    const wchar_t* label;            // 原版显示文字（已本地化）')
    w('    const wchar_t* attr;             // 模块属性名，用于配置持久化')
    w('    float vmin, vmax, vstep;         // 仅 PK_SLIDER 有效')
    w('    const wchar_t* const* options;   // 仅 PK_ENUM 有效')
    w('    int  nopts;')
    w('};')
    w('')
    for i, (fname, label, opts) in enumerate(opt_names):
        w('static const wchar_t* const kOpt%d[] = { %s };'
          % (i, ', '.join(cstr(o) for o in opts)))
    if opt_names:
        w('')
    w('inline const ParamInfo kParams[] = {')
    for kind, label, attr, vmin, vmax, vstep, oi, nopts in flat:
        if oi is None:
            optexpr = 'nullptr, 0'
        else:
            optexpr = 'kOpt%d, %d' % (oi, nopts)
        w('    { %-11s %s, %s, %.4ff, %.4ff, %.4ff, %s },'
          % (kind + ',', cstr(label), cstr(attr), vmin, vmax, vstep, optexpr))
    w('};')
    w('')
    w('inline const int kParamCount = %d;' % len(flat))
    w('')
    w('struct FeatureInfo {')
    w('    const wchar_t* en;      // 英文名（内部标识 / 配置主键）')
    w('    const wchar_t* zh;      // 中文名（原版 CHINESE_FEATURES）')
    w('    int  group;             // 分类')
    w('    bool action;            // 原版第三列 is_action：一次性动作')
    w('    int  paramFirst;        // kParams 里的起始下标')
    w('    int  paramCount;')
    w('};')
    w('')
    w('inline const FeatureInfo kFeatures[] = {')
    per_group = {}
    for (name, group, is_action), (first, cnt) in zip(feature_defs, ranges):
        per_group[group] = per_group.get(group, 0) + 1
        w('    { %s, %s, %d, %-5s %d, %d },'
          % (cstr(name), cstr(chinese.get(name)),
             group_order.index(group),
             'true,' if is_action else 'false,', first, cnt))
    w('};')
    w('')
    w('inline const int kFeatureCount = %d;' % len(feature_defs))
    w('')
    w('inline int FeatureCountInGroup(int g) {')
    w('    int n = 0;')
    w('    for (int i = 0; i < kFeatureCount; ++i) {')
    w('        if (kFeatures[i].group == g) ++n;')
    w('    }')
    w('    return n;')
    w('}')
    w('')
    w('}  // namespace xiaoyu')

    out = '\n'.join(L) + '\n'
    with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
        f.write(out)

    print('\nparams   : %d total, %d modules with rows'
          % (len(flat), sum(1 for _f, c in ranges if c)))
    print('opt arrs : %d' % len(opt_names))
    print('per group: %s' % per_group)
    print('wrote %s (%d bytes)' % (OUT, len(out.encode('utf-8'))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
