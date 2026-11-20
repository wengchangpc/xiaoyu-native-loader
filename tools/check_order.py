#!/usr/bin/env python3
"""前向引用粗检：本机没有编译器，靠这个在推送前抓 C3861（找不到标识符）。

判定规则和 C++ 一致：**名字第一次"可见"的位置必须早于任何使用点**。
  * 可见 = 定义（`{` 结尾）**或**声明（`;` 结尾，含 `struct X;` 前向声明）
  * 有声明在前的，后面随便用 —— 不会误报（memory.h 的 Read 重载就是这样）

做法：逐行扫描，**不做跨行正则**（跨行 + 嵌套量词会回溯爆炸，之前踩过）。

局限（写清楚，别指望它能代替编译）：
  * 不做类型检查、不做重载解析、不认识宏生成的名字
  * 只报「定义/声明都在使用点之后」，对「压根没定义」不吭声（那通常是宏或成员函数）
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "client" / "src"
EXTS = {".h", ".hpp", ".cpp"}

RE_TYPE = re.compile(r"^\s*(?:typedef\s+)?(?:struct|class|union)\s+([A-Za-z_]\w*)")
RE_ENUM = re.compile(r"^\s*enum\s+(?:class\s+)?([A-Za-z_]\w*)")
RE_CONST = re.compile(r"^\s*(?:inline\s+|static\s+)?constexpr\s+[\w:<>,\*&\s]+\s([A-Za-z_]\w*)\s*=")
RE_CONST2 = re.compile(r"^\s*(?:static\s+)?const\s+[\w:<>,\*&\s]+\s([A-Za-z_]\w*)\s*=")
RE_NAME_PAREN = re.compile(r"([A-Za-z_]\w*)\s*\(")

KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "catch", "do", "else",
    "case", "new", "delete", "static_cast", "reinterpret_cast", "const_cast",
    "dynamic_cast", "decltype", "alignof", "typeid", "throw", "try", "operator",
    "defined", "assert",
}


def strip_comments(src: str) -> str:
    """去注释，保留行数（注释内容换成空格）。字符串/字符字面量原样保留。"""
    out = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            out.append("  ")
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        elif c == '"' or c == "'":
            quote = c
            out.append(c)
            i += 1
            while i < n:
                if src[i] == "\\":
                    out.append("  ")
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == quote:
                    i += 1
                    break
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def collect_symbols(lines: list[str]) -> tuple[dict[str, int], dict[str, int]]:
    """返回 (defs, decls)：名字 -> 最早的定义行 / 最早的声明行。"""
    defs: dict[str, int] = {}
    decls: dict[str, int] = {}

    def put_def(name: str, ln: int) -> None:
        if name in KEYWORDS or len(name) < 3:
            return
        if name not in defs:
            defs[name] = ln

    def put_decl(name: str, ln: int) -> None:
        if name in KEYWORDS or len(name) < 3:
            return
        if name not in decls:
            decls[name] = ln

    for idx, line in enumerate(lines):
        ln = idx + 1
        s = line.strip()
        if not s:
            continue

        # 类型 / 枚举：可能是定义（{）也可能是前向声明（;）
        m = RE_TYPE.match(line) or RE_ENUM.match(line)
        if m:
            (put_def if s.endswith("{") else put_decl)(m.group(1), ln)
            continue

        for rx in (RE_CONST, RE_CONST2):
            m = rx.match(line)
            if m:
                put_def(m.group(1), ln)

        if s.startswith(("return ", "if ", "while ", "for ", "switch ", "else ",
                         "#", "//", "using ", "typedef ", "template")):
            continue

        # 函数：定义（{ 结尾，或下一行是 {） vs 声明（; 结尾）
        ends_open = s.endswith("{")
        if not ends_open:
            nxt = lines[idx + 1].strip() if idx + 1 < len(lines) else ""
            ends_open = nxt.startswith("{") and not s.endswith(";")
        is_proto = s.endswith(";") and "(" in s and "=" not in s

        if not (ends_open or is_proto):
            continue

        m2 = re.search(r"([A-Za-z_]\w*)\s*\(", s)
        if not m2:
            continue
        pre = s[: m2.start()]
        if pre.rstrip().endswith((".", "->", "::")):
            continue   # 成员调用 / 限定名，不是本文件定义的符号
        (put_def if ends_open else put_decl)(m2.group(1), ln)

    return defs, decls


def check_macro_names(path: Path) -> list[str]:
    """抓「变量名撞 Windows 宏」这一类错误。

    来历（真实踩过，CI 报错但源码看起来完全正常）：
        bool near = false;          // windef.h: #define near
      展开后变成
        bool  = false;              // error C2513: 'bool': no variable declared before '='
      编译器报的位置和真正的问题不在一个地方，肉眼排查基本没有胜算。

    只查**声明变量**的位置（`类型 名字 =` / `类型 名字 ;`），
    不去查表达式里的用法 —— TRUE/NULL 那种在表达式里是正常的。
    """
    WIN_MACROS = {
        # win16 遗产，windef.h 里定义成空
        "near", "far", "small", "huge",
        # windef.h，除非显式 NOMINMAX
        "min", "max",
        # objbase.h: #define interface struct
        "interface",
    }
    TYPE_WORDS = (
        r"(?:bool|char|short|int|long|unsigned|float|double|signed|"
        r"size_t|uintptr_t|intptr_t|uint\d+_t|int\d+_t|"
        r"WORD|DWORD|BYTE|BOOL|UINT|LONG|ULONG|SHORT|USHORT|FLOAT|DOUBLE|"
        r"ULONGLONG|LONGLONG|HWND|HANDLE|HMODULE|HINSTANCE|LPVOID|DWORD_PTR)"
    )
    pat = re.compile(
        r"^\s*(?:static\s+|const\s+|constexpr\s+|volatile\s+|register\s+)*"
        + TYPE_WORDS + r"\s+(?:const\s+)?([A-Za-z_]\w*)\s*(?:=|;|\[|,|\))"
    )
    raw = path.read_text(encoding="utf-8", errors="replace")
    lines = strip_comments(raw).split("\n")
    problems = []
    for idx, line in enumerate(lines):
        m = pat.match(line)
        if not m:
            continue
        name = m.group(1)
        if name in WIN_MACROS:
            problems.append(
                f"{path.name}:{idx + 1}: 变量名 '{name}' 撞了 Windows 宏，"
                f"声明会被展开掉（改个名字，如 has_{name} / {name}_v）"
            )
    return problems


def check_file(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    lines = strip_comments(raw).split("\n")
    defs, decls = collect_symbols(lines)
    if not defs:
        return []

    problems = []
    for name, defline in defs.items():
        earliest = min(defline, decls.get(name, defline))
        pat = re.compile(r"(?<![\w:])" + re.escape(name) + r"\s*\(")
        for idx, line in enumerate(lines):
            ln = idx + 1
            if ln >= earliest:
                break
            if name not in line:
                continue
            if pat.search(line):
                where = "定义" if defline <= decls.get(name, defline) else "声明"
                problems.append(
                    f"{path.name}:{ln}: 用了 {name}，但它最早只到 {where}于 {earliest} 行"
                )
                break
    return problems


def main() -> int:
    files = [p for p in sorted(ROOT.rglob("*")) if p.suffix in EXTS]
    all_problems: list[str] = []
    for f in files:
        try:
            all_problems.extend(check_macro_names(f))
        except Exception as exc:  # noqa: BLE001
            print(f"跳过 {f.name}(宏检查): {exc}")
        try:
            all_problems.extend(check_file(f))
        except Exception as exc:  # noqa: BLE001
            print(f"跳过 {f.name}: {exc}")
    if not all_problems:
        print(f"OK：{len(files)} 个文件没发现前向引用 / 宏名冲突")
        return 0
    print(f"发现 {len(all_problems)} 处可疑问题：\n")
    for p in sorted(all_problems):
        print("  " + p)
    return 1


if __name__ == "__main__":
    sys.exit(main())
