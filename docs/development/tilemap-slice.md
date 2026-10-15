# Native Tilemap：64×64 双层可运行切片

本规格承接 [迁移矩阵](village-rim-migration-matrix.md)，定义本轮已实现的原生引擎增量与验收契约；当前验证结果见 [开发状态](status.md)，实际调用见 [Tilemap API](../lua-api/tilemap.md)。验收项目使用自制几何 atlas，没有导入 Village_Rim 资产，原 defense fixture 保持原规则。

## 交付范围

一个原生 `Tilemap` component，承载 64×64 ground/solid 两层；规则 atlas；世界/cell 查询和有界修改；32×32 dirty chunk；可见区域裁剪；Box2D 静态墙；场景引用和最小数值编辑/保存；真实键盘驱动的 4×4 种收与拆墙测试。Lua、SDL、Box2D 仍在拥有线程。此切片全驻留，不做异步流式、自动拼接、多边形 tile、旋转地图、Unity importer、通用存档或新 ECS。

复用现有 `ActorHandle`、`Transform`、component 生命周期、`SceneFormat` 引用、`ImageDrawRequest` 图集选格、Renderer nearest/排序、Box2D world/fixture metadata、JSONL step/observe 和 CTest。地图格子不是 actor，也不各自运行 Lua OnUpdate。

## 资源与 component 契约

新资源放在项目 `maps/`，扩展名 `.tilemap.json`。Scene 中仅记录相对于 maps 的明确路径，不使用同名递归搜索：

```json
{
  "uid": 10,
  "name": "terrain",
  "components": {
    "map": {"type": "Tilemap", "map_resource": "farm.tilemap.json", "enabled": true},
    "transform": {"type": "Transform", "x": -8, "y": -8, "rotation": 0}
  }
}
```

Tilemap actor 必须是 root，world rotation 必须为 0，不允许同一 actor 再有 Rigidbody 或第二个 Tilemap。先支持平移；editor 移动 root 时，显示、cell 转换和静态 body 在同一安全提交点更新。违反这些限制的资源/场景载入应报告 actor UID、component key 和原因，不静默忽略旋转或套一层兼容实现。

地图 schema 如下。为可读性展示一份**合法 4×4 小测试资源**；64×64 fixture 使用同 schema 和每层 4096 个值：

```json
{
  "version": 1,
  "width": 4,
  "height": 4,
  "cell_size": 1.0,
  "atlas": {
    "image": "tiles/farm.bmp",
    "columns": 4,
    "rows": 2,
    "tile_width_px": 16,
    "tile_height_px": 16
  },
  "layers": {
    "ground": {
      "sorting_order": -100,
      "cells": [1,1,1,1, 1,2,2,1, 1,2,2,1, 1,1,1,1]
    },
    "solid": {
      "sorting_order": 0,
      "cells": [0,0,0,0, 0,0,0,0, 0,0,0,5, 0,0,0,5]
    }
  }
}
```

字段规则：

- `version` 仅接受 1；width/height 是 1..512 的整数，cell_size 是有限正数；本切片固定两层 ground/solid。加载前检查维度乘法和数组长度，再分配；非法 ID、错误类型、缺字段或未知版本给明确错误。
- `cells` 为 row-major，索引 `y * width + x`。0 是空，不绘制、不碰撞；1..columns×rows 是 atlas 的从 1 起编号，`column=((id-1)%columns)+1`、`row=floor((id-1)/columns)+1`，与现有 SpriteRenderer 的选格编号一致。
- atlas image 相对于项目 `images/`。图片实际尺寸必须等于 columns×tile_width_px、rows×tile_height_px，均为正整数；不截断余数，不接受图集外编号。图集矩形与 cell 完全重合，不支持超出 cell 的图块。
- atlas 的此份网格数据以地图资源为准；复用 `SpritesheetGridSpec` 和现有 source-rectangle 算法，不另外写 `.pocketengine/spritesheets.json` 造成两个可修改真值。该图片作为普通 SpriteRenderer 使用时仍遵守普通 sprite 的元数据，editor 不应偷偷同步两份配置。
- ground 的任何非零值只有显示；solid 的任何非零值同时有显示和实心碰撞。作物/耕地 ID 的游戏含义留在 fixture Lua，不把 farming 类型写入通用 Tilemap。
- 初始数据在场景组装阶段完成解析与验证，frame 0 已可查询。OnStart 只创建待提交的物理/渲染状态，不做磁盘扫描。每个运行实例拥有独立 cell 数据；两个 actor 引用相同资源时，修改其中一个不改变另一个或源文件。

