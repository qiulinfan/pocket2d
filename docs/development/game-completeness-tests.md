# 整游戏完整度测试：Village_Rim 迁移与 PocketTerraria

日期：2026-10-16。两个项目都在 `Projects/` 下（引擎仓库忽略，各自是本地 Git 仓库，
不推送）：`Projects/VillageRim` 迁移 Unity 版 Village_Rim 的真实世界（`pocket import unity` 导入），
`Projects/PocketTerraria` 用引擎从零做一个类泰拉瑞亚沙盒（Boss 只做克苏鲁之眼）。
目的不是交付完整游戏，而是用真实内容找出引擎缺口。两者都有通过 agent 协议、
用真实输入（动作、按键、鼠标）游玩的 playtest。

## Village_Rim 迁移

`pocket import unity` 读取 `Gameplay.unity` 及其中所有 prefab 实例
（含 prefab 修改项、stripped 对象别名），导出 49 个 tilemap（53,081 格，按每个
Tile 资产的 `m_ColliderType` 决定 solid/ground）、189 个精灵、111 个静态碰撞体
（box/circle/capsule）、10 个实体 `SpawnMarker`（带非静态 Rigidbody2D 的对象）
和 30 个按名称前缀导入的出生点。
Unity sorting layer 映射为 `sorting_order = 层序号 * 100000 + order`，全部精灵
按 y 排序（与原工程 2D Renderer 的自定义排序轴一致）。`tools/pack_characters.py`
为每个角色写 animation 资源（clip 直接引用 Unity 每动作一张的表，含 Golem 的 128 px
四向表），`tools/pack_items.py` 从物品 ScriptableObject 读名称、堆叠和图标精灵，
裁出图标并生成 `items/village.items.json`（12 种物品）。

手写玩法组件：

- `VRPlayer`/`VRGoblin`：移动/奔跑/体力、剑击命中帧、受伤无敌帧，哥布林侦测/追击/
  前摇攻击；入侵者（30 HP）按 `EnemyMovementInvader` 先攻击本岛最近的祭坛，只在
  2 秒内被玩家打过或本岛祭坛全毁时转向玩家。
- `VRInventory`：原生 Inventory 的 30 格背包，前 9 格为快捷栏（1–9/滚轮），Tab 面板
  点击搬运；武器决定攻击力，食物回血，盾牌在快捷栏时伤害减半。
- `VRDialogue` + `VillageGame` 任务链：NPCSO 原文台词（Priest 的 Introduction/
  GetRestore/PlaceRestore/Continue，Knight 给盾，SwordsMan 给指南针，Soldier 的
  BossFight）；修复宝石复活祭坛、风宝石赐福（移速 ×1.3）；指南针传送到已发现且
  未毁的祭坛。
- `VRInvasion`：三座岛的祭坛（500 HP，每秒 +2）与出生点；到访且祭坛存活后按
  `max(10, base − wave×5)` 秒倒计时，生成 `base + wave×growth` 个入侵者；祭坛全毁
  则本岛入侵停止，不判负。
- `VRGolem`：200 HP，侦测 15、攻击距离 3、冷却 2 秒、前摇 0.8 秒，50/50 选择
  360° 砸地（20 伤害、半径 3）或三圈各 12 发的旋转蓝火（每发 10）；击败即胜利并存档。
- 存档：F5/F9 用 `Save.Write/Read` 存玩家、背包、祭坛与岛屿入侵、任务与 NPC 赠品、
  Boss 与地上掉落；读档先完整校验，再重新加载场景并应用（入侵者不存，与原作一致）。
- 任务指引：`Physics.FindPath` 规划，目标在别的可行走区域时按传送门图（BFS）先走
  向第一个传送门；同时作为 HUD 提示和 agent 观测。

