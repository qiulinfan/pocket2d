# 脚本运行时与未来 TypeScript 接入评估

研究与更新日期：2026-10-15。官方 Lua 5.4.9 维护升级已完成，LuaBridge 2.7 的对照完成后，第十三阶段已接入固定的 LuaBridge3 rc12（含一处同状态指针快路径），未引入另一种 VM；升级阶段的 Debug/Release 与适用 ASan/UBSan 回归已经通过。随后实现的 agent 指令/heap 预算和进程监督器已通过 Debug 回归，最终集成矩阵及新增平台验证以 [status](status.md) 为准。其他 VM/语言路线仍是评估。与 [roadmap](roadmap.md)、[agent protocol](agent-protocol.md)、[预算边界](script-budgets.md) 一起阅读。

现有 Lua 宿主已补齐实例默认值隔离，以及 agent 模式下的限额、外部超时和失败后 reset。目前已有 curated typed host API、显式事务 reload、场景准备及统一的 checked 调用／诊断边界；LuaBridge3 已完成集成验证。单换 sol2、Luau 或 JavaScript 引擎不会自动解决这些宿主契约。未来 TypeScript 可作为第二种游戏脚本语言，但不需要为了让外部 agent 使用 TypeScript 工具就提前嵌入 JS VM：现有 JSONL 协议已经允许任意语言的客户端控制真实游戏。

TypeScript 游戏脚本也不必引入第二 VM：TypeScriptToLua 已提供保留当前 Lua 运行时的可选 authoring 路径，下文与 QuickJS/V8 分开评估。共同投资是 typed host API 描述、声明生成与结构化诊断，当前不创建空的通用 VM 抽象层。

## 当前实现：已确认的事实与缺口

| 事实或缺口 | 本地证据 | 对下一步的影响 |
| --- | --- | --- |
| 研究起点为 Lua 5.4.6、LuaBridge 2.7；目前 Lua 5.4.9／LuaBridge3 rc12 已完成集成构建与全量回归。Lua 源码作为 C 静态库编译。 | `thirdparty/Lua/lua.h`、`thirdparty/Lua/UPSTREAM.md`、`thirdparty/LuaBridge/LuaBridge.h`、`thirdparty/CMakeLists.txt`。 | VM 升级与绑定库更换分开验证。不能把当前实现称为裸 Lua C API，也不能把新绑定库当成新 VM。 |
| 一个全局 Lua state；组件以文件同名全局 table 注册，实例通过 `__index` 继承类型 table。 | `src/engine/scripting/manager/Runtime.cpp::LoadComponentTypes/EstablishInheritance`。 | 模块命名、实例默认值与 reload 需要明确契约。当前不是每场景隔离环境或多实例运行时。 |
| **研究起点实测，现已修复：默认数组曾在实例间共享。** 第一个 `Probe` 实例修改默认数组会影响第二个实例。 | `Binding.cpp` 现在按实例复制默认 table 图、明确的向量/handle 值，覆盖 table-valued `__index` 原型继承。`component_isolation_test` 验证双实例、深层图、别名/环、metatable、覆盖、模板、动态添加及 reset。 | 函数/upvalue 和方法直接访问的全局仍明确共享；未知 owned userdata/coroutine 默认值被拒绝，应在 `OnStart` 构造。JSON 属性/观察覆盖仍是受限子集。 |
| **研究起点故障，agent 路径现已受控。** 当时 `frames=1` 的死循环需测试 watchdog 终止；现在普通 Lua 循环触发指令错误，移除 hook 或 native sleep 由 supervisor deadline 终止 worker，显式 reset 后恢复。 | `ScriptBudget.cpp` 使用自定义 `lua_newstate` allocator 与 interval=1 count hook；`AgentSupervisor.cpp` 用 SDL3 子进程保护，`lua_budget_test` / `agent_budget_test` 覆盖协程、吞错、OOM、阻塞和恢复。 | 限制是 agent 请求执行与 Lua allocator 存活字节，不是整进程 RSS 或 OS sandbox。图形可信模式默认仍不限额。精确边界见 [script-budgets](script-budgets.md)。 |
| 统一的 LuaBoundary 已用于受保护字段访问和生命周期／事件调用。 | `LuaBoundary.h/.cpp` 检查结果、忽略回调返回值、保留调用前栈并提供有界 UTF-8 traceback；`Shared.cpp` 保留错误计数。`lua_boundary` 和 `event_bus` 验证恢复与嵌套调用。 | 生产保持库 result mode，避免全局异常绕过 Lua pcall。预算／进程恢复和显式 reload 合同保留；原始 nil-self 崩溃、更多结构化上下文和 TS source map 仍待完善。 |
| Lua 细节跨越宿主边界。 | `src/engine`、`include/engine` 中多个文件直接使用 `luabridge::LuaRef`；`ActorHandle` 的组件查询仍返回 LuaRef，EventBus 持有 LuaRef。 | TypeScript 适配不能直接复用这些返回类型。应先抽出少量纯 C++ 查询/写入契约，再让各语言绑定负责包装结果。 |
| 已增加代际 ActorHandle，不能再把所有脚本 actor 访问概括为原始指针。 | `include/engine/scene/ActorHandle.h`、`ActorAPI.cpp`；UID + generation 在调用时解析。 | 第二种语言应复用同一寿命规则，继续测试销毁、场景替换与 UID 复用。不可让 JS 再持有直接 `Actor*`。 |
| 显式 reload 已在独立有界候选 VM 准备代码，在 idle 边界保留实例/native 状态并更新 prototype、事件与生命周期缓存。 | `script-reload.md`、`adr-003-script-reload.md`、真实 agent/editor 和编译 TS 重载回归。 | 这是有边界的活实例重载；任意 heap/schema/continuation 迁移、旧 module-local 状态迁移仍不支持。 |
| Inspector/agent 的属性模型是标量与同质数组，不是任意 Lua heap。 | `Actor::ComponentPropertyValue`、`SceneFormat.cpp`、`Queries.cpp`。 | JSON observe/reset 不等于保存闭包、协程、任意嵌套 table 或完整物理状态。跨语言数据先遵循现有可序列化子集。 |

