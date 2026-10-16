# Pocket2D agent-native 现代化规划

日期：2026-10-16。状态：规划已获 owner 对主要方向的决定（见“Owner 决定”），尚未开始实现。
本文不改变已验证的 Phase 15 检查点，也不声称任何方向已经完成。

## Owner 约束（2026-10-16）

- Pocket2D 是 **2D 引擎**，不做 3D。
- 目标是 **agent-first、agent-native gameplay**。
- **Lua 运行时与 C++ 宿主不变**：游戏逻辑仍是 Lua（可经 TypeScriptToLua 编写），引擎仍是 C++ 宿主。
- 其余（物理、渲染、协议、数据模型、工具链、并行模型、编辑器）都可以重新规划。

## “agent-native”指什么

Pocket2D 要服务两类 agent，它们共享同一份真实运行时，但需求不同：

| | 作者 agent（LLM 编码 agent） | 玩家/测试 agent（LLM 玩家、RL policy、自动 playtest） |
| --- | --- | --- |
| 做什么 | 读 API、写 Lua/TS 组件、改场景与资产、跑测试、修错 | reset → act → observe 循环，完成任务或评估玩法 |
| 最需要 | 可发现的类型化 API；文本化且有 schema 的资产；一条 `check → run → observe → diagnose` 的快速闭环；可撤销的编辑 | 语义化动作与观测；确定性与可重放；比实时快得多的步进；并行与分支；视觉观测 |
| 失败成本 | 一次错误编辑会污染项目文件 | 一次非确定性会让训练/回放/回归测试失效 |

两类 agent 都不应该经过人类专用的 GUI 才能工作；人类编辑器反过来应是同一套命令的一个客户端。

## 现状：已有基础与关键缺口

已有基础（见 [status](status.md)）：

- 共享的固定 1/60 s 模拟步，headless agent host（JSONL v1：`describe/observe/step/reload_scripts/reset/quit`），带 Lua 指令/堆预算和 worker 进程监督。
- 种子化 reset、同平台同构建的种子回放、稳定的 UID + generation actor 句柄。
- 单一 `host-api.json` 生成 TypeScript 声明、LuaLS 元数据和 `describe.script_api`。
- 显式事务化脚本热重载、场景准备/预检、编辑器命令事务（ADR 003–006）。
- 72 个 CTest 用例，含真实 worker 驱动的玩法测试；一次外部 agent 编写评测（GLM，16/16 场景）。

对 agent-native 目标而言，结构性缺口如下（均有代码证据）：

| 缺口 | 证据 | 对 agent 的影响 |
| --- | --- | --- |
| **运行时是全局单例** | `Input.h`（11 个静态成员）、`Renderer.h`（10）、`AudioManager.h`（17）、`ParticleManager.h`（4）静态状态；`RayCast.cpp` 的 `g_physics_world`；`APIRegistrationDetail::g_lua_state`。 | 一个进程只能有一个世界：无法在进程内并行多个环境、无法 fork/分支、编辑器预览与运行时互相耦合。并行只能靠多进程。 |
| **物理是 Box2D 2.4** | `thirdparty/box2d/b2_world.h`，`Rigidbody.cpp` 中 `new b2World`。 | 无跨平台确定性保证，物理只能单线程，世界不能低成本快照。 |
| **只有原始键鼠动作** | `step` 只接受 `keys`/`mouse`。 | agent 必须知道“右方向键=向右走”，动作空间是整个键盘；与游戏语义脱钩，换键位即失效。 |
| **观测是全量属性转储** | `observe` 返回所有 actor 的受支持属性。 | 场景一大，LLM token 成本与 RL 观测维度都失控；没有按需查询，也没有游戏声明的观测。 |
| **headless 下没有画面** | `runtime_bootstrap.cpp`：`--frames/--capture require graphical mode`。 | 多模态 agent 在 agent 模式下“看不见”游戏；视觉回归只能走另一条图形路径。 |
| **没有快照/分支/回放文件** | agent-protocol.md：checkpoint restoration、concurrent environments 待做。 | 无法“从第 N 帧重试另一种操作”，搜索与调试都要从头重放。 |
| **没有“跑到某条件为止”** | `step` 只能跑固定帧数。 | LLM agent 只能反复轮询，浪费调用与 token。 |
| **作者侧没有 agent 接口** | agent-protocol.md：“There is no new agent wire command for editor mutations.” | 作者 agent 只能直接改 JSON 文件，绕过编辑器的校验、事务与撤销。 |
| **没有任务/场景定义** | 验收逻辑写在各 Python 测试中。 | 每个评测要手写驱动脚本；agent 无法以数据形式拿到“目标、限制、判定”。 |

## 目标架构

