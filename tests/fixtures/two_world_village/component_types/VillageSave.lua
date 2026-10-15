-- Explicit fixture checkpoints. VillageState owns input and the frozen command tick.
local filters = {"subweapon", "head", "chest", "legs"}
local stack_limits = {seed=99, food=9, sword=1}
local function plain(value)
    return type(value)=="table" and getmetatable(value)==nil
end
local function exact(value, keys)
    if not plain(value) then return false end
    local count=0
    for key in next,value do if not keys[key] then return false end;count=count+1 end
    local expected=0
    for key in next,keys do if rawget(value,key)==nil then return false end;expected=expected+1 end
    return count==expected
end
local function integer(value, minimum, maximum)
    return type(value)=="number" and math.type(value)=="integer" and value>=minimum and value<=maximum
end
local function finite(value, minimum, maximum)
    return type(value)=="number" and value==value and value>=minimum and value<=maximum
end
local function dense(value, length)
    if not plain(value) then return false end
    local count=0
    for key in next,value do
        if not integer(key,1,length) then return false end
        count=count+1
    end
    return count==length
end
local function status(self, core, field, code, detail)
    self[field]=code;core[field]=code
    self.last_error=detail or "";core.last_error=self.last_error
end
local function stable(core, for_load)
    local called,ok,code=pcall(core.IsCheckpointStable,core,for_load)
    if not called then return false,"not_ready",tostring(ok) end
    if not ok then return false,code or "busy",code or "checkpoint is busy" end
    return true
end

