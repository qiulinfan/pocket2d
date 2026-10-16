# 整游戏完整度测试：Village_Rim 迁移与 PocketTerraria

日期：2026-10-16。两个项目都在 `Projects/` 下（引擎仓库忽略，各自是本地 Git 仓库，
不推送）：`Projects/VillageRim` 迁移 Unity 版 Village_Rim 的真实世界，
`Projects/PocketTerraria` 用引擎从零做一个类泰拉瑞亚沙盒（Boss 只做克苏鲁之眼）。
目的不是交付完整游戏，而是用真实内容找出引擎缺口。两者都有通过 agent 协议、
用真实输入（动作、按键、鼠标）游玩的 playtest。

## Village_Rim 迁移

`tools/import_unity_world.py` 读取 `Gameplay.unity` 及其中所有 prefab 实例
（含 prefab 修改项、stripped 对象别名），导出 49 个 tilemap（53,081 格，按每个
Tile 资产的 `m_ColliderType` 决定 solid/ground）、189 个精灵、111 个静态碰撞体
（box/circle/capsule）和 10 个实体 `SpawnMarker`（带非静态 Rigidbody2D 的对象）。
Unity sorting layer 映射为 `sorting_order = 层序号 * 100000 + order`，全部精灵
按 y 排序（与原工程 2D Renderer 的自定义排序轴一致）。`tools/pack_characters.py`
把每个动作一张的角色表打包成一张图集和一个 animation 资源。

手写玩法（`VillageGame`、`VRPlayer`、`VRGoblin`）移植了移动/奔跑/体力、
剑击（命中帧）、受伤无敌帧、哥布林侦测/追击/前摇攻击/受击/死亡、A/B 世界配对
传送门（淡入淡出）、镜头与 HUD。`tools/playtest.py`：3 只哥布林在 406 帧内被击败，
随后经传送门对 1 进入 B 世界。

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

（VR = Village_Rim，PT = PocketTerraria。）两个游戏都已改用这些能力，并重新通过各自的 playtest。

## 仍存在的缺口

- **Tilemap 只有 ground/solid 两层**：Village_Rim 的 49 个 Unity tilemap 对应 49 个 actor；
  泰拉瑞亚的背景墙和树干共用 ground 层。Unity Sprite 形状碰撞近似为整格；有缩放/旋转的
  tilemap（2 个）被忽略。
- **新实例化的 actor 下一帧才 OnStart**：同一帧内 OnStart 设置的字段为空，agent 观测也为空值。
  这是现有生命周期设计（类似 Unity 的 Start），脚本需在使用前检查。
- **agent 指令预算按请求计算**：长 step 需拆分或用 `--instruction-budget` 调大；属于设计内的隔离。
- **Unity 导入器位于 Village_Rim 项目内**（`tools/import_unity_world.py`），尚未成为 `pocket import unity`。
- **Village_Rim 未迁移**：对话/任务、背包与快捷栏 UI、祭坛与元素、入侵、Boss、农场、存档、音频；
  原工程玩家出生点位于水面碰撞体内（运行时另有放置逻辑，未迁移）。

## 性能

Release、macOS（Apple M5）：PocketTerraria 在 headless agent 模式下（带逐指令计数钩子）
从 Lua 光照时的 1.56 ms/帧降到原生光照后的 0.076 ms/帧，Village_Rim 约 0.12 ms/帧；
窗口模式被垂直同步锁在 60 Hz。
绘制排序键从 8 字节增加到 16 字节后，bunnymark 50k 交替 A/B 的结果落在噪声范围内
（前 17.9/16.0/21.1 ms，后 22.6/18.3/16.1 ms），W1 sprites 也在噪声内。
原始输出保留在被忽略的 `build/` 中。以上不是跨引擎对比。
