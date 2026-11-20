// sdk.h - 已确认的偏移与链路（全部实测，不是猜的）
//
// 这个文件是"战果存档"：每一条都标注了怎么发现的、怎么验证的。
// 后面写模块时只认这里的东西，不再回头翻日志。
//
// ────────────────────────────────────────────────────────────────
// 目标客户端身份（★ 曾经搞错过一次，记在这里防止再错）
// ────────────────────────────────────────────────────────────────
//   真实运行路径  C:\MCLDownload\MinecraftBENetease\x64_mc\Minecraft.Windows.exe
//   不是          C:\Users\<user>\AppData\Local\Chronal\Bedrock\versions\...\
//                 那个只是磁盘上的一份，没有在跑。
//
//   构建路径（从 .exp 符号表读出来的）
//     D:/win/release/minecraftPE/build/win32_x64_netease_haldra_launcher/
//     Minecraft/Minecraft.Windows
//
//   渲染是 D3D12，不是 OpenGL：
//     目录里有 dxcompiler.dll / dxil.dll / nvngx_dlss.dll，
//     .exp 里 24 个 NVSDK_NGX_D3D12_* 导入（CreateFeature / EvaluateFeature /
//     GetParameters ...）。
//     曾经在 dllmain.cpp 里写过"实测是 OpenGL、没有 DXGI 交换链"——那个论据是
//     错的。但"不挂 Present、改用覆盖窗口"这个决定本身仍然成立，因为覆盖窗口
//     和游戏的渲染方式完全解耦。
//
//   PE 磁盘上完全加密：65,838,088 字节，ClientInstance / LocalPlayer / Actor /
//   Vec3 / BlockPos 等字符串全部 0 命中，前 7 个节连名字都没有，
//   .winlice 有 15MB 虚拟空间但磁盘占用 0。运行时才解密。
//
// ────────────────────────────────────────────────────────────────
// 运行时布局
// ────────────────────────────────────────────────────────────────
//   ImageBase(磁盘)  0x140000000
//   运行时基址       ASLR，必须用 mem::Base() 拿
//   SizeOfImage      0xD8F9000
//   节数             14
//
//   第 0 节（真 .text）：RVA 0x1000，vsize 0x088E2057（143MB）
//     磁盘熵 7.97（加密） -> 运行时熵 6.88（已解密）
//   .boot 节  RVA 0xD061000，运行时熵仍 7.958（保持加密）
//   .winlice 节 RVA 0xC125000，运行时熵 5.80（运行时才有内容）
//
// ────────────────────────────────────────────────────────────────
// ★ 静态全局：LocalPlayer 可以直接从固定 RVA 读出来
// ────────────────────────────────────────────────────────────────
//   RVA 0x0B6E9648   LocalPlayer*
//
//   读法（纳秒级，不需要扫堆）：
//       uintptr_t lp = *(uintptr_t*)(mem::Base() + 0x0B6E9648);
//
//   怎么发现的：拿本次会话确认的 LocalPlayer 绝对地址当 needle，
//   扫主模块所有非 .text 节，唯一命中就在这个 RVA。落点是 VA=0x0A666000
//   （VSize 0x013A76C0 = 20.6MB，磁盘只占 1.24MB）那一节 —— 典型的运行时
//   填充 BSS/.data。
//
//   周围结构：每 0x80 字节一组，形态是
//       +0x00  shared_ptr 的 ptr（或 _Ref_count_obj2<UControlBlock> 的 ctrl）
//       +0x08  对象指针
//       +0x20  std::mutex
//   相邻几组分别是 FlightReader / UserManager::Social / ProfilingManager /
//   Bedrock::Threading::PendingConditionals / ClientBlobCacheTrackingData /
//   DBStoragePerformanceData / PlayerRenderChunkModificationLatencyTracker /
//   LevelChunkMetaDataDebug。
//   这是 Bedrock::EnableNonOwnerReferences 的静态注册表，按注册顺序排列。
//
//   ClientInstance / MultiPlayerLevel / OverworldDimension 在这一节里零命中，
//   说明它们只被堆上的对象（Minecraft）持有，没有静态指针。
//
// ────────────────────────────────────────────────────────────────
// ★ 定位链路（每一步都由反向 RTTI 验过类名）
// ────────────────────────────────────────────────────────────────
//   LocalPlayer  = *(BASE + 0x0B6E9648)                 ← 静态全局
//   BlockSource  = *(LocalPlayer + 0x0DE8)
//   Dimension    = *(BlockSource + 0x0030)      // 实测 OverworldDimension
//   Level        = *(BlockSource + 0x0020)      // 实测 MultiPlayerLevel
//
//   另一条（从 ClientInstance 往下，需要先拿到 ClientInstance）：
//   Minecraft    = *(ClientInstance + 0x01B0)
//   wrapper      = *(Minecraft + 0x00B8)
//   Level        = *(wrapper + 0x0040)
//
//   ★ Level 的实际对象是派生类 MultiPlayerLevel，所以对象的 +0 放的是
//     MultiPlayerLevel 自己的 vtable。拿基类 Level 的 vtable 去扫堆永远是
//     0 命中 —— 这是 probe 里 Level/Dimension 找不到的原因。
//
// ────────────────────────────────────────────────────────────────
// LocalPlayer（= Actor 基类布局）字段
// ────────────────────────────────────────────────────────────────
//   +0x000  vtable
//   +0x368  float  0.6           碰撞箱宽度（常量锚点，Player 的 AABB）
//   +0x374  float  1.0
//   +0x378  float  0.4
//   +0xCE0  Vec3f  mPos          当前位置
//   +0xCEC  Vec3f  mPosPrev      上一帧位置（与 mPos 交替变化）
//   +0xDE8  BlockSource*         所在维度的方块访问器
//
//   怎么确认的：差分扫描 12 次 × 0.35s，+0xCE0 三连变化幅度
//   0.674 / 1.987 / 8.683，其余字段要么不动要么只在 ±0.1 抖动。
//   +0xCEC 与 +0xCE0 交替变化 -> mPos / mPosPrev。
//
//   ★ 这两个偏移是所有 Actor 共用的（都在 Actor 基类里），explorer.cpp
//     就是拿 "vtable 在主模块 + 这两个位置都是合法坐标 + 两者距离 < 64 米"
//     当判据来扫全堆认实体的。
//
// ────────────────────────────────────────────────────────────────
// vtable 表（RVA，用于运行时校验）
// ────────────────────────────────────────────────────────────────
//   ★ 2026-11-20 从本 build（3.10.5 / build 301511）的 IDA 符号库
//     （3dot10dot5_301511_unpacked_Minecraft.Windows.exe.i64）逐个读出来的
//     真值，不是猜的，也不是上一版留下的：
//
//       ??_7ClientInstance@@6B@    RVA 0x8B9A6F0
//       ??_7LocalPlayer@@6B@       RVA 0x8C97B08
//       ??_7Actor@@6B@             RVA 0x8F39A20
//       ??_7Level@@6B@             RVA 0x8FE28B0
//       ??_7Dimension@@6B@         RVA 0x908DE38
//       ??_7BlockSource@@6B@       RVA 0x8FDDFD8
//       ??_7Container@@6B@         RVA 0x8ACD800
//       ??_7Player@@6B@            RVA 0x8FA6348
//       ??_7Mob@@6B@               RVA 0x8F747C0
//       ??_7Minecraft@@6B@         RVA 0x8F39F00
//       ??_7ItemStack@@6B@         RVA 0x8FD9F38
//       ??_7ItemActor@@6B@         RVA 0x8FD0BA8
//
//   上一版这批值整体偏 +0x1000（Actor 偏 +0x1010），BlockSource 干脆是另一个
//   地址 —— 那是上一版客户端的布局。这个 build 的 .text 段比上一版大
//   23MB，节数也不同，两者不能混用。
//
//   probe.log 里有完整的 12 个类的 TypeDescriptor / COL / vtable 三元组。
//
// ────────────────────────────────────────────────────────────────
// 成员偏移（反向 RTTI 标注，vtable[-1] -> COL -> COL+12 -> TypeDescriptor）
// ────────────────────────────────────────────────────────────────
//   ClientInstance
//     +0x188  MinecraftGame
//     +0x1A0  RayTracingOptions
//     +0x1B0  Minecraft                  ★
//     +0x1C0  LevelRenderer
//     +0x1D8  LoopbackPacketSender
//     +0x1E0  VanillaMoveInputHandler
//     +0x1F0  MinecraftKeyboardManager
//     +0x1F8  HitDetectSystem
//     +0x210  VanillaSceneFactory
//     +0x760  ActorRenderDispatcher
//     +0x778  ItemInHandRenderer
//     +0x780  ItemRenderer
//
//   MultiPlayerLevel
//     +0x0410 LoopbackPacketSender
//     +0x0530 BedrockSpawner
//     +0x0560 BlockHashPalette
//     +0x0570 BlockReducer
//     +0x0590 PortalForcer
//     +0x05D0 ContainerRegistry
//     +0x0610 LevelRandom
//     +0x0630 LegacyClientNetworkHandler
//     +0x0650 Scheduler
//     +0x06B8 LevelSoundManager
//     +0x0728 EntityRegistry
//     +0x0748 Minecraft（回指）
//     +0x0768 EntitySystemsManager
//     +0x0778 FeatureTypeFactory
//     +0x07F8 BlockActorLevelListener
//     +0x0838 ActorManager               ★
//     +0x0840 SubscriptionBody<Actor&>
//
//   BlockSource
//     +0x0008 自己
//     +0x0020 MultiPlayerLevel
//     +0x0030 OverworldDimension         ★
//     +0x0788 BlockSource（另一个实例）
//     +0x07A0 MultiPlayerLevel
//     +0x07B0 OverworldDimension
//
//   OverworldDimension
//     +0x0058 自己
//     +0x0060 _Ref_count_obj2<OverworldDimension>
//     +0x00C0 MultiPlayerLevel
//     +0x00F0 BlockSource
//     +0x0140 OverworldBrightnessRamp
//     +0x0148 CompoundTag
//     +0x0150 _Ref_count_obj2<LevelChunkMetaData>
//     +0x0158 RuntimeLightingManager
//     +0x0188 BlockEventDispatcher
//     +0x0190 TaskGroup
//     +0x0198 TaskGroup
//     +0x01B0 NetworkChunkSource
//     +0x01C0 Weather
//     +0x0328 TickingAreaList
//     +0x05D8 ChunkBuildOrderPolicy
//     +0x0788 DelayActionList
//     +0x07D0 ClientDimensionExtensions
//     注：0x2600~0x2A00 那一大片是 Molang 编译产物，指针密度极低。
//
//   ActorManager
//     +0x0020 ClientActorManagerProxy     （实体表可能在这层，未验）
//     +0x0048 / +0x0050  registry B       ★ 有 rotation / statevector
//     +0x01C8 / +0x01D0  registry
//     +0x0270 / +0x0278  registry A       ★ 有 ActorOwnerComponent
//
// ────────────────────────────────────────────────────────────────
// ★★ 实体列表：Bedrock 用的是 EnTT 风格 ECS，不是 vector<Actor*>
// ────────────────────────────────────────────────────────────────
//   这条推翻了之前所有「在 Level / Dimension / ActorManager 里找
//   vector<Actor*>」的思路。实测（recon/ecs.py + players.py + rot.py）：
//
//     Level +0x0838 -> ActorManager
//     ActorManager+0x0270 -> registry A
//     registry A +0x0558  -> basic_storage<VActorOwnerComponent, EntityId>
//     storage +0x0020     -> packed EntityId 数组（4 字节一个）★长度在这
//     storage +0x0050     -> 「instance 分段」vector，只 1 个元素
//       *(storage+0x0050) -> 裸 Actor* 数组
//
//   ★ 数组后面紧跟字符串常量（在 [33] 读到 ASCII "position"），
//     所以**不能**靠「读到非法指针就停」来取长度。
//     真实长度 = (*(storage+0x28) - *(storage+0x20)) / 4，实测 33。
//
//   ★ ActorManager 下有 6 个 registry，只有 +0x0270 那个身上挂着
//     ActorOwnerComponent；+0x01C8 的 +0x478 处是空的（0xFFFFFFFF），
//     早期按 ecs.log 里另一个 registry 的偏移去读才读得到，别搞混。
//
// ────────────────────────────────────────────────────────────────
// ★★★ 旋转（yaw / pitch）：另一个 registry，用 EntityId 索引对齐
// ────────────────────────────────────────────────────────────────
//   registry A 里只有 mob 标记 + 摄像机组件，**没有 rotation**。
//   rotation 在 registry B（ActorManager+0x0048）：
//     registry B +0x0528 -> basic_storage<UActorRotationComponent>
//     registry B +0x0548 -> basic_storage<UStateVectorComponent>
//
//   实测三个 storage（ActorOwner / Rotation / StateVector）的 packed
//   EntityId 列表**完全一致、顺序也一致**（33 个），所以下标可以直接
//   互通：actors[i] 的 rotation 就是 rot_seg[i * 0x10]。
//
//   元素布局（实测反推）：
//     UActorRotationComponent  步长 0x10
//       +0x00 float  pitch（度，低头为正）
//       +0x04 float  yaw  （度，0=+Z，90=-X）
//       +0x08 Vec2   mRotPrev（同一对值）
//     UStateVectorComponent    步长 0x30
//       +0x00 Vec3 mPos
//       +0x0C Vec3 mPosPrev
//       +0x18 Vec3 mVelocity
//   怎么确认步长的：拿 7 个 Actor 已知的 +0xCE0 坐标在 statevector 段里
//   定位，命中位置是 0x00 / 0x30 / 0x60 / 0x90 / 0xC0 / 0xF0 / 0x120，
//   等差 0x30；而且第 k 组正好是 Actor#k，跨 storage 下标对齐成立。
//   rotation 段的原始 float 是 (pitch, yaw) 成对重复两次，例如
//   玩家 6.570 / 91.916，同场景 18 个 RemotePlayer 的 yaw 集中在
//   81~92 度、pitch 都是 0 左右 —— 符合「站成一排看着同一方向」。
//
// ────────────────────────────────────────────────────────────────
// ★ ItemActor 是未激活池，别当实体用
// ────────────────────────────────────────────────────────────────
//   实体表里有 14 个 ItemActor，但它们的 +0xCE0 全是 (0,0,0)，
//   而且在对象头 0x2000 字节里找不到任何「附近坐标」的三连浮点
//   （recon/entity2.py 全扫过）。更能说明问题的是 BlockSource：
//   18 个 RemotePlayer 的 +0xDE8 全指向有效的 BlockSource，
//   14 个 ItemActor 里 13 个是 0、剩下 1 个是 0x801 这种垃圾值。
//   结论：它们还没被放进世界，ESP 只画 Player 子类。
//
// ────────────────────────────────────────────────────────────────
// ★ 已排除的路径（避免重复劳动）
// ────────────────────────────────────────────────────────────────
//   1. Level 的直接成员里没有回指 LocalPlayer 的字段（grep 零命中）
//   2. Level 里所有 vector 候选都不是实体表：
//        两个最大的 1.5MB / 2MB 是方块更新队列的量级，元素大小对不上
//        +0x0438 的第 0 个是 BlockActorLevelListener（监听器列表）
//   3. ActorManager 里没有 vector<Actor*>；
//      逐 qword 直接当 Actor* 验坐标只出 4 个假阳性
//      （全是 _Ref_count_resource<Pack> 里的随机浮点）
//   4. Dimension 里同理
//   5. 在 ActorManager / Dimension / Level 的内存里搜 LocalPlayer 地址，
//      零命中 —— 实体表不是"对象里一串 Actor*"这种形态
//   6. 对象 +0 只会放最派生类的 vtable，扫基类 vtable 只会命中栈上的临时值
//   7. 外部扫堆会被反作弊几秒内静默终止（4 次验证），必须进程内做
//
// ────────────────────────────────────────────────────────────────
// 反作弊行为记录
// ────────────────────────────────────────────────────────────────
//   只读主模块映像（217MB，VirtualQueryEx 逐区域读）    -> 成功，0 页失败
//   遍历堆找对象 needle（4 次，各种分块策略）           -> 几秒内进程被静默终止
//                                                        日志停在途中，连
//                                                        traceback 都来不及写
//   进程内扫堆（probe / explorer）                      -> 成功
//
//   结论：反作弊盯的是"外部进程反复访问堆"这个行为模式，
//         进程内读自己的内存不受影响。
#pragma once

