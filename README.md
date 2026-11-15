# xiaoyu-native-loader

给**网易《我的世界》中国版 基岩版（`Minecraft.Windows.exe`，x64）**用的注入式加载器（探针版）。

它**不是**把 Python 编译成 DLL——那件事不存在。它的做法是：

```
注入 DLL
  └─ DllMain 起线程（不能在 loader lock 里干活）
       └─ 枚举进程模块，找 python*.dll
            └─ GetProcAddress 动态解析 Py_IsInitialized / PyGILState_* / PyRun_SimpleString
                 └─ 等解释器初始化完成
                      └─ 把内嵌的 python/bootstrap.py 丢进游戏自己的解释器执行
```

所以它只是**加载器**，真正的逻辑还是 Python（可以复用现有 198 个文件的客户端）。

## 为什么先做探针

现有客户端完全建立在 ModSDK 之上（`mod.client.extraClientApi`、`_pynetmodule`、UI 控件）。
这些是引擎**加载了行为包的存档**里才初始化的。纯注入时有两个未知：

| 未知 | 后果 |
|---|---|
| `_pynetmodule` 是否已被引擎 import | 没加载就完全没有引擎绑定 |
| UI 管理器有没有该 mod 的 namespace | 没有则 `RegisterUI` 无处注册，ClickGui 建不出来 |

这两条不能猜，只能实机跑一次探针。探针结果决定后面走哪条路：

- **R1（复用）**：探针显示 `_pynetmodule` / `extraClientApi` 可用 → DLL 只当加载器，Python 代码原样复用。
- **R2（重写）**：探针显示不可用 → 只能 hook D3D11 `Present` + 输入自绘 GUI，用 C++ 重写功能。

## 构建

本地：

```powershell
python tools/embed.py            # 生成 src/embedded_bootstrap.h
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物：`build/Release/xiaoyu_loader.dll`（**必须 x64**，位数不对注入会失败）。

GitHub Actions：推送到 `main` 或手动 `workflow_dispatch`，跑完在 Artifacts 里下载
`xiaoyu-loader-x64`。工作流会顺带校验 PE 头的 `machine == 0x8664`，防止误出 32 位。

## 使用

1. 启动游戏，**先进入一个存档**（让 ModSDK 初始化；探针也会等到解释器就绪）
2. 用任意注入器把 `xiaoyu_loader.dll` 注入 `Minecraft.Windows.exe`
3. 看两个日志：

```
C:\Xiaoyu\loader.log    加载器自己的过程（找模块、解析导出、PyRun 返回值）
C:\Xiaoyu\probe.log     游戏进程里跑出来的探针报告
```

`loader.log` 里这几行是判读关键：

```
[py] module=python27.dll ok=1 run=1 flags=1 gil=1     ← 找到解释器且导出齐全
[py] version=2.7.18 ...                               ← 顺便确认运行时到底是 2.7 还是 3.x
[run] PyRun 返回 0                                    ← 0 才代表脚本没抛异常
```

`probe.log` 里重点看：

```
==== _pynetmodule ====
import OK / import FAIL: ...
==== mod.client.extraClientApi ====
import OK
  has GetEngineCompFactory: True
```

- 全是 `import OK` 且 `GetEngineCompFactory: True` → **R1 可行**，DLL 当加载器即可
- 任何一项 `import FAIL` → 需要改成「进世界后再注入」，仍失败就准备 R2

## 已知限制

- 只做 x64；模拟器（x86）不适用
- 通过 `PyRun_SimpleString` 执行，脚本必须 **纯 ASCII 且 2.7/3.x 双兼容**
  （py2 下这里是裸字节，没有 coding cookie，一个中文就 SyntaxError）
- `PyRun_SimpleString` 会吃掉异常只返回 -1，所以脚本内部自己 try/except 并写日志
- 注入本身在多数服务器眼里就是可疑行为（多一个非签名模块 + 远程线程），
  比行为包/MCP 路线更容易被检测 —— 这条是取舍，不是 bug

## 目录

```
src/dllmain.cpp        DllMain + 引导线程
src/log.cpp            文件日志（C:\Xiaoyu\loader.log，失败退回 %TEMP%）
src/python_host.cpp    找 python*.dll、解析导出、持 GIL 执行脚本、枚举模块
python/bootstrap.py    探针脚本（注入后在游戏进程里跑）
tools/embed.py         把 bootstrap.py 转成 C 字节数组
.github/workflows/     windows-latest + MSVC 出 x64 DLL
```