```
  传输层   stdio JSONL v2 │ MCP server │ Python/TS 客户端（Gym 风格）│ 编辑器 live link
                 └──────────────┬──────────────┘
  命令核心   一份带版本的命令 schema（生成文档、类型、校验），按能力分组：
             project / world / sim / observe / act / snapshot / scenario / edit / asset / diag
                                │
  会话层     Supervisor：进程隔离、预算、超时、崩溃恢复（现有设计保留）
                                │
  World 实例（可多个）  每个 World 拥有：Lua state、物理世界、输入状态、粒子、
             Tilemap/动画、音频事件、渲染命令列表、随机源、帧号
                                │
  呈现层     SDL 窗口 │ 离屏渲染（视觉观测/截图）│ 空呈现（纯模拟）
```

核心原则：**World 是一个值**——可以创建多个、独立步进、序列化、销毁；**命令只有一份**——人类编辑器、MCP、JSONL 和 Python 客户端都调用同一组命令；**呈现不推进模拟**（ADR 001 已确立）。

## 方向与切片

每个方向给出：做什么、为什么对 agent 重要、代价/风险、验收方式。顺序见下一节。

### M1. World 实例化：去掉全局单例

- **做什么**：引入 `World`（或 `Runtime`）对象，持有 Lua state、物理世界、输入、粒子、Tilemap、动画、音频事件队列和渲染命令列表。`Engine` 退化为“一个窗口 + 若干 World”的宿主。纹理/字体等**只读资源缓存**可跨 World 共享（只在拥有渲染的线程上传 GPU），可变状态全部归 World。
- **为什么**：这是快照、进程内并行环境、编辑器“编辑态世界 + 运行态世界”分离、并排确定性测试的前提。
- **线程规则**：一个 World 在任一时刻只属于一个线程；不同 World 可以在不同线程上并行步进。这与 AGENTS.md 的“Lua 和物理在其拥有线程上变更、绝不跨 worker 共享 LuaRef”一致，只是“拥有线程”从“主线程”变为“该 World 的线程”。
- **代价/风险**：触及大量文件（Input、Renderer、Audio、Particle、RayCast、APIRegistration、ComponentManager）。按子系统逐个迁移，每步保持 72 个测试通过。
- **验收**：同一进程中两个 World 加载同一项目、相同种子与输入，逐帧观测完全相同；交错步进与顺序步进结果相同；ASan/TSan 通过。

### M2. 确定性、时间控制与回放

- **物理迁移到 Box2D v3（当前稳定版 3.1.1）**：v3 使用 `b2WorldId` 句柄（天然多世界）；自身不创建线程，由宿主把任务系统接入 world 定义（可接现有 `TaskExecutor`）；接触/传感器以 Step 之后读取的事件数组给出，替代 v2 在 Step 内回调 Lua 的模式，从结构上消除 Phase 15 发现的“回调期间重入”问题。
  官方 FAQ 声明：相同输入 + 相同二进制可复现；线程数不影响结果；自 3.1 起跨平台确定。前提是禁用 fast-math、`-ffp-contract=off`、使用 Box2D 自带三角函数；同一进程内重跑可能需要重建 world（free-list 顺序会改变 body 顺序）。这些前提要写入构建配置与 ADR，并由 Pocket2D 自己的回放测试证明，不直接沿用上游声明。
  3.1.1 **没有快照/恢复 API**。尚未发布的 3.2（`main` 分支，2026-10 仍在活跃开发）新增了录制/回放（`b2World_StartRecording`、`b2ValidateReplay`）和整世界快照（`b2World_GetSnapshot`、`b2World_Restore`、`b2CreateWorldFromSnapshot`）。快照只在同一构建内有效（校验内部结构布局哈希），恢复后 user data 为空，不能部分快照，API 名称仍在变动。因此宿主层必须把 Lua 可见的身份（actor UID、generation、组件键）放在 Box2D 之外的表里，不依赖 `b2*` id 或 user data 跨快照存活。
- **回放文件**：记录 `{项目内容哈希, 引擎版本, 种子, 每帧动作}`，`pocket replay` 可重放并逐帧比对观测摘要；失败时报告第一处分歧帧和字段。
- **时间控制命令**：`run_until`（直到某事件/谓词成立或达到帧上限，返回触发原因）、`step` 支持批量动作序列、可选“仅在事件发生时返回”。
- **快照与分支**：分两级。
  1. 基础：检查点 = `{种子, 动作日志前缀}`，通过确定性重放恢复。实现简单、永远正确，但恢复耗时与帧数成正比。
  2. 加速：周期性关键帧 + 重放尾部；在 POSIX 上可选用 worker 进程 `fork()` 做 O(1) 分支（写时复制，worker 需在 fork 点处于无辅助线程状态）。Box2D 3.2 发布后，物理部分可用其快照替代重建，但 Lua 堆仍需重放或 fork。完整的“Lua 堆 + 物理世界”序列化不作为首选目标。
