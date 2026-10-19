# Gameplay save data

此接口已完成第四阶段集成。当前验证结果见 [开发状态](../development/status.md)。
它保存游戏显式选择的数据，不是任意 Lua VM 或物理世界快照。

项目 `game.config` 声明稳定的命名空间：

```json
{"game_title":"My Village","initial_scene":"farm","save_namespace":"my_village"}
```

命名空间和 slot 只能是 1–64 个 ASCII 字母、数字、下划线或连字符，首字符必须是
字母或数字。没有配置命名空间的项目照常运行，但 Save 调用返回 `not_configured`。
保存位置默认由 SDL 的平台偏好目录确定。`game --save-dir ROOT` 或进程环境变量
`POCKET_SAVE_ROOT` 将位置改为 `ROOT/<save_namespace>/`，包括 agent reset 后的
新 worker。不要把 ROOT 指向游戏源码，测试应使用临时目录。

```lua
local result = Save.Write("main", 1, {
    player = {x = 3.5, y = 1.0, health = 10},
    harvested = 2,
    irrigation_unlocked = true
})
if not result.ok then
    Debug.Log(result.code .. ": " .. result.error)
end

local saved = Save.Read("main")
if saved.ok then
    if saved.schema_version ~= 1 then
        -- 拒绝未知游戏数据版本；先完整校验数据，再改变世界。
        return
    end
    local state = saved.data
    -- 校验所有字段后，调用真实的游戏恢复函数。
end
```

`Write(slot, schema_version, data)` 成功返回 `{ok=true}`。
`Read(slot)` 成功返回 `{ok=true, schema_version=..., data=...}`；`Read(slot, {fallback = true})`
在存档缺失或损坏而上一版本（`<slot>.save.json.bak`，每次覆盖时自动保留）完好时返回上一版本并带
`recovered=true`；不带 fallback 时损坏照常报错。
两者的预期失败都返回 `{ok=false, code=..., error=...}`，不会把文件错误当作成功。
错误类别包括 `invalid_argument`、`not_configured`、`not_found`、`invalid_data`、
`unsupported_format` 和 `io_error`。游戏自己拥有 `schema_version`，引擎不会替游戏
猜测旧档迁移策略。文件外层的 `format_version` 由引擎管理。

数据根必须是 table，值可以是 bool、有效 UTF-8 string、有限 number、table。
Lua 整数保留 signed 64-bit 值；需要传给 TypeScript 的实体身份仍宜用字符串。
字符串键构成对象；从 1 开始连续的整数键构成数组。稀疏数组、混合键、函数、
userdata、thread 和循环引用被拒绝。共享 table 会复制其数据，不保留引用身份。
读取后的对象和数组都是普通 Lua table；空 table 保存为对象，读取的空 JSON 数组
再保存也可能变成空对象，游戏 schema 应把它们都视作空集合或另行声明集合类型。

转换只读取实际 table 内容，不调用 `__pairs`、`__index` 或其他元方法。不要直接
保存整个 component 实例：其中的 actor/userdata、函数和临时状态不是纯存档数据。
嵌套深度上限 32（根为 1），节点上限 100,000，完整文件上限 4 MiB。超限、非法
UTF-8、非有限数值、损坏或未知引擎格式会明确失败。

Write 在替换目标文件前完整验证和编码数据，并使用同目录的独占临时文件，
刷到磁盘（macOS 用 F_FULLFSYNC）后原子替换并同步目录；崩溃或断电后槽位要么是旧档
要么是新档。校验或写入失败不能先截断旧存档。文件同步、原子替换与断电持久性的精确边界见
[存储格式](../development/save-format.md)。这不是不可信脚本或敌对文件系统的 sandbox。

Read 只返回数据，不改变角色、地图或输入。玩法恢复代码必须先校验整份 schema，
然后统一应用合法状态。是否恢复速度、战斗过程、输入按键和绝对时钟，由游戏明确
规定；不能因为 JSON roundtrip 成功就宣称完整 checkpoint 等价。