VillageSave={
    save_status="none",load_status="none",last_error="",save_count=0,load_count=0,
    Validate=function(self,data)
        if not exact(data,{world_id=true,player=true,bag=true,equipment=true,crops=true,route=true,guardian=true,stats=true}) then
            return false,"payload fields"
        end
        if data.world_id~="village" and data.world_id~="underworld" then return false,"world_id" end
        if not exact(data.player,{x=true,y=true,health=true}) or not integer(data.player.health,1,6) then
            return false,"player fields/health"
        end
        local min_x,max_x=-6.75,6.75
        if data.world_id=="underworld" then min_x,max_x=17.25,30.75 end
        if not finite(data.player.x,min_x,max_x) or not finite(data.player.y,-3.75,3.75) then
            return false,"player position/world"
        end
        if not exact(data.guardian,{x=true,y=true,health=true}) or not integer(data.guardian.health,0,6)
            or not finite(data.guardian.x,17.25,30.75) or not finite(data.guardian.y,-3.75,3.75) then
            return false,"guardian fields/position/health"
        end
        if not exact(data.route,{closed=true,wave=true,remaining_ticks=true,altar_health=true,source_health=true})
            or type(data.route.closed)~="boolean" or not integer(data.route.wave,1,1000000)
            or not integer(data.route.altar_health,1,8) then return false,"route fields/wave/altar" end
        if data.route.closed then
            if not integer(data.route.source_health,0,0) or not integer(data.route.remaining_ticks,0,0) then
                return false,"closed route must have a dead source and no countdown"
            end
        elseif not integer(data.route.source_health,1,9) or not integer(data.route.remaining_ticks,1,900) then
            return false,"open route must have a living source and pending countdown"
        end
        if not dense(data.crops,16) then return false,"crops must contain all sixteen plots" end
        local planted_now=0
        for index=1,16 do
            local crop=data.crops[index]
            if not exact(crop,{stage=true,remaining_ticks=true,drop_count=true})
                or not integer(crop.stage,2,4) or not integer(crop.drop_count,0,1000000) then
                return false,"crop fields/stage/drop_count at "..index
            end
            if crop.stage==3 then
                if not integer(crop.remaining_ticks,1,120) then return false,"growing crop time at "..index end
            elseif not integer(crop.remaining_ticks,0,0) then return false,"non-growing crop time at "..index end
            if crop.stage~=2 then planted_now=planted_now+1 end
        end
        if not exact(data.stats,{planted=true,harvested=true,consumed=true,crops_destroyed=true,defeated=true}) then
            return false,"stats fields"
        end
        for key,value in next,data.stats do
            if not integer(value,0,1000000) then return false,"stats."..key end
        end
        if data.stats.planted~=data.stats.harvested+data.stats.crops_destroyed+planted_now
            or data.stats.harvested<data.stats.consumed then return false,"crop/stat relationship" end
        local sword_count=0
        for group,slots in ipairs({data.bag,data.equipment}) do
            local capacity=group==1 and 30 or 4
            if not dense(slots,capacity) then return false,group==1 and "bag capacity/shape" or "equipment capacity/shape" end
            for index=1,capacity do
                local slot=slots[index]
                if not exact(slot,{item_id=true,count=true,instance_id=true})
                    or type(slot.item_id)~="string" or type(slot.instance_id)~="string" then
                    return false,"inventory slot shape"
                end
                if slot.item_id=="" then
                    if not integer(slot.count,0,0) or slot.instance_id~="" then return false,"empty inventory slot" end
                else
                    local maximum=stack_limits[slot.item_id]
                    if not maximum or not integer(slot.count,1,maximum) then return false,"inventory item/count" end
                    if group==2 and (index~=1 or slot.item_id~="sword") then return false,"equipment filter" end
                    if slot.item_id=="sword" then
                        if slot.instance_id~="sword-island-1" then return false,"sword instance" end
                        sword_count=sword_count+1
                    elseif slot.instance_id~="" then return false,"stackable item instance" end
                end
            end
        end
        if sword_count~=1 then return false,"exactly one island sword must belong to bag/equipment" end
        return true
    end,
    Preflight=function(self,core,data)
        local valid,reason=self:Validate(data)
        if not valid then return false,"invalid_game_schema",reason end
        -- Only read fixed world references. This must not create or destroy actors.
        local called,available,code=pcall(core.CanApplySaveState,core,data)
        if not called then return false,"world_unavailable",tostring(available) end
        if not available then return false,code or "world_unavailable",code or "fixed world reference unavailable" end
        -- Native semantic validation on scratch containers cannot mutate the
        -- actual bag/equipment, their revisions, world bodies, or actor indexes.
        local prepared,result=pcall(function()
            local bag=core.catalog:Create(30)
            local equipment=core.catalog:Create(4,filters)
            return Inventory.RestoreGroup({{inventory=bag,slots=data.bag},{inventory=equipment,slots=data.equipment}})
        end)
        if not prepared then return false,"invalid_inventory",tostring(result) end
        if type(result)~="table" or not result.ok then
            return false,"invalid_inventory",type(result)=="table" and result.code or "native inventory preflight failed"
        end
        return true
    end,
    SaveGame=function(self,core)
        local allowed,code,reason=stable(core,false)
        if not allowed then status(self,core,"save_status",code,reason);return false,code end
        local captured,data=pcall(core.CaptureSaveState,core)
        if not captured then status(self,core,"save_status","world_unavailable",tostring(data));return false,"world_unavailable" end
        local valid,validation_code,detail=self:Preflight(core,data)
        if not valid then status(self,core,"save_status",validation_code,detail);return false,validation_code end
        local result=Save.Write("main",1,data)
        if not result.ok then
            status(self,core,"save_status",result.code or "io_error",result.error or "save failed")
            return false,result.code or "io_error"
        end
        self.save_count=self.save_count+1
        status(self,core,"save_status","saved","")
        return true
    end,
    LoadGame=function(self,core)
        local allowed,code,reason=stable(core,true)
        if not allowed then status(self,core,"load_status",code,reason);return false,code end
        local result=Save.Read("main")
        if not result.ok then
            status(self,core,"load_status",result.code or "io_error",result.error or "load failed")
            return false,result.code or "io_error"
        end
        if not integer(result.schema_version,1,1) then
            status(self,core,"load_status","unsupported_game_schema","expected game schema 1")
            return false,"unsupported_game_schema"
        end
        local valid,validation_code,detail=self:Preflight(core,result.data)
        if not valid then status(self,core,"load_status",validation_code,detail);return false,validation_code end
        -- Apply performs one actual native RestoreGroup before any world/state
        -- write. A normal rejection returns false before world changes. Do not
        -- swallow unexpected post-commit errors and pretend a rollback occurred.
        local applied,apply_code=core:ApplySaveState(result.data)
        if not applied then
            status(self,core,"load_status",apply_code or "invalid_inventory",apply_code or "native inventory restore rejected")
            return false,apply_code or "invalid_inventory"
        end
        self.load_count=self.load_count+1
        status(self,core,"load_status","loaded","")
        return true
    end
}
