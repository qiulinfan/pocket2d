# Native Tilemap

`Tilemap` 是原生组件。地图格子存放在独立资源中，不是单独的 Actor 或 Lua
component。每个运行实例有自己的 cell 数据；引用同一资源的两个地图不会共享
游戏中的种植、拆墙等修改。

可运行示例见 [tilemap_farm fixture](../../tests/fixtures/tilemap_farm/)，其中
[FarmLogic.lua](../../tests/fixtures/tilemap_farm/component_types/FarmLogic.lua)
实现种植、生长和采收，[CollisionEditProbe.lua](../../tests/fixtures/tilemap_farm/component_types/CollisionEditProbe.lua)
演示物理回调内排队修改。

## 配置地图

在 scene 或 actor template 中声明资源引用和 root Transform：

```json
{
  "uid": 10,
  "name": "terrain",
  "components": {
    "map": {
      "type": "Tilemap",
      "map_resource": "demo.tilemap.json",
      "enabled": true
    },
    "transform": {"type": "Transform", "x": 0, "y": 0, "rotation": 0}
  }
}
```

`map_resource` 是相对于项目 `maps/` 的明确路径，文件名必须以
`.tilemap.json` 结尾。它不使用递归同名搜索。资源及 atlas 路径不能为绝对路径、
包含 `..` 或越出各自的资源目录。`atlas.image` 始终相对于项目 `images/`，
不相对于地图所在子目录。

一个 actor 最多有一个 Tilemap，必须没有 parent，world rotation 必须为 `0`，
不能同时有 Rigidbody。其他 actor 上的 Rigidbody 可以与地图碰撞。当前支持平移，
不支持旋转地图、流式加载或任意 polygon tile；额外装饰层见“资源格式”，自动拼接见下文“地形自动拼接”。

非空资源在场景组装时解析并校验，frame 0 就能读取初始格子。地图原点、世界范围
和渲染缩放还必须能被引擎的浮点类型表达。无效资源不能通过静默截断或占位图片
替代来完成地图载入。

编辑器允许先添加尚未选择资源的空组件。空组件的 `width`、`height` 为 `0`，
`GetCell` 返回 `nil`，`InBounds` 返回 `false`，不创建碰撞或绘制。Lua 没有
`Tilemap()` 构造器，也没有用于加载资源的 `SetResource` 方法；在作者工具中配置
资源，或实例化已经配置好资源的 actor template。

## 资源格式

下面的 2×2 地图需要 `images/tiles/demo.png`，图片尺寸必须恰好为 32×16：

```json
{
  "version": 1,
  "width": 2,
  "height": 2,
  "cell_size": 1.0,
  "atlas": {
    "image": "tiles/demo.png",
    "columns": 2,
    "rows": 1,
    "tile_width_px": 16,
    "tile_height_px": 16
  },
  "layers": {
    "ground": {"sorting_order": -100, "cells": [1,1,1,1]},
    "solid": {"sorting_order": 0, "cells": [0,0,2,0]}
  }
}
```

- `version` 仅支持整数 `1`；`width`、`height` 是 1..4096 的整数，格子总数不超过 4,194,304；
  `cell_size` 必须有限且大于零。
- 必须有 `ground`、`solid` 两层（只有 `solid` 产生碰撞），另可有至多 6 个额外装饰层，
  名称为 `[a-z0-9_]{1,32}`，各有自己的 `sorting_order`；每层恰好有 `width * height` 个整数。
  Lua 与 agent 查询按层名访问任何存在的层，`map:GetLayerNames()` 按 ground、solid、额外层顺序列出。
  JSON schema 拒绝缺字段、未知字段、重复字段、错误类型和非法编号。
- 数组为 row-major，cell `(x,y)` 的零基索引是 `y * width + x`。
  Lua 中如自行保存一个普通数组，同一位置的 Lua 数组索引要再加 `1`。
- `0` 表示空格。非零 ID 从 `1` 到 `columns * rows`，按 atlas 从左到右、
  从上到下排列。列号为 `((id-1) % columns) + 1`，行号为
  `math.floor((id-1) / columns) + 1`。
