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
裁出图标并生成 `items/village.items.json`（17 种物品），`tools/pack_route.py` 裁出门的
关闭精灵与宝箱开启帧（导入时门的 Pad 存的是打开的精灵，宝箱贴图在被跳过的 UISprites 里）。

手写玩法组件：

- `VRPlayer`/`VRGoblin`：移动（速度 4）、只有风祝福时可奔跑（×2，体力 −28/+20，耗尽后
  锁到回满）、剑击命中帧、受伤无敌帧，哥布林侦测/追击/前摇攻击。
- `VRInvader` + `VRProjectiles`：入侵的弓箭手与炸弹手（30 HP，向树/祭坛走 4、追玩家跑 7，
  冷却 2 秒，每 0.5 秒重选目标：仇恨中的玩家 → 本岛最近的树 → 最近的祭坛 → 回出生点）。
  弓箭手射程 10、箭速 14、伤害 10，玩家靠近 2 以内会后撤 2 并停 1 秒；炸弹手射程 8，
  炸弹飞向投掷时目标位置，0.8 秒引信（红圈），半径 2.5 内 20 伤害（有盾 ×0.25）。
  每岛按 `SpawnRandomEnemy` 抽取预制件：每波至多一个弓箭手（A、B 岛只有弓箭手，每波一个；
  C 岛弓箭手与炸弹手）。B1 掉钥匙的 GoblinArcher_KeyDrop 是守点的弓箭手（40 HP）。
- `VRInventory`：原生 Inventory 的 30 格背包，前 9 格为快捷栏（1–9/滚轮），Tab 面板
  点击搬运；武器决定攻击力，食物回血，盾牌在快捷栏时伤害减半。
- `VRDialogue` + `VillageGame` 任务链：NPCSO 原文台词（Priest 的 Introduction/
  GetRestore/PlaceRestore/Continue，Knight 给盾，SwordsMan 给指南针，Soldier 的
  BossFight）；修复宝石复活祭坛；指南针传送到已发现且未毁的祭坛。
- `VRElements`：宝石放上未毁的祭坛即设定该祭坛元素，换宝石时旧宝石弹出、3 秒冷却；
  玩家的祝福是全部未毁祭坛元素的并集（每 0.5 秒及放置时刷新，可叠加）。风：可奔跑；
  草：每 5 秒 +5 HP、毒免疫，并在祭坛周围长出 10 株装饰植物；水：挥剑时消耗 20 法力
  （上限 100，每秒 +16）射出水弹（速 10、伤 10）；火：玩家周围的光（半径 4）、地火免疫；
  土：无效果（与原作一致）。元素果实给元素直到下次刷新。左上角书按钮打开元素图鉴
  （原作四条说明，含原文拼写），旁边是当前祝福图标与法力条。
- `VRFarm`：选中种子左键点草地格（WorldA `Grass` 有、`path`/`Decorations` 无、空、2 以内）
  种树；5 阶段各 5 秒，60 HP，成熟前一击即倒；入侵者先打本岛的树；树 6 以内的祭坛每
  0.5 秒 +3 HP（原作遮罩写成 "Plant" 而层名是 "plant"，Unity 里这一加速实际不生效）。
- `VROpening`：开场过场按 Timeline（1.5 倍速）复刻：士兵、巫师、剑士与三只哥布林交战，
  祭坛五次受击后于 8.73 被毁，士兵倒下，牧师两次治疗，9.9 秒后淡出，玩家出现在
  `manualPosition`、巡逻队开始行动；每个存档目录只播放一次，Esc 可跳过。
- `VRMinimap`：原作只启用的 MiniMapCameraA（中心 (−18.4, 271.3)、正交尺寸 63、
  256×256）视野由 `tools/pack_minimap.py` 按渲染顺序烘焙；右下角带边框，叠加玩家标记、
  Golem 图标和遭入侵岛屿的标记（B 世界里仍显示 A 世界，与原作一致）。
- `VRAudio`：A/B 世界、Boss 战和过场的音乐，挥剑、受伤、死亡、哥布林、祭坛毁/复、
  各元素宝石、拾取、进食、草回血、传送门、箭、炸弹、树、对话打字等 28 段原音效
  （长 wav 音乐转为 ogg）。
- `VRInvasion`：三座岛的祭坛（500 HP，每秒 +2）与出生点；到访且祭坛存活后按
  `max(10, base − wave×5)` 秒倒计时，生成 `base + wave×growth` 个入侵者；祭坛全毁
  则本岛入侵停止，不判负。