- **确定性 CI**：参照 GGRS SyncTest 的做法，在测试中每帧回退 N 帧重新模拟并比较状态校验和，第一处分歧即失败。
- **验收**：同平台 Debug/Release 回放逐帧一致；Box2D v3 迁移后所有 fixture 通过；分支后两条路径各自可重放；跨平台一致性在有第二平台 CI 前只记录为“未验证”。

### M3. 统一命令核心与 MCP

- **命令 schema**：把现有 JSONL 命令、tilemap 查询、reload、未来的 snapshot/scenario/edit 命令统一描述在一份带版本的 schema 中（与 `host-api.json` 同样的“单一来源 → 生成”模式），生成：协议文档、Python/TS 客户端类型、MCP tool 定义、请求校验代码。
- **MCP server**：作为一等传输层，使 Claude Code、Codex、OMP 等编码 agent 无需自写驱动脚本即可：读项目与 API、校验项目、运行场景测试、启动世界、步进、观测、截图、热重载、查看诊断。MCP server 只是命令核心的适配器，背后仍是受监督的 worker 进程。
  当前 MCP 规范（2026-07-28）的核心是无状态的：取消了 `initialize` 握手与会话 ID，跨调用状态用服务器签发的显式句柄作为普通参数传递。这正好对应 `world_id`、`snapshot_id`、`run_id`。长时间的 playtest 或批量 rollout 走官方 tasks 扩展（轮询 `tasks/get`）；工具的输入/输出用 JSON Schema 2020-12 描述，可由命令 schema 直接生成；Logging 已弃用，诊断走 stderr 或结构化结果。
- **权限分级**（参照 Defold 的 agent 文档）：只读（查询、文档）、验证（check/test/replay）、修改（编辑、写文件）、特权（任意 Lua eval、shell）。默认不开特权级，每一级可单独启用。
- **工具集参照**：Roblox Studio 内置 MCP 的工具覆盖面最完整：脚本读写/grep、场景树搜索与检查、按 Edit/Client/Server 上下文执行代码、开始/停止试玩、读控制台、截图、有序键鼠输入、角色导航，且每次调用必须指定实例 ID。Pocket2D 的工具集按这个覆盖面设计，但名称和粒度遵循“少量、命名空间化、高层、结果精简、错误可操作”的原则。
- **保留 JSONL**：stdio JSONL 是最小依赖的传输层，继续作为测试与 RL 客户端的基础；升级为 v2 时保留 v1 的语义测试。
- **验收**：同一组协议测试在 JSONL 与 MCP 两种传输上都通过；一个外部编码 agent 只通过 MCP 完成一项 authoring 任务（沿用 agent-authoring-evaluation 的冻结任务与独立 validator）。

### M4. 语义动作、观测与任务

- **动作映射（Action map）**：项目数据中声明 `move: vector2`、`attack: button`、`use_item: discrete(0..8)` 等动作及其键鼠/手柄绑定。人类输入经绑定变成动作，agent 直接发送动作；Lua 读取动作而不是按键（`Input.GetAction("move")`）。原始键鼠命令保留，用于测试输入层本身。
- **观测**：三层，按需组合。
  1. 通用世界查询：按组件类型、标签、区域、actor 子树过滤，选择字段，限制数量（取代全量转储）。
  2. 游戏声明的观测：项目中的 Lua 函数或组件声明返回结构化观测（例如“玩家生命、背包摘要、最近敌人”），带 schema，供 LLM 和 RL 使用。
  3. 视觉观测：见 M5。
- **事件流**：EventBus 的发布可按主题订阅进 agent 响应（例如 `crop_harvested`、`player_damaged`），附帧号；`run_until` 以事件为终止条件。
- **任务/场景（Scenario）作为数据**：一个场景文件声明起始状态（场景 + 存档 + 种子）、可用动作、观测、奖励/终止谓词、帧上限和判定器。同一文件可用于：RL 环境、LLM 玩家评测、回归测试、作者 agent 的验收标准。
- **多 agent**：按控制通道区分动作来源（玩家 1、玩家 2、AI 队友），观测可按通道裁剪。
- **对齐现有标准**：scenario 的 Python 客户端直接符合 Gymnasium 1.x 约定（`reset(seed, options) → (obs, info)`、`step → (obs, reward, terminated, truncated, info)`，每步返回独立的观测对象，向量环境支持 autoreset 模式），多 agent 对齐 PettingZoo Parallel API。是否另做 OpenEnv 适配（`reset/step/state`，WebSocket，容器化）留到它的 API 稳定后再定，官方目前仍标注为早期开发。
- **验收**：用 two_world_village 写 3 个 scenario（种植收获、守住一波、摧毁入侵源），分别被 Python Gym 风格客户端、LLM 玩家和回归测试使用；改键位不影响 agent 测试。

