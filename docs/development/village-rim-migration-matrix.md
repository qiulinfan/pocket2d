# Village_Rim → Pocket2D 能力迁移矩阵

检查日期：2026-10-15。依据两个仓库当前工作树中的实现和测试代码；本文件同时记录已实现的功能切片和剩余迁移规划，不表示完整游戏已迁移。本矩阵的核对与更新只读取 Village_Rim，未在这项文档工作中复制或修改 Unity 场景、资源、脚本或存档。

Pocket2D 的目标栈和总体范围见 [roadmap.md](roadmap.md)。当前 defense fixture 证明移动、拾取、三波防守、摧毁入侵来源和失败这几个规则可经过真实 Lua/Box2D 运行；新增 tilemap_farm fixture 已实现原生双层地图、4×4 种收、拆墙通行、编辑草稿和 agent cell 查询。`two_world_village` 进一步把种植、原生库存/装备、真实伤害、同场景双区域门户、持续入侵与永久来源关闭接成一个可保存恢复的闭环，已通过 Debug、Release 和 ASan/UBSan 自动化验证。这些都是自制内容的功能切片，不具备原游戏全部内容。代码能力、自动化验证和人类可玩性分别判断，不能互相替代。

文内 `U:` 路径相对于同级 `Village_Rim` 仓库，`P:` 路径相对于本仓库。优先级含义：**P0** 为有代表性的游戏迁移所需基础；**P1** 为基础之后的可运行扩展；**P2** 为完整内容、编辑效率或平台体验的后续补齐。它们不是本次已承诺实现的清单。

## 能力与具体缺口

