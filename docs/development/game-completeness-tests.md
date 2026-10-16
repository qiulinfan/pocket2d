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

| 问题 | 修复 |
| --- | --- |
| `auto_sorting_order` 用 `int(y)` 取代 sorting_order，丢失图层且 1 格粒度 | 绘制请求增加 `sort_y` 次键：先 sorting_order，再 y，再提交顺序 |
| 相邻 tile 在浮点下各自取整，镜头偏移时出现 1 像素缝 | 无旋转绘制在输出像素空间吸附两侧边缘；再加 1e-3 px 偏置，避免 0.27 这类不可精确表示的缩放在回乘时被截断 |
| `Physics.MoveCharacter` / `Rigidbody:SetPosition` 只移动 Box2D body，非动态刚体在下一物理步被 Transform 拉回 | 同时写回所属 Transform（换算到父级局部坐标）；physics_api 测试覆盖跨步保持 |

## 仍存在的缺口

- **动画资源**只能引用一张图；Unity 那种每个动作一张表需要工具打包。
- **寻路**：没有 navmesh/网格寻路，敌人直线追击并贴着碰撞体滑动。
- **2D 光照**：没有光照系统。Village_Rim B 世界用 UI 叠加色调模拟；
  PocketTerraria 在 Lua 中计算光照网格，每帧约 1,450 次叠加绘制。
- **Box2D mover 只有胶囊体**：方块平台需要方形碰撞，PocketTerraria 改用 Lua 格子碰撞。
- **Tilemap 只有 ground/solid 两层**：Village_Rim 的 49 个 Unity tilemap 变成 49 个 actor；
  泰拉瑞亚的背景墙和树共用 ground 层；Unity Sprite 形状碰撞近似为整格；
  有缩放/旋转的 tilemap（2 个）被忽略。
- **UI 图片不能缩放，也不能指定源矩形**：HUD 美术预先放大 2 倍，血条用 4 px 小段拼接。
- **屏幕→世界坐标**：没有窗口尺寸或坐标换算 API，脚本只能硬编码 1280×720 与缩放来换算。
- **物品目录**的 kind 固定（material/food/weapon/armor），没有工具/放置物/消耗品类型，也没有图标；游戏只能自己维护对照表。
- **新实例化的 actor** 下一帧才执行 OnStart，同一帧内读到的字段为空；agent 观测也是空值。
- **SetCell 要到提交后才对 GetCell 可见**：游戏启动时用 176k 次 GetCell 建立 Lua 镜像。
- **agent 协议**：`step` 不支持 tilemaps 查询（只有 `observe` 支持）；按请求计算的指令预算
  会限制一次 step 的帧数（PocketTerraria 每帧约 8 万条 Lua 指令，600 帧超出默认 5,000 万上限）。
- **Village_Rim 未迁移**：对话/任务、背包与快捷栏 UI、祭坛与元素、入侵、Boss、农场、存档、音频；
  行为树（Behavior Designer）；原工程玩家出生点位于水面碰撞体内（运行时另有放置逻辑，未迁移）。

## 性能

Release、macOS（Apple M5）：PocketTerraria 在 headless agent 模式下（带逐指令计数钩子）
约 1.56 ms/帧，Village_Rim 约 0.12 ms/帧；窗口模式被垂直同步锁在 60 Hz。
绘制排序键从 8 字节增加到 16 字节后，bunnymark 50k 交替 A/B 的结果落在噪声范围内
（前 17.9/16.0/21.1 ms，后 22.6/18.3/16.1 ms），W1 sprites 也在噪声内。
原始输出保留在被忽略的 `build/` 中。以上不是跨引擎对比。