### M5. 视觉观测与 2D 渲染

- **headless 离屏渲染**：agent 模式可选创建离屏渲染目标（不开窗口），`observe` 可附带指定分辨率、指定相机的帧（PNG/原始 RGBA）。software 后端作为像素级确定的参考，用于 golden 图测试；硬件后端用于速度。
- **2D 渲染管线现代化**（在 profiling 证明需要时）分两档：
  1. 先用 SDL 3.4 的 GPU 渲染器（`SDL_CreateGPURenderer`，底层 Vulkan/D3D12/Metal）加 `SDL_CreateGPURenderState` 自定义片元着色器，覆盖 2D 光照、调色板、描边、后处理等需求，同时保留 SDL_Renderer 的 API 和 software 参考路径。这一步也补上路线图要求的 Vulkan/Direct3D 后端。
  2. 只有当绘制调用数或顶点格式成为实测瓶颈时，才下沉到 SDL_GPU 自建批处理管线（图集、按材质/层排序、实例化四边形）。着色器跨编译依赖 SDL_shadercross，目前仍是 preview 版，需要固定版本并离线编译。
- **调试叠加层**：碰撞体、Tilemap 格子、actor UID 标注可以渲染进 agent 截图，帮助视觉模型定位。
- **验收**：headless agent 截图与图形模式同帧截图在 software 后端逐像素一致；批处理渲染对现有 tilemap/动画/背包像素测试保持一致。

### M6. 作者工作流（编码 agent 的闭环）

- **`pocket` 命令行**（headless，无 GUI 依赖）：`pocket check`（schema 校验场景/模板/配置/tilemap/动画/物品目录 + LuaLS 类型检查 + TSTL 编译）、`pocket test`（运行项目内 scenario）、`pocket run --agent`、`pocket replay`、`pocket render`、`pocket new`。全部输出结构化诊断（文件、行列、组件、actor、错误码、修复提示）。
- **资产格式**：保持 JSON 文本，为每种资产发布 JSON Schema（参照 LDtk 从源码生成 schema 的做法），定义规范化格式（稳定键序、稳定数组序、默认值省略规则），引用用稳定字符串 ID 而非位置索引，使 agent 改动的 diff 最小且可审查。避免 Defold 那种“文本里再嵌转义文本”的格式。
- **类型检查**：继续从 `host-api.json` 生成 `---@meta` 定义；`pocket check` 调用 LuaLS `--check`，并评估 EmmyLua 的 `emmylua_check`（更快、可输出 SARIF、错误时非零退出）作为 agent 闭环中的检查器。Luau 和 Teal 都是不同方言，不符合“Lua 不变”的约束，不采用。
- **文档发现**：引擎和每个项目提供 `llms.txt`，命令 schema 提供可机读的发现入口（类似 Defold 编辑器的 `/openapi.json`）。
- **编辑器 live link**：编辑器暴露同一命令核心（`edit` 能力组走 ADR 006 的事务），agent 的编辑进入撤销栈，人类可见、可撤销；agent 可读取当前选择、视口截图。
- **项目内 agent 指南**：`pocket new` 生成的项目带有 `AGENTS.md`、类型声明和示例 scenario。
- **验收**：一个外部编码 agent 在不读引擎源码的前提下，只用 `pocket` CLI 或 MCP 完成“新增一个可玩机制 + 对应 scenario 测试”。

### M7. 并行

- **环境级并行（收益最大）**：M1 之后，一个进程内 N 个 World 各占一个线程，适用于 RL 批量采样与并行 playtest。进程级并行（多个 worker）继续可用，并提供更强隔离。
- **系统内并行**：Box2D v3 的任务回调接到现有 `TaskExecutor`；粒子已并行；Tilemap 分块、动画推进、渲染命令构建按测量逐个接入。Lua 在单个 World 内保持串行。
- **验收**：N 个 World 并行步进与串行步进结果逐帧一致；吞吐按 Release 基准记录分布，不作跨引擎声称。

### M8. 资产管线

- 导入器：Aseprite JSON（动画，可用其 headless CLI 导出 `--sheet` + `--format json-array --list-tags`）、LDtk/Tiled JSON（Tilemap）→ Pocket2D 原生格式，保留来源与许可信息。
- 所有资产热重载（图片、tilemap、动画、物品目录、场景）走与脚本 reload 相同的“准备 → 校验 → 提交”事务。
- 这是 Village_Rim 迁移的生产力前提，但不是 agent-native 的首要缺口，排在后面。

## 明确不做或推迟

