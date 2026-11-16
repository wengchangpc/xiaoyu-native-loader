# xiaoyu 原生客户端（DXGI 覆盖层）

给**网易《我的世界》基岩版（`Minecraft.Windows.exe`，x64）**用的原生 GUI 客户端。

## 为什么不用 Python 加载器

原方案是「注入 DLL → 找到进程里的 `python27.dll` → `PyRun_SimpleString` 跑现有 198 个 Python 文件」。
实机验证这条路在本客户端上走不通：

| 检查项 | 结果 |
|---|---|
| 进程模块表（119 个模块） | 没有 `python*.dll` |
| 全盘 `python2*/python3*.dll` | 只有 Chronal 工作台的**编译**环境里有一份 |
| 主程序导出表 | 无 `Py*`，只有 28 个 NGX/AMD 桩函数 |
| 主程序节表 | 14 个节、节名全空、熵 7.96–7.99、存在 `.winlice` 节 → **WinLicense/Themida 加壳** |
| `libenvsdk.dll` | `.text/.rdata/.data/.idata` 的 RSize 全为 0 → 运行时解密 |

结论：Python 被静态编进加壳的主程序里，磁盘上既没有可加载的运行时 DLL，也没有可解析的符号。
所以改成原生实现，不再依赖 Python。

## 结构

```
注入 xiaoyu_client.dll
  └─ DllMain 起线程（不能占着 loader lock 建 D3D 设备）
       └─ 建临时交换链拿 IDXGISwapChain vtable，挂第 8 项 Present
            └─ 游戏第一次 Present：取 HWND、子类化窗口、建 Direct2D 设备
                 └─ 每帧：LayoutPanels → 处理输入 → 绘制 → 原函数 Present
```

用 vtable 而不是 inline hook，是因为主程序代码段在磁盘上是加密的，按字节特征下钩子无从下手；
vtable 是运行时对象里的函数指针表，改它不需要碰被保护的代码段。

## 保留了什么

UI 是从原版 `ClickGui.py` 的 `nilore` 皮肤一比一搬过来的：

| 项目 | 值 |
|---|---|
| 栏宽 / 栏头高 / 行高 / 间距 / 行内缩 | 120 / 20 / 20 / 6 / 1 |
| 背景 / 栏头 / 行 / 行悬停 | `#171717` / `#131313` / `#202020` / `#272727` |
| 强调色 | `#2DE8CA`（45,232,202） |
| 面板透明度 | 0.94 |
| 分类顺序 | Combat / Movement / World / Visual / HUD / Misc / Client |
| 文字 | 微软雅黑，标题加粗；中英可切 |

模块目录**不是手抄的**，`tools/gen_catalog.py` 直接解析原版 `ClickGui.py`，导出：

- 116 个模块（含 `is_action` 标记）
- 124 条中文翻译（`CHINESE_FEATURES`）
- 554 条参数：86 组滑块 + 77 组开关/枚举，外加每个模块的快捷键行

所以名字、顺序、参数范围跟原版完全一致。

## 现在能用的

- 面板：拖动 / 单击栏头折叠 / 内容滚动 / 越界钳位 / 位置与折叠状态持久化
- 模块行：点一下切开关；右侧箭头展开参数
- 参数：滑块拖动、开关点击、枚举循环切换、快捷键绑定（点一下再按键）
- HUD：`FPS` / `CPS` / `ArrayList` / `Keystrokes`
- 整体缩放：`Ctrl + [` / `Ctrl + ]`
- 配置：`C:\Xiaoyu\native_config.txt`
- 日志：`C:\Xiaoyu\native.log`

## 还没搬过来的

模块**功能逻辑**仍然在 Python 里。要让 `KillAura`、`Fly`、`ESP` 这些真正生效，
需要把对应的内存读写 / 引擎调用用 C++ 重写，这是下一阶段的事。
当前点开关只会改变状态并存盘 —— UI 骨架先立住。

## 构建

```powershell
cd client
cmake -S . -B build -A x64
cmake --build build --config Release
```

产物 `build/Release/xiaoyu_client.dll`（必须 x64）。

推送到 `main` 或手动触发 `build-client` workflow，在 Artifacts 里下载 `xiaoyu-client-x64`。

## 使用

1. 启动游戏，**先进入一个存档**
2. 用注入器把 `xiaoyu_client.dll` 注入 `Minecraft.Windows.exe`
3. 默认按 **右 Shift** 开关面板
4. 看日志确认：`C:\Xiaoyu\native.log` 里应有
   ```
   [hook] installed. Present=...
   [d2d] ready fmt=... size=...
   [overlay] first frame ok, 1920x1080
   ```

## 已知限制

- 只做 x64；模拟器（x86）不适用
- 主程序带 Themida + CrashHunter，注入本身有被检测风险，这是取舍不是 bug
- 覆盖层渲染在 `Present` 里，若游戏切独占全屏需改成无边框窗口