上述行为测试区分研究起点的故障复现和本轮已经完成的修复，只证明相应本地构建的结果。没有运行候选 VM 的 Pocket2D benchmark，也没有证据说明 Lua 执行本身已成为整帧主瓶颈。此前层级查询与粒子优化的收益属于 C++ 算法和数据并行，不能归因于脚本 VM。

## Lua 路线：分别选择 VM、绑定和开发工具

截至查阅日，Lua 官方版本页列出 5.5.1 为当前版本，5.4.9 为 5.4 系列最后版本。官方同时说明：同一 `x.y` 下补丁版本保持 ABI 兼容，跨版本则不保证。这里采用发布公告口径，不把下载文件时间戳当作发布日期。[Lua 版本历史](https://www.lua.org/versions.html)

| 候选 | 能解决什么 | 对 Pocket2D 的具体代价与限制 | 建议验证顺序 |
| --- | --- | --- | --- |
| 当前基线：Lua 5.4.9 | 缩小版本维护差距，保持现有语言模型；便于单独验证宿主整改。 | 5.4 已结束后续发布；升级补丁不能取代预算、handles、模块和诊断设计。 | 可作低变量的对照组，不把它当长期维护终点。 |
| Lua 5.5.1 | 当前官方维护线，适合优先评估长期 PUC-Lua 路线。 | 有语言/C API 差异，例如 `global` 成为保留字、for 控制变量只读、`lua_newstate` 增加 seed 参数。现有 LuaBridge 2.7 的兼容性必须实际编译验证；不能假定升级 archive 即完成。 | 与 5.4 对照，执行当前全部脚本/绑定/回放测试后再决定。 |
| LuaJIT 2.1 | 在满足条件的脚本热点上评估 JIT；保留 Lua 风格开发。 | 官方 C API 基线为 Lua 5.1，不是 Lua 5.4 的无修改替换。当前整数/数组分类、`lua_isinteger` 等调用及平台 JIT 条件需要检查；JIT 热身与尾延迟必须计入。 | 仅在剖析确认脚本 CPU 占比值得时做并列实验。 |
| Luau | 渐进类型、静态分析与面向嵌入的隔离机制，契合“先发现生成脚本错误”的目标。 | 基于 Lua 5.1 的独立语言路线；没有 PUC-Lua 的 64 位整数类型，也移除了直接文件加载等能力。当前 `luaL_dofile`、IO、属性数值分类与绑定不能原样搬入。 | 一个受限组件实验即可；先量化类型反馈收益，再考虑迁移全部脚本。 |
| LuaBridge3 | 更接近当前注册风格；官方测试矩阵覆盖 Lua 5.5、LuaJIT 和 Luau。 | 这是绑定升级，不是 VM。`ActorAPI.cpp` 直接使用 LuaBridge 内部 class-registry 接口实现相等比较，迁移必须覆盖它；公开兼容矩阵不等于项目已通过。 | 与 Lua 5.5 配对验证的优先绑定候选。 |
| sol2/sol3 | 较丰富的 C++ userdata、容器与 protected-function 接口，可比较可维护性和错误表达。 | 仍然是 Lua 绑定；现有 LuaRef、查询、EventBus 与注册点需要改写。更丰富的模板 API 不自动产生更快帧率、运行时配额或 TypeScript 支持。 | 保留相同 VM、安全设置和工作负载，比较绑定调用开销、构建成本及维护成本。 |

表中兼容性依据：[Lua 5.5 不兼容项](https://www.lua.org/manual/5.5/manual.html#8)、[LuaJIT 安装/API 说明](https://luajit.org/install.html)、[LuaJIT 维护状态](https://luajit.org/status.html)、[Luau 兼容性](https://luau.org/compatibility/)、[Luau 类型系统](https://luau.org/types/)、[LuaBridge3 官方仓库](https://github.com/kunitoki/LuaBridge3)、[sol2 官方仓库](https://github.com/ThePhD/sol2)。选择顺序是基于当前代码适配成本的工程判断，不是这些项目的性能排名。

在保留 Lua 的情况下，也可以先生成 LuaCATS 类型注释与 API 定义，改善补全和静态诊断；这不要求替换 VM。类型提示不能代替运行时参数检查。sol2 的安全功能同样需要按官方配置启用、验证，而不是只把函数类型改为 `protected_function`。[LuaLS 注释文档](https://luals.github.io/wiki/annotations/)、[sol2 安全配置](https://sol2.readthedocs.io/en/latest/safety.html)

### 已完成的 Lua 5.4.9 维护升级

**源码替换、集成构建与升级回归已经完成。** 替换前对官方 5.4.6 archive 与 vendored C/C++ 源文件的只读比较未发现项目补丁，因此无需移植私有 Lua 修改。与官方 5.4.9 比较，`lua.h` 的差异仅涉及版本/版权信息，`lauxlib.h`、`lualib.h`、`lua.hpp` 未变；`luaconf.h` 增加模块名称处理标记。当前保留 LuaBridge 2.7，并新增 `tests/lua_vm_test.cpp` 覆盖 hook/yield、GC/weak userdata、加载/运行错误与现有绑定。原版源码留在忽略的 `build/lua-upgrade-baseline/`；不能把头文件相近当作运行正确证明。

升级来源应直接固定为 [lua.org 的 5.4.9 官方 archive](https://www.lua.org/ftp/lua-5.4.9.tar.gz)，对照 [官方下载目录](https://www.lua.org/ftp/)；完整保留源码中的许可，校验输出仅保存在忽略目录。官方已知缺陷和差异包含 hook/yield 处理、错误定位、弱 table/GC、finalizer 栈空间、非法 UTF-8 等修复。其中部分错误处理与 GC 路径和后续预算/失败恢复工作相关；REPL 专属修复则不直接作用于目前排除 `lua.c` 的嵌入构建。[Lua 官方 bug 列表](https://www.lua.org/bugs.html)、[5.4.8→5.4.9 官方差异](https://www.lua.org/work/diffs-lua-5.4.8-lua-5.4.9.html)

升级按独立依赖切片完成，保留既有 binding，经过 editor/runtime 构建、默认项目/村庄 fixture、同 seed 回放、嵌套异常、句柄、GC userdata 和 EventBus 回归。预算 allocator 是随后单独实现的宿主改动，不属于上游 Lua 补丁。后续再更新 VM 时仍应隔离依赖变更，不用一次升级同时承担绑定/脚本格式迁移；验收是维护正确性，没有“补丁升级更快”的预设。

风险仍包括修复本身改变此前错误行为、GC 时机变化和现有绑定对边界行为的依赖。5.4.9 是最终维护版，不承诺以后发现的问题都得到回补；满足 5.5 的接口/脚本兼容性与回归门槛后，再决定是否转到 5.5 维护线。这一补丁切片不要求预先替换 LuaBridge3。

## TypeScript 作为第二种游戏脚本语言

标准 `tsc` 路线擦除 TypeScript 类型标注，输出 JavaScript；这条路线才需要 JS 引擎。TypeScriptToLua 则利用类型信息输出 Lua。两者都不意味着所有运行时输入自动得到类型验证；编译、bundling、source map 与宿主 bindings 也仍是不同工作。[TypeScript 官方基础文档](https://www.typescriptlang.org/docs/handbook/2/basic-types.html)、[TypeScriptToLua 官方介绍](https://typescripttolua.github.io/)

| 路线 | 值得验证的理由 | 必须补齐的宿主工作 |
| --- | --- | --- |
| TypeScript → TypeScriptToLua → 现有 Lua | 保留同一 VM、Lua bindings、生命周期与事件系统，用 TS 静态检查和声明改善 authoring；适合首先验证“agent 用 TS 写现有 Lua 组件”的收益。 | 对当前 `self`/全局组件注册编写声明与生成入口，固定 Lua target 和 lualib 打包，关联 TS 源位置。它不是完整 JavaScript 兼容层，普通 npm JS 依赖不能直接搬入。 |
| TypeScript → JavaScript → QuickJS | 可先做体积和冷启动受控的小实验。官方提供内存上限、栈限制和中断回调。 | JSValue 的显式引用寿命、异常提取、模块解析、Promise job 调度、TS source-map 诊断；一个 JSRuntime 内不支持并发访问。原版 Windows 构建路线也需专门验证。 |
| TypeScript → JavaScript → V8 | 若实际需要 Inspector 调试协议、成熟 JS 执行工具链或经测量的高脚本吞吐，可评估。 | isolate/context/handle scope、对象寿命、初始化和平台构建、内存与终止策略；引擎之外仍需自己的模块和宿主 API。不能因为 V8 也用于浏览器/Node 就声称已拥有那些平台。 |
| TypeScript 只作为外部 agent/tool 客户端 | 已有 JSONL step/reset/observe，可直接编写 TS 客户端并使用类型化协议。 | 维护协议声明与验证；不用改变游戏脚本。适合先验证 agent 编程反馈是否更好。 |

依据：[QuickJS 官方手册](https://bellard.org/quickjs/quickjs.html)、[V8 嵌入指南](https://v8.dev/docs/embed)、[V8 Inspector](https://v8.dev/docs/inspector)。QuickJS-NG 是另一个需要单独固定版本与测试的候选，不应把它的行为或平台支持与原版 QuickJS 混称；其嵌入接口有自己的维护文档。[QuickJS-NG C API](https://quickjs-ng.github.io/quickjs/developer-guide/intro/)

TypeScriptToLua 的选择有明确语义成本：Lua 的 truthiness 会使 `0`/空字符串与 JS 不同；`null` 和 `undefined` 都落到 `nil`；稀疏数组长度、排序稳定性和 Promise 回调时点也不能照搬 ECMAScript 假设。游戏逻辑需要针对这些差异的约束和测试，不能只通过 `tsc` 就视为正确。[TSTL caveats](https://typescripttolua.github.io/docs/caveats/)

官方配置提供 `luaTarget: "5.4"`、lualib 引入/打包与 source-map traceback 选项。第五阶段固定的 TypeScriptToLua 1.37.1 在安装包 `dist/CompilerOptions.d.ts` 中也声明了 `Lua55`；早期网页列表不再足以描述该版本。当前 Pocket2D 实验明确使用 Lua 5.4，尚未验证 Lua 5.5 runtime/生成代码组合。声明须区分静态宿主函数与带 `self` 的方法/回调，检查 `this: void` 等互操作规则，避免参数整体错位。[TSTL 配置](https://typescripttolua.github.io/docs/configuration/)、[self 参数规则](https://typescripttolua.github.io/docs/the-self-parameter/)

TSTL 可使用带声明的 Lua 包；普通 npm 包若只发布 JavaScript，不能靠补一份 `.d.ts` 变成可执行 Lua。Node/DOM 依赖也不存在。需要源码/生成 Lua 可用且语义兼容的依赖，不能向 agent 宣传可直接使用整个 npm 生态。[TSTL 外部代码说明](https://typescripttolua.github.io/docs/external-code/)

建议先用 TS→Lua 重写一个 actor 查找、Transform 读写、固定 tick 和事件组件，与手写 Lua 对照。若真实需求要求 ECMAScript 行为或依赖库，再以相同 host 契约做 QuickJS/V8 实验。只有获得实际 authoring/诊断/性能证据后，才增加新的生产路径。

## 可以被两种语言复用的宿主契约

第五阶段已实现 curated typed host 描述、LuaLS/TypeScript 声明生成与真实绑定契约测试，第六阶段加入库存API。`describe.script_api` 与生成文件来自同一JSON。完整API覆盖、统一运行时traceback/TS source-map仍是缺口；它不是通用反射/RPC层。

| 契约 | 应明确的内容 | 当前最小落点 |
| --- | --- | --- |
| 类型化 API 描述 | 名称、参数/返回值、可空性、数值范围、可变字段、拥有线程、失败类型、是否改变世界。 | 已有单一host-api.json生成LuaLS、.d.ts及agent能力说明，并由真实绑定测试校验；继续按实际玩法扩展覆盖。 |
| 值与寿命 | 由 host 拥有的 UID + generation handles；查询结果用 owned values 或稳定 handles；语言对象仅留在适配层。 | ActorHandle 已具备寿命基础，但返回 LuaRef 的查询与 EventBus 仍需逐步解耦。不要把 `LuaRef` 简单换成 `std::any`。 |
| 数值与身份 | 明确整数范围、浮点有限性、空值与 unset 的差别。JS/Luau `number` 不能精确表达全部 uint64 UID。 | 当前 JSONL 的 uid/parent_uid 已使用无损十进制字符串。新语言继续采用 opaque handles，显式处理字符串/BigInt，而非直接转为 JS number；Lua/JS 适配仍需验证完整整数边界。 |
| 错误 | 稳定错误码、语言、源路径/行列、actor、component key、生命周期、tick、traceback、是否可重试/需 reset。 | 保留现有字符串消息，先增加内部统一记录；Lua/JS 各自负责把原生异常转成该记录。 |
| 调度与确定性 | 固定 tick、输入边沿、事件顺序、随机源、clock、异步完成的应用时点。 | Lua/JS、Box2D 与 SDL 各守拥有线程；后台任务只处理 owned CPU 数据，按确定顺序在 tick 边界应用结果。 |

对 TypeScript 的 Promise/async 不能采用“线程完成就立即改 actor”的规则；需要声明 job 在何时被 pump、每 tick 的预算、失败如何回到当前会话。跨 VM 事件应传符合类型约束的值和 handles，不直接传 Lua table、JS object、闭包或裸指针。先限制一份场景选择一种脚本适配器，是否允许 Lua/JS 组件混用留到真实需求出现后决定。

## 隔离、预算与 reload 的最小方案

| 阶段 | 可交付切片 | 必须通过的验证 |
| --- | --- | --- |
| S1：默认状态/actor 句柄已完成，其余接口隔离待验证 | 每实例可变默认状态；Actor 与组件对象的代际/有效性检查；明确 `actor`、`key` 等身份字段由 host 维护，以及可写 gameplay 字段的范围。 | 两实例数组不污染；销毁/同 UID 重建/组件移除后旧引用不能改新对象或重新创建已脱离管理的资源；错类型与非法 float 给可定位错误。 |
| S2：agent 指令预算已实现 | 顶层/bootstrap 同一作用域；命令内生命周期、事件与协程共享逐指令计数，内层作用域不补配额。移除 hook、吞错或 native 阻塞另由父进程 deadline 处理。 | 单 callback 死循环、递归 publish、脚本内部 `pcall` 尝试吞掉限额错误时，外部仍可结束请求；reset 可恢复；普通场景不误杀。 |
| S3：Lua heap 预算已实现 | 自定义 allocator 计量存活请求字节；越限原因粘性保留，父进程销毁失败 worker，reset 创建新进程。native host 资源和进程 RSS 不在此 cap 内。 | 字符串/table 暴涨或 OOM 不能拖死 supervisor；失败 worker 被回收，显式 reset 可恢复，正常 VM 关闭的分配计数归零。允许 worker panic/退出由父进程隔离，不承诺 OOM 后原 VM 可继续使用。 |
| S4：curated typed surface已实现，统一运行时诊断仍待补 | 小型 API 描述生成 Lua 定义、TSTL/未来 JS `.d.ts`、能力说明；统一错误上下文。 | 同一坏调用在静态检查与运行时验证中被正确定位；声明与真实绑定有契约测试，不允许两份手写接口长期漂移。 |
| S5：显式事务重载切片已实现 | 独立有界候选Lua VM、受限顶层声明、图转移、idle边界提交、实例/事件/生命周期统一替换。 | 真实agent/editor重载、语法/执行/预算拒绝后旧world继续、named回调重绑定、OnStart不重跑、原生库存/刚体保留、typed bundle实际重载。限制见script-reload.md。 |

S1 中的 typed userdata 不是“换成类型化语言即可安全”：当前 native 组件通过属性绑定暴露对象，第二语言需要同样的所有权检查。哪些旧字段改为只读须单独检查现有脚本并明确兼容范围，本研究未修改这些接口。S2/S3 失败也不能声称已经回滚整个 tick；先诚实报告失败位置与已执行帧，再按契约 reset。

1. **保持已经建立的实例隔离。** 当前默认可变 table 图与已知 native value 按实例复制；函数/upvalue 与显式全局保持约定的共享。后续 typed userdata/模块改动继续运行实例隔离回归。
2. **调用边界统一。** top-level load、生命周期、EventBus、工具执行和 reload 采用相同 protected-call/traceback 路径，保存进入时 stack top，退出后恢复。不要把重复 `lua_settop(L,0)` 当作跨嵌套调用的通用设计。
3. **预算独立于 tick 数。** 现已使用自定义 allocator、逐指令 count hook 和外层进程 watchdog；hook 本身不能抢占耗时/阻塞的 native 函数。当前 `luaL_openlibs` 提供 IO/OS 能力；agent 自动运行与可信本地脚本应明确能力策略，不能把 stderr 路由称为 sandbox。[Lua 5.4 C API 与 hook](https://www.lua.org/manual/5.4/manual.html#lua_sethook)
4. **reload 先准备后提交。** 在隔离环境中解析并验证新代码，失败继续使用上一份可运行版本；在 tick 边界替换。实例生命周期状态、EventBus 订阅和函数存在性索引一起更新。是否重跑 OnStart 必须固定；不能一次编辑意外触发二次奖励、二次建刚体。
5. **区分保留状态与迁移状态。** 当前显式reload保留原instance table、nested数据图与nativeuserdata身份，新增缺省字段单独克隆；候选闭包/upvalue图可转移，但旧module-local值不迁移。instance函数、协程、未知table-key对应关系等明确拒绝。任意字段schema升级不自动转换，兼容新代码或显式reset仍由游戏负责。

Luau 的 sandbox 文档可以作为能力边界设计的参考，但宿主暴露的 C++ API 仍决定脚本能做什么。改用 Luau 并不自动使当前 native API 或外部文件操作安全。[Luau 嵌入隔离说明](https://luau.org/sandbox/)

## 下一次实验的验收条件

默认数组隔离、有界agent请求、预算失败后新进程reset、curated typed描述以及显式事务重载均已有实际切片。第八阶段已将场景准备移到显式提交之前，并缓存已执行的组件来源。第九阶段已验证 Rigidbody/ParticleSystem 的运行时数值边界，第十阶段已实现完整 runtime actor 准备。下一步是维护中的 binding 版本评估和统一错误上下文/traceback；不能由当前 reload 测试认证这些尚未完成的能力。

之后按两个独立实验推进：

- **未来 PUC-Lua 升级实验**：以已验证的 5.4.9 为基线，独立评估 5.5.1 与现有绑定/LuaBridge3；复用 `event_bus_test`、`agent_errors`、`runtime_actor_lifetime`、`hierarchy_queries`、`scene_validation` 等当前测试及完整默认项目脚本。保留资源许可和来源，验证 C++26 与三平台构建。
- **第二语言原型**：同一 C++ host 契约、同一脚本任务和同一输入回放，先比较手写 Lua 与 TS→Lua；再按需求比较 Luau 或 TS→QuickJS，仅在工具/性能需求明确时加入 V8。记录构建体积、冷启动、host crossing、每 tick P50/P95/P99、分配/GC 暂停、错误定位质量与 agent 修复轮次。

计时使用相同 Release 优化、相同安全检查、相同工作量，并分开记录冷启动与热运行；JIT 必须包含热身成本。保存正确性输出，先确认生命周期、事件顺序和最终游戏状态一致。当前没有这些候选的项目实测结果，因此不承诺任何 VM 的倍率、SOTA 或“换成 TypeScript 后 AI 必然更可靠”。

## 第五阶段实际进展

现已固定 TypeScriptToLua 1.37.1 / TypeScript 6.0.2 并交付可选 authoring 工具。
单一结构化 host contract 生成 `.d.ts`、LuaLS metadata 与 agent describe 中的
`script_api`；它是显式 curated 子集，并非所有 Lua/engine API 的完整类型覆盖。
真实 SDK 正向/负向编译和原生绑定契约测试已接入，最终矩阵见 [status](status.md)。

实验 controller 使用本地 TypeScript helper 模块，打包为单个 Lua 组件并内嵌所需
lualib；仍在同一 Lua 5.4.9 VM、同一 Input/Box2D/animation/save 路径执行。手写 Lua
与编译 TypeScript 已完成 102 次完整可观察状态对比，包含存档字节、坏 schema、
重置和新进程恢复。尚未测量 AI 在 TS 上的修复轮次优势、全 ECMAScript 兼容性、
全部 npm 依赖或运行时 source-map 错误定位；不能从这个实验推导这些结论。

## 第七阶段实际进展

独立32 MiB/100万指令的候选Lua状态已接入真正的组件热重载，并通过同一
ComponentManager入口供agent命令与编辑器Tools窗口使用。保存原实例/原生对象，
按唯一prototype方法身份重绑定active/pending事件，不重跑OnStart；不支持的
instance闭包、匿名订阅和元表明确拒绝。错误文本有4096字节UTF-8安全边界，
候选loop/heap失败不消耗主VM的配额或要求reset。主VM资源耗尽仍是fatal/reset
边界，编辑器也不宣称硬墙钟超时或完整安全沙箱。

真实TypeScript bundle已增加31次重载后状态对照，和此前102次基线比较分开记录。
第八阶段随后移除了场景预检中的旧重复加载路径：可信启动脚本执行一次，已知来源
按路径缓存；新来源用受限候选VM准备，成功才发布。可信启动的任意外部IO、提交后的
回调副作用和主VM耗尽仍不承诺回滚，详见[场景合同](scene-transitions.md)、
[ADR 004](adr-004-scene-preparation.md)与[重载合同](script-reload.md)。

## 下一阶段的绑定实验

第十一阶段的编辑事务、事件改名和延期激活回归已加入完整验证。下一步按
[binding-upgrade-experiment](binding-upgrade-experiment.md) 进行固定 LuaBridge3
版本的独立边界探针，保持 Lua 5.4.9。该实验现已运行，两版的四项隔离测试分别通过
Debug/Release/ASan；真实引擎 TU 的不兼容、属性可写性、返回值、错误传播、所有权
与预算结果见 [实验结果](binding-experiment-results.md)。生产引擎仍是 2.7；
[ADR 007](adr-007-checked-lua-binding.md) 指定下一步完整接入及不可弱化的验证门槛。

## 第十三阶段实际进展

LuaBridge3 已接入真实引擎，保留 Lua 5.4.9 和既有 Lua／TypeScript 可见 API。
[统一宿主边界](../../include/engine/scripting/LuaBoundary.h) 检查转换结果，保留
回调忽略返回值的规则、既有 Lua 栈、错误计数及有界 UTF-8 traceback。生产保持
库的 result mode，不打开全局异常开关：真实契约测试发现该开关会使错误 self
绕过 Lua pcall。完整 Debug／Release／ASan+UBSan 各 66 项通过；保留的 GLM
答案也再次通过 16 个场景、96 个检查点，未重新调用模型。

同机器真实 headless 动画负载中，最终原生模式 p50 改善约19–24%；Lua密集
模式相对2.7仍有0.9–1.7%的 p50 差距，不宣称全面提速或 SOTA。采样驱动的
同状态指针快路径保留不同协程／VM比较，并有单独测试和来源记录。完整数据见
[binding-runtime-performance](binding-runtime-performance.md)。显式 nil 方法
接收者在旧基线也会造成 worker 崩溃，仍需单独修复；Lua5.5与其他VM仍是后续实验。