| 能力 | Unity 实现证据 | Pocket2D 当前实现 | 应有验证场景 | 缺口与优先级 |
| --- | --- | --- | --- | --- |
| 格子地图、坐标与分层 | `U:Assets/Scenes/Gameplay.unity` 的文本记录包含 32 个 `Tilemap`、28 个 `TilemapRenderer`、1 个 `Grid`；`U:Assets/Scripts/Farm/FarmPlot.cs` 使用 `WorldToCell`、`HasTile`、`GetCellCenterWorld`。这是序列化记录计数，不是运行时可见地图数。 | 已有 `P:include/engine/scene/Tilemap.h`、`P:src/engine/scene/Tilemap.cpp` 和 `P:src/shared/tilemap/TilemapAsset.cpp`：版本1资源、ground/solid 两层、规则 atlas、floor 坐标、Get/SetCell、32×32 dirty chunk 和可见区域裁剪。`P:tests/fixtures/tilemap_farm` 为64×64实际场景，格子不单独生成 actor。 | `P:tests/tilemap_test.cpp`、`P:tests/tilemap_runtime_test.cpp` 和 `P:tests/agent_tilemap_test.py` 覆盖边界、排队提交、实际种收和查询；渲染测试另覆盖 tile 像素与裁剪。 | **基础切片已实现；P1/P2**：完整 Unity 地图内容、更多图层语义、旋转/父级变换、非规则图块和导入仍未覆盖。当前组件只支持 root、rotation=0、ground/solid 两层，不称全部 Tilemap 功能对等。 |
| 地形碰撞与可变地表 | `U:Assets/Scenes/Gameplay.unity` 有 11 个 `TilemapCollider2D` 记录；`U:Assets/Scripts/Environment/WaterSpreadEffect.cs` 保留、修改和恢复 TileBase 数据，并区分装饰 tilemap 的 collider。 | `P:src/engine/scene/Tilemap.cpp` 已从 solid 非空格子按行合并矩形 fixtures，按 chunk 管理 static Box2D bodies；ground 不生成碰撞，dirty 提交在安全物理边界进行。 | 真实玩家先被墙挡住，移除目标 cell 后通过；callback 内编辑延至下一 tick；disable/enable、删除和过期引用不遗留/复活碰撞，有 runtime 回归。 | **基础切片已实现；P1**：原关卡碰撞轮廓、材料、非整格形状和大范围环境变更仍需逐项核对；没有证明所有 Unity TilemapCollider2D 都可等价成整格矩形。Box2D mutation 仍属于主线程。 |
| 可耕种格子、树木与元素地面状态 | `U:Assets/Scripts/Farm/FieldAttributeMap.cs` 按 cell 记录来源、元素、过期时间与最后应用时间；`Tree/TreeFarm.cs` 排除 path/decoration 层；`CropGrowth.cs` 管理生长阶段和可重复采收。 | 原生 Tilemap 的16格种收已有独立 fixture；`P:tests/fixtures/two_world_village/component_types/FarmRules.lua` 现将种植接到原生背包：成功时消耗种子，120 tick成熟，采收生成固定池地面食物余量，按 Inventory.Add 实际接收量扣减。敌人会优先破坏作物。Lua 作物状态是存档权威，地图修改排队提交。 | `tilemap_runtime_test.cpp` / `agent_tilemap_test.py` 保留底层时序与 reset 回归；`agent_two_world_village_test.py` 的三配置流程验证种收、无种子拒绝、满包余量保留、作物→祭坛入侵路径与恢复。 | **种收/库存/存档整合切片已实现；P1**：树木、再生品种、元素来源叠加/过期、冻结融化和原作物全集仍缺。固定16格与120 tick是本切片参数，不代表已核实全部 Unity 序列化调参。 |
| Sprite animation 与动作状态 | `U:Assets/Scripts/Player/Movement_and_Attack/PlayerAnimator.cs` 实际加载 Idle/Walk/Run/Sword/Pickaxe/Damage/Dead，三行 down/up/right，左向镜像右向；循环和一次性动作使用不同路径。`U:Assets/Scripts/EnemyNew/EnemyAnimation.cs` 有帧计时、方向、一次播放和伤害闪烁。 | 已有 `P:src/engine/rendering/SpriteAnimation.cpp`、`P:src/shared/animation/SpriteAnimationAsset.cpp`：共享不可变clip、整数tick、循环/一次播放、Play幂等/Restart、暂停/停止、帧marker与完成事件。SpriteRenderer临时pose保留作者配置、镜像、pivot、tint。`P:tests/fixtures/sprite_animation` 用自制PNG实现idle/walk/attack返回idle。 | `P:tests/sprite_animation_test.cpp`、`sprite_animation_runtime_test.cpp`、`agent_animation_test.py` 覆盖精确tick/输入/replay/事件重入/代际失效；`sprite_animation_render_test.cpp` 验证真实atlas像素、镜像、透明度和缩放；getter重入另有真实worker回归。 | **基础播放器已实现；P1/P2**：完整四向动作资源、受击/死亡/工具状态、动画时间线编辑器、Unity clip导入及生产手感仍缺。自制测试图不等于已迁移原游戏角色或获准导入八向美术。 |
| Sprite 尺寸、图集与像素表现 | Unity `SpriteRenderer`、Sprite 导入器和 `.meta` 保存切片/pivot/PPU；PlayerAnimator 的载入结果依赖 Unity 的子资源顺序。 | `P:src/engine/rendering/SpriteRenderer.cpp` 支持 pivot、缩放、选格；`P:src/engine/rendering/Renderer.cpp` 使用 100 pixels/world-unit，当前加载纹理已显式 nearest。普通Sprite的规则图集元数据在 `.pocketengine/spritesheets.json`；Tilemap资源自行保存经尺寸校验的atlas网格，并按cell_size映射world尺寸。 | 同一自制 atlas 的每格边界、pivot、负缩放、透明边缘与 pixel readback；匹配 world size 后测 1×/2×/非整数 zoom。 | **P0**：明确可移植的 source rectangle、native pixels、world size/pivot 数据。不能把 Unity GUID、Sprite 子资源索引或 PPU 隐式当作 Pocket2D 文件名/100 PPU。非规则切片与导入工具列 **P1**。 |
| 玩家、物品、箱子与装备存档 | `U:Assets/Scripts/Utils/InventorySaveManager.cs`、`ChestSaveManager.cs` 和 `AnimationSaveManager.cs` 保存物品、快捷栏、装备及玩家状态；源码默认30格背包、9个快捷引用、4装备位。 | 原生 `ItemCatalog` / `InventoryModel`、Lua `Inventory.Open` 容器和 `RestoreGroup` 已接 `Save`。`inventory_village` 保存30格背包、12格箱子、4装备、9键映射、实例ID、生命/位置、世界拾取余量和消耗；食物回血与装备属性实际生效。 `two_world_village` 另将30格背包与4装备位接入种收和战斗：食物回血，唯一剑实例装备后增加实际伤害，两个容器一起 RestoreGroup。 | `item_catalog_test.cpp`、`inventory_model_test.cpp`、`inventory_lua_test.cpp`、`agent_inventory_test.py` 覆盖容量、守恒、过滤、重复实例、后部字段错误不部分提交、真实新进程恢复；渲染测试检查实际装备反馈。 `agent_two_world_village_test.py` 已在 Debug、Release 和 ASan/UBSan 验证徒手与持剑击杀差异及整合后的跨进程恢复。 | **基础库存及农场/战斗整合切片已实现；P1**：原游戏物品全集、完整掉落/制作、箱子与双世界玩法的进一步整合、Unity存档转换尚未迁移。独立实例由游戏分配，事务检查参与容器内唯一性；不是全局物品注册中心或任意Lua堆checkpoint。 |
| 世界永久进度与中途恢复 | `U:Assets/Scripts/Utils/AltarSaveManager.cs` 保存祭坛生命/元素/发现与 pillarAlive；`BossSaveManager.cs` 保存 boss ID/killed 与融雪标志；`U:Assets/Scripts/World/TreeSaveManager.cs` 保存 cell、seed ID、stage、timeInStage。`U:Assets/Scripts/UI/PauseMenuController.cs` 在回主菜单前协调多个保存器。 | 已有显式数据 Save API；`persistent_village` 保存拆墙、作物、玩家和永久灌溉升级。`two_world_village` 现保存双区域玩家位置/生命、原生背包/装备、作物剩余时间/地面余量、祭坛生命、守卫、波次/倒计时与来源关闭；摧毁来源取消当前压力并阻止后续入侵，新进程恢复后仍关闭。 | `agent_two_world_village_test.py` 的 Debug 验证覆盖完整闭环、跨进程恢复、失败检查点恢复、活动入侵取消、busy保存保留旧文件、损坏/非法档拒绝且冻结输入边界后的游戏状态不变。 | **单路线双区域永久进度已实现；P1**：多岛发现/配对、多祭坛元素状态、boss/融雪、完整战斗中途快照和原存档转换仍缺。当前只在稳定边界保存；活动波次和门户过渡不是可保存状态，速度/输入锁存/绝对时钟不恢复。 |
| 文件定位、资源目录与流式加载 | PlayerAnimator/EnemyAnimation 使用 `Resources.LoadAll<Sprite>`；`U:Assets/Scripts/Camera/MusicController.cs` 有全目录 AudioClip 扫描；`TransportPortal.cs` 是场景内位置传送和 camera warp。已读的这些实现不证明 Unity 工程具备真正的区域资源流式加载。 | `P:include/shared/resources/ResourcePath.h::ResolveResourcePath` 先直接定位，失败后递归扫描并按优先目录选文件；`P:src/engine/rendering/Renderer.cpp::LoadTextureResource` 首次使用同步加载并缓存；image cache 有全量 clear，没有字节预算/LRU/异步区域状态。Tilemap资源使用明确相对路径并预载验证，地图全驻留；32×32 chunk是裁剪/重建单位，不是流式区域。 | 多区域往返、同名文件明确消歧、冷/热首次显示延迟、资源加载失败、离开区域后内存回落、加载完成前占位和取消语义。 | **P1**：先以清单/明确相对路径避免重复目录扫描，再根据真实 residency 数据决定异步读取、CPU 解码与主线程上传。TaskExecutor 只证明 CPU 粒子更新，未实现流式资源系统。无需为了“对齐”引入 Unity Addressables。 |
| 输入动作、控制权和屏幕到世界坐标 | `U:Assets/InputActions/Players.inputactions` 与 `GamePlayInputManager.cs` 有 Move/Run/Weapon/Inventory/DropItem/Dialogue、暂停和对话阻断、鼠标瞄准；`InventoryDragService.cs` 使用 UI raycast。 | `P:src/engine/input/Input.cpp` 和 Lua InputAPI 提供键鼠 held/down/up/wheel；`P:src/app/runtime/AgentSession.cpp` 支持同一路径的键鼠注入和有界只读tilemaps cell查询；editor overlay 有 viewport 坐标映射。Tilemap已有world/cell转换，fixture用脚下格子交互。没有游戏用动作映射、可重绑配置或控制器层。 | 打开背包期间鼠标点击不攻击；关闭 UI 不遗留按键；camera/zoom/Retina 下点选相同世界 cell；对话暂停后恢复；物理与 agent 输入得到相同结果。 | **P0/P1**：先定游戏输入上下文和屏幕/世界转换 API，避免每个 Lua 脚本复制坐标公式。动作重绑、手柄导航与多设备配置为后续 **P1/P2**。 |
| 游戏 UI、物品拖放与文字 | `U:Assets/Scripts/Inventory/HotbarController.cs`、`SlotWidget.cs`、`InventoryDragService.cs` 和 `PauseMenuController.cs` 使用UGUI/TMP。 | `inventory_village` 已有固定960×720真实库存UI：鼠标整栈/半栈拖放、取消、源revision检查、9键快捷引用、键盘focus/移动/确认、食物/装备反馈；使用原有Image.DrawUI与自制位图字体。 | `agent_inventory_test.py` 同一路径键鼠输入验证合并/交换/拆分/取消/非法装备/余量与存档；`inventory_render_test.cpp` 检查软件、Metal、OpenGL实际像素和拖放。 | **玩法切片已实现；P1/P2**：尚无通用widget、响应式布局、可重绑动作、文本本地化/IME或完整UGUI对等。Move与TransferAll是独立事务，不等价于Unity PlaceSmart的先补全目标栈再点击交换。人类可用性仍需playtest。 |
| 世界切换与镜头反馈 | `U:Assets/Scripts/World/TransportPortal.cs` 有配对 portal、到达冷却、方向偏移、淡出、Cinemachine warp、世界事件；`DevilTree/WorldManager.cs` 切换两世界引导 UI。 | `P:tests/fixtures/two_world_village` 已实现同一 scene 内两个常驻区域、成对门户、42 tick淡出/黑屏/淡入、到达保护与离开触发区后重新武装；切换玩家位置、输入锁和镜头。村庄倒计时与入侵在地下访问期间继续，世界状态纳入同一检查点。门户不调用 Scene.Load。 | `agent_two_world_village_test.py` 已在 Debug、Release 和 ASan/UBSan 验证往返、持续停留防重复传送、缺失配对端点拒绝、过渡期间失败后恢复及跨进程世界恢复；`two_world_render_test.cpp` 的 software、Metal、OpenGL 路径检查实际画面。 | **单岛配对门户切片已实现；P1/P2**：多岛路线、发现/解锁引导、完整原作世界事件与镜头手感仍缺；常驻双区域不证明异步流式加载。人类需验收过渡节奏、战斗可读性和移动反馈。 |
| 编辑、导入和资产可追溯性 | Unity scenes/prefabs/ScriptableObjects、`.meta` GUID、InputAction 与各类 Inspector 构成当前作者工作流。 | `P:src/editor/documents/SceneDocument.cpp`、`SceneMutations.cpp` 和各 panel 已有 scene/template/component 编辑；新增 `P:src/editor/documents/TilemapDocument.cpp` 负责地图作者副本、数值编辑、undo/redo、显式原子保存、磁盘Revert与纯内存Discard。Overlay按资源匹配预览，保留无map场景下的全局草稿入口。 | `P:tests/tilemap_editor_test.cpp` 与 `P:tests/tilemap_overlay_test.cpp` 使用真实ImGui/Overlay验证绑定、dirty切换、预览、same-UID新代际重载、源文件损坏/删除时的草稿恢复；不是人类编辑效率验收。 | **地图作者工具第一片已实现；P1/P2**：画笔/自动拼接、clip/物品编辑、重命名引用追踪、Unity importer和完整资产转换清单仍缺。迁移只选择明确获准的资产子集并保留来源/许可；本矩阵不授权复制。 |