- atlas 的 grid 和 tile 像素尺寸必须为正整数，乘积不能溢出支持范围。
  实际图片必须恰好满足 `width = columns * tile_width_px`、
  `height = rows * tile_height_px`。
- `ground` 非零 cell 只显示；`solid` 非零 cell 同时显示并产生实心碰撞。
  ID 是否代表作物、成熟状态或材料，由玩法脚本决定。

Tilemap 的 atlas grid 以地图 JSON 为准，不读取普通 SpriteRenderer 的
`.pocketengine/spritesheets.json` 网格配置，也不自动改写它。tile 的世界宽高均为
`cell_size`；例如 16 px 的 tile 配合 `cell_size=1`，占一整格世界单位，
不会缩成 0.16 格。贴图使用 nearest 采样，允许透明像素；透明的 solid tile
仍然产生整个 cell 的碰撞。

## 地形自动拼接

可选的 `autotile` 段按四邻居选择 tile（类似 Godot terrain、Unity RuleTile）：

```json
"autotile": {
  "frame_on_load": true,
  "terrains": [
    {"name": "dirt", "connect": "any", "frames": [[45, 46, 47], [40], "... 共 16 项"]}
  ]
}
```

- `frames` 恰好 16 项，下标是四邻居掩码：上 1、右 2、下 4、左 8，邻居“相连”时置位。
  每项至少一个 tile ID；多个 ID 时按格子坐标的稳定哈希选取变体。
- 一个 tile ID 最多属于一个地形；格子的地形由其 ID 判定。
- `connect` 为 `any`（默认，同层任意非零格都相连，适合泰拉瑞亚式方块互相融合）
  或 `same`（只与同一地形相连）。地图外视为相连，地图边缘不出现收边。
- `frame_on_load` 为 `true` 时，载入（运行时与编辑器）会按规则重选所有地形格，
  生成器只需写入该地形的任一 ID。

| 方法 | 返回与行为 |
| --- | --- |
| `GetTerrain(layer, x, y)` | 已提交格子的地形名；无地形为 `""`，地图外为 `nil`。 |
| `SetTerrain(layer, x, y, name)` | 按邻居排队写入该地形的 tile（`""` 清空），并重选上下左右四个地形邻居。与 `SetCell` 一样在提交后生效，计算时包含尚未提交的编辑。未知名称报错。 |

## 格子光照

`map:ComputeLight(x, y, width, height, options)` 在原生代码中计算一个格子矩形
（可超出地图）的彩色光照，并把每格的光色写入一张动态图片，每格一个像素：

```lua
map:ComputeLight(x0, y0, w, h, {
  image = "dynamic:light",          -- 必须以 dynamic: 开头
  sky = 1.0,                         -- 白色天光强度（昼夜由游戏调节）
  air_decay = 0.91, solid_decay = 0.56,  -- 光离开一格时乘上该格的系数
  ambient = 0.02,                    -- 最暗处的亮度下限
  open_ground = {"trunk"},           -- 这些地形的 ground 格仍透天光
  sources = {tx, ty, 1.2, 0.9, 0.6, ...},  -- 光源：格子坐标与 r、g、b 强度，每 5 个一组
  smooth = true,                     -- 在格子中心之间线性插值（默认）
})
Image.DrawEx("dynamic:light", x0, y0, 0, 100 * cell_size, 100 * cell_size, 0, 0, 255, 255, 255, 255, 40)
```

没有 solid 格、且没有 ground 格（或 ground 属于 `open_ground` 地形）的格子接收天光；
地图上方视为天空。光离开一格时每个通道乘以该格的衰减（空气 `air_decay`，实心
`solid_decay`），所以紧贴天空的第一层实心格几乎全亮，往下逐格渐暗而不是突然变黑；
用两轮正反扫描近似。每个像素是该格的光色（不低于 `ambient`，超过 1 截断），
绘制时乘到已画内容上（暖色光源会给地块染色）。`smooth = false` 时每格是一块纯色。
未知选项会报错。

## 地图图片

`map:ComputeMap(x, y, width, height, options)` 在原生代码中把一个格子矩形（可超出地图）
画成一张动态图片，每 `step` 格一个像素，用于小地图和全屏地图：

