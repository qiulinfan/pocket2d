# 整游戏完整度测试：Village_Rim 迁移与 PocketTerraria

日期：2026-10-16。两个项目都在引擎仓库之外的 `~/Desktop/pocket2d-games/` 下，即私有仓库
`qiulinfan/pocket2d-games`（每个游戏一个目录，后续的游戏也放在这里）：`VillageRim` 迁移 Unity 版 Village_Rim 的真实世界（`pocket import unity` 导入），
`PocketTerraria` 用引擎从零做一个类泰拉瑞亚沙盒（Boss 只做克苏鲁之眼）。项目内的工具脚本默认在
同级的 `~/Desktop/pocket2d` 找引擎，`POCKET2D_ENGINE` 可改。
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
  炸弹飞向投掷时目标位置，0.8 秒引信（红圈），半径 2.5 内 20 伤害（举起的盾在爆炸范围内时 ×0.25）。
  每岛按 `SpawnRandomEnemy` 抽取预制件：每波至多一个弓箭手（A、B 岛只有弓箭手，每波一个；
  C 岛弓箭手与炸弹手）。B1 掉钥匙的 GoblinArcher_KeyDrop 是守点的弓箭手（40 HP）。
- `VRInventory`：原生 Inventory 的 30 格背包，前 9 格为快捷栏（1–9/滚轮），Tab 面板
  点击搬运；武器决定攻击力，食物回血。盾牌（`ShieldBehaviour`，冷却 0.5 秒）在快捷栏时右键朝光标
  举起 0.25 秒的 1.31×1.44 格挡框：0.25 秒完美弹反窗口内碰到的近战判定不掉血并使攻击者眩晕 1.5 秒
  （星星特效、"Perfect Block!"），之后为半伤的弱格挡；火球和剑气被反弹向施法者（1.2 倍速、改伤敌人、
  眩晕施法者）。
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
  `manualPosition`、巡逻队开始行动；每个存档目录只播放一次，Esc 可跳过。各角色、牧师的治疗特效和
  祭坛按 Timeline 的动画轨道逐帧显示（`tools/pack_opening.py` 解析 `OpeningTimeline.playable` 与各
  `.anim` 的精灵关键帧、Hold 外推和祭坛受击的红色渐变，生成 `VROpeningData.lua`）；Timeline 的
  wrap mode 是 None，结束时祭坛与特效恢复场景原样。
- `VRMinimap`：原作只启用的 MiniMapCameraA（中心 (−18.4, 271.3)、正交尺寸 63、
  256×256）视野由 `tools/pack_minimap.py` 按渲染顺序烘焙；右下角带边框，叠加玩家标记、
  Golem 图标和遭入侵岛屿的标记（B 世界里仍显示 A 世界，与原作一致）。
- `VRAudio`：A/B 世界、Boss 战和过场的音乐，挥剑、受伤、死亡、哥布林、祭坛毁/复、
  各元素宝石、拾取、进食、草回血、传送门、箭、炸弹、树、对话打字、盾牌、各 Boss 战等 39 段原音效
  （长 wav 音乐转为 160 kbps mp3）。
- `VRInvasion`：三座岛的祭坛（500 HP，每秒 +2）与出生点；到访且祭坛存活后按
  `max(10, base − wave×5)` 秒倒计时，生成 `base + wave×growth` 个入侵者；祭坛全毁
  则本岛入侵停止，不判负。
- `VRGolem`：200 HP，侦测 15、攻击距离 3、冷却 2 秒、前摇 0.8 秒，50/50 选择
  360° 砸地（20 伤害、半径 3）或三圈各 12 发蓝火（每发 10，直线速 7.5、精灵自转
  120°/s、9 帧 × 0.15 秒后消失，相隔 0.35 秒、每圈偏转 10°）；击败即胜利并存档。