**真实世界导入（2026-10-16）。** `~/Desktop/pocket2d-games/VillageRim` 用 Unity YAML 导入器导出了 Gameplay 场景的
tilemap、精灵、碰撞体和实体占位（隐藏的 tilemap 以禁用状态导入），并在其上移植了从开场到结局的
整条游戏：任务链与 NPC 支线、元素祭坛与种树、三岛入侵与魔树、B 世界上锁房间的岛间路线、箭陷阱、
B 世界敌人与四个 Boss、Golem 与通往 D 岛的桥、最终 Boss、名单与春季融化、主动盾牌、检查点与存档；
两个 agent playtest 用真实输入走完整条路线（22 项）并覆盖各系统（30 项）。
发现的引擎缺口与修复见 [game-completeness-tests.md](game-completeness-tests.md)。
该项目不在引擎仓库中，原游戏资产不随引擎分发。

## 已执行的第一步与下一可运行切片

本矩阵最初选择“原生64×64双层Tilemap + 可修改格子”作为下一步，完整工程合同保留在 [tilemap-slice.md](tilemap-slice.md)。本轮已按该方向落地数据/API、ground-solid碰撞、可见区域提交、4×4种收、作者编辑草稿和agent查询；实际fixture独立放在 `tests/fixtures/tilemap_farm/`，没有重写原defense fixture，也没有导入Unity内容。