```lua
map:ComputeMap(0, 0, map.width, map.height, {
  image = "dynamic:map",                     -- 必须以 dynamic: 开头
  colors = {grass = {40, 180, 60}, stone = {120, 120, 130}},  -- 按地形名给 RGB
  sky = {90, 150, 230, 255}, cave = {20, 15, 15, 255},       -- 空格子：cave_y 行以上为 sky，以下为 cave
  cave_y = 150, step = 2,
})
```

每个像素取该格最上层有颜色的地形：先 solid，再 ground，再额外层；没有颜色的地形视为空。
图片尺寸为 `ceil(width/step) × ceil(height/step)`，`step` 为 1..64。未知选项会报错。

战争迷雾：`map:RevealMap(x, y, radius)` 把以某格为圆心、半径内的格子标为已探索；
`map:IsRevealed(x, y)` 查询。`ComputeMap` 的 `fog = {r, g, b, a}` 选项把从未探索的格子画成该颜色。
`map:SaveRevealed()` 返回探索状态文本（按行优先、未探索与已探索交替的游程长度，如 `"12,5,300"`），
可存进存档；`map:LoadRevealed(text)` 恢复，游程总和必须恰好覆盖地图。

## 流动液体

地图可以带一层泰拉瑞亚式的流动液体（原生 `LiquidGrid`，第一次使用时创建）：每格一种液体
（`"water"`、`"lava"`、`"honey"`，`""` 为空）和 0..255 的量。

- `map:SetLiquid(x, y, kind, amount)` 设置一格；`map:GetLiquid(x, y)` 返回种类，
  `map:GetLiquidAmount(x, y)` 返回量；`map:GetLiquids(x, y, w, h)` 按行优先返回一块区域的
  `种类 * 256 + 量`（1 水、2 熔岩、3 蜂蜜，0 为空或地图外）。
- `map:StepLiquid({solid = "solid", harden = "obsidian", budget = 20000})` 推进一步：
  只处理"活跃"格，先往下落，再与左右少至少两单位的空格拉平；熔岩每 3 步、蜂蜜每 4 步才动一次。
  `solid` 层（含未提交的编辑）的非零格挡住液体；水碰到熔岩时熔岩格清空，并在 `solid` 层放上
  `harden` 地形（为空则只清空）。每步最多处理 `budget` 格，其余留到下一步；返回
  `{active = 剩余活跃格数, hardened = {x1, y1, x2, y2, ...}}`（硬化的格子，方便游戏同步自己的镜像）。
  静止的水体不再活跃，几乎不花时间；任何层的格子编辑都会唤醒周围 3×3 的液体。处理顺序固定，
  同样的起点得到同样的结果。
- `map:LiquidFromTerrain(layer, {water = "water", lava = "lava"})` 把某层上这些地形的格子
  变成满格液体并清掉地形，返回转换的格数（用来把地图里画好的水和熔岩变成液体）。
- `map:ComputeLiquid(x, y, w, h, {image = "dynamic:liquid", colors = {water = {r, g, b, a}}, resolution = 8})`
  把一块区域画进动态图片，每格 `resolution`（1..16）像素见方，按量从格子底部填充，上方有液体的格子
  画满；图片可以像光照一样整张画在地图上。
- `map:SaveLiquid()` 返回游程文本（`"数量:编码"`，逗号分隔），`map:LoadLiquid(text)` 恢复。

## 获取组件与属性

```lua
local terrain = Actor.Find("terrain")
local map = terrain:GetComponentByKey("map")
-- 同一 actor 只允许一个 Tilemap，也可以用 GetComponent("Tilemap")。
```

| 属性 | 访问 | 含义 |
| --- | --- | --- |
| `map_resource` | 只读 | 配置的地图资源相对路径。 |
| `width`, `height` | 只读 | 已加载地图的 cell 维度。 |
| `cell_size` | 只读 | 每个正方形 cell 的世界边长。 |
| `revision` | 只读 | 已提交的 cell 数据版本，与 `GetRevision()` 相同。 |
| `enabled` | 读写 | 请求的启用状态；绘制和碰撞在提交点共同应用。 |
| `actor`, `key` | 引擎管理 | 所属 actor handle 与组件 key；玩法脚本不要重新赋值。 |