#include <windows.h>

#include <cmath>
#include <cstdint>

#include "log.h"
#include "memory.h"

namespace xiaoyu {
namespace sdk {

// ── 静态全局 ────────────────────────────────────────────────────
constexpr uintptr_t kLocalPlayerGlobalRva = 0x0B6E9648;

// ── 本 build 的 vftable RVA（从 IDA 符号库按类名读出的真值）────────
//
// 这些值是「对象第一个指针 == Base()+RVA」的判据，用来在运行时确认
// 我们手里的指针确实是那个类的实例。客户端一换 build 就会全部失效，
// 所以 ChainReport() 会把每一个比对结果写进日志 —— 只要有一行 MISMATCH，
// 就说明这份 DLL 不是这个客户端的，别再去调上层逻辑。
namespace vt {
constexpr uintptr_t kClientInstance = 0x8B9A6F0;
constexpr uintptr_t kLocalPlayer    = 0x8C97B08;
constexpr uintptr_t kActor          = 0x8F39A20;
constexpr uintptr_t kLevel          = 0x8FE28B0;
constexpr uintptr_t kDimension      = 0x908DE38;
constexpr uintptr_t kBlockSource    = 0x8FDDFD8;
constexpr uintptr_t kContainer      = 0x8ACD800;
constexpr uintptr_t kPlayer         = 0x8FA6348;
constexpr uintptr_t kMob            = 0x8F747C0;
constexpr uintptr_t kMinecraft      = 0x8F39F00;
constexpr uintptr_t kItemStack      = 0x8FD9F38;
constexpr uintptr_t kItemActor      = 0x8FD0BA8;
}  // namespace vt

// 代码段结束的 RVA。用来把「落在代码段里的命中」和「数据段里的全局变量」分开：
// 反查「谁持有某个对象」时，代码段里的命中都是 `mov reg, imm64` 之类的立即数，
// 真正的全局变量一定在 .text 之后。
//
// 本 build 节表：.text va=0x1000 vsize=0x88E1F67 → 下一节从 0x88E3000 开始。
// 这个值只用于粗筛，不要求精确 —— 猜小了大不了多留几个候选，猜大了会把
// 真正的全局漏掉，所以宁可取小一点。
constexpr uintptr_t kCodeEndRva = 0x88E3000;

// ── LocalPlayer / Actor 字段 ────────────────────────────────────
constexpr uintptr_t kOffPos = 0x0CE0;
constexpr uintptr_t kOffPosPrev = 0x0CEC;
constexpr uintptr_t kOffBlockSource = 0x0DE8;
constexpr uintptr_t kOffHitboxWidth = 0x0368;

// ── BlockSource 字段 ────────────────────────────────────────────
constexpr uintptr_t kOffBsLevel = 0x0020;
constexpr uintptr_t kOffBsDimension = 0x0030;

// ── Level(MultiPlayerLevel) 字段 ───────────────────────────────
constexpr uintptr_t kOffActorManager = 0x0838;
constexpr uintptr_t kOffLevelMinecraft = 0x0748;

// ── ClientInstance 字段 ─────────────────────────────────────────
constexpr uintptr_t kOffCiMinecraft = 0x01B0;

// ── Minecraft 字段 ──────────────────────────────────────────────
constexpr uintptr_t kOffMcLevelWrapper = 0x00B8;

// ── wrapper 字段 ────────────────────────────────────────────────
constexpr uintptr_t kOffWrapperLevel = 0x0040;

// ── Minecraft 字段 ──────────────────────────────────────────────
constexpr uintptr_t kOffMcClientInstanceA = 0x0018;   // 与下面同一个对象
constexpr uintptr_t kOffMcClientInstanceB = 0x0168;

// ── ClientInstance 字段 ─────────────────────────────────────────
constexpr uintptr_t kOffCiLevelRenderer = 0x0138;
constexpr uintptr_t kOffCiOptions = 0x0CC0;

struct Vec3 {
    float x;
    float y;
    float z;
};

// 判断一个地址像不像某个 SDK 类的实例：首 8 字节必须是主模块内的 vftable。
// 这一步是整个自动定位的地基 —— vftable 指针有三个好性质：
//   1) 值一定落在主模块映像里（mem::InImage 能挡住绝大多数垃圾）
//   2) 值一定 8 字节对齐，而且指向的地址本身是代码
//   3) 同一个类只有一个主 vftable，所以「首字等于 base+vtRVA」的对象基本唯一
inline bool LooksLikeObject(uintptr_t p) {
    if (p == 0 || (p & 7) != 0) return false;
    const uintptr_t vt = mem::ReadPtr(p);
    return vt != 0 && mem::InImage(vt);
}

// 对象首字是否正好是某个已知 vftable
inline bool HasVtable(uintptr_t p, uintptr_t vt_rva) {
    if (p == 0) return false;
    const uintptr_t vt = mem::ReadPtr(p);
    return vt == mem::Base() + vt_rva;
}

// ── 全局槽位自动定位 ────────────────────────────────────────────
//
// 硬编码的 kLocalPlayerGlobalRva 只对某一个 build 成立。这个客户端每次
// 更新都会把它整体挪走（实测上一个 build 的整批 vftable 都差 0x1000，
// 而全局偏移差得更多），一旦失配，LocalPlayer() 读出来的是**已释放的堆
// 内存** —— 首字是 0，整条 ESP 链路静默失效，还查不出原因。
//
// 所以这里做一次运行期反查，两跳：
//   ① 堆里找「首 8 字节 == base + vt::kLocalPlayer」的对象 → 真身地址
//   ② 主模块映像里找「值 == 这个地址」的位置 → 就是持有它的全局变量
// 两跳都不依赖任何预先知道的偏移，只有 vftable RVA 来自符号表（可靠）。
//
// 只在硬编码偏移失效时才跑，结果缓存在 LpState() 里。
//
// ★ 扫描必须放后台线程。全堆几 GB，即使 4MB 一块 + memchr 加速也要几十秒，
//   在渲染线程里做等于把游戏冻住 —— 用户只会看到画面卡死然后以为崩了。
//   所以这里拆成「主线程只查状态」+「后台线程扫描」，扫描期间 ESP 不画，
//   但游戏完全正常。
struct LpLocate {
    volatile LONG state = 0;    // 0=还没开始 1=扫描中 2=成功 3=失败
    uintptr_t global_rva = 0;   // 反查出来的全局槽位 RVA
    uintptr_t cached_lp = 0;
    size_t scanned_mb = 0;
    int tries = 0;              // 已经扫了几次
    ULONGLONG next_try = 0;     // 下次允许重扫的时间点
};

inline LpLocate& LpState() {
    static LpLocate s;
    return s;
}

// vtable 自检：RVA 指向的位置，头 8 字节必须是一个指向代码段的函数指针。
//
// 这一步零成本但极其关键。如果符号表给的 RVA 本身是错的（比如给的是
// 上一个 build 的值），那"扫堆找带这个 vtable 的对象"必然一个都扫不到，
// 而且失败日志只会说"没找到"，看不出根因是 RVA 错还是对象不存在。
// 先把每个类的 vtable 摸一遍，日志里 OK/BAD 一目了然。
inline void VtableSelfCheck() {
    struct Probe {
        const char* name;
        uintptr_t rva;
    };
    static const Probe kAll[] = {
        {"ClientInstance", vt::kClientInstance}, {"LocalPlayer", vt::kLocalPlayer},
        {"Player", vt::kPlayer},                 {"Actor", vt::kActor},
        {"Level", vt::kLevel},                   {"Dimension", vt::kDimension},
        {"BlockSource", vt::kBlockSource},       {"Minecraft", vt::kMinecraft},
    };
    const uintptr_t base = mem::Base();
    const uintptr_t code_end = base + kCodeEndRva;
    LogLine("[sdk] vtable 自检（首项应指向代码段 0x%llX..0x%llX）：",
            (unsigned long long)base, (unsigned long long)code_end);
    for (const Probe& p : kAll) {
        const uintptr_t vt = base + p.rva;
        const uintptr_t fn0 = mem::ReadPtr(vt);
        const bool ok = (fn0 >= base && fn0 < code_end);
        LogLine("[sdk]   %-14s rva=0x%-9llX 首项=0x%llX %s", p.name,
                (unsigned long long)p.rva, (unsigned long long)fn0,
                ok ? "OK" : "BAD（这个 RVA 不是 vtable）");
    }
}

// 在堆里找 LocalPlayer 真身。
//
// 只扫 LocalPlayer 一个 needle，不顺手把 Actor/Mob 也带上：Actor 的实例
// 有几十个，混在一起扫会瞬间把候选数组占满，真正的 LocalPlayer 反而被挤掉。
// 找不到再单独试一次 Player（多继承下有的 build 把主 vtable 指到基类那份）。
inline uintptr_t FindLocalPlayerObject(size_t* scanned_out) {
    const uintptr_t base = mem::Base();
    const uintptr_t try_rva[2] = {vt::kLocalPlayer, vt::kPlayer};
    size_t total = 0;

    for (int k = 0; k < 2; ++k) {
        const uint64_t needle = (uint64_t)(base + try_rva[k]);
        mem::HeapHit hits[32];
        size_t scanned = 0;
        // 30 秒上限：这个进程的私有可写内存不大，正常两三秒就扫完；
        // 万一 vtable RVA 是错的，扫不到就会一路扫到底 —— 那种情况下
        // 让它卡 30 秒再放弃，比拖垮游戏强。
        const int n = mem::ScanHeapQword(&needle, 1, hits, 32, &scanned,
                                         GetTickCount64() + 30000);
        total += scanned;
        LogLine("[sdk] 扫堆(%s)：%.1f MB，%d 个候选",
                k == 0 ? "LocalPlayer" : "Player",
                (double)scanned / (1024.0 * 1024.0), n);
        for (int i = 0; i < n; ++i) {
            LogLine("[sdk]   候选 #%d = 0x%llX", i, (unsigned long long)hits[i].addr);
        }
        if (n > 0) {
            if (scanned_out != nullptr) *scanned_out = total;
            return hits[0].addr;
        }
    }
    if (scanned_out != nullptr) *scanned_out = total;
    return 0;
}

// 反查谁持有这个对象。返回全局槽位的 RVA（相对主模块基址）。
inline uintptr_t FindGlobalHolding(uintptr_t obj) {
    uintptr_t hits[64];
    const int n = mem::FindAllQword((uint64_t)obj, hits, 64, 0, 0);
    const uintptr_t base = mem::Base();
    LogLine("[sdk] 反查持有 0x%llX 的槽位：%d 处", (unsigned long long)obj, n);
    // 全局变量在数据段：代码段里的命中都是 `mov reg, imm64` 的立即数，
    // 直接排除。剩下的取 RVA 最大的那个（.data 在 .text 后面）。
    uintptr_t best = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t rva = hits[i] - base;
        LogLine("[sdk]   rva=0x%llX%s", (unsigned long long)rva,
                rva < kCodeEndRva ? "（代码段，跳过）" : "");
        if (rva < kCodeEndRva) continue;
        if (rva > best) best = rva;
    }
    if (best == 0 && n > 0) best = hits[n - 1] - base;
    return best;
}

inline DWORD WINAPI LocateThread(LPVOID param) {
    (void)param;
    LpLocate& st = LpState();
    const ULONGLONG t0 = GetTickCount64();

    VtableSelfCheck();

    size_t scanned = 0;
    const uintptr_t obj = FindLocalPlayerObject(&scanned);
    st.scanned_mb = scanned / (1024 * 1024);

    if (obj != 0) {
        const uintptr_t rva = FindGlobalHolding(obj);
        if (rva != 0) {
            st.global_rva = rva;
            st.cached_lp = obj;
            InterlockedExchange(&st.state, 2);
            LogLine("[sdk] 定位成功：LocalPlayer 全局 RVA = 0x%llX，对象 = 0x%llX"
                    "（耗时 %llu ms，扫了 %llu MB；把这个 RVA 写死到 "
                    "kLocalPlayerGlobalRva 可以省掉每次扫描）",
                    (unsigned long long)rva, (unsigned long long)obj,
                    (unsigned long long)(GetTickCount64() - t0),
                    (unsigned long long)st.scanned_mb);

            // 对象有了，但坐标在哪个偏移还是未知（kOffPos 是上一个 build 的）。
            // 顺手把候选打出来，和游戏里 F3 的坐标一对就能定死。
            PosGuess g[16];
            const int ng = FindPosCandidates(obj, g, 16);
            LogLine("[sdk] 坐标候选 %d 个（对照游戏 F3 的坐标挑一个）：", ng);
            for (int i = 0; i < ng; ++i) {
                LogLine("[sdk]   #%d off=0x%llX stride=0x%llX "
                        "pos=(%.3f,%.3f,%.3f) prev=(%.3f,%.3f,%.3f) gap=%.3f",
                        i, (unsigned long long)g[i].off,
                        (unsigned long long)g[i].stride, g[i].cur.x, g[i].cur.y,
                        g[i].cur.z, g[i].prev.x, g[i].prev.y, g[i].prev.z,
                        g[i].gap);
            }
            // 硬编码的 kOffPos 也验一下：读出来合理就打个 OK
            Vec3 hard;
            if (mem::Read(obj + kOffPos, &hard, sizeof(hard)) && PlausiblePos(hard)) {
                LogLine("[sdk] 硬编码 kOffPos=0x%llX 读出来是 (%.3f,%.3f,%.3f)，"
                        "看起来仍然有效", (unsigned long long)kOffPos,
                        hard.x, hard.y, hard.z);
            } else {
                LogLine("[sdk] 硬编码 kOffPos=0x%llX 不可用，按上面的候选改",
                        (unsigned long long)kOffPos);
            }
            return 0;
        }
    }
    InterlockedExchange(&st.state, 3);
    LogLine("[sdk] 自动定位失败（耗时 %llu ms）：%s",
            (unsigned long long)(GetTickCount64() - t0),
            obj == 0 ? "堆里没找到带 LocalPlayer vtable 的对象"
                     : "找到对象但反查不到数据段里的槽位");
    return 0;
}

inline void StartLocateAsync() {
    LpLocate& st = LpState();
    // 失败过也要允许重试：注入时人可能还在标题界面，那会儿堆里根本没有
    // LocalPlayer 对象，扫一次必然是空的。等进了世界再给几次机会。
    const LONG s = InterlockedCompareExchange(&st.state, 1, 1);
    if (s == 1) return;                        // 正在扫
    if (s == 2) return;                        // 已经成功，不用再扫
    if (st.tries >= 4) return;                 // 扫够次数就放弃
    if (s == 3 && GetTickCount64() < st.next_try) return;
    if (InterlockedCompareExchange(&st.state, 1, s) != s) return;   // 抢到再走

    ++st.tries;
    st.next_try = GetTickCount64() + 8000;
    HANDLE t = CreateThread(nullptr, 0, LocateThread, nullptr, 0, nullptr);
    if (t != nullptr) {
        CloseHandle(t);
    } else {
        LogLine("[sdk] 定位线程创建失败 err=%lu", (unsigned long)GetLastError());
        InterlockedExchange(&st.state, 3);
    }
}

// 拿当前 LocalPlayer。读不到（还在标题界面 / 全局还没写）返回 0。
//
// ★ 这里必须兜底调一次 mem::Init()。以前只有 probe / explorer 会调它，
//   两个都关掉之后 mem::Base() 一直是 0，整个 SDK 静默失效 —— ESP 一进
//   渲染函数就 return，连一行日志都不会留。mem::Init() 是幂等的。
inline uintptr_t LocalPlayer() {
    uintptr_t base = mem::Base();
    if (base == 0) {
        (void)mem::Init();
        base = mem::Base();
    }
    if (base == 0) return 0;

    LpLocate& st = LpState();

    // ① 反查成功：直接用那个槽位。对象重生/换世界时槽位不变、指针会变，
    //    所以每次都要重新读出来验证，不能缓存对象地址本身。
    if (InterlockedCompareExchange(&st.state, 2, 2) == 2) {
        const uintptr_t p = mem::ReadPtr(base + st.global_rva);
        if (HasVtable(p, vt::kLocalPlayer) || HasVtable(p, vt::kPlayer)) {
            st.cached_lp = p;
            return p;
        }
        // 槽位还指着东西但已经不像对象了 —— 说明这一跳也过期了，
        // 退回硬编码路径重新判定。
    }

    // ② 硬编码偏移：必须验证首字是主模块里的 vtable 才认。
    //    以前不做验证，读到已释放的堆内存也照用，症状是"ESP 什么都不画
    //    但也不报错"，查了很久才发现。
    const uintptr_t raw = mem::ReadPtr(base + kLocalPlayerGlobalRva);
    if (HasVtable(raw, vt::kLocalPlayer) || HasVtable(raw, vt::kPlayer)) {
        st.cached_lp = raw;
        return raw;
    }

    // ③ 两者都不成立：拉起后台反查，本帧先返回 0（ESP 这帧不画，游戏不受影响）
    StartLocateAsync();
    return 0;
}

inline bool LocalPlayerPos(uintptr_t lp, Vec3* out) {
    if (lp == 0 || out == nullptr) return false;
    return mem::Read(lp + kOffPos, out, sizeof(Vec3));
}

// ── 成员偏移自动发现：坐标 ──────────────────────────────────────
//
// 找到 LocalPlayer 对象只解决了一半问题 —— 坐标在对象里哪个偏移，每个
// build 都会变（当前 kOffPos=0xCE0 是上一个 build 的值）。
// 但 mPos（眼睛）和 mPosPrev（上一 tick 位置）有三个很强的特征：
//
//   1) 是两个 Vec3，紧挨着放（stride 0xC 或 0x10，末尾会 padding）
//   2) 两者距离极小 —— 一 tick 内玩家移动不超过 2 格
//   3) y 落在 -100..400（世界高度），x/z 绝对值远小于 3e7
//
// 速度向量 (vx,vy,vz) 也满足 3)，但它旁边不会有第二个"几乎一样的自己"，
// 所以条件 2) 就是用来把它排除掉的。
//
// 扫出来的是候选列表，不是唯一解 —— 相机偏移、投掷物起点也会长得像。
// 所以结果会全打进日志，配合"日志里的坐标 vs 游戏 F3 里看到的坐标"
// 一次就能确定哪个是对的。
struct PosGuess {
    uintptr_t off;
    uintptr_t stride;
    Vec3 cur;
    Vec3 prev;
    float gap;      // 与 prev 的距离，越小越像
};

inline bool FiniteVec(const Vec3& v) {
    // NaN 会让后面所有比较都变成 false，必须先显式排掉
    return v.x == v.x && v.y == v.y && v.z == v.z &&
           v.x > -1e30f && v.x < 1e30f &&
           v.y > -1e30f && v.y < 1e30f &&
           v.z > -1e30f && v.z < 1e30f;
}

inline bool PlausiblePos(const Vec3& v) {
    if (!FiniteVec(v)) return false;
    if (v.x < -3.0e7f || v.x > 3.0e7f) return false;
    if (v.z < -3.0e7f || v.z > 3.0e7f) return false;
    if (v.y < -100.0f || v.y > 400.0f) return false;
    // 全 0 更像未初始化的空槽，不是真实坐标
    if (v.x == 0.0f && v.y == 0.0f && v.z == 0.0f) return false;
    return true;
}

// 试探步长：Vec3 是 12 字节，但编译器可能把它对齐到 16。
inline int FindPosCandidates(uintptr_t obj, PosGuess* out, int max_out,
                            uintptr_t span = 0x2000) {
    if (obj == 0 || out == nullptr || max_out <= 0) return 0;
    static const uintptr_t kStride[2] = {0x0C, 0x10};
    int n = 0;
    for (uintptr_t off = 0; off + 0x20 < span && n < max_out; off += 4) {
        Vec3 a;
        if (!mem::Read(obj + off, &a, sizeof(a))) break;
        if (!PlausiblePos(a)) continue;
        for (int k = 0; k < 2 && n < max_out; ++k) {
            Vec3 b;
            if (!mem::Read(obj + off + kStride[k], &b, sizeof(b))) continue;
            if (!PlausiblePos(b)) continue;
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            const float gap = sqrtf(dx * dx + dy * dy + dz * dz);
            // 完全相等说明是同一个值的两份拷贝（比如 UI 缓存），不是 pos/prev
            if (gap <= 0.0f || gap > 2.0f) continue;
            out[n].off = off;
            out[n].stride = kStride[k];
            out[n].cur = a;
            out[n].prev = b;
            out[n].gap = gap;
            ++n;
            break;      // 同一个 off 不用两种 stride 都记
        }
    }
    return n;
}

inline uintptr_t BlockSourceOf(uintptr_t lp) {
    if (lp == 0) return 0;
    return mem::ReadPtr(lp + kOffBlockSource);
}

inline uintptr_t DimensionOf(uintptr_t lp) {
    const uintptr_t bs = BlockSourceOf(lp);
    if (bs == 0) return 0;
    return mem::ReadPtr(bs + kOffBsDimension);
}

inline uintptr_t LevelOf(uintptr_t lp) {
    const uintptr_t bs = BlockSourceOf(lp);
    if (bs == 0) return 0;
    return mem::ReadPtr(bs + kOffBsLevel);
}

inline uintptr_t ActorManagerOf(uintptr_t lp) {
    const uintptr_t lv = LevelOf(lp);
    if (lv == 0) return 0;
    return mem::ReadPtr(lv + kOffActorManager);
}

// Level -> Minecraft -> ClientInstance
inline uintptr_t MinecraftOf(uintptr_t lp) {
    const uintptr_t lv = LevelOf(lp);
    if (lv == 0) return 0;
    return mem::ReadPtr(lv + kOffLevelMinecraft);
}

inline uintptr_t ClientInstanceOf(uintptr_t lp) {
    const uintptr_t mc = MinecraftOf(lp);
    if (mc == 0) return 0;
    uintptr_t ci = mem::ReadPtr(mc + kOffMcClientInstanceA);
    if (ci == 0) ci = mem::ReadPtr(mc + kOffMcClientInstanceB);
    return ci;
}

// 游戏设置对象。FOV / 视角 / 灵敏度都在里面 —— 目前还没定位到具体字段，
// 先用 esp.cpp 里配置的 fov，等 Options 的字段挖出来再切过来。
inline uintptr_t OptionsOf(uintptr_t lp) {
    const uintptr_t ci = ClientInstanceOf(lp);
    if (ci == 0) return 0;
    return mem::ReadPtr(ci + kOffCiOptions);
}

// 校验一个地址是不是指定 RVA 的 vtable（用于确认链路没走歪）
inline bool VtableIs(uintptr_t obj, uintptr_t rva) {
    if (obj == 0) return false;
    const uintptr_t vt = mem::ReadPtr(obj);
    return vt == mem::Base() + rva;
}

// 把整条链路的 vtable 逐个比对一遍，写进日志。
//
// 为什么要这个：客户端一换 build，所有 RVA 和成员偏移同时失效，而症状
// 只是"ESP 不画了"——从外面看和"没进世界 / 没玩家 / 投影算错"完全一样。
// 有一次这样的日志，就能一眼分清是"这份 DLL 不是这个客户端的"还是
// "逻辑写错了"。判据很硬：对象的第一个指针是不是本 build 的 vftable。
//
// 只在拿到 LocalPlayer 之后调（菜单界面全局为空，会全是 0，没有意义）。
inline void ChainReport(const char* why) {
    const uintptr_t lp = LocalPlayer();
    if (lp == 0) {
        LogLine("[sdk] 链路自检(%s)：LocalPlayer 为空，跳过", why ? why : "-");
        return;
    }
    struct Hop {
        const char* name;
        uintptr_t obj;
        uintptr_t rva;
    };
    const uintptr_t bs = BlockSourceOf(lp);
    const uintptr_t dm = DimensionOf(lp);
    const uintptr_t lv = LevelOf(lp);
    const uintptr_t mc = MinecraftOf(lp);
    const uintptr_t ci = ClientInstanceOf(lp);
    const Hop hops[] = {
        {"LocalPlayer", lp, vt::kLocalPlayer},
        {"BlockSource", bs, vt::kBlockSource},
        {"Dimension", dm, vt::kDimension},
        {"Level", lv, vt::kLevel},
        {"Minecraft", mc, vt::kMinecraft},
        {"ClientInstance", ci, vt::kClientInstance},
    };
    int bad = 0;
    for (int i = 0; i < (int)(sizeof(hops) / sizeof(hops[0])); ++i) {
        const uintptr_t got = hops[i].obj ? mem::ReadPtr(hops[i].obj) : 0;
        const uintptr_t want = mem::Base() + hops[i].rva;
        const bool ok = (got == want);
        if (!ok) ++bad;
        LogLine("[sdk] 链路自检(%s) %-14s obj=0x%llX vtable=0x%llX 期望=0x%llX %s",
                why ? why : "-", hops[i].name, (unsigned long long)hops[i].obj,
                (unsigned long long)got, (unsigned long long)want,
                ok ? "OK" : "MISMATCH");
    }
    LogLine("[sdk] 链路自检(%s) 结论：%d/%d 命中%s", why ? why : "-",
            (int)(sizeof(hops) / sizeof(hops[0])) - bad,
            (int)(sizeof(hops) / sizeof(hops[0])),
            bad == 0 ? "，这份 DLL 与本客户端匹配"
                     : "，有 MISMATCH —— 这份 DLL 不是这个 build 的");
}

// ══════════════════════════════════════════════════════════════════
// 高层对象链（2026-11-18 recon/dump_all.py 实测）
// ══════════════════════════════════════════════════════════════════
//   Level     +0x0748  Minecraft
//   Minecraft +0x0018  ClientInstance      ← 和 +0x0168 指向同一对象
//   Minecraft +0x0168  ClientInstance
//   Minecraft +0x0198  MinecraftGame
//
//   ClientInstance 内部（扫前 0x1400 字节）：
//     +0x00F8  MinecraftEventing
//     +0x0100  MinecraftGame
//     +0x0118  RayTracingOptions
//     +0x0128  Minecraft
//     +0x0138  LevelRenderer            ★ 世界矩阵 / 相机大概率在这层
//     +0x0168  MinecraftKeyboardManager
//     +0x0170  HitDetectSystem
//     +0x0CC0  Options                  ★ FOV / 视角 / 各设置项
//     +0x0CC8  _Ref_count_resource<$Options>
//
//   用途：ESP 现在是自算投影（玩家眼睛 + rotation 组件 + 配置里的 FOV）。
//   拿到 Options 之后 FOV 可以改成读游戏设置，投影就和真实画面完全一致；
//   LevelRenderer 里若能挖出 view-projection 矩阵，连相机都省了。
//
// ── ECS 组件清单（两次 dump 的并集，只列对模块有用的）────────────
//   UStateVectorComponent         pos / posPrev / velocity       步长 0x30
//   UActorRotationComponent       pitch / yaw                    步长 0x10
//   UAABBShapeComponent           碰撞箱（Reach / ESP 精确框）
//   UActorUniqueIDComponent       实体唯一 id（追踪、去重）
//   UOnGroundFlagComponent        是否着地（Fly / NoFall）
//   UGroundTravelFlagComponent    地面移动标志
//   UAirSpeedComponent            空中速度（Fly / Speed）
//   UFallDistanceComponent        下落距离（NoFall）
//   UHorizontalCollisionComponent 水平碰撞（Scaffold / 卡墙判断）
//   UActiveCameraComponent        当前激活相机实体（第三人称也对得上）
//   UPassengerComponent / UVehicleComponent   骑乘关系
//
//   ★ 没有 HealthComponent，也没有 InventoryComponent：血量、饥饿、背包
//     不在 ECS 里，得从 Actor / Player 的字段读。dump_all.py 第 8 节在
//     LocalPlayer 前 0x1000 字节里既没找到 float 20.0 也没找到 int 20，
//     下一步要往 0x1000 之后继续找。
//
// ══════════════════════════════════════════════════════════════════
// ECS 实体表（全部实测于 2026-11-18，33 个实体：1 本机 + 18 远程 + 14 掉落物）
// ══════════════════════════════════════════════════════════════════
// registry A（ActorOwnerComponent）在 ActorManager 上的偏移
constexpr uintptr_t kOffActorOwnerRegistry = 0x0270;
constexpr uintptr_t kOffActorOwnerStorage = 0x0558;

// registry B（rotation / statevector）在 ActorManager 上的偏移
constexpr uintptr_t kOffRotRegistry = 0x0048;
constexpr uintptr_t kOffRotStorage = 0x0528;
constexpr uintptr_t kOffStateVecStorage = 0x0548;

// basic_sparse_set / basic_storage 内部
constexpr uintptr_t kOffStoragePackedBegin = 0x0020;   // 4 字节 EntityId 数组
constexpr uintptr_t kOffStoragePackedEnd = 0x0028;     // 长度 =(end-begin)/4
constexpr uintptr_t kOffStorageSegVec = 0x0050;        // 元素（裸数组）指针

// 组件步长
constexpr size_t kRotStride = 0x10;   // { Vec2 mRot; Vec2 mRotPrev; }
constexpr size_t kStateVecStride = 0x30;  // { Vec3 pos; Vec3 prev; Vec3 vel; }

// AABB 组件。和 rotation / statevector 在**同一个 registry**，下标直接互通。
// 布局实测（步长 0x20）：
//     +0x00  Vec3 min        +0x0C  Vec3 max
//     +0x18  float width     +0x1C  float height
constexpr uintptr_t kOffAabbStorage = 0x0520;
constexpr size_t kAabbStride = 0x20;

// ══════════════════════════════════════════════════════════════════
// ★ mPos 到底是脚底还是眼睛（2026-11-18 用 AABB 反证）
// ══════════════════════════════════════════════════════════════════
// 同一时刻、同一个实体的两组实测值：
//
//     AABB   min = (-101.504, 19.000, -66.337)
//            max = (-100.904, 20.800, -65.737)      → 0.6 × 1.8
//     mPos   = (-101.204, 20.620, -66.037)
//
//   mPos 的 X/Z 正好是 AABB 的中心；Y 比 min.y 高 **1.620**，也就是标准
//   眼睛高度。旁证：第二个远程玩家的 AABB min.y 同样是整数 19.000
//   （站在方块顶面），mPos.y 同样是 20.620 —— 同一地面上的人眼睛高度必然
//   一致，脚底也必然一致，而 19.000 才是方块边界。
//
//   ⇒ **StateVectorComponent::mPos / Actor+0xCE0 是眼睛位置，不是脚底。**
//
//   以前 ESP 按"脚底"处理，又给相机加了 +1.62。这两处错正好互相抵消：
//   给相机和所有框点同时加同一个向量，投影结果完全不变。所以框能出来，
//   但框高永远写死 1.8 —— 对怪物、掉落物、潜行的玩家全错。
//   现在相机原点直接用 mPos，框直接用 AABB，两个都不再猜。
constexpr float kEyeHeight = 1.62f;   // 只在 AABB 读不到时兜底
constexpr float kPlayerWidth = 0.6f;
constexpr float kPlayerHeight = 1.8f;

struct ActorEntry {
    uintptr_t actor;
    uint32_t entity_id;
    Vec3 pos;      // ★ 眼睛位置（0xCE0 的 mPos），**不是**脚底
    Vec3 bb_min;   // AABB 最小角
    Vec3 bb_max;   // AABB 最大角
    float pitch;   // 度，低头为正
    float yaw;     // 度，0=+Z，90=-X
    bool has_bb;   // bb_min/bb_max 是否可信
};

// ── 类型名反查：vtable -> COL -> TypeDescriptor -> 名字 ──────────
// 只在同一个 vtable 上做一次，之后走 64 项小缓存。
// 返回的指针指向静态存储，调用方不要释放。
inline const char* ClassNameOf(uintptr_t obj) {
    struct Slot {
        uintptr_t vt;
        char name[80];
    };
    static Slot s_cache[64];
    static int s_used = 0;

    const uintptr_t vt = mem::ReadPtr(obj);
    if (vt == 0) return nullptr;
    for (int i = 0; i < s_used; ++i) {
        if (s_cache[i].vt == vt) return s_cache[i].name;
    }
    if (s_used >= 64) s_used = 0;  // 满了就从 0 覆盖，几十个类足够用

    const uintptr_t base = mem::Base();
    if (!mem::InImage(vt)) return nullptr;
    const uintptr_t col = mem::ReadPtr(vt - 8);
    if (col == 0 || !mem::InImage(col)) return nullptr;
    uint32_t td_rva = 0;
    if (!mem::Read(col + 12, &td_rva) || td_rva == 0) return nullptr;

    char raw[160] = {0};
    if (!mem::Read(base + td_rva + 16, raw, sizeof(raw) - 1)) return nullptr;

    // 去掉 MSVC 修饰前缀 ".?AV" 和结尾的 "@@"
    const char* p = raw;
    if (p[0] == '.' && p[1] == '?' && p[2] == 'A') p += 3;
    if (p[0] == 'V' || p[0] == 'U') ++p;

    Slot& s = s_cache[s_used++];
    s.vt = vt;
    int n = 0;
    for (; p[n] != '\0' && p[n] != '@' && n < (int)sizeof(s.name) - 1; ++n) {
        s.name[n] = p[n];
    }
    s.name[n] = '\0';
    return s.name;
}

// 类名里带 Player 的都算玩家（LocalPlayer / RemotePlayer / 可能还有
// ServerPlayer / Player 派生类）。ItemActor、Mob 之类一律不算。
inline bool IsPlayerClass(const char* name) {
    if (name == nullptr) return false;
    for (const char* p = name; *p != '\0'; ++p) {
        if (p[0] == 'P' && p[1] == 'l' && p[2] == 'a' && p[3] == 'y' &&
            p[4] == 'e' && p[5] == 'r') {
            return true;
        }
    }
    return false;
}

// 枚举当前维度里的全部实体。返回实际写入 out 的条数。
// lp 传 0 时内部自己取。max 建议 >= 512。
inline int EnumActors(ActorEntry* out, int max, uintptr_t lp = 0) {
    if (out == nullptr || max <= 0) return 0;
    if (lp == 0) lp = LocalPlayer();
    if (lp == 0) return 0;

    const uintptr_t am = ActorManagerOf(lp);
    if (am == 0) return 0;
    const uintptr_t reg_a = mem::ReadPtr(am + kOffActorOwnerRegistry);
    if (reg_a == 0) return 0;
    const uintptr_t sto = mem::ReadPtr(reg_a + kOffActorOwnerStorage);
    if (sto == 0) return 0;

    const uintptr_t pb = mem::ReadPtr(sto + kOffStoragePackedBegin);
    const uintptr_t pe = mem::ReadPtr(sto + kOffStoragePackedEnd);
    if (pb == 0 || pe <= pb) return 0;
    int n = (int)((pe - pb) / 4);
    // 先做合理性检查再截断：正常世界几十到几千个实体，读到上百万说明
    // 链路歪了（比如 storage 被释放后在原地被别的对象复用）。
    if (n <= 0 || n > 65536) return 0;
    if (n > max) n = max;

    const uintptr_t segvec = mem::ReadPtr(sto + kOffStorageSegVec);
    if (segvec == 0) return 0;
    const uintptr_t seg = mem::ReadPtr(segvec);
    if (seg == 0) return 0;

    // rotation 段：拿不到就只填位置，pitch/yaw 留 0
    uintptr_t rot_seg = 0;
    // AABB 段：和 rotation 同一个 registry，下标一一对应
    uintptr_t bb_seg = 0;
    const uintptr_t reg_b = mem::ReadPtr(am + kOffRotRegistry);
    if (reg_b != 0) {
        const uintptr_t rsto = mem::ReadPtr(reg_b + kOffRotStorage);
        if (rsto != 0) {
            const uintptr_t rv = mem::ReadPtr(rsto + kOffStorageSegVec);
            if (rv != 0) rot_seg = mem::ReadPtr(rv);
        }
        const uintptr_t bsto = mem::ReadPtr(reg_b + kOffAabbStorage);
        if (bsto != 0) {
            const uintptr_t bv = mem::ReadPtr(bsto + kOffStorageSegVec);
            if (bv != 0) bb_seg = mem::ReadPtr(bv);
        }
    }

    int got = 0;
    for (int i = 0; i < n; ++i) {
        const uintptr_t a = mem::ReadPtr(seg + (size_t)i * 8);
        if (a == 0) continue;
        ActorEntry e;
        e.actor = a;
        e.entity_id = 0;
        e.pitch = 0.0f;
        e.yaw = 0.0f;
        e.bb_min.x = 0.0f; e.bb_min.y = 0.0f; e.bb_min.z = 0.0f;
        e.bb_max.x = 0.0f; e.bb_max.y = 0.0f; e.bb_max.z = 0.0f;
        e.has_bb = false;
        (void)mem::Read(pb + (size_t)i * 4, &e.entity_id);
        if (!mem::Read(a + kOffPos, &e.pos)) continue;
        if (rot_seg != 0) {
            float rp[2] = {0.0f, 0.0f};
            if (mem::Read(rot_seg + (size_t)i * kRotStride, rp, sizeof(rp))) {
                e.pitch = rp[0];
                e.yaw = rp[1];
            }
        }
        if (bb_seg != 0) {
            // 一次读 8 个 float：min.xyz / max.xyz / width / height
            float bb[8] = {0};
            if (mem::Read(bb_seg + (size_t)i * kAabbStride, bb, sizeof(bb))) {
                e.bb_min.x = bb[0]; e.bb_min.y = bb[1]; e.bb_min.z = bb[2];
                e.bb_max.x = bb[3]; e.bb_max.y = bb[4]; e.bb_max.z = bb[5];
                // 合理性：三维跨度都得是正数且不离谱。实体被移除后 storage
                // 里的槽位会被复用，偶尔能读到全 0 或者垃圾值，这两种都拒掉，
                // 让调用方走写死的 0.6×1.8 兜底，而不是画出一个几十米的大框。
                const float dx = e.bb_max.x - e.bb_min.x;
                const float dy = e.bb_max.y - e.bb_min.y;
                const float dz = e.bb_max.z - e.bb_min.z;
                e.has_bb = (dx > 0.01f && dy > 0.01f && dz > 0.01f &&
                            dx < 64.0f && dy < 64.0f && dz < 64.0f);
            }
        }
        out[got++] = e;
    }
    return got;
}

// 只取玩家（含本机）。返回条数。
inline int EnumPlayers(ActorEntry* out, int max, uintptr_t lp = 0) {
    if (out == nullptr || max <= 0) return 0;
    ActorEntry tmp[512];
    int n = EnumActors(tmp, 512, lp);
    int got = 0;
    for (int i = 0; i < n && got < max; ++i) {
        if (!IsPlayerClass(ClassNameOf(tmp[i].actor))) continue;
        out[got++] = tmp[i];
    }
    return got;
}

}  // namespace sdk
}  // namespace xiaoyu
