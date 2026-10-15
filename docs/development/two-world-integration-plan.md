# 两世界玩法整合计划

状态：规划，尚未实现或试玩。日期：2026-10-15。

下一片建议验证“一座岛、两个区域、永久关闭一条入侵路线”：种植所得进入真实库存，食物和装备影响真实战斗，玩家经配对传送门追击入侵源，关闭路线后跨进程保留进度。它把现有农场、库存和防守样例连成一个可玩的过程；不以继续扩展绑定层审计作为该玩法的前置目标。

## 原项目证据与未确认项

以下源码路径属于 `qiulinfan/Village_Rim`，核对版本为
[`c2f86f0280d53165977d63e648f5dd4df1901c5a`](https://github.com/qiulinfan/Village_Rim/tree/c2f86f0280d53165977d63e648f5dd4df1901c5a)。核对时列出的规则源码在该工作树中没有修改。本次只读源码和设计资料，没有运行 Unity、导入素材或核对场景／prefab 中的序列化参数。

| 规则 | 已核实的证据 | 对本切片的约束 |
| --- | --- | --- |
| 双世界压力与永久解除 | `DEVBLOG.txt` 明确保留跨世界压力，并通过摧毁各岛关联的恶魔树永久停止该路线；`design/gdd.md` 将准备、探索、防守和永久世界变化连接成循环。 | 打完若干波只算暂时守住；永久解除来自对应入侵源被摧毁。不能沿用 defense fixture 的“三波即全局胜利”。 |
| 入侵资格 | [`Assets/Scripts/IslandInvasions/IslandZone.cs`](https://github.com/qiulinfan/Village_Rim/blob/c2f86f0280d53165977d63e648f5dd4df1901c5a/Assets/Scripts/IslandInvasions/IslandZone.cs) 要求已访问、柱仍存在、至少一个关联祭坛仍存在。两祭坛岛使用 OR 条件。`InvasionTriggerSystem.cs` 跳过 pending／active 岛。 | 本片只设置一岛一祭坛，状态机不可重复排同一波。 |
| 波次默认参数 | 同文件的源码默认值是 `max(10, 60 - 5 × waveIndex)` 秒、`3 + 2 × waveIndex` 个敌人；检查间隔默认 4 秒。`InvasionControl.cs` 在安排新倒计时时增加波次，恢复存档时直接采用保存波次。 | 这些是源码默认值，不是已核实的关卡调参。切片若使用它们或加速参数，应在自己的 CONTRACT 中明确。恢复 countdown 不得再次增加波次或重算已安排人数。 |
| 防守目标 | [`Assets/Scripts/Enemies/InvadersEnemies/EnemyMovementInvader.cs`](https://github.com/qiulinfan/Village_Rim/blob/c2f86f0280d53165977d63e648f5dd4df1901c5a/Assets/Scripts/Enemies/InvadersEnemies/EnemyMovementInvader.cs) 优先找作物，没有作物再找祭坛。 | 至少一个测试要证明种植成果确实受到入侵威胁，不能只让敌人永远直奔祭坛。 |
| 摧毁入侵源 | [`Assets/Scripts/HealthSystem/EnemyHealth.cs`](https://github.com/qiulinfan/Village_Rim/blob/c2f86f0280d53165977d63e648f5dd4df1901c5a/Assets/Scripts/HealthSystem/EnemyHealth.cs) 的 `isPillar` 死亡分支记录 `treeID`，将目标岛的 `pillarAlive` 置 false，发布 `PillarDestroyedEvent`。`InvasionControl.cs` 取消当前倒计时，并在活动入侵中停止生成、清理对应入侵者。 | 用稳定 route ID 关联村庄与地下入侵源；关闭路线同时阻止未来排波，并取消该路线当前压力。 |
| 传送方式 | [`Assets/Scripts/World/TransportPortal.cs`](https://github.com/qiulinfan/Village_Rim/blob/c2f86f0280d53165977d63e648f5dd4df1901c5a/Assets/Scripts/World/TransportPortal.cs) 通过 `portalId` 配对端点，直接修改玩家位置，使用出生点／进入方向偏移、到达冷却、淡出与 camera warp，发布世界切换事件。 | 这是同一场景内传送，不是 `Scene.Load`。本片采用同一 scene 中的两个常驻区域，让离开村庄后的入侵和成长自然继续运行。 |
| 保存范围 | `Assets/DevilLoadManager.cs` 的死亡字典仅 `DontDestroyOnLoad`；磁盘永久状态另见 `Assets/Scripts/Utils/AltarSaveManager.cs` 的 pillar 状态恢复。`InvasionSaveManager.cs` 保存地区、剩余倒计时、人数、波次；`InvasionControl.TryGetPendingCountdown` 只返回等待中的倒计时。 | 不能把跨场景内存字典称为磁盘持久化，也不能由这些源码推断已实现任意战斗中途快照。 |
| 种植和产出 | `Assets/Scripts/Farm/FarmPlot.cs` 消耗种子；`CropGrowth.cs` 根据 SeedItem 生成收获和可选种子掉落。 | 将种植／收获接入实际物品守恒；作物品种、随机产量、返种比例不是本次已确认的平衡参数。 |

“恶魔树”与代码中的 pillar／treeID 在本片统一表现为一处可攻击的地下入侵源。更完整的世界命名、岛屿关系与剧情仍需要产品设计确认。

## 当前可复用能力

- `tests/fixtures/persistent_village` 已表达 16 格作物、剩余成长 tick、拆墙、显式 Save/Load 和永久升级；它目前使用供应计数，没有与原生库存整合。
- `tests/fixtures/inventory_village` 已使用原生容器完成拾取余量、30 格背包、箱子、装备、快捷引用、食物回血及保存；其攻击目标仍是木桩。
- `tests/fixtures/village_defense` 已通过真实 Lua／Box2D 表达移动、入侵、祭坛受损及摧毁来源；它没有双世界门户与整合存档。
- 原生 Tilemap、SpriteAnimation、Rigidbody、Inventory、Event、Camera、Save，以及真实键鼠／agent 输入路径可表达本片。种子可用 catalog 的 `material` 条目加游戏侧种植定义，不必为了一个样例添加引擎级 seed 枚举。

计划首先组合这些现有能力。该判断不是“引擎不存在缺口”的证明：集成测试若发现可复现缺陷，再单独提出最小引擎修复。当前无需预先引入流式加载、第二个 VM、通用伤害框架或跨场景离线世界模拟。

## 建议的可运行合同

新项目放在 `tests/fixtures/two_world_village/`，保留旧 fixtures 及其回归。使用自制几何图与已有测试素材，不复制 Unity 美术或存档。

1. **村庄准备。** 一个背包、一个装备栏、一种种子和一种食物。种植成功消耗一颗种子；不可种植或库存不足时不消耗。成熟作物产生固定一份食物掉落，拾取按原生 `moved` 扣除地面余量，满包不丢物。首片不随机返种；初始种子数量、成长 tick 和物品参数属于切片调参。
2. **实际战斗。** 装备攻击属性改变敌人／入侵源受到的伤害，食物消费恢复玩家生命；移除木桩和调试扣血作为唯一验证手段。保留有界攻击冷却与一次死亡结算。村庄入侵者先破坏可攻击作物，再攻击祭坛；地下区域放一个使用相同伤害规则的敌人。
3. **跨世界压力。** 村庄和地下各有一张地图、一个固定配对门。玩家在地下时，村庄成长、倒计时和已生成敌人继续更新。HUD 持续显示所在世界、祭坛生命、倒计时／波次及入侵源状态。
4. **明确传送状态。** 用已有位置、速度、Camera 与 UI 绘制 API 实现过渡。期间游戏侧暂停玩家移动和交互，落点位于目标触发区外；目的端冷却并要求离开触发区后重新启用。后者是新增防抖规则，不声称原脚本已有。此片不使用 `Scene.Load`，也不宣称覆盖资源驻留或流式区域切换。
5. **永久结果。** 入侵源生命归零时，关闭其 route ID，取消其倒计时／生成，清理该路线入侵者。清一波后仍可能再排波；只摧毁来源才获得永久解除。祭坛或玩家生命归零进入失败状态，可加载已有检查点。
6. **显式检查点。** 使用新 namespace `two_world_village`、slot `main` 和版本化游戏 schema。保存背包／装备、地面余量、作物及剩余成长时间、玩家生命／位置／世界、祭坛状态、route 状态和等待中波次。只在稳定位置且处于 countdown 或路线关闭状态保存；活动波次／传送途中显示 busy，保留原存档。该限制与当前原项目仅明确保存 pending invasion 的证据相容，但仍是本片的产品约束，不等于原游戏完整存档契约。

Load 必须先验证完整 schema 和跨字段关系，再修改 live 状态。采用相对剩余 tick；不得保存 LuaRef、ActorHandle generation、原生指针或绝对引擎帧号。`route_closed` 必须与来源死亡、无 pending 波次一致。Reset 恢复作者初始状态并保留磁盘存档；新进程显式 Load 恢复永久关闭结果。

## 建议分工与文件

| 所有者 | 拟新增文件／责任 |
| --- | --- |
| 游戏规则 | `component_types/VillageState.lua`、`FarmRules.lua`、`Combatant.lua`、`InvasionRoute.lua`、`PortalPair.lua`；统一游戏时钟、动作仲裁和稳定 route ID。 |
| 内容与反馈 | fixture 的 `scenes/`、`maps/`、`items/`、`actor_templates/`、HUD，以及自制几何生成脚本；两个区域应有可辨识的颜色和路线提示。 |
| 持久化 | `component_types/VillageSave.lua`、`CONTRACT.md`；schema 校验、明确 busy／失败反馈、物品和永久进度关系。 |
| QA／集成 | `tests/agent_two_world_village_test.py`、对应 runtime／render 测试，构建接线及迁移矩阵更新。实际 ownership 在实施前分配，避免多人同时改同一控制脚本。 |

只读观察应暴露 world ID、route ID／阶段、剩余倒计时、波次、祭坛／来源生命、库存数量和保存状态。agent 用同一键鼠输入完成流程；不提供直接改生命、直接关闭路线等验收捷径。

## 验收与失败路径

- 种植拒绝不扣种子；成熟只结算一次；满包／部分拾取后，库存、地面余量和已消费数量守恒。
- 同一敌人使用未装备／已装备武器时，实际所需攻击次数不同；食物消耗与回血一致，死亡不重复掉落或计数。
- 入侵者能破坏作物并转向祭坛；清一波后会再次排波。玩家在地下等待不会冻结村庄计时或敌人。
- A→B→A 多次往返，持续按键、停在端点和快速重入均不产生传送循环；镜头、落点、世界标签一致。缺失配对端点必须失败并恢复控制权。
- countdown 与 active 两种情况下摧毁来源都能关闭该路线；等待超过下一波时间仍无新入侵。新进程 Load 后结果保持，倒计时恢复不重复增加 wave index。
- 保存 busy／写入失败不覆盖旧检查点。未来 schema、损坏末字段、非法物品实例 ID／地图坐标、矛盾的路线状态在任何 live 修改前拒绝。
- 相同种子和输入序列可重放到一致观察。图形运行检查 HUD、portal 过渡、攻击反馈和路线关闭提示；软件 readback 与可用硬件后端分别验证。
- 人类试玩确认：能理解“备粮／装备—守一波—进入地下摧毁来源”的目标，能看懂离开村庄的风险，且失败后知道如何恢复。自动测试不能替代此项。

## 完整迁移边界

本片没有迁移原游戏全部岛屿、双祭坛规则、NPC／任务、boss、元素反应、天气冻融、完整作物与物品表、任意战斗快照、Unity 资产导入或旧存档转换。同场景双区域也不证明跨场景世界模拟或流式资源系统完成。它的价值是首次让已经验证的引擎能力共同支撑原项目的核心压力与永久进展，而不是再增加一个互不相干的功能演示。

后续能力范围仍以 [roadmap.md](roadmap.md) 和 [village-rim-migration-matrix.md](village-rim-migration-matrix.md) 为准；本计划不改变当前发布状态，也不代表实现或最终验收已经完成。