## 坐标和 Lua API

本切片保持 Pocket2D 的 world X 向右、Y 向下。origin 为 root Transform 的 world x/y，cell_size 是正方形边长，单位为 world units。atlas 像素大小不能隐式改变碰撞尺寸：例如 16 px tile、cell_size=1，在当前 100 pixels/world-unit 渲染约定下需要显式映射为 100 world-render pixels。

| API | 精确语义 |
| --- | --- |
| `map:WorldToCell(world_x, world_y)` | 返回现有 Vector2 类型，x/y 值为整数：`floor((world-origin)/cell_size)`；允许得到负值或超出地图的值，不 clamp。输入非有限数报错。 |
| `map:GetCellCenter(cell_x, cell_y)` | 返回 Vector2：`origin + (cell+0.5)*cell_size`；数学转换可接受地图外的整数坐标。非整数/非有限坐标报错。 |
| `map:InBounds(cell_x, cell_y)` | `0 <= x < width && 0 <= y < height`；整数数值可以是 Lua 的 1.0，不能悄悄截断 1.2。 |
| `map:GetCell(layer, x, y)` | 返回**已提交** tile ID；地图外返回 nil，地图内空格返回 0。layer 只能是 ground/solid；未知 layer 或非整数坐标报错。 |
| `map:SetCell(layer, x, y, id)` | 校验后排队；返回 true 表示有效的新修改进入队列，false 表示地图外或与已有待提交/当前值相同。非法 layer/非整数/越界 atlas ID 报错且不改变队列。 |
| `map:GetRevision()` | 初始 0；每次提交中只要有至少一个最终 cell 值改变就加 1。一次提交多个格子也只加 1；重复赋同值或改回原值不增加。 |

`SetCell` 不立即改变 `GetCell` 的结果。它使用和当前延迟生命周期一致的明确提交语义：同一帧对同一 cell 的最后一次写入胜出；在 OnUpdate/OnLateUpdate 提交的修改，于 `FinalizePrePhysicsDestructions` 之后、Box2D `Step` 之前统一提交。提交内容同时用于之后的碰撞和绘制。frame 完成后的 observe 能看到新值。

当前 collision/trigger 回调发生在物理步内，不能在其中重建 fixtures。由这些回调调用的 SetCell 留在队列中，到下一次 pre-physics 提交才生效；本帧剩余查询/绘制仍使用旧的 committed data。测试必须覆盖这个延迟，不制造一帧“画面已开洞、碰撞还没开洞”的状态。

所有方法遵守现有 generation-safe ActorHandle：过期 component 或 scene 代际不能修改另一张新地图。销毁时取消待提交修改。`enabled=false` 时地图数据仍可查询/修改，但提交后不绘制，静态 body 停用；重新启用时使用当前 committed cells。

坐标基准例：origin=(-8,-8)、cell_size=1。WorldToCell(-8,-8)=(0,0)；(-8.001,-8) 的 x=-1；(-7,-8) 的 x=1；GetCellCenter(8,8)=(0.5,0.5)；world x=56 已在 64 格地图右边界之外。浮点位置不以近似 rounding 代替 floor。

## Dirty chunk、裁剪与排序

chunk 边长固定为 32，不作为本切片配置项。64×64 地图每层四块；每层拥有独立 render dirty 标记，只有 solid 修改会标记 collision dirty。同一 chunk 内 64 次修改只重建一次。跨 x=31/32 的两个修改必须命中两块；cell 图形没有邻居依赖，因此不把相邻块也标脏。

render chunk 缓存只保存 CPU 侧非空 cell/atlas 记录；不先增加 render-to-texture 地图缓存或 GPU 管线。首次加载与实际改变后重建记录，正常静止帧不重扫完整 cell 数组。加载后的路径解析、图片解码和 atlas metadata 查询不放进逐 cell 循环。

根据 camera 与实际 viewport/zoom 得到 world 可见矩形，转换为 cell 半开区间 `[floor(min), ceil(max))`，再与地图范围相交；先跳过不相交 chunk，再选择相交 cell。地图外空视野提交 0 个 tile draw；边界恰好落在 cell 边上不多画下一格。使用浮点坐标和现有 nearest，不能在 DPI/缩放转换中重新引入整数截断。