- 存档：F5/F9 用 `Save.Write/Read` 存玩家、背包、祭坛与元素、岛屿入侵、任务与 NPC
  赠品、Boss、地上掉落、门与宝箱、树和法力、魔树、陷阱、桥、春季融化、已击杀的 Boss、支线任务和检查点
  （schema 4）；读档先完整校验，再重新加载场景并应用（入侵者不存，与原作一致）。死亡 2 秒后在
  最后碰到的检查点满血重生并存档（`PlayerHealth`/`CheckpointManager`）。
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
- `VREnemy` + `VRSpawns` + `VRHazards`：B 世界与 D 岛的敌人（新 `EnemyControl` 行为树的数据化移植：
  近距、视锥与射线视线侦测，前摇/判定/收招与冷却，中立怪受击后仇恨，保持距离的后撤）。
  幽灵（水弹 11/s、8 伤害，墙挡）、绿/蓝蘑菇（咬击并留下酸液，中毒 3 秒每秒 3、至多两层）、
  腐烂蘑菇王（半径 3 的全向攻击与三圈酸液）、骷髅王 GhostWizard（落点 0.5 秒的陨石，死后
  右到左淡入的木桥）、哥布林剑圣（近身热浪、火焰剑气，开战时光墙封住房间，死后掉钥匙）、
  哥布林大祭司（火球、四道旋转火龙卷与地火，玩家贴近时后撤；最终 Boss）。火祝福减半火焰伤害、
  免疫地火；草祝福免疫中毒。场景里放置的 Boss 被击杀后不再出现；BP1–BP3 房间进入时按
  `EnemySpawnTrigger` 生成、离开时清除。
- `VRPillars`：B、C 岛各一棵魔树（120 HP，B 世界里显示到魔树的距离和近处血条）；砍倒后该岛
  入侵停止、清场，原地出现奖励宝箱（水/草宝石与番茄）。A 岛没有魔树，从不被入侵。
- `VRTraps`：24 个箭陷阱，射线 20 内看到玩家就按房间速度射箭（冷却 1 秒）；斧头可以砍坏。
- `VRBridges` + `VREnding` + `VRReveal`：Golem 死后每 0.3 秒架出一段通往 D 岛的桥（桥的瓦片在此前
  隐藏、挡路碰撞体逐段移除）；大祭司死后滚动制作人员名单（任意键返回）并以玩家为中心逐圈
  显现春季的 D 岛（`MeltOnlySpringReveal`）。
- `VRAltarLooks`：祭坛元素地面（`Prefabs/AltarEffect`，`tools/pack_altar_effects.py` 导入为每元素的
  tilemap 模板与精灵），换元素时逐圈显现。
- `VRSideQuests`：NPC 支线（InvasionGuide 镜头展示魔树 5 秒、CompassUse、TreePlantGuide、
  UseShield、ShieldTesting），原文台词、提示和 NPC 头上的 "!"；A3start 后 Soldier 才开口。
- `VRField` + `VRFieldLab`：田地农场（`FarmPlot`/`CropGrowth`）只存在于原作较早的 A 世界预制件
  `WorldAPhase1`（Gameplay 的 A 世界是同一布局北移 259，没有田地），所以单独导入为
  `scenes/field_lab.scene`，用引擎新的 `game --scene field_lab` 启动。手持种子左键点 3 以内的耕地格
  种下（消耗一粒），番茄作物六阶段各 1 秒；点成熟作物或按 E 收获，按种子掉落番茄 1–2 / 玉米 1、
  树种无产出。原作祭坛给田地格加元素的 `ApplyAura` 被注释掉，所以总是中性作物；
  `FreezingCompleteEvent`/`InitCropEvent` 在任何场景都没有发布者，`FarmHighlight` 也从未被开启，
  这些不移植。

`tools/playtest.py` 只用真实输入和游戏观测，跟随任务指引完成：与 Priest 对话、
击杀携带宝石的哥布林并拾取、回报、用鼠标在背包里搬运物品、修复祭坛并放置风宝石、
确认 A 岛不被入侵、F5 存档 → `reset` 新 worker → F9 读档、APortal1/BPortal1 往返；
然后沿原作路线：B1 用鼠标开宝箱 14、把两把钥匙拖进快捷栏、选中钥匙点开两扇门，
到 B 岛找 SwordsMan（镜头展示魔树）、从 A1 宝箱 8 拿指南针、守住 B 岛一波入侵、用指南针
传回祭坛，B2 开宝箱 16 开 leftdoor_B2 并击败封门的剑圣（它掉的钥匙开 updoor_B3），到 C 岛听
Soldier 的话，经 BPortal6/APortal6 到 Golem 并击败它，看着桥逐段架起，过桥到 D 岛击败大祭司，
名单出现、春季显现、按任意键离开；再从 Golem 战前存档用其它种子重打（门与宝箱按存档恢复），
覆盖两种攻击。死亡时在检查点重生后继续。22 项检查全部通过，同一种子两次运行的结果逐项一致。

