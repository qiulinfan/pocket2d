# Agent 脚本预算与 worker 生命周期

本文件描述 `--headless --agent` 的实际进程边界。游戏仍运行在真实 Lua 5.4.9 / Box2D / Engine 中；外层 supervisor 负责协议与超时，内部 worker 持有游戏世界。它是故障隔离与资源预算，不是限制文件、网络或 OS 能力的安全 sandbox。

```sh
./build/ninja-debug/game --headless --agent \
  --project tests/fixtures/village_defense \
  --instruction-budget 50000000 \
  --lua-heap-budget 67108864 \
  --request-timeout-ms 5000
```

`--agent-worker` 是同一可执行文件的内部入口，只供 supervisor 使用。直接调用它不会获得外层 deadline。图形 runtime/editor 默认不启用 Lua 限额和 count hook；这些参数目前由 agent worker 接收。CLI 限额必须为正整数，timeout 范围为 100–60,000 ms。

公共 `describe` 返回实际 `execution_limits`。`instruction_limit` 与 `lua_heap_limit_bytes` 是十进制字符串，避免 JavaScript 丢失 uint64 精度；`request_timeout_ms`、`max_response_bytes`、`max_request_bytes` 是数值。它还明确给出 Lua-only heap 范围和 `filesystem_sandbox:false`。普通 `observe` 不重复这些配置。

## 限额的精确定义

| 限额 | 默认值 | 实际限制范围 |
| --- | --- | --- |
| `instruction-budget` | 50,000,000 | Lua count hook 报告的 VM 指令：`step`/`run_until` 每推进一个模拟 tick 重新补满（按 tick 计），其他请求按整个请求作用域计。预算模式 hook interval 为 1；同一作用域的生命周期、嵌套事件和协程共享计数。不是 C++ 指令、解析时间或墙钟时间。长 step 不会因帧数累计超限，单个失控 tick 仍会失败。 |
| `lua-heap-budget` | 64 MiB | Lua allocator 请求的存活字节；包括 Lua table/string/userdata 分配。不是进程 RSS，也不包含 allocator 额外开销、C++ 容器、SDL 纹理、Box2D、native userdata 内部另行分配的缓冲。 |
| `request-timeout-ms` | 5,000 ms | supervisor 为完整命令设置的单一 deadline；覆盖向 worker 写入、等待执行/响应、读取响应及相应校验。初始化、reset 和 quit 也受监控。 |
| 输入长度 | 64 KiB | 一条公共 JSONL 命令，不含换行。超长行在固定上限缓冲之外只丢弃字节，直到行结束，然后返回错误；不会污染下一条命令。 |
| worker 响应长度 | 16 MiB | 一条内部响应的累计管道数据，包含启动握手中的初始观察。超限强制销毁 worker。 |
| JSON 嵌套 | 64 层 | 公共命令和 worker 响应先检查深度，再使用迭代解析及 UTF-8 校验。非法或过深的 worker JSON 不递归进入宿主对象序列化。 |

指令预算在最外层 `RequestScope` 开始时重置，`step`/`run_until` 在每个 tick 前再补满（`ScriptBudget::ReplenishForTick`，不清除已发生的失败）。较大的 hook interval 会遗漏短协程退出时尚未用完的局部倒计数，因此当前选择 interval 1；`lua_budget_test` 用大量短协程验证它们累计消耗同一配额。可信图形模式的零限额不安装 hook。预算是执行成本，不能宣传为免费检查。

heap 越限采用严格、粘性的失败语义：一次分配请求将超过 cap 时，allocator 拒绝它并记录失败。即使 Lua emergency GC 后重试成功，或脚本用 `pcall` 吞下 OOM，宿主仍把该请求视为失败。free/shrink 仍可执行。请求结束只清理作用域，失败原因保留供协议层检查；新请求开始才重置指令计数/失败标志，Lua 的已有存活分配不会凭空归零。

## 正常启动与请求

supervisor 创建同一 binary 的 worker，并发送内部 `_bootstrap`。worker 从 VM 创建前开始持有同一个启动预算，经过顶层组件代码、discovery 和初始 observe 后才结束它；初始观察作为 `_initial_observation` 随握手返回。父进程验证并缓存这些数据，正常启动不主动向公共 stdout 发送握手内容。

每次父子交换带有私有 `_pocket_request_id`，由随机进程前缀和计数构成。worker 回显后，父进程检查匹配、响应类型和字段形状，再移除私有字段输出给调用者。启动失败可能早于 worker 读到握手，所以只对该阶段的 `ok:false` 允许缺少 ID。一个看起来像合法 JSON 的日志也不能当作命令响应。

公共客户端不能发送 `_bootstrap`、`_pocket_request_id` 或 `_initial_observation`。这些字段不会出现在公共顶层响应。非法 JSON、缺少字符串 `cmd`、超长输入或保留字段由父进程本地拒绝，健康 worker 保留，游戏不会因此推进。详见 [公共协议](agent-protocol.md)。

首次启动如果失败，返回一条结构化 JSON 错误并以非零码退出；需要修正项目/参数后重新启动。CLI 使用错误、缺少必须的参数或不合法的项目路径可能在 supervisor 创建前就以 stderr + 非零码退出，不属于已经建立的 JSONL 会话。