`enabled` 的读取反映当前请求值，不能在物理回调中据此断言碰撞已经切换。
提交过程会快照该请求，并将同一个值应用到所有 chunk body 和绘制状态。
如果 `EndContact` 回调又写入 `enabled`，新请求等下一次提交处理。

禁用地图后，cell 仍可查询和修改；提交仍进行，只是不绘制且静态 body 停用。
重新启用使用当前已提交的数据，不会重新载入磁盘资源。

## Cell API

坐标系为 world X 向右、Y 向下，origin 为 root Transform 的已提交世界位置。
以下返回的 `Vector2` 是现有的原生类型，通过 `.x`、`.y` 读取。

| 方法 | 返回与行为 |
| --- | --- |
| `WorldToCell(world_x, world_y)` | `Vector2(floor((world_x-origin_x)/cell_size), floor((world_y-origin_y)/cell_size))`，不 clamp。 |
| `GetCellCenter(x, y)` | `Vector2(origin_x+(x+0.5)*cell_size, origin_y+(y+0.5)*cell_size)`，允许地图外整数坐标。 |
| `InBounds(x, y)` | 坐标是否位于 `[0,width) × [0,height)`。 |
| `GetCell(layer, x, y)` | 已提交的整数 ID；地图外返回 `nil`，地图内空格返回 `0`。 |
| `SetCell(layer, x, y, id)` | 校验后排队。有效的新写入返回 `true`；地图外、无有效地图或值与已有待提交/当前值相同返回 `false`。 |
| `GetCells(layer, x, y, width, height)` | 已提交格子的矩形批量读取，返回一个按行排列（row-major）的 Lua 数组（从 1 开始）；矩形必须完全在地图内，否则报错。用于游戏侧镜像或规划，避免逐格调用 `GetCell`。 |
| `GetRevision()` | 初始为 `0`；一次提交至少有一个最终 cell 值改变时加 `1`。 |

`layer` 必须精确为 `"ground"` 或 `"solid"`。cell 坐标和 ID 必须是有限整数数值；
Lua 的 `1.0` 可以接受，`1.2` 不会被截断。未知 layer、非法 ID、非整数坐标或
不能表达的坐标转换会抛 Lua 错误，而不是悄悄 clamp。有效配置上的非法
`SetCell` 不改变待提交队列。

例如 origin=(-8,-8)、cell_size=1：

```lua
map:WorldToCell(-8, -8)       -- (0, 0)
map:WorldToCell(-8.001, -8)   -- (-1, 0)
map:WorldToCell(-7, -8)       -- (1, 0)
map:GetCellCenter(8, 8)      -- (0.5, 0.5)
```

## 提交与生命周期

`SetCell` 不会立即改变 `GetCell`。同一提交前多次写同一格，最后一次写入胜出。
改回原值不会增加 revision；一次提交改 64 格也只增加一次 revision。
平移和 enabled 切换本身不会增加 cell revision。

正常 tick 顺序是待执行的 `OnStart` → `OnUpdate` → `OnLateUpdate` →
延迟销毁处理 → Tilemap 提交 → Box2D Step。提交前排队的有效编辑，在该 tick
的物理步和绘制中一起生效。
碰撞/trigger 回调发生在物理步内，其中的编辑留到下一次提交；当前完成帧的
`GetCell`、raycast、绘制仍使用旧的已提交格子。`observe` 不会主动 flush 编辑。

Tilemap 的 Lua 绑定不暴露 `CommitPending`、`ReplaceAsset` 或地图磁盘保存方法。
编辑器在冻结预览前
使用同一个安全提交点应用作者修改，不推进 Lua gameplay 或 FrameClock。
运行时修改不自动写回地图 JSON，scene 保存也不把当前 cell 数组嵌入 scene。
作者工具的 **Save Map** 保存其独立作者副本，先成功写临时文件再替换目标。

移除地图使用正常的延迟 API：

```lua
terrain:RemoveComponent(map) -- 保留 terrain actor
-- 或 Actor.Destroy(terrain)
```

`OnStart`、`OnDestroy` 虽属于注册的生命周期入口，玩法脚本不应手工调用。
在物理回调中直接调用 `Tilemap:OnDestroy()` 会明确报错并提示使用上述延迟 API，
不会在 locked world 中删除 Box2D body。销毁会取消待提交编辑、移除碰撞并解除
fixture metadata。