`tools/playtest_systems.py`（32 项检查，逐项可复现）：新存档目录播放开场（音乐、祭坛被毁、
交还控制），reset 后跳过；把 F5 存档改成后期局面（背包里的宝石、树种、番茄，修复的祭坛，
别处的火祭坛，两座即将被入侵的岛）后 F9 读入；清掉巡逻队；风宝石后奔跑距离翻倍；草替换风
（风宝石弹出并被拾回）、5 秒回 5 HP；水替换草，挥剑射出水弹并扣法力；火在另一座祭坛上叠加；
点开/关闭图鉴；在祭坛旁点击种树并长成，祭坛回血从每秒 2 变为 6，祭坛地面换成元素样式；C 岛
弓箭手和炸弹手轰炸祭坛、A 岛弓箭手先射树，并被击退；树与法力经 F5/reset/F9 保留；小地图在
画面上；全部音效请求都解析到文件（引擎新的 `audio.play`/`music.play` 事件）。之后改存档跳到
各场景：砍倒 B 岛魔树（入侵停止、奖励宝箱）、陷阱射中玩家、斧头砍坏陷阱；蘑菇咬出酸液并
中毒；幽灵射水弹；击败骷髅王（陨石、桥、记录击杀）；盾牌完美弹反剑圣的热浪（不掉血、
剑圣眩晕）并把大祭司的火球弹回伤到它；死亡后回到检查点满血。最后另起一个以 `--scene field_lab`
启动的运行时：种下番茄与玉米（够不到的格子种不下），作物成熟后点击与按 E 收获并拾起产出。

迁移时的适配（原作行为依赖未迁移内容或不可测）：

- 路线上需要宝箱 14、16 的钥匙和剑圣掉的钥匙（或宝箱 9 的）；宝箱 15 和弓箭手的钥匙是原作的富余。
- 原作修复宝石只有 15% 掉率；任务所需的修复宝石与风宝石由起始岛巡逻队的第一只
  哥布林必掉（其余敌人仍是 15%、全局上限 5）。
- ShieldTesting 在举盾（`ShieldUsedEvent`）时完成；Golem 的蓝火与幽灵水弹在原作中不是可反弹的
  `FireballProjectile`，盾牌也挡不住。玉米的回血在原代码中被注释掉，这里用其存储的 10。宝石用 F 放到 2.5 以内的祭坛上，代替原作在光标处丢出
  宝石、滑进祭坛触发器。
- 原作 `spawnedArcherThisWave` 从不重置，A、B 岛整局只会来一个弓箭手；这里按变量名的
  本意每波重置。原作的树加速因层名大小写不生效，这里按设计生效。
- 小地图是烘焙的静态世界加动态标记，不是运行时第二台相机（原作的相机只拍静态世界）。

## PocketTerraria

目标（2026-10-17 起）：粗略地完整复现原版泰拉瑞亚的进度，从第一棵树到月亮领主。
内容（方块、200 种物品、118 个配方、工作站、46 种敌人、boss、城镇 NPC 商店）集中在
`tools/content.py`，生成原生物品目录与 Lua 数据表。1000×400 世界：地表、地下、洞穴、地狱四层，
森林、雪原、沙漠、丛林（蜂巢、神庙）、腐化之地（裂谷、魔矿）、海滩、地牢；宝箱、生命水晶、
恶魔祭坛、岩浆。近战/远程/魔法武器与弹幕、盔甲套装与饰品、法力、钱币、存档读档；
数据驱动敌人（史莱姆、步行、飞行、蠕虫、施法五种 AI）按层、群系、昼夜刷新；
已实现史莱姆王和克苏鲁之眼，其余 boss 按里程碑推进（见项目 `AGENTS.md`）。