复用 `ImageDrawRequest` 和现有 stable sorting：每层 sorting_order 是全局绘制排序值。先依明确的 ground/solid 层次提交；同一层按 chunk row-major、chunk 内 row-major 保持确定顺序。相同 sorting_order 保留提交顺序；SpriteRenderer 的 actor 排序不被 Tilemap 重新洗牌。tile 以左上 pivot=(0,0) 放在 cell origin，world 尺寸严格为 cell_size。暂不扩展 y-sort、图块超范围遮挡或 material 系统。

## Box2D solid 层与生命周期

每个有 solid cells 的 chunk 拥有一个 static body。按每一行合并连续非零 cell 为一个矩形 fixture，不跨 chunk 合并，不要求跨行 polygon union。使用现有 `Rigidbody::EnsurePhysicsWorld()`、`RayCast::RegisterFixture/UnregisterFixture`，metadata 指向地图 owning actor，kind 为 Collider。初版 friction=0、restitution=0，不为材料系统增加未用字段。

pre-physics 提交先准备新的 cell/fixture 描述，再在 world 未锁定时替换受影响 chunk 的 fixtures；数据准备失败不破坏旧 committed map。销毁 fixtures 前解除 metadata；销毁 Tilemap/scene 时清除全部 body、metadata 和 pending commands。对旧 body 的 OnCollisionExit 可以发生，但不得通过过期 handle 继续编辑新地图。

拆一个 cell 的验收是**真实通行**：视觉、raycast、Box2D 碰撞均改变，其他 chunk 的 fixture/body 不重建。销毁一个地图后，另一个地图的碰撞仍有效；重载同 UID 不能复用旧 Lua handle。先串行实现这些小块准备；只有 Release profile 证明重建值得并行时，才用现有 TaskExecutor 处理纯 CPU 描述并在主线程提交，不为 4 个小 chunk 默认启动线程。

## Scene/editor 保存与 agent observation

SceneFormat 继续保存 actor/component 的 `map_resource`、enabled 和 Transform；不能把 8192 个 cell 塞进普通 component 的任意 Lua property JSON。独立 `TilemapAsset` reader/writer 保存版本化地图数据，使用临时文件写入成功后替换目标。scene 保存不会把运行中种植、拆墙结果写回地图资产。

editor 最小作者流程是选择地图资源、查看维度/atlas/层、输入 layer/x/y/tile ID 后应用单 cell 编辑，并提供 Save Map。无需本切片实现鼠标画笔。用一个窄的 map document 保存作者副本与 old/new cell undo/redo；scene actor 的创建/移动/引用仍用现有 SceneDocument/SceneEditCommand。修改资产和修改运行实例是两种明确操作，Play/Stop 不自动保存 gameplay delta。

edit-mode cell 修改应更新 preview 的显示和碰撞，但不推进 FrameClock 或 Lua gameplay；在 frozen render 前走同样的安全 map 提交步骤。保存/关闭/重开后作者 cell 数据和 atlas 引用相同，取消未保存编辑恢复磁盘版本。地图载入失败保留当前可用 scene，不清空 editor 文档或运行世界后再报错。

原 JSONL `observe` 的 actor/component 结果保持兼容，Tilemap 普通 properties 至少包含 map_resource、enabled、width、height、cell_size、revision。另支持可选的有界查询，避免每次 dump 全地图：

```json
{"cmd":"observe","tilemaps":[{"actor_uid":10,"component_key":"map","layer":"ground","cells":[[8,8],[9,8],[-1,0]]}]}
```

扩展响应增加 `tilemaps`，每项标识 actor_uid/component_key/layer/revision，并按请求顺序返回 `{x,y,tile}`，地图外 tile 为 null。每请求最多 16 个 map query、总计 256 个 cell。先完整校验请求，任何非法类型/unknown layer/missing component 都返回结构化 error，不改变帧或地图。未传 tilemaps 时不增加 cell payload。observe 不 flush 待提交修改，不触发渲染或资源载入，也不允许借观察命令写地图。