缓存的过期 map 引用不能修改新地图：有效参数的 `GetCell` 返回 `nil`，
`SetCell` 返回 `false`。同 UID 的新场景对象不会复用旧实例身份。
`map.actor:IsValid()` 只检查 actor；组件可能已被单独移除，所以需要时重新通过
`actor:GetComponentByKey(...)` 获取当前组件。旧引用的尺寸、revision 或数学
坐标转换不是组件存活检查。

## 在编辑器中绘制

选中带 Tilemap 的 actor 后，Inspector 的 **Map Asset** 区打开地图的作者副本：

- **Layer** 列出资源的全部图层：`ground`、`solid`（唯一产生碰撞的层）和每个额外层。
- 勾选 **Paint in Scene view** 后，在 Scene 视图中按住左键拖动即在所选图层上绘制，
  选择和拖动 actor 暂停；悬停处的方框显示笔刷覆盖的格子。快速拖动也会连成不断开的线。
- **Tile** 笔刷写入所选 tile ID（`0` 擦除），可在 atlas 调色板中点选；**Terrain (autotile)**
  笔刷放置所选地形（`(clear)` 清除），与 Lua `SetTerrain` 一样按邻居选出该格的 tile，
  并重选上下左右四个地形邻居。**Brush Size** 设定方形笔刷边长。
- 一次拖动（含被重选的邻居）是一步撤销：Scene 视图聚焦时 `Ctrl+Z`（macOS 为 `Cmd+Z`）撤销，
  `Ctrl+Shift+Z` 或 `Ctrl+Y` 重做，Inspector 也有 **Undo Map** / **Redo Map**。
- **Cell by coordinates** 按输入的 X/Y 应用同一笔刷。

预览只把改动的格子写入场景中显示该资源的地图实例，只重建涉及的 32×32 chunk，
因此大地图上逐帧绘制不复制整张地图；实例在编辑器之外被改过时才整张替换。
绘制只改作者副本，**Save Map** 后才写入资源。

## 最小玩法脚本

这个组件对上面的 `terrain` 地图，在新按下 E 时切换一个 ground cell：

```lua
PaintTile = {
    OnStart = function(self)
        self.map = Actor.Find("terrain"):GetComponentByKey("map")
    end,
    OnUpdate = function(self)
        if not Input.GetKeyDown("e") then return end
        local before = self.map:GetCell("ground", 0, 0)
        if before == nil then return end
        local next_id = before == 1 and 2 or 1
        if self.map:SetCell("ground", 0, 0, next_id) then
            assert(self.map:GetCell("ground", 0, 0) == before)
            -- 本 tick 的 pre-physics 提交后才能读到 next_id。
        end
    end
}
```

内部 chunk 固定为 32×32。地图只重建实际变更 layer 的相关 CPU 绘制记录，solid
变化另外重建该 chunk 的碰撞。每个含 solid cell 的 chunk 使用一个 static body，
chunk 内每块连通 solid 区域的外轮廓（以及其中空洞的内轮廓）合并共线边后成为一条闭合
Box2D chain，所以物体沿相邻 cell 滑动不会在接缝处卡住。chain 是单面的：只从 solid 区域
外侧碰撞，完全嵌入 solid 内部的物体不会被推出。`GetFixtureCount()` 返回轮廓数。
没有每 tile 的 actor、脚本或独立碰撞事件身份；raycast/contact 命中的是 owning actor。绘制请求先提交 ground，再提交 solid；
最终显示顺序由各层的全局 `sorting_order` 决定，同序值保持提交顺序。
当前没有额外 y-sort 或超出 cell 的图块。

有界的 agent cell 查询见 [agent protocol](../development/agent-protocol.md#bounded-tilemap-observations)。
格式和验收范围见 [Tilemap slice](../development/tilemap-slice.md)；真实用例见
[native tests](../../tests/tilemap_test.cpp)、[runtime tests](../../tests/tilemap_runtime_test.cpp)
和 [pixel readback tests](../../tests/tilemap_render_test.cpp)。