- 3D 渲染、3D 物理。
- 替换 Lua 运行时或引入第二个 VM（owner 约束）。
- 全量 ECS 重写：ADR 001 的判断仍成立。World 实例化之后，原生组件内部可以按测量改为 SoA 池，但 Lua 组件模型保持 actor-component。
- 完整 Lua 堆/协程序列化作为快照机制（成本高、边界多，先用重放与 fork）。
- 跨平台确定性的声明：在有第二平台的实际回放验证之前不作声称。

## 建议顺序

每一步都保持编辑器与运行时可运行、现有测试全部通过。

1. **M1 World 实例化**（先输入、粒子、渲染队列这类叶子状态，再物理、Lua）。它解锁后面几乎所有方向。
2. **M2a Box2D v3 迁移**：在 World 实例化过程中一起完成物理世界的所有权迁移，避免改两遍。
3. **M3 命令核心 + MCP** 与 **M2b 回放文件/`run_until`**：让编码 agent 立即受益。
4. **M4 语义动作/观测/scenario**：用 two_world_village 落地。
5. **M5 headless 离屏渲染与视觉观测**。
6. **M6 `pocket` CLI 与编辑器 live link**（`pocket check` 可以更早，作为小切片插入 3 之后）。
7. **M7 环境级并行**，然后按 profiling 接入系统内并行与渲染管线现代化。
8. **M8 资产管线**，回到 Village_Rim 迁移主线。

## 物理库比较（2026-10-16）

Owner 决定先迁移到 Box2D 3.1，并要求同时看是否有更现代的库。结论：目前没有比 Box2D v3 更适合 C++ 宿主 + Lua 的 2D 物理库。

| 库 | API / 许可 | 最新版本 | 确定性 | 多世界 | 快照 | 线程 | 结论 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Box2D 3.1.1 / 3.2-dev | C / MIT | 3.1.1（2025-06-04）；3.2 未发布 | 跨平台、与线程数无关（需固定编译选项） | 是（上限 128） | 3.2-dev：整世界，仅同一构建 | 宿主任务回调或内置调度 | **采用**：先 3.1.1，跟踪 3.2 |
| Rapier2D | Rust；0.36 起有官方 C/C++ 绑定 / Apache-2.0 | 0.36.1（2026-10-10） | `enhanced-determinism` 下跨平台 | 是 | `r2SerializeWorld`，仅同一构建 | 只有 Rayon，无宿主任务钩子 | 备选：C API 刚发布数周，库仍在 1.0 前 |
| Avian 2D | Rust，绑定 Bevy | 0.7.0（2026-06-20） | 可选 | 依赖 Bevy | serde | Bevy | 不适用于 C++ 宿主 |
| Jolt | C++ / MIT | 5.6.0（2026-07-11） | 跨平台选项（约 8% 开销） | 是 | `StateRecorder` 增量保存 | 宿主 job system | 仅 3D；作为快照设计参照 |
| Maul2D | C / MIT | 0.0.1（2026-09） | 声称逐位一致 | 未核实 | 声称支持 | 声称宿主调度 | 太新，观察 |
| Chipmunk2D、LiquidFun | C/C++ | 冻结或已归档 | 无保证 | | 无 | | 不考虑 |

由此得出的约束：Lua 绑定只面向 Pocket2D 自己的包装类型（Rigidbody、Collider、RayCast 结果），不向脚本暴露 `b2*` id。这样 3.2 的快照、以后可能的 Rapier C API 都只影响宿主层。

## Owner 决定（2026-10-16）

1. **物理**：先迁移到 Box2D 3.1（3.1.1 标签），同时调研更现代的库（结论见上节）。
2. **进程内多 World**：做 World 实例化（M1）。
3. **Agent 接口**：统一命令 schema，同时提供 MCP 与 JSONL。
4. **起步切片**：World 实例化 + Box2D v3。

## 技术栈现代化清单（owner 2026-10-16 全部同意）

第一切片（World 实例化 + Box2D 3.1.1，ADR 008）之后的工作。版本信息查于 2026-10-16。