`tools/playtest.py` 只用真实输入和游戏观测，跟随任务指引完成：与 Priest 对话、
击杀携带宝石的哥布林并拾取、回报、用鼠标在背包里搬运物品、修复祭坛并放置风宝石、
领取指南针、守住第一波入侵、F5 存档 → `reset` 新 worker → F9 读档、APortal1/BPortal1
往返、听 Soldier 的话经他开启的传送门到 Golem 岛并击败 Golem、用指南针传回祭坛；
再从战前存档用其它种子重打，覆盖两种攻击。15 项检查全部通过，同一种子两次运行的
结果逐项一致。

迁移时的适配（原作行为依赖未迁移内容或不可测）：

- 原作各岛之间靠谜题内容连通（需钥匙的门与宝箱、幽灵墙、骷髅王死后出现的桥、
  冻结水面等，未迁移）；Soldier 的"面前的传送门会带你去找他"按字面实现为一个通往
  Golem 岛的单向传送门，并选用起始岛上的 Soldier。
- 原作修复宝石只有 15% 掉率；任务所需的修复宝石与风宝石由起始岛巡逻队的第一只
  哥布林必掉（其余敌人仍是 15%、全局上限 5）。
- 入侵者用近战哥布林代替弓箭手/炸弹手（每次 10 点祭坛伤害、2 秒冷却）；盾牌在快捷栏
  时被动减半伤害，代替右键举盾；玉米的回血在原代码中被注释掉，这里用其存储的 10。

## PocketTerraria

种子化 400×220 世界（地表起伏、洞穴、铜/铁/金/魔矿、树、背景墙、沙漠与雪地），
泰拉瑞亚式平台移动（分轴格子碰撞与自动上台阶）、挖掘/放置/砍树、火把与光照传播、
昼夜、史莱姆、原生 Inventory 作为 50 格背包与热键栏、合成界面、两阶段克苏鲁之眼
（悬停召唤仆从 + 三连冲刺；半血旋转变形后无仆从、更快的冲刺；天亮或玩家死亡逃走）。
地块与远景来自 Kenney CC0，其余为原创像素图。

`tools/playtest.py` 用真实输入完成整条流程：砍树、竖井挖到铜矿、放置火把、
垫方块爬回地面、放方块、打史莱姆拿凝胶、在合成界面点击合成召唤物和药水、
等到夜晚、召唤并击败克苏鲁之眼（进入二阶段）。9 项检查全部通过。

## 发现并已修复的引擎问题

