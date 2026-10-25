# 第一切片实施计划：World 实例化 + Box2D 3.1.1

日期：2026-10-16。状态：A1–A7、B1、B2、B4 已完成；B3 推迟（见 ADR 008 与下文）。上位规划：[agent-native-modernization.md](agent-native-modernization.md) 的 M1 与 M2a。
每一步结束时 Debug 构建下 72 个 CTest 全部通过，阶段结束时再跑 Release 与 ASan/UBSan；每个通过的步骤单独提交。

## 设计：World 与“当前 World”

当前运行时状态分散在全局变量与类静态成员中（盘点见下表）。逐个改成显式参数需要改动几乎所有调用点。
本切片采用与 Dear ImGui `SetCurrentContext` 相同的模式：

- `World` 拥有一份完整的可变运行时状态：脚本运行时、EventBus、物理、输入、粒子、帧内渲染请求、随机种子与诊断计数。
- `World::Current()` 是**线程局部**指针；`WorldScope` 以 RAII 方式绑定与恢复。现有静态 API（`Input::`、`ComponentManager::`、`ParticleManager::` 等）保持签名，内部改为访问当前 World。
- `Engine` 拥有一个 `World`，在初始化、步进、渲染、事件处理与关闭时绑定它。编辑器通过 `Engine` 使用同一个 World。
- 不同线程可以同时步进不同 World；同一 World 任一时刻只属于一个线程。Lua state、物理世界和 `LuaRef` 都归属其 World，不跨 World 共享。
- 与 SDL 渲染器绑定的只读资源缓存（纹理、字体、文字纹理）以及音频设备仍是进程级资源，只在拥有窗口/设备的线程上使用；headless World 不触碰它们。

不做的事：本切片不改变 Lua API、场景格式和 agent 协议；不引入 ECS；不为“多 World”新增 agent 命令（属于 M3）。

## 全局状态盘点

| 状态 | 位置 | 去向 |
| --- | --- | --- |
| 脚本运行时 `g_runtime`（Lua state、组件记录、actor 索引、待处理队列） | `scripting/manager/Shared.cpp`、`Internal.h` | World |
| 随机种子、print 重定向、组件类型警告 | `scripting/manager/Runtime.cpp` | World |
| `g_lua_state`、`g_engine` | `scripting/luaapi/APIRegistration.cpp` | World |
| EventBus 订阅、待处理操作、分发深度、hooks | `scripting/EventBus.cpp` | World |
| Actor 模板缓存 `g_template_cache` | `scene/Actor.cpp` | World |
| 当前场景子目录 | `scene/Scene.h` | World |
| 物理世界、fixture 元数据、接触监听、力累计步号 | `physics/RayCast.cpp`、`Rigidbody.cpp` | World |
| `PhysicsMutationGuard` 深度 | `physics/PhysicsMutationGuard.h` | World |
| 键鼠状态与本帧边沿 | `input/Input.h` | World |
| 发射器与粒子数据 | `particles/ParticleManager.h` | World |
| 帧内绘制请求（scene/ui/pixel/text/particle batch）与渲染警告 | `rendering/Renderer.h/.cpp` | World |
| 纹理/字体/文字缓存 | `rendering/Renderer.h` | 进程级，渲染线程专用 |
| 音频设备、剪辑缓存 | `audio/AudioManager.h` | 进程级；播放开关与失败计数归 World |
| 编辑器面板状态 | `editor/panels/*.cpp` | 不动（编辑器 UI 状态） |

## 步骤

### 阶段 A：World 实例化（仍使用 Box2D 2.4）

1. **A1 World 骨架**：新增 `src/engine/core/World.h` 与实现；`Engine` 持有 World 并在所有入口绑定；测试辅助代码同样绑定。行为不变。
2. **A2 输入**：`Input` 的静态状态迁入 `World::input`。
3. **A3 粒子**：`ParticleManager` 的发射器数据迁入 World。
4. **A4 渲染请求与音频开关**：帧内绘制队列、渲染/音频诊断计数迁入 World；资源缓存保持进程级。
5. **A5 脚本运行时**：`g_runtime`、EventBus、API 注册状态、随机种子、模板缓存、场景目录迁入 World。
6. **A6 物理**：物理世界、fixture 元数据、接触监听、力累计步号、mutation guard 迁入 World。
7. **A7 多 World 测试**：同进程两个 World 加载同一项目与种子，交错步进与顺序步进的逐帧观测一致；两个线程各步进一个 World，结果与串行一致（ASan，另跑 TSan）。

### 阶段 B：Box2D 3.1.1

1. **B1 引擎自有 Vector2**：Lua `Vector2`、`Collision`、`RayCast::HitResult` 和 Rigidbody 接口改用引擎自己的 `Vector2`，语义与当前 `b2Vec2` 一致（`Normalize` 返回原长度、长度小于 `FLT_EPSILON` 时不修改）。仍在 Box2D 2.4 上验证。
2. **B2 换用 Box2D 3.1.1**：CMake 通过 FetchContent 固定 `v3.1.1` 标签，开启 `-ffp-contract=off`、禁止 fast-math。重写 Rigidbody、Tilemap 墙体、RayCast 与接触/传感器事件转换；删除 vendored 2.4。事件语义要保持：
   - 碰撞事件：两个碰撞体之间，由 contact begin/end 事件生成，点与法线来自 begin manifold，end 事件使用哨兵值。
   - 触发事件：只在 trigger 与 trigger 之间；3.1 的传感器可以检测其他传感器，每个方向各产生一个事件。为保持 2.4 的“至少一方为 dynamic”规则，宿主记录已报告的 begin 对，只对已报告的对发 end。
   - phantom shape 不产生事件，也不被射线命中。
   - Lua 可见身份（actor UID、generation、组件键）保存在宿主表中，不依赖 Box2D user data 或 id 跨快照存活。
3. **B3 接入 TaskExecutor（推迟）**：Box2D 求解器为每个 worker 派发一个任务，这些任务在共享的阶段计数上自旋，必须同时运行；`TaskExecutor::ParallelFor` 是同步执行，接入会死锁。需要先给线程池加异步提交/等待接口，或等 Box2D 3.2 自带调度器。当前每个 World 只有几十个 body，环境级并行已由多 World 多线程提供。
4. **B4 收尾**：更新 status、build-and-test、第三方许可说明；跑 Release 与 ASan/UBSan 全量矩阵，并做图形截图检查。

## 风险

- Box2D 3 的软接触求解与子步进会改变接触后的精确位置。依赖精确坐标的测试需要判断：是断言了玩法语义（应保持）还是断言了 2.4 的数值（按 3.1 的结果更新，并在提交说明中列出）。
- 3.1 的接触结束事件在销毁 body 后的下一次步进中给出，而 2.4 在 `DestroyBody` 时同步回调。对“销毁时收到 exit”有依赖的测试与脚本需要单独核对。
- 当前 Rigidbody 的数值校验按 2.4 的积分公式推导中间量；3.1 的积分顺序不同，需要按 3.1 源码重新核对，不能只做机械替换。