- `VRGolem`：200 HP，侦测 15、攻击距离 3、冷却 2 秒、前摇 0.8 秒，50/50 选择
  360° 砸地（20 伤害、半径 3）或三圈各 12 发蓝火（每发 10，直线速 7.5、精灵自转
  120°/s、9 帧 × 0.15 秒后消失，相隔 0.35 秒、每圈偏转 10°）；击败即胜利并存档。
- 存档：F5/F9 用 `Save.Write/Read` 存玩家、背包、祭坛与元素、岛屿入侵、任务与 NPC
  赠品、Boss、地上掉落、门与宝箱、树和法力（schema 3）；读档先完整校验，再重新加载
  场景并应用（入侵者不存，与原作一致）。
- `VRRoute`：原作的岛间路线。A 世界四岛与 Golem 小岛互不相连，只经 B 世界的房间
  走通：起始岛 → APortal1 → B1（downdoor_B1、rightdoor_B1）→ BPortal2 → B 岛 →
  APortal3 → B2（leftdoor_B2）→ BPortal4 → C 岛（Soldier）→ APortal5 → B3
  （updoor_B3）→ BPortal6 → APortal6 → Golem。五扇 `DoorController` 门（任意钥匙，
  `KeyUseManager`：选中钥匙后左键点门，鼠标距玩家 2 以内，消耗一把；碰到关着的门
  提示 "Key required"），15 个宝箱（`ChestComponent` 默认物品按 TryAdd 堆叠；在 3 以内
  左键点开，16 格窗口与背包间点击搬运，Tab 关闭，四帧开合动画），以及 B1 里死后必掉
  钥匙的 GoblinArcher_KeyDrop（40 HP）。门、宝箱内容和弓箭手存入存档。
- 任务 NPC 是原作 `NPCs/` 组（导入了胶囊碰撞体的那几个）：B 岛 SwordsMan 给指南针、
  C 岛 Soldier 给 Boss 任务；起始岛上同名的是开场过场的 `OpeningNPCs`，不对话。
- 任务指引：`Physics.PathRegions` 一次给传送门两端、门两侧和宝箱标出可行走区域，
  再在"传送门（单向）+ 关着的门"的区域图上 BFS；下一步是锁着的门时，有钥匙就引到门前
  （`action: unlock`，附点击的屏幕坐标），没有就引向最近的钥匙来源（地上的钥匙、
  有钥匙的宝箱 `open_chest`、带钥匙的弓箭手）。路径用 0.25 格（有的宝箱在 1 格宽的
  过道里）。同时作为 HUD 提示和 agent 观测。

`tools/playtest.py` 只用真实输入和游戏观测，跟随任务指引完成：与 Priest 对话、
击杀携带宝石的哥布林并拾取、回报、用鼠标在背包里搬运物品、修复祭坛并放置风宝石、
守住第一波入侵、F5 存档 → `reset` 新 worker → F9 读档、APortal1/BPortal1 往返；
然后沿原作路线：B1 用鼠标开宝箱 14、把两把钥匙拖进快捷栏、选中钥匙点开两扇门，
到 B 岛找 SwordsMan 领指南针，B2 开宝箱 16 开 leftdoor_B2，到 C 岛听 Soldier 的话，
B3 开宝箱 9 开 updoor_B3，经 BPortal6/APortal6 到 Golem 并击败它、用指南针传回祭坛；
再从战前存档用其它种子重打（门与宝箱按存档恢复），覆盖两种攻击；bot 从宝箱顺手拿
番茄和一面盾牌，Boss 战中低血时进食，蓝火前摇起退到射程外。17 项检查全部通过，
同一种子两次运行的结果逐项一致。

`tools/playtest_systems.py`（19 项检查，逐项可复现）：新存档目录播放开场（音乐、祭坛被毁、
交还控制），reset 后跳过；把 F5 存档改成后期局面（背包里的宝石、树种、番茄，修复的祭坛，
别处的火祭坛，两座即将被入侵的岛）后 F9 读入；清掉巡逻队；风宝石后奔跑距离翻倍；草替换风
（风宝石弹出并被拾回）、5 秒回 5 HP；水替换草，挥剑射出水弹并扣法力；火在另一座祭坛上叠加；
点开/关闭图鉴；在祭坛旁点击种树并长成，祭坛回血从每秒 2 变为 6；C 岛弓箭手和炸弹手轰炸
祭坛、A 岛弓箭手先射树，并被击退；树与法力经 F5/reset/F9 保留；小地图在画面上；
全部音效请求都解析到文件（引擎新的 `audio.play`/`music.play` 事件）。