## 失败、reset、quit 与 EOF

以下失败会让现有 worker 不再接收世界访问，并返回 `requires_reset:true`：Lua 指令/heap 限额、外部 deadline、worker 退出、非法/额外/不匹配的响应、超限响应。

- `observe` / `step` 随后返回需要 reset 的错误，不会自动偷偷重跑上一条动作。
- `describe` 可以返回最近一次通过验证的缓存，标记 `runtime_available:false` 和 `requires_reset:true`。它不是重新读取了已修改的项目。
- `reset` 使用新的 worker 进程，重新读取项目并应用 seed；不尝试在可能 OOM/panic 的旧 Lua VM 中修复。新 worker 的顶层初始化、discovery 和初始观察共用一份指令预算，不再分成多次补充配额的内部命令。
- 健康旧 worker 在 reset 前有最多 100 ms 的退出宽限，超出即强杀。旧世界清理与新世界启动仍使用外部 reset 的同一个 wall deadline；旧进程清理不与新进程共享 Lua 指令计数。
- `quit` 不要求世界可观察。先尝试正常退出，阻塞的 native OnDestroy 会被强制终止；确认清理后返回 compact quit 响应。
- stdin EOF 不额外发送一条响应，父进程终止并回收直接 worker，然后退出。

强杀后的 reap 采用独立、最多约 1 秒的轮询清理窗口，所以用户看到的超时返回可比设置的 request timeout 略晚。正常 native sleep 可以被进程终止打断。若这个窗口内无法确认退出，supervisor 非零退出，不继续创建更多 worker。

worker 能报告实际已完成帧时沿用它的 frame。若 worker 被强杀、没有可验证的当前快照，父进程只能给出最后已观察的 frame，并加 `frame_is_last_observed:true`。失败不表示回滚了此前 tick、文件写入或其他外部副作用。

## deadline 不涵盖的行为

这些边界必须保留在对外说明中：

1. **等待客户端输入不是 worker 请求执行。** `ReadCommand` 可以等待完整行；未发送换行的客户端尚未提交一条完整命令。不断发送一条不结束的超长行不会无限增加缓冲，但不受 worker 请求 deadline 限制。
2. **消费者背压不属于游戏预算。** 调用者不读取公共 stdout 时，父进程写响应可能阻塞。评测器应持续读取 stdout/stderr，并配置自己的总会话 timeout。
3. **OS API 不由该轮询器抢占。** `SDL_CreateProcessWithProperties`、进程终止/关闭资源等系统调用本身不是 Lua hook 可中断的工作。极端 OS/文件系统停滞不被宣称必在指定毫秒数内返回。对 supervisor 自身的最终保护仍属于外层任务 runner。
4. **只管理直接 worker。** 脚本自行启动的孙进程、父 supervisor 被外部强杀后的进程树回收、文件/网络/OS 权限控制，都不由本实现承诺。不要把它当作不可信代码执行容器。
5. **Lua hook 可以被脚本改写。** 当前开放的 debug 能力可移除 hook；`pcall` 或协程也可能吸收普通 Lua 错误。宿主粘性失败检查处理可返回的路径，独立进程 deadline 处理不返回和 native 阻塞路径。
6. **预算不等于完整内存隔离。** native API 仍可申请 Lua allocator 以外的内存。supervisor 的输入/响应缓冲有明确上限，但这不构成 worker 或宿主进程的整体 RAM/cgroup 上限。

## 实现与验证入口

核心文件：

- `src/engine/scripting/ScriptBudget.h`、`src/engine/scripting/ScriptBudget.cpp`：allocator、count hook、嵌套作用域与粘性原因。
- `src/engine/scripting/manager/Runtime.cpp`：创建/关闭预算化 Lua state；不改变 LuaBridge 或 VM 版本。
- `src/app/runtime/AgentSession.cpp`：启动/命令作用域、失败上报和内部握手。
- `src/app/runtime/AgentSupervisor.cpp`：非阻塞 SDL3 process pipes、deadline、关联、校验、kill/reap 与恢复。

```sh
ctest --preset ninja-debug \
  -R '^(lua_budget|agent_budget|agent_protocol|agent_errors|agent_mouse)$' \
  --output-on-failure
```

`lua_budget_test.cpp` 覆盖主线程指令、嵌套作用域不补充配额、短/长协程、吞异常、Lua heap 上限、失败创建、释放计数和无限额模式。`agent_budget_test.py` 使用真实可执行文件与临时项目，覆盖死循环、coroutine/pcall、移除 hook、Application.Sleep、OOM、worker 退出、超大/非法 UTF-8 响应、提前到达的合法 JSON 噪声、startup、reset（含健康世界重载顶层 native sleep 超时后修复再重置）、quit、EOF，以及 64 KiB 边界和协议私有字段。

独立复核已通过最新 `agent_budget_test.py` 与 `lua_budget_test`。最终 Debug/Release、sanitizer 和平台集成矩阵以 [status](status.md) 为唯一状态入口，本页不复制易过时的测试数量。POSIX `/dev/stdout` 噪声注入测试不代表 Windows 上同一路径已实测；Windows/Linux process 管道与清理需要各自平台执行证据。