现有校验含真实Lua/Box2D、software/Metal图形路径与编辑器Overlay；人类仍需判断选格反馈、输入手感和作者工具的使用效率。原计划中的“地图全驻留，不称流式”“不建立新ECS”“不做全套Unity importer”边界继续有效。完整迁移仍以表格中的未完成内容为准。

**共享 SpriteAnimation clip 的基础切片已实现。** 自制角色已通过idle/walk循环、一次性attack、暂停和完成事件的真实headless/图形验证；Lua API见 [SpriteAnimation](../lua-api/sprite-animation.md)，测试玩法见 [playtest](sprite-animation-playtest.md)。1k/10k headless更新对照见 [性能边界](animation-performance.md)，当前结果不证明原生比简单Lua循环更快。完整动作内容和时间线工具仍是后续能力。

**版本化游戏存档的基础切片已实现。** [Save API](../lua-api/savegame.md) 提供独立命名空间、schema版本和原子替换；[persistent_village合同](../../tests/fixtures/persistent_village/CONTRACT.md) 列出已跨进程恢复的地图/作物/玩家/灌溉字段。后续inventory_village已补原生库存、装备、快捷栏、实例ID和拾取余量的真实保存。类型化接口与可选TS→Lua前端也已实现基础切片。农场、战斗与单路线双区域状态现由下述整合切片连通；原游戏物品全集、多岛祭坛/恶魔树、元素与boss内容仍未完整迁移。

