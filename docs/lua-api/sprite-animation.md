# Native SpriteAnimation

`SpriteAnimation` 以固定 tick 播放 atlas 帧，控制同一个 actor 上指定的
`SpriteRenderer`。可运行示例见 [sprite_animation fixture](../../tests/fixtures/sprite_animation/)。
它提供片段播放、帧事件和完成事件；本轮没有 blend tree、骨骼动画、浮点播放速率或动画资源编辑器。

## 场景和资源

```json
{
  "uid": 1,
  "name": "player",
  "components": {
    "transform": {"type": "Transform"},
    "sprite": {"type": "SpriteRenderer", "sprite": "base.png"},
    "anim": {
      "type": "SpriteAnimation",
      "animation_resource": "hero.animation.json",
      "renderer_key": "sprite",
      "clip": "walk"
    }
  }
}
```

资源位于 `animations/hero.animation.json`：

```json
{
  "version": 1,
  "image": "characters/hero.png",
  "columns": 2,
  "rows": 1,
  "tile_width_px": 16,
  "tile_height_px": 16,
  "clips": [
    {"name": "walk", "loop": true, "frames": [
      {"row": 1, "column": 1, "ticks": 2},
      {"row": 1, "column": 2, "ticks": 2, "event": "footstep"}
    ]}
  ]
}
```

`image` 精确相对于项目 `images/`，上例实际图片必须为 32×16 px。
资源路径必须以 `.animation.json` 结尾；资源及图片均不做 basename 搜索，
不允许绝对路径、`..`、反斜杠或越出各自根目录的符号链接。
图片尺寸必须恰好等于列数 × 单帧宽、行数 × 单帧高。

所有示例字段都必填，只有 frame 的 `event` 可省略。格式拒绝未知或重复字段、
重名 clip、错误类型、非法 UTF-8 和越界行列。行列从 `1` 开始；每帧 `ticks`
为 `1..36000` 的整数。资源最多 4 MiB、256 个非空 clip、总共 65,536 帧；
clip 名称和显式 event 为 `1..128` 个 UTF-8 字节，区分大小写。
省略 event 表示无 marker，显式空字符串不合法。

每个 renderer 最多由一个配置了资源的 animator 控制。场景加载会在清空旧世界前
校验资源、renderer key 和初始 clip。空 `animation_resource` 是不播放的作者占位组件；
空 `clip` 表示初始停止，不会默认选择首个 clip。编辑器清空资源会清除动画姿态；
无效属性修改保留原有有效配置。Lua 通过播放 API 切换状态，不直接改作者资源字段。

## Lua API

```lua
local animation = self.actor:GetComponentByKey("anim")
animation:Play("walk")
```

| 方法 | 行为 |
| --- | --- |
| `Play(name)` | 选择片段并显示第一帧，切换成功返回 `true`。已选同名片段返回 `false`，即使暂停或已完成也不重播。 |
| `Restart(name)` | 显式从片段第一帧重新播放。 |
| `Pause()` / `Resume()` | 暂停或继续当前片段；暂停保留姿态及待发事件。 |
| `Stop()` | 清空当前播放和事件，恢复 SpriteRenderer 的作者姿态。 |

未知 clip 名称报错，保留已有播放状态。没有资源或组件已失效时，`Play` / `Restart`
同样报错。玩法脚本不要手工调用注册的 `OnStart` / `OnDestroy` 生命周期入口；
移除组件用 `actor:RemoveComponent(animation)`，销毁 actor 用 `Actor.Destroy(actor)`。

| 属性 | 访问与含义 |
| --- | --- |
| `animation_resource`, `renderer_key`, `clip` | Lua 只读作者配置；`clip` 是初始配置，不随播放切换。 |
| `enabled` | 可读写；禁用冻结时间和事件，保留当前姿态。 |
| `current_clip` | 只读当前选中名称；停止时为空。 |
| `frame` | 只读片段内帧编号，从 `1` 开始；停止时为 `0`。 |
| `elapsed_ticks` | 只读当前帧已消耗的 tick，换帧后归零，不是整个片段的累计时长。 |
| `playing` | 只读；有效、启用、已选中且未暂停或完成时为 `true`。 |
| `paused`, `completed` | 只读暂停和完成状态。 |
| `actor`, `key` | 引擎管理的所属 actor handle 与组件 key，玩法脚本不要重新赋值。 |

初始配置在 frame 0 就显示首帧。动画推进发生在 pending `OnStart` 后、普通
`OnUpdate` 前；在 tick T 的 `OnUpdate` 调用 Play，会立即显示第一帧，T+1 才消耗
首个 tick。每帧保持其整数 duration，循环片段回到第一帧；单次片段在最后一帧
duration 耗尽后仅完成一次，并保持最后姿态。冻结编辑器预览不推进时间或发送事件。

动画姿态是临时 renderer override，不改写作者 sprite、行列或
`.pocketengine/spritesheets.json`。它保留 tint、pivot、scale/mirroring、排序与 transform。
Stop、移除、销毁和 reset 清理其拥有的 override；运行中切换片段不写回资源文件。

## 动画事件

```lua
Footsteps = {
    OnAnimationEvent = function(self, event)
        if event.type == "frame" and event.marker == "footstep" then
            -- 在这里调用游戏自己的音效或脚步逻辑。
        elseif event.type == "completed" and event.clip == "attack" then
            self.actor:GetComponentByKey(event.component_key):Play("walk")
        end
    end
}
```

每次进入帧都会产生 `type="frame"`，即使 marker 为空。单次片段结束产生
`type="completed"`。payload 包含 `type`、`clip`、`frame`、`marker` 和
`component_key`；frame 是片段内编号，不是 atlas 列号。完成事件 marker 为空。
Play 的首帧事件在后续动画推进阶段派发。

事件按组件 key 顺序发送给所属 actor 上已启动、启用的 Lua 组件，在普通 OnUpdate
之前执行。接收者和动画身份会在可执行 getter 之后重新校验：在 callback 或 getter
中 Stop、Restart、移除动画或销毁 actor，会取消剩余旧播放事件；新播放事件留给后续
tick。新增组件不会进入当前接收者快照。Lua getter 和 callback 错误走正常结构化
runtime error 路径，不应使 native worker 崩溃。

测试包括 [资源校验](../../tests/sprite_animation_asset_test.cpp)、
[原生播放](../../tests/sprite_animation_test.cpp)、
[像素读回](../../tests/sprite_animation_render_test.cpp)、
[agent 时序和重放](../../tests/agent_animation_test.py)及
[getter 重入](../../tests/animation_getter_reentry_test.py)。最终集成矩阵以
[development status](../development/status.md) 为准。