| 优先级 | 项目 | 内容 |
| --- | --- | --- |
| 高 | 独立 World 的 Play-in-Editor | Play 时复制编辑态 World 运行，Stop 丢弃；替代现有的编辑草稿保护逻辑。 |
| 高 | 结构化日志 + Tracy | 替换源码中约 45 处 `std::cout`/`std::cerr`，输出带帧号、World 与分类的 JSON 行诊断；接入 Tracy 0.14 与 Box2D 的 `BOX2D_PROFILE`。 |
| 高 | 物理能力补全 | 角色移动用 3.1.1 的 `b2World_CastMover`/`CollideMover`/`b2SolvePlanes`；Tilemap 墙体改 chain shape 消除接缝卡顿；Lua 暴露 joints、capsule/polygon/segment、碰撞分层、shape cast 与 overlap 查询。 |
| 中 | SDL 3.4 GPU 渲染器 + SDL_ttf text engine | 自定义着色器、Vulkan/D3D12 后端；ImGui 改用 `imgui_impl_sdlgpu3` 以支持多窗口；文字改用共享图集的 `TTF_Text`。 |
| 中 | Glaze + 资产 schema | RapidJSON 自 2016 年无发布；Glaze 由 C++ 类型定义 schema 并可导出 JSON Schema（注意其大版本变动频繁）。 |
| 中 | ImGui 1.93 + ImGui Test Engine | 1.93 重写了形状绘制，需像素回归；Test Engine 对开源项目免费，用于编辑器测试和 agent 驱动编辑器。ImGuizmo 已恢复维护。 |
| 低 | RmlUi 6.3、doctest/Catch2、清理 `thirdparty/SDL2*` | 游戏内 UI 用文本描述；测试框架替换；移除不再链接的 SDL2。 |
| 等待上游 | Box2D 3.2、C++26 反射、LuaBridge3 正式版 | 3.2 带快照/回放；反射目前只有 GCC 16 实验支持，Clang/Apple Clang 不支持；LuaBridge3 仍是 rc12。 |

已完成的相关项：Lua 以 `LUA_USE_POSIX` 构建（分支 `claude/fervent-napier-d7f463` 的提交 620f38c，Lua 循环每步 p50 约降 38%）。

## 性能路线：与前沿 2D 引擎同台比较（2026-10-16）

目标是在相同硬件、相同画面输出和相同工作量下，帧时间分布不差于前沿引擎。按 AGENTS.md，在完成这种对比之前不作领先声称。

### 外部基线的现状

几乎没有可直接引用的公开数字。Bevy、Godot、Unity 都没有官方的精灵吞吐数据；社区 bunnymark 混用硬件、平台、年代和更新逻辑。可用的线索：

- Defold（Lua 引擎）的 bunnymark：C++ 侧的 `go.animate` 快于单个 Lua 控制循环，后者又快于每个对象一个 `update()`；论坛报告约 32k 对 9k 对象 @60 fps（未注明硬件，未核实）。官方建议不写 `update()`。
- Godot 4.4 起在 Forward+/Mobile 加回 2D 批处理；文档说节点回调在数千到数万节点时开始成为瓶颈。
- Megabunny 只画静态精灵（无逻辑），分离渲染与绑定开销；SDL2/OpenGL 约 21.9M 精灵/秒（未注明硬件）。
- Box2D：大金字塔（5050 body，4 线程）M2 约 0.95 ms/步（2024-08）；仓库里的 v3.2 基准页报告比 3.1.1 快约 2 倍，但 3.2 尚未发布。

因此可信的比较只能自己做：在同一台机器上运行 Bevy、Godot、Defold、LÖVE 的同构 bunnymark 移植，以及 Box2D 自带的 benchmark 场景。

### 本地已测到的结构性瓶颈

- 65,536 个独立精灵的 Metal 场景（`tilemap_benchmark 256 metal`）中，主线程约 77% 的样本在 `SpriteRenderer::QueueDraw`。每个精灵每帧都要校验 spritesheet 元数据缓存（拷贝并比较 `std::filesystem::path`）、拼元数据键、按字符串查组件、复制图片名；之后还有对整个请求数组的 `stable_sort`。真正的绘制提交约占 9%。
- 逻辑路径：每个组件每帧一次 `OnUpdate`，每次经过多次受保护的字段读取和存活检查。`LUA_USE_POSIX` 已去掉 `pcall` 的信号掩码系统调用（Lua 循环 p50 约降 38%）；World 线程局部查找带来约 8–10% 开销。

### 改造方向（按依赖顺序）