**双世界玩法整合切片已实现并通过三配置验证。** [two_world_village 合同](../../tests/fixtures/two_world_village/CONTRACT.md) 定义固定池、原生容器、门户、入侵与 schema 1；[来源规则和整合计划](two-world-integration-plan.md) 区分原源码默认行为、尚未核实的序列化调参与单岛切片假设。玩家能种植并采收食物、装备剑提高实际攻击伤害、经同场景配对门户进入地下、击败守卫并永久关闭入侵来源，再保存并在新进程恢复。清空一波仅开启下一倒计时，不冒充永久胜利。

本轮证据来自 `tests/agent_two_world_village_test.py` 的实际 Engine/Lua/Box2D 输入流程及 `tests/two_world_render_test.cpp` 的 software、Metal、OpenGL 渲染检查；已覆盖满包产出保留、无种子、伤害差异、作物优先/祭坛后续攻击、端点缺失、传送防抖、活动入侵取消、失败恢复与存档拒绝。完整72项CTest已在Debug、Release、ASan/UBSan通过；最终证据见 [Phase 15](phase15-checkpoint.md)。F5/F9 会先停止动态物体速度再返回，普通失败不改变该冻结输入边界之后的游戏模型；这不等于回滚整帧物理或任意 Lua 堆。人工试玩尚未完成，操作手感、伤害与门户反馈、节奏和难度仍待验收；本片不覆盖多岛、元素/boss、旧 Unity 存档和原美术迁移。

## 可比较的性能 workload

以下数量是拟定的**合成测试参数**，不是从 Unity 场景测得的运行规模。已有 `tests/tilemap_benchmark.cpp` 提供真实SDL3 render/present、640×360、ground-only/无物理、fixed/scrolling/edit1/edit64的本引擎对照，并检查两条路径的最终像素相同；下表的更多层、1080p和混合负载仍是扩展目标。生成器、随机seed、atlas、输入轨迹与正确性断言都应固定后再比较。

