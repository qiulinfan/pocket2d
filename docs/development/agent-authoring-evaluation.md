# 外部 agent 编写玩法：一次有界评测

日期：2026-10-15。结论：**OMP + GLM 5.3 Flash 在本次单组件任务上完成了真实引擎编写/运行循环，独立验证 16/16 场景通过。** 这是一轮、一个模型、一个已提供清晰 API 的小任务；不证明完整游戏开发能力，也不是 GLM 与 DeepSeek 的比较。

## 任务与边界

任务要求编写单个 `Repair.lua`，挂到现有 defense fixture 的 player：E 的新按下边沿触发；玩家到 altar 的实际 Rigidbody 位置距离不超过 1.25；`gathered >= 1`、`0 < health < 3`、状态非 won/lost 时消耗一个 supply 恢复一点生命。持续按住不能重复；远距离、无资源、满血/死亡和终局不改变计数。

稳定任务、API 摘要、空 stub 与独立 validator 位于：

- `tests/agent_authoring/repair_task.md`
- `tests/agent_authoring/repair_api.md`
- `tests/agent_authoring/Repair.stub.lua`
- `tests/agent_authoring/validate_repair.py`

外部 agent 只收到复制的 fixture、上述公开 API/任务内容和一个调用现有 runtime 的 wrapper。它只能改复制项目的 Repair.lua，并在自己的 scratch 目录创建自测副本。没有提供凭据、私人仓库内容或引擎源代码；未安装软件、修改 OMP 设置或开启额外工具。原项目中的 fixture 没有加入该 repair 功能；模型产物保留在 ignored build 下作为评测产物。

执行使用已安装的 OMP 18.8.7 和现有原生认证。选择 `zai/glm-5.3-flash`、thinking low；最大 agent 时间 300 秒。启动选项为：

```text
--print --no-session --no-title --no-extensions --no-skills --no-rules
--no-lsp --no-pty --tools read,write,edit,bash,glob,grep
--max-time 300 --model zai/glm-5.3-flash --thinking low --mode json @TASK.md
```

没有添加 auto-approve 或改写已有 approval 配置。CLI 的工作目录是独立 build 子目录。没有发生 quota 失败，因此没有调用作为 quota 备用的 DeepSeek Flash，也没有启动第二轮请求。

## 实际过程

模型读取 API、stub 与 scene，写入一次 Repair.lua，再通过真实 Pocket2D `--headless --agent` 运行 JSONL。它验证了初始有效修复、持续按键、释放重按、远距离和无资源等路径。六次 bash 工具调用包含测试场景准备和真实引擎运行；没有用 Python 模拟 Lua 规则代替引擎。

它自测时出现一次准备错误：第一次调整远距离场景只打印了修改后的 JSON，没有把结果写回文件，因此该次运行仍发生修复。模型从 observe 返回的原始位置识别了问题，随后写回 scratch 副本并重跑成功。这个是自测准备失误，不是工具进程报错；11 次工具调用均完成且没有工具级 error。模型没有直接自测 won/lost，最终报告也承认了这一点；这些条件由独立测试覆盖。

工作目录前后字节比较显示：原有文件中仅 `project/component_types/Repair.lua` 改变，没有删除原有文件；新增文件全部在 scratch 下。工具调用记录也只使用了提供的工作目录和执行 wrapper。这个审计是本次行为检查，不是一个操作系统级 sandbox 保证。

## 独立结果

评测者没有修补模型答案。validator 每个 case 都从原始 fixture 建立全新副本，仅导入最终 Repair.lua，然后注入初始 health/resources/state/player/altar 位置并运行真实 runtime。模型自己的 scratch 改动及自报结果不参与判分。

16 个 case 全部通过，每个包含 6 个状态检查点：

| 组别 | 覆盖 |
| --- | --- |
| 有效操作 | 两次修复、最后一个资源、释放后重新按下 |
| 输入边沿 | 按下前不变化、持续 10 tick 不重复、释放不触发 |
| 距离 | 轴向恰好 1.25、刚超出、远处、对角线边界、对角线超出、移动后的 altar 坐标 |
| 资源/生命 | 零资源、负资源、满血、超过上限、死亡 |
| 状态 | won、lost、其他非终局状态 |

先用空 stub 跑负对照：16 case 中 6 个应执行修复的 case 失败，10 个应保持不变的 case 通过。这确认测试确实要求行为发生，而不是一律“不变”也会成功。随后模型候选为 16/16，通过 96 个检查点。

这是按案例计分：某些 case 同时检查多个条件，不能将其当成 96 个互相独立的能力样本。测试没有覆盖角色缺失、热重载、恶意代码、完整 UI、网络或复杂存档；任务也不要求这些能力。

## 可得耗时与用量

| 项目 | 本次记录 |
| --- | ---: |
| OMP 进程 wall time（包含启动、推理与工具） | 100.293 秒 |
| 退出码 / host timeout | 0 / false |
| Assistant message 数 | 10 |
| 工具调用 | read 2、glob 1、grep 1、write 1、bash 6 |
| 累计 usage.input | 18,737 |
| 累计 usage.cacheRead | 75,264 |
| 累计 usage.output | 2,309 |
| 累计 usage.totalTokens | 96,310 |
| 可得 usage.reasoningTokens 合计 | 703 |

这些数字来自 OMP JSON 事件中的实际 usage 字段，按 assistant message 累加；totalTokens 含重复/缓存读取的上下文用量，不是最终源文件大小。reasoning 字段没有另加到总量里。未读取账单或额度后台，不把 harness 的价格估算当成实际扣费，也不推算费用。

## 本地证据与重放

原始日志、模型答案、自测副本和测试输出全部位于 ignored：

```text
build/agent-authoring/glm-repair-attempt-1/
  invocation.json
  completion.json
  events.jsonl
  stderr.log
  audit-summary.json
  bash-commands.json
  workspace/project/component_types/Repair.lua
  independent-validation/result.json
  independent-validation/<case>/
build/agent-authoring/stub-negative-control-16/
```

对保留候选进行离线重验，不再请求模型：

```sh
python3 tests/agent_authoring/validate_repair.py \
  --runtime build/sdl3-debug/game \
  --fixture tests/fixtures/village_defense \
  --candidate build/agent-authoring/glm-repair-attempt-1/workspace/project/component_types/Repair.lua \
  --output-dir build/agent-authoring/repair-revalidation
```

输出目录必须尚不存在，以免覆盖以前的证据。测试运行在本次 C++26/SDL3 工作树构建上；基线 Git commit 为 `223c03a1f31f5ad5acd44aa69b736871cf25464a`，包含本轮尚未提交的 engine 变化，不应将该基线 commit 单独视为最终可复现实验源码。

本次结果支持继续采用“清晰 API + 小型真实场景 + 外部 agent 实现 + 独立判分”的工作流。下一轮若要比较模型，应固定相同初始材料、预算和 validator，并预先定义多任务样本；不要在看完一个模型的结果后再改变判分条件。

最终集成复验：在 Lua 5.4.9、默认状态隔离及最终 Release runtime 上，使用同一份未修改的模型候选再次独立验证，仍为 16/16、96 个检查点通过。输出保留在忽略的 `build/agent-authoring/final-release-validation/`，没有再次请求模型。

第十三阶段复验：相同、未修改的 GLM Repair.lua 在迁移后的 LuaBridge3／Lua 5.4.9
最终 Release runtime 上再次通过 16/16 场景、96 个检查点。输出位于忽略的
`build/agent-authoring/phase13-release-validation/`；没有再次请求模型或修改答案。
这仍是同一组件任务的兼容性复验，不是新增模型能力或跨模型比较。