render draw/dirty rebuild 统计可作为测试诊断，不能混进必须在 headless/图形间一致的游戏状态快照；地图 revision 与 committed cell 才是玩法状态。此 observation 仍不是 savegame/checkpoint 格式。

## 4×4 种收与拆墙 fixture 的精确步骤

新增 `tests/fixtures/tilemap_farm/`，不改变原 `village_defense` 测试的初始条件。地图 origin=(-8,-8)，size=64×64，cell_size=1；ground 默认 ID1，x/y=8..11 的 16 格为 ID2。ID2=空耕地、ID3=生长中、ID4=成熟；这些含义仅属于 fixture Lua。solid 在 x=16、y=8..11 放 ID5。玩家从 (0.5,0.5) 开始，圆碰撞半径0.25，移动速度6，箭头控制，E keydown 操作脚下格子。

Lua FarmLogic 每 OnUpdate 先把自己的 tick 加1、处理到期生长，再处理 E：空耕地变 ID3，并记录 `mature_at = tick + 120`；成熟地采收变 ID2、harvested 加1；生长中/非耕地/越界无变化。只在新按下操作，持续按住不能种收循环。脚本公开 harvested/planted_count/mature_count；planted_count 是生长中和成熟作物总数，mature_count 是其中成熟的数量，格子数据通过原生 API 查询。

| 请求序列（每行接上一行） | 完成后的 frame | 必须看到的 committed 状态 |
| --- | ---: | --- |
| 初始 observe | 0 | (8,8)=2；harvested=0、planted_count=0。 |
| step 1，keys=[e] | 1 | (8,8)=3；planted_count=1；mature_at=121。 |
| step 59，keys=[e] | 60 | 仍为3，只种植一次。 |
| step 1 keys=[]；step 1 keys=[e]；step 1 keys=[] | 63 | 未成熟，重复按下不能收获或重置成熟时间。 |
| step 57，keys=[] | 120 | 仍为3，harvested=0。 |
| step 1，keys=[] | 121 | (8,8)=4，mature_count=1。 |
| step 1，keys=[e] | 122 | (8,8)=2，harvested=1，planted/mature counts=0。 |
| step 10，keys=[e] | 132 | 仍为2，不能因持续按住再种一轮。 |
| step 1 keys=[]；step 1 keys=[e] | 134 | 第二轮 ID3，mature_at=254。 |
| step 40 keys=[right]；step 1 keys=[e] | 175 | 玩家在约(4.5,0.5)，脚下(12,8)=1；harvested仍1，第一块第二轮作物不受影响。 |

以上 frame 是完成 step 后的编号。FarmLogic 的 tick 从1开始，与请求序列严格绑定；作者不能通过把成熟断言放宽为“最终变熟”来掩盖 off-by-one。

拆墙使用单独重置的 case：将玩家初始放在 (7.5,1.5)，向右30 tick，center x 应停在不超过7.80的位置；Box2D 容差只用于接触位置，不改变 cell 边界。Space keydown 让 fixture 工具操作面对的相邻 cell，清除 solid(16,9)。该 tick 完成后目标 ID=0、对应 chunk 重建一次；再右移30 tick，center x 必须大于8.5。上下相邻墙格仍为5且仍阻挡。工具仅为验证地图编辑，不声称移植原剑/镐战斗。

另设 collision callback 修改 case：本帧回调请求拆墙，当前 frame 的 committed ID/碰撞保持旧状态；下一 tick pre-physics 后才变0。另测 scene reset 后 map 恢复磁盘数据、FarmLogic归零、旧 map handle 无效。图形运行必须看到相同种植阶段、洞口和计数，但人类仍需验证交互提示是否清楚。

## 文件 ownership 与实施顺序

下列新增路径是实现分工，不表示文件已存在。遵守最多一个 writer 修改同一文件；协调者整合共享入口和 CMake。