| Workload | 固定输入和工作量 | 正确性门槛 | 记录的指标 |
| --- | --- | --- | --- |
| Tilemap 静态/滚动/局部修改 | 256²、512² 格，2层当前数据合同；4层是后续扩展目标，16×16 native tile，固定 1920×1080 输出和世界视野；同一 camera 轨迹；每 tick 修改 0/1/64 格；分别全填充与稀疏布局。 | 可见 tile 集合与层次相同；裁剪边缘无漏格；最后的 tile 数据一致。 | 主线程准备/提交 CPU 时间、GPU 时间（有可靠计时才报）、draw/visible cell/dirty chunk 数、p50/p95/p99 frame、显存和 RSS。总地图变大而视野不变应单独观察 scaling。 |
| Animated actors | 1K/10K sprites，同一 atlas、四向与镜像、idle/walk/一次 attack，预先规定相同 clip 帧时长和状态切换脚本。纯动画更新与包含绘制分别测。 | 每 tick 的 clip/cell、结束事件计数和稳定排序相同；暂停/恢复一致。 | 动画 CPU、Lua 调用数、分配量、绘制提交/纹理切换、整帧分布。不能把 Pocket 静态图对比 Unity 动画控制器的不同工作量。 |
| 存档与恢复 | 1K/10K 作物、100/1K slot、8 祭坛、固定世界标志；数据 schema 与内容一致，使用独立临时目录。 | 新进程加载后字段与约定增长阶段一致；文件截断、磁盘写入失败不覆盖最后有效档；未知版本明确拒绝或有已定义策略。 | 序列化、写入、读取、反序列化、actor/physics 恢复分别计时；文件字节数、峰值 RSS、加载后首次可操作延迟。 |
| 区域资源驻留 | 相同图片格式/分辨率与总字节，固定区域访问序列；小于/大于驻留预算两组；纹理上传和解码分开。 | 离开区域释放约定资源；重复进入不拿错同名图；取消加载不使新场景接收旧结果。 | 首次可见 p95/p99、主线程最长停顿、磁盘读取/解码/上传字节、cache hit、驻留内存。新进程仅是进程冷启动，不等价于清空 OS 文件缓存。 |
| 交互 UI | 源码默认9-slot hotbar、30-slot 背包、4装备位与箱子；100-slot背包另列压力测试，使用同一拖放脚本；固定字体/字号、100%/200% DPI 与窗口尺寸；每 tick 0/20 项变化。 | 物品总量守恒、取消拖动可恢复、焦点不触发战斗、鼠标坐标在不同 DPI 下命中同一控件。 | input→状态→可见反馈延迟、布局/文本更新 CPU、文本 cache miss、分配和 frame 分布。固定 bitmap HUD 不能代替动态库存 UI 做对比。 |
| 混合村庄 | 固定 tilemap + 200 作物 + 100 移动敌人 + 1K/16K 粒子 + HUD，固定 60 Hz 输入轨迹；分别有/无资源边界切换。 | 同一胜负、击杀/收获/碰撞数量、地图最终状态；同时检查缺失资产和 Lua 错误数。 | 整帧 p50/p95/p99、超预算帧数、CPU/GPU/RSS、各子系统耗时；独立 microbenchmark 收益是否仍在混合负载成立。 |

对照必须固定机器、系统、电源状态、编译优化、分辨率/Retina 输出、VSync、可见数量、碰撞设置、过滤与视觉内容。先预热，再用相同采样时长记录分布；吞吐测试关闭限帧，交互测试另外按 60 Hz 检查延迟。Pocket2D 单线程/多线程和修改前/后的 Release 构建是第一组对照；之后再建立功能等价的 Unity/其他引擎基准场景。

本次没有执行 Unity 性能测试，也没有建立跨引擎比较。原项目当下的授权/可运行状态必须在实际基准时再核实。原始结果、截图、trace 和 checksum 输出保留在 ignored `build/` 或 CI artifact；本文件只保存稳定 workload 和验收定义，不保存一轮运行日志。已有层级查询和粒子局部加速不能外推为整引擎领先。

## 当前检查边界

已直接检查两仓库上表所列源文件、Pocket2D 当前 API 注册与 scene/资源格式、现有测试和 Unity Gameplay 的相关序列化类型记录。没有对全部 Unity prefab/ScriptableObject 内容完成审计，没有建立全部 item/quest/animation 的转换清单，也没有读取或转换用户实际游戏存档。后续逐系统迁移仍需补齐这些具体内容与授权资产范围。