`tools/playtest.py` 只用真实输入完成开局（砍树、工作台、木剑、史莱姆、铜矿、方块与火把），
`tools/progression_test.py` 检查世界各地点、真实 UI 的合成链、每个 boss 用对应阶段装备
以真实输入击败并拿到掉落、以及存档后重开进程进度不丢。

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
| 导入器丢掉隐藏的 tilemap（VR 春季的 D 岛、Golem 的桥在原作中先隐藏） | 隐藏或渲染器关闭的 tilemap 以 `enabled: false` 导入，其道具精灵同样禁用，游戏运行时再启用 |
| 没有"任意键"输入（VR 名单的 "Press any key"） | `Input.GetAnyKeyDown()`：本帧任意键或鼠标键按下 |
| `FindPath` 的净空半径只避开碰撞体，不避开实心瓦片，路径贴着小数原点的瓦片边走（VR B2） | 净空半径内的实心瓦片格也算净空格（半径小于半格） |
| 盒形 mover 贴着瓦片面滑到拐角时，按命中法线忽略了相邻面的接触而滑进瓦片，进去后又被瓦片背面挡住出不来（VR B2 卡死） | 瓦片轮廓（chain）对盒形 mover 是单面的：只挡朝外侧面向内的运动，与 Box2D 接触和射线一致 |
| 场景重载后首帧约 1.8 秒（任务指引的区域泛洪），负载下超过 agent 请求的 5 秒期限（VR playtest） | 格搜索改用开放寻址的格表，占据查询每次只列一次有实心格的 tilemap；`path_benchmark` 中 PathRegions 80.5 → 10.4 ms、FindPath 11.6 → 1.3 ms，VR 重载首帧约 0.06 秒 |
| 每次 `Actor.Instantiate` 动画模板、每次加载 tilemap 都解码整张图集 PNG 只为检查尺寸（VR 房间刷怪、重载） | `ImageSize::Decoded` 按文件记住尺寸，文件大小或修改时间变化时才重新解码 |
| 只能从 game.config 的 `initial_scene` 启动，原作 Lab 式的独立场景无法直接运行或 playtest（VR 田地实验场景） | `game --scene NAME`（图形与 agent 模式，reset 后的 worker 继承），缺失的场景是启动错误 |
| 导入器直接丢弃未启用的实体（VR 旧剑圣、Altar1） | 未启用的实体与 tilemap 一样以 `enabled: false` 导入，场景与 Unity 逐对象对应 |
| 光照每格一块、最近邻放大、按固定量减光，地块逐格变暗且几格后突然全黑（PT） | `ComputeLight` 改为 RGB 乘法衰减（`air_decay`/`solid_decay`）、彩色光源、`ambient`，光照图线性采样并乘法混合到画面 |
| Tilemap 宽高上限 512，装不下泰拉瑞亚尺度的世界（PT 1000×400） | 宽高上限 4096、格子总数上限 4,194,304（区块化碰撞与绘制不受影响） |

（VR = Village_Rim，PT = PocketTerraria。）两个游戏都已改用这些能力，并重新通过各自的 playtest。

## 仍存在的缺口与暂不修改的原因

- **导入近似**：Unity Sprite 形状碰撞按整格；有缩放/旋转的 tilemap、九宫格精灵和 trigger 只记录在
  `import_report.json`。
- **Village_Rim 中原作自身就不可达的内容**：场景里未启用、也没有任何对象引用或激活的旧系统剑圣
  （以及 AltarWithHalo）按原样以禁用的 `SpawnMarker` 导入，与 Unity 中一样不出现；旧版 GoblinHighPriest
  与 EnemyGenerator 预制件不在 Gameplay 中。海面/陆地结冰（`GameControl`、`IceSpreadFromEdges`）与
  冰面行走在任何场景或预制件中都没有使用。原工程玩家出生点位于水面碰撞体内（运行时另有放置逻辑，
  未迁移）。幽灵墙（`GhostObstacle`，ghostborder 层与所有层碰撞）在原作里就是永久实心，已按实心瓦片
  导入。这些不是已知的引擎缺口。

## 性能

Release、macOS（Apple M5）：PocketTerraria 在 headless agent 模式下（带逐指令计数钩子）
从 Lua 光照时的 1.56 ms/帧降到原生光照后的 0.076 ms/帧，Village_Rim 约 0.12 ms/帧；
窗口模式被垂直同步锁在 60 Hz。`tests/path_benchmark`（100×100 房间网格、0.25 格、2 次预热、
9 次取中位数，结果摘要不变）：PathRegions 80.5 → 10.4 ms，FindPath 11.6 → 1.3 ms；Village_Rim
F9 重载加首帧从约 1.9 秒降到约 0.1 秒（测量时机器有无关负载）。
绘制排序键从 8 字节增加到 16 字节后，bunnymark 50k 交替 A/B 的结果落在噪声范围内
（前 17.9/16.0/21.1 ms，后 22.6/18.3/16.1 ms），W1 sprites 也在噪声内。
原始输出保留在被忽略的 `build/` 中。以上不是跨引擎对比。