1. **测量框架先行（P0）**：固定工作量 W1 静态精灵（只渲染）、W2 带逻辑的 bunnymark（Lua）、W3 Box2D 标准场景、W4 Tilemap 滚动、W5 混合村庄。统一输出分辨率，做像素校验，记录 p50/p99 帧时间与 CPU/GPU 分项；在本机对照 Bevy、Godot、Defold、LÖVE 的同构实现。结果只存在被忽略的 `build/`。
2. **精灵提交热路径（P1，低成本）**：spritesheet 元数据按图片解析一次并缓存为整数资源 id；绘制请求不再携带字符串；组件 handle 校验移出每帧循环；排序改为 64 位排序键（层/材质/纹理/深度）加基数排序；视口裁剪前移；帧内请求数组复用，不再每帧分配。
3. **原生组件存储（P2）**：Transform、SpriteRenderer、SpriteAnimation、Rigidbody 同步状态改为 World 内的连续 SoA 池，按密集数组迭代；Transform 层级只在脏标记时重算；Lua 侧仍是同一组件 API（userdata 指向池内槽位，带代际检查）。新原生系统显式接收 World 状态，不走线程局部查找。
4. **GPU 批处理（P3）**：基于 SDL_GPU 的 vertex-pulling 精灵批（参照 SDL_gpu_examples 的 `PullSpriteBatch`：每帧一次 storage buffer 上传、`cycle` 防止停顿、单次 draw）；图集或 array texture 消除纹理切换；SDL_Renderer 保留为 software 参考路径与像素测试基准。
5. **Lua 执行模型（P4）**：保留每组件 `OnUpdate` 的兼容性，同时提供“系统式”脚本（每种类型每帧调用一次，遍历该类型的实例数组）和批量读写 API；常用行为（补间、移动器、动画）下沉到 C++，参照 Defold 的结论。缓存方法引用，去掉每次回调的重复受保护字段读取。运行时保持 PUC Lua（owner 约束），不引入 LuaJIT。
6. **帧流水线与任务（P5）**：线程池增加异步提交/等待接口；渲染提取（extract）后，模拟 N+1 与渲染 N 重叠；裁剪、排序、动画推进、粒子按测量接入任务；Box2D 多线程求解接入同一线程池。Lua 与 Box2D 世界变更仍在 World 拥有线程上。
7. **物理（P6）**：Box2D 3.2 发布后升级（其基准报告约 2 倍提速，需本地复测）；x86 构建评估 `BOX2D_AVX2`。

每一步都以 P0 的工作量前后对比验收，并保持 74 个功能测试和像素测试通过。

### 进展与按数据作出的取舍（2026-10-16）

- P1 完成：精灵请求携带驻留的图片 id，纹理按 id 查找，spritesheet 版本与场景子目录每帧读一次。
- P2 的一部分：组件记录缓存原生对象指针与序号，逐帧系统按"拥有某种内建组件的 actor"缓存列表迭代。完整的 SoA 池未做：在列表化之后，65,536 精灵场景里剩余的主线程时间主要是每个精灵的请求构造与排序，池化不会消除这部分。
- P3 暂缓：在 `gpu` 渲染器上采样，SDL 提交（`SDL_RenderTextureRotated` 及其下层）已不到主线程样本的 2%，自建 SDL_GPU 顶点拉取批处理没有可测收益。SDL_Renderer 的 `gpu` 后端已加入像素测试。若以后的工作量证明提交成为瓶颈，再按原计划实现。
- 文字改用 SDL_ttf 的 renderer text engine（`TTF_Text` 共享图集），见 status.md 的测量。
- 自定义着色器（`SDL_CreateGPURenderState`）暂缓：目前没有需要它的玩法或画面需求，跨 Vulkan/D3D12/Metal 还需要离线着色器编译链。
- ImGui 改用 `imgui_impl_sdlgpu3` 与 ImGui 升级一并处理。
- P5：Box2D 多线程求解已接入共享线程池（醒着的物体不少于 2,048 时）。把 65,536 精灵的请求构建分块交给线程池只快约 6%：单线程时这一段每帧约 1.15 ms，数据在主核缓存中；分到 8 个线程后各核缓存是冷的，每个 actor 的成本升到 4–6 倍，因此未保留。渲染提取与模拟重叠的帧流水线也暂缓：当前帧 CPU（约 3.5–4 ms）低于显示节拍（约 8 ms），而渲染器、Lua 与 Box2D 都必须留在各自的拥有线程上，改造主循环的风险没有测量收益支撑。

### 暂缓项及原因（2026-10-16）

- Glaze 替换 RapidJSON：资产格式已有 JSON Schema 与校验器；改用 Glaze 需要重写全部加载器，目前没有 JSON 解析的性能或正确性问题，且 Glaze 大版本变动频繁。
- doctest/Catch2：现有约 95 个 CTest 用例都用直接的检查函数与独立可执行文件，换框架只有改写成本，没有新增覆盖。
- RmlUi 6.3：还没有需要声明式 UI 的游戏；泰拉瑞亚复刻若需要，再按需求接入。
- `imgui_impl_sdlgpu3` 与编辑器多窗口：SDL_Renderer 持有窗口交换链，不能与直接的 SDL_GPU 渲染在同一窗口混用；需要把编辑器呈现整体改到 SDL_GPU（游戏用离屏 GPU 渲染器），并做人工 GUI 验证。
- 编辑器 live link（编辑器暴露同一命令核心、agent 编辑进入撤销栈）：需要编辑器内的命令服务与 ADR 006 事务对接，单独切片。
- 等待上游：Box2D 3.2（快照/回放）、C++26 反射（Apple Clang 未支持）、LuaBridge3 正式版。

## 尚未决定