迁移时的适配（原作行为依赖未迁移内容或不可测）：

- 路线上只需宝箱 14、16、9 的钥匙；宝箱 15 和弓箭手的钥匙是原作的富余。
- 原作修复宝石只有 15% 掉率；任务所需的修复宝石与风宝石由起始岛巡逻队的第一只
  哥布林必掉（其余敌人仍是 15%、全局上限 5）。
- 盾牌在快捷栏时被动减半伤害，代替右键举盾（因此也不反弹蓝火）；玉米的回血在原代码中
  被注释掉，这里用其存储的 10。宝石用 F 放到 2.5 以内的祭坛上，代替原作在光标处丢出
  宝石、滑进祭坛触发器。
- 原作 `spawnedArcherThisWave` 从不重置，A、B 岛整局只会来一个弓箭手；这里按变量名的
  本意每波重置。原作的树加速因层名大小写不生效，这里按设计生效。
- 开场过场的士兵、巫师、剑士用导入的单帧精灵按轨迹移动（攻击为抖动、倒下为旋转淡出），
  哥布林用其动画；Timeline 中各 NPC 的逐帧动画片段没有逐一移植。小地图是烘焙的静态
  世界加动态标记，不是运行时第二台相机。

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
| headless agent 看不到游戏播放了什么声音，也看不出引用了不存在的音频（VR 音效） | `Audio.Play`/`Music.Play`/`Music.Halt` 作为 `audio.play`/`music.play`/`music.halt` 事件进入请求的事件流，含 clip 是否存在 |
| 判断两点是否连通只能逐对 `FindPath`，不可达的目标要泛洪整片区域；任务指引每开一扇门重算区域要数秒（VR） | `Physics.PathRegions(points, cell, radius, max_nodes)`：每个连通区域一次泛洪，按 FindPath 的格规则给一批点标区域号 |
| 起点在净空边距内时，A* 把边距格当作自由格，路径贴着墙走，盒形玩家卡在瓦片墙角（VR 路线上 B2 的门后） | 进入净空格的一步按四步计价，路径先离开边距再前进 |

（VR = Village_Rim，PT = PocketTerraria。）两个游戏都已改用这些能力，并重新通过各自的 playtest。

## 仍存在的缺口与暂不修改的原因

- **导入近似**：Unity Sprite 形状碰撞按整格；有缩放/旋转的 tilemap、九宫格精灵和 trigger 只记录在
  `import_report.json`。
- **Village_Rim 未迁移的内容**：Golem 之后的内容（它死后 `BridgeManager` 架出通往 D 岛的桥，
  D 岛的最终 Boss GoblinHighPriest，死后的春季融化 `MeltOnlySpringReveal`；导入场景里这座桥的
  瓦片现在一直显示，挡路的碰撞体也一直在）；B 世界的敌人与 Boss（Ghost、蘑菇、GoblinSwordSaint、
  骷髅王 GhostWizard 及其桥——那座桥的碰撞体是 3D BoxCollider，不影响 2D 通行，不在路线上）、
  魔树柱（摧毁后停止该岛入侵）与箭陷阱，以及 `TreePlantGuide` 等不在已知任务链上的 NPC 任务；
  原作的田地农场（`FarmPlot`）不在 Gameplay 场景中，故不迁移。原工程玩家出生点位于水面碰撞体内
  （运行时另有放置逻辑，未迁移）。
  幽灵墙（`GhostObstacle`，ghostborder 层与所有层碰撞）在原作里就是永久实心，已按实心瓦片导入。
  这些是内容移植，不是已知的引擎缺口。

## 性能

Release、macOS（Apple M5）：PocketTerraria 在 headless agent 模式下（带逐指令计数钩子）
从 Lua 光照时的 1.56 ms/帧降到原生光照后的 0.076 ms/帧，Village_Rim 约 0.12 ms/帧；
窗口模式被垂直同步锁在 60 Hz。
绘制排序键从 8 字节增加到 16 字节后，bunnymark 50k 交替 A/B 的结果落在噪声范围内
（前 17.9/16.0/21.1 ms，后 22.6/18.3/16.1 ms），W1 sprites 也在噪声内。
原始输出保留在被忽略的 `build/` 中。以上不是跨引擎对比。