| Owner | 独占文件/责任 | 交接合同 |
| --- | --- | --- |
| Native map owner | 新 `include/shared/tilemap/TilemapAsset.h`、`src/shared/tilemap/TilemapAsset.cpp`；新 `include/engine/scene/Tilemap.h`、`src/engine/scene/Tilemap.cpp`。拥有 schema、cell/pending/commit、CPU chunk records、Box2D chunks 和 ActorHandle 生命周期。 | 提供小而明确的 load/validate/commit/queue-draw/read-cell API；所有测试先可在无窗口状态运行。不改 Lua注册和 Engine.cpp。 |
| Editor owner | 新 `include/editor/documents/TilemapDocument.h`、`src/editor/documents/TilemapDocument.cpp`；`src/editor/panels/InspectorPanel.cpp` 和必要的 SceneView preview 接线。 | 使用同一 TilemapAsset parser/writer；负责作者undo/redo与显式保存，不复制 runtime 数据格式。涉及 engine preview入口由协调者接。 |
| QA/fixture owner | 新 `tests/tilemap_test.cpp`、`tests/tilemap_runtime_test.cpp`、`tests/tilemap_benchmark.cpp`、`tests/agent_tilemap_test.py`、`tests/fixtures/tilemap_farm/**`。 | 使用真实组件/Box2D/JSONL，不写另一套模拟来代替实现；先交 failing correctness cases 和生成器，再加入性能 workload。 |
| Coordinator | `ComponentManager.h`、manager 的 Shared/Binding/Queries/Lifecycle/Runtime 接口、`BuiltinComponentAPI.cpp`、`Engine.cpp`、`AgentSession.cpp`、必要 SceneFormat 验证及全部 CMake。现有 Renderer 只有需要复用入口时才由此 owner 改。 | 合并前确认 native/editor API；安排 pre-physics/frozen-preview commit；串行构建和集成测试；避免三个 worker 同改大型入口。 |

实施顺序：schema/API 单测 → native 初始化/安全提交/碰撞 → culling/图集渲染 → agent 查询与 farming fixture → editor 明确保存 → Release 对照。每阶段保持原 runtime/editor 能运行，不把完整新组件半成品注册进默认场景。

## 验证与性能门槛

正确性必须通过：合法小图和64²图；所有schema拒绝路径；负坐标/精确边界/OOB空值区别；atlas首尾ID与错误尺寸；同值/同帧覆盖/改回原值；两层独立与两个实例独立；x31/32 dirty边界；callback延迟提交；真实拆墙通行、raycast与fixture清理；disabled/re-enabled；销毁/重载旧handle；editor roundtrip/undo；图形与headless同输入后的 cell/revision/收获状态一致。

渲染用现有 `render_backend_test.cpp` 的 SDL readback 方法，software 和本机 Metal 分别验证 atlas ID、透明边缘、层顺序、viewport边界与DPI缩放；可选backend初始化不可用与像素错误分别报告。既有 runtime、actor lifetime、input、event、audio、agent tests 全部保留并运行。

Release 基线先测，禁止预先编造毫秒或 SOTA 数值。建立同一生成器的两条真实路径：当前 SpriteRenderer-per-tile 作为本引擎旧能力基线，新的 Tilemap 作为候选。它们使用相同 occupied cells、atlas、视野、排序、碰撞（有/无分开）和输入轨迹。纯绘制对照先隔离物理；物理对照比较相同实心区域和通行/raycast结果，同时分别报告fixture数量，不能把合并前后不同的原始contact回调数量当作相同工作量。规模从64²、128²、256²递增；基线达到明确时间/内存上限就记录未完成，不能算成“无限加速”。512²只在两条路径的预算允许时运行。

固定相机、滚动相机和每 tick 0/1/64 cell 编辑分别测；预热之后记录 p50/p95/p99、CPU准备/提交、draw数、visible cells、重建chunk/fixtures、分配与RSS。资源加载与首帧单独计时，不混进稳定帧。关闭VSync测吞吐，另在60Hz检查最长停顿。headless逻辑不能代替GPU渲染对比。

不依赖硬件速度的通过门槛：

- 固定视野时 native tile draw 数由可见非空 cells 决定，不随地图总面积增长；地图外视野为0。
- 无编辑的稳定 tick 不解析地图文件、不重建chunk或fixtures；单格修改仅重建所属layer/chunk，ground变化不重建碰撞。
- native地图自身保持一个actor/component，不创建每tile Lua对象；地图有多少cell不能增加每tick Lua回调数。
- 1次相同输入重放的最终cells/revision/计数相同；性能计时前后的正确性检查都通过。

若新路径没有在目标大地图负载降低CPU/提交开销，或小图明显回退，先报告测量并定位；不能仅凭采用chunk或C++就宣布优化。原始日志、截图、计时结果和校验摘要只放ignored `build/`，源码保留测试和workload定义。