1. **2D 渲染管线**：已决定接入 SDL 3.4 GPU 渲染器（见上表），并按性能路线 P3 自建 SDL_GPU 精灵批处理。
2. **README**：当前仍写 C++17/SDL2，事实部分是否由 agent 按现状更新。
3. **Box2D 3.2**：发布前是否固定到 `main` 的某个提交以提前获得快照/回放。建议等正式版，期间用重放与 fork 实现检查点。

## 外部参照（2026-10-16 查阅）

- 性能基线与技术：[Defold bunnymark 讨论](https://forum.defold.com/t/know-your-engine-limitations/9742)、[Defold Lua 手册](https://defold.com/manuals/lua)、[Godot 4.4 dev 3 2D 批处理](https://godotengine.org/article/dev-snapshot-godot-4-4-dev-3/)、[Godot CPU 优化](https://docs.godotengine.org/en/4.3/tutorials/performance/cpu_optimization.html)、[Bevymark](https://bevy.org/examples/stress-tests/bevymark/)、[Megabunny](https://github.com/electronstudio/megabunny)、[SDL_gpu_examples](https://github.com/TheSpydog/SDL_gpu_examples)、[Box2D SIMD Matters](https://box2d.org/posts/2024/08/simd-matters/)、[Box2D v3.2 基准页](https://raw.githubusercontent.com/erincatto/box2d/main/benchmark/performance_v320.md)、[ecs_benchmark](https://github.com/abeimler/ecs_benchmark)
- 技术栈：[Dear ImGui v1.93.0](https://github.com/ocornut/imgui/releases/tag/v1.93.0)、[ImGui Test Engine 许可](https://github.com/ocornut/imgui_test_engine/blob/main/imgui_test_engine/LICENSE.txt)、[ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo)、[SDL_ttf 3](https://wiki.libsdl.org/SDL3_ttf/TTF_CreateRendererTextEngine)、[RmlUi](https://github.com/mikke89/RmlUi)、[Tracy](https://github.com/wolfpld/tracy/releases)、[Glaze](https://github.com/stephenberry/glaze)、[Clang C++ 状态](https://clang.llvm.org/cxx_status.html)、[LuaBridge3](https://github.com/kunitoki/LuaBridge3)
- 物理库：[Box2D releases](https://github.com/erincatto/box2d/releases)、[Box2D 3.1 release notes](https://box2d.org/documentation/md_release__notes__v310.html)、[Box2D blog](https://box2d.org/posts/)、[Rapier changelog](https://github.com/dimforge/rapier/blob/master/CHANGELOG.md)、[Rapier C bindings](https://github.com/dimforge/rapier/tree/master/bindings/c)、[Rapier determinism](https://rapier.rs/docs/user_guides/rust/determinism)、[Avian](https://github.com/avianphysics/avian)、[Jolt architecture](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)、[Maul2D](https://github.com/Rawframe-Project/maul2d)
- Box2D 确定性与线程模型：[FAQ](https://box2d.org/documentation/md_faq.html)、[Determinism](https://box2d.org/posts/2024/08/determinism/)、[Foundation](https://box2d.org/documentation/md_foundation.html)
- 回放与回滚测试参照：[GGRS](https://docs.rs/ggrs/latest/ggrs/)
- MCP：[2026-07-28 changelog](https://modelcontextprotocol.io/specification/2026-07-28/changelog)、[Writing tools for agents](https://www.anthropic.com/engineering/writing-tools-for-agents)
- 引擎 agent 接口：[Defold AI agents](https://defold.com/manuals/ai-agents)、[Roblox Studio MCP](https://create.roblox.com/docs/studio/mcp)、[Unity MCP](https://docs.unity3d.com/Packages/com.unity.ai.assistant@2.0/manual/unity-mcp-overview.html)、[Bevy Remote Protocol](https://docs.rs/bevy_remote)
- 环境接口：[Gymnasium release notes](https://gymnasium.farama.org/gymnasium_release_notes/)、[PettingZoo Parallel API](https://pettingzoo.farama.org/api/parallel/)、[OpenEnv](https://huggingface.co/docs/openenv/main/index.md)
- SDL 渲染：[SDL3 GPU](https://wiki.libsdl.org/SDL3/CategoryGPU)、[SDL 3.4.0 公告](https://discourse.libsdl.org/t/announcing-sdl-3-4-0/65438)、[SDL_shadercross](https://moonside.games/posts/introducing-sdl-shadercross/)
- 资产与类型检查：[LDtk JSON Schema](https://ldtk.io/docs/game-dev/json-overview/json-schema/)、[Aseprite CLI](https://www.aseprite.org/cli/)、[LuaLS](https://github.com/LuaLS/lua-language-server/releases)、[emmylua_check](https://www.mintlify.com/EmmyLuaLs/emmylua-analyzer-rust/cli/emmylua-check)