| 问题（来源） | 修复 |
| --- | --- |
| `auto_sorting_order` 以 `int(y)` 取代 sorting_order（VR） | 绘制请求增加 `sort_y` 次键：sorting_order → y → 提交顺序 |
| 相邻 tile 浮点取整出现 1 px 缝（VR、PT） | 无旋转绘制在输出像素空间吸附边缘，并加 1e-3 px 偏置防止回乘截断 |
| `MoveCharacter`/`SetPosition` 移动的运动学刚体下一步被 Transform 拉回（VR） | 同时写回 Transform；文档说明运动所有权 |
| 没有自动拼接，Terraria 方块帧需游戏自写（PT） | Tilemap `autotile` 地形：16 种四邻掩码 × 变体，`frame_on_load`，`SetTerrain`/`GetTerrain` |
| UI 图片不能缩放、不知道尺寸（PT） | `Image.DrawUIScaled`（含镜像）、`Image.GetSize`（headless 可用） |
| 没有屏幕↔世界换算（PT） | `Camera.ScreenToWorld`/`WorldToScreen`/`GetScreenSize`/`GetViewSize` |
| 逐格 `GetCell` 建镜像（17.6 万次）（PT） | `Tilemap:GetCells` 矩形批量读取 |
| `step` 后还要 `observe` 才能看格子（PT bot） | `step`/`run_until` 接受 `tilemaps` 查询，推进前校验 |
| 动画资源只能引用一张图（VR） | clip 可选 `sheet`，原样使用 Unity 每动作一张的表 |
| Box2D mover 只有胶囊，方块边缘滑落（PT） | box 碰撞体按轴扫掠，直角不滑；文档注明需从无重叠位置开始 |
| 没有寻路（VR，原 NavMesh） | `Physics.FindPath`：网格 A*，静态碰撞体与实心 tile 阻挡，部分路径 |
| Lua 光照每帧 ~1,450 次绘制（PT） | `Tilemap:ComputeLight` 原生光照写入 `dynamic:` 动态图片；PT headless 1.18 → 0.076 ms/帧 |
| 物品 kind 固定、无图标（PT） | 新增 `tool`/`placeable`/`consumable` 与 `icon` 字段 |
| Tilemap 只有 ground/solid 两层（VR、PT） | 至多 6 个额外命名装饰层（各有排序，只有 solid 碰撞）；PT 背景墙改用独立 `walls` 层 |
| 新实例化的 actor 同帧内字段未初始化（VR、PT） | `OnAwake`：发布即执行（Instantiate/AddComponent 返回前，场景内先全部 awake 再 start） |
| agent 指令预算按整个请求计，长 step 失败（PT） | `step`/`run_until` 每个 tick 补满预算，失控 tick 仍失败 |
| Unity 导入器只在游戏项目里（VR） | `pocket import unity`：场景与 prefab 实例、tilemap、精灵、碰撞体、实体；VR 重新导入逐字节一致 |
| 出生点不能按名字导入（VR） | `--point-prefix`：命名对象成为 `SpawnMarker` 点；本身有精灵的点把标记挂在那个精灵 actor 上，不再重复导入 |
| 站得比净空半径更贴墙时 `FindPath` 出不了起点格（VR 任务指引在墙边来回抖动） | 区分实心格与净空格：距起点或终点 `ceil(radius/cell)+1` 格内的净空格可通行，实心格仍不可 |
| 较早 actor 的 `OnStart` 里查不到后面 actor 的静态碰撞体（VR 传送门区域在 OnStart 里算错） | 同一启动批次先启动内建组件再调用 Lua `OnStart`；`OnStart` 中启用的刚体仍在本批次启动 |
| 受击时进入传送门，锁定期间强制播放 idle，受击动画的完成事件不再到来，玩家永远卡在 hurt（VR 游戏代码） | 游戏侧修复：锁定期间只在 move 状态播放 idle |

（VR = Village_Rim，PT = PocketTerraria。）两个游戏都已改用这些能力，并重新通过各自的 playtest。

## 仍存在的缺口与暂不修改的原因

- **导入近似**：Unity Sprite 形状碰撞按整格；有缩放/旋转的 tilemap、九宫格精灵和 trigger 只记录在
  `import_report.json`。
- **Village_Rim 未迁移的内容**：岛间通行谜题（门/钥匙/宝箱 UI、幽灵墙、骷髅王与桥、冻结与融化）、
  B 世界的魔树柱（摧毁后停止该岛入侵）与陷阱、农场与种树、风以外的元素与元素图鉴、弓箭手/炸弹手的
  投射物、开场过场、小地图和音频；原工程玩家出生点位于水面碰撞体内（运行时另有放置逻辑，未迁移）。
  这些是内容移植，不是已知的引擎缺口。

## 性能

Release、macOS（Apple M5）：PocketTerraria 在 headless agent 模式下（带逐指令计数钩子）
从 Lua 光照时的 1.56 ms/帧降到原生光照后的 0.076 ms/帧，Village_Rim 约 0.12 ms/帧；
窗口模式被垂直同步锁在 60 Hz。
绘制排序键从 8 字节增加到 16 字节后，bunnymark 50k 交替 A/B 的结果落在噪声范围内
（前 17.9/16.0/21.1 ms，后 22.6/18.3/16.1 ms），W1 sprites 也在噪声内。
原始输出保留在被忽略的 `build/` 中。以上不是跨引擎对比。
