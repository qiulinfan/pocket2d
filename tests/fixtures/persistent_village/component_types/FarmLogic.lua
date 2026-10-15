-- Explicit save schema for the persistent farm workload. No automatic loading.
FarmLogic = {
    tick=0, harvested=0, supplies=0, planted_count=0, mature_count=0,
    planted_tick=-1, mature_at_tick=-1, walls_removed=0, remaining_ticks=0,
    irrigation_unlocked=false, save_status="none", load_status="none", last_error="",
    save_count=0, load_count=0,
    OnStart=function(self)
        self.map=Actor.Find("terrain"):GetComponent("Tilemap")
        self.player=Actor.Find("player")
        self.growing={};self.mature={}
    end,
    SetPumpColor=function(self)
        local sprite=Actor.Find("irrigation"):GetComponent("SpriteRenderer")
        sprite.r=self.irrigation_unlocked and 255 or 120
        sprite.g=self.irrigation_unlocked and 255 or 120
        sprite.b=self.irrigation_unlocked and 255 or 120
    end,
    UpdateRemaining=function(self)
        self.remaining_ticks=math.max(0,(self.growing[8*64+8] or self.tick)-self.tick)
    end,
    Capture=function(self)
        local crops={}
        for y=8,11 do for x=8,11 do
            local index=y*64+x
            crops[#crops+1]={x=x,y=y,stage=self.map:GetCell("ground",x,y),
                remaining_ticks=math.max(0,(self.growing[index] or self.tick)-self.tick)}
        end end
        local walls={}
        for y=8,11 do walls[#walls+1]=self.map:GetCell("solid",16,y) end
        local body=self.player:GetComponent("Rigidbody")
        local movement=self.player:GetComponent("FarmMovement")
        local position=body:GetPosition()
        return {crops=crops,walls=walls,harvested=self.harvested,supplies=self.supplies,
            irrigation_unlocked=self.irrigation_unlocked,
            player={x=position.x,y=position.y,facing_x=movement.facing_x,facing_y=movement.facing_y}}
    end,
    Validate=function(self,data)
        local function exact(value,keys)
            if type(value)~="table" then return false end
            local count=0
            for key in pairs(value) do if not keys[key] then return false end;count=count+1 end
            local expected=0
            for key in pairs(keys) do if value[key]==nil then return false end;expected=expected+1 end
            return count==expected
        end
        local function integer(value,min,max)
            return type(value)=="number" and value==math.floor(value) and value>=min and value<=max
        end
        local function dense(value,count)
            if type(value)~="table" or #value~=count then return false end
            local actual=0
            for key in pairs(value) do if not integer(key,1,count) then return false end;actual=actual+1 end
            return actual==count
        end
        if not exact(data,{crops=true,walls=true,harvested=true,supplies=true,irrigation_unlocked=true,player=true}) then return false end
        if not integer(data.harvested,0,1000000) or not integer(data.supplies,0,1000000)
            or type(data.irrigation_unlocked)~="boolean" then return false end
        if data.supplies~=data.harvested-(data.irrigation_unlocked and 1 or 0) then return false end
        if not dense(data.crops,16) or not dense(data.walls,4) then return false end
        for i,crop in ipairs(data.crops) do
            if not exact(crop,{x=true,y=true,stage=true,remaining_ticks=true})
                or crop.x~=8+(i-1)%4 or crop.y~=8+math.floor((i-1)/4)
                or not integer(crop.stage,2,4) then return false end
            if crop.stage==3 then
                if not integer(crop.remaining_ticks,1,120) then return false end
            elseif crop.remaining_ticks~=0 then return false end
        end
        for _,tile in ipairs(data.walls) do if tile~=0 and tile~=5 then return false end end
        local p=data.player
        if not exact(p,{x=true,y=true,facing_x=true,facing_y=true}) then return false end
        if type(p.x)~="number" or p.x~=p.x or p.x< -7.75 or p.x>55.75
            or type(p.y)~="number" or p.y~=p.y or p.y< -7.75 or p.y>55.75 then return false end
        if not integer(p.facing_x,-1,1) or not integer(p.facing_y,-1,1)
            or math.abs(p.facing_x)+math.abs(p.facing_y)~=1 then return false end
        return true
    end,
    SaveGame=function(self)
        local data=self:Capture()
        if not self:Validate(data) then self.save_status="invalid_game_schema";self.last_error="live state outside game schema";return end
        local result=Save.Write("main",1,data)
        self.save_status=result.ok and "saved" or result.code
        self.last_error=result.ok and "" or result.error
        if result.ok then self.save_count=self.save_count+1 end
    end,
    LoadGame=function(self)
        local result=Save.Read("main")
        if not result.ok then self.load_status=result.code;self.last_error=result.error;return end
        if result.schema_version~=1 then self.load_status="unsupported_game_schema";self.last_error="expected game schema 1";return end
        if not self:Validate(result.data) then self.load_status="invalid_game_schema";self.last_error="invalid persistent village data";return end
        -- No live mutations occur above this line. Every coordinate/value below
        -- belongs to the fixed validated native map contract.
        local data=result.data
        self.growing={};self.mature={};self.planted_count=0;self.mature_count=0
        self.planted_tick=-1;self.mature_at_tick=-1
        for _,crop in ipairs(data.crops) do
            local index=crop.y*64+crop.x
            self.map:SetCell("ground",crop.x,crop.y,crop.stage)
            if crop.stage==3 then
                self.growing[index]=self.tick+crop.remaining_ticks
                self.planted_count=self.planted_count+1
                if index==8*64+8 then self.mature_at_tick=self.growing[index] end
            elseif crop.stage==4 then
                self.mature[index]=true;self.mature_count=self.mature_count+1
                self.planted_count=self.planted_count+1
            end
        end
        self.walls_removed=0
        for i,tile in ipairs(data.walls) do
            self.map:SetCell("solid",16,7+i,tile)
            if tile==0 then self.walls_removed=self.walls_removed+1 end
        end
        self.harvested=data.harvested;self.supplies=data.supplies
        self.irrigation_unlocked=data.irrigation_unlocked;self:SetPumpColor()
        local body=self.player:GetComponent("Rigidbody")
        body:SetPosition(Vector2(data.player.x,data.player.y));body:SetVelocity(Vector2(0,0))
        local movement=self.player:GetComponent("FarmMovement")
        movement.facing_x=data.player.facing_x;movement.facing_y=data.player.facing_y
        self:UpdateRemaining();self.load_status="loaded";self.last_error="";self.load_count=self.load_count+1
    end,
    OnUpdate=function(self)
        -- Save/load input ticks deliberately pause the farm clock. Failed loads
        -- cannot consume crop time or perform an incidental plant/harvest action.
        if Input.GetKeyDown("f9") then self:LoadGame();return end
        if Input.GetKeyDown("f5") then self:SaveGame();return end
        self.tick=self.tick+1
        for index,deadline in pairs(self.growing) do
            if self.tick>=deadline then
                self.map:SetCell("ground",index%64,math.floor(index/64),4)
                self.growing[index]=nil;self.mature[index]=true;self.mature_count=self.mature_count+1
            end
        end
        local position=self.player:GetComponent("Rigidbody"):GetPosition()
        local cell=self.map:WorldToCell(position.x,position.y)
        local index=cell.y*64+cell.x
        if Input.GetKeyDown("b") and not self.irrigation_unlocked and self.supplies>=1
            and (position.x-1.5)^2+(position.y+0.5)^2<=2.25 then
            self.supplies=self.supplies-1;self.irrigation_unlocked=true;self:SetPumpColor()
        end
        if Input.GetKeyDown("e") then
            local tile=self.map:GetCell("ground",cell.x,cell.y)
            if tile==2 then
                if self.map:SetCell("ground",cell.x,cell.y,3) then
                    local duration=self.irrigation_unlocked and 60 or 120
                    self.growing[index]=self.tick+duration;self.planted_tick=self.tick
                    self.mature_at_tick=self.tick+duration;self.planted_count=self.planted_count+1
                end
            elseif tile==4 or (tile==3 and self.mature[index]) then
                self.map:SetCell("ground",cell.x,cell.y,2);self.mature[index]=nil
                self.mature_count=self.mature_count-1;self.planted_count=self.planted_count-1
                self.harvested=self.harvested+1;self.supplies=self.supplies+1
            end
        end
        if Input.GetKeyDown("space") then
            local movement=self.player:GetComponent("FarmMovement")
            local x,y=cell.x+movement.facing_x,cell.y+movement.facing_y
            local tile=self.map:GetCell("solid",x,y)
            if tile~=nil and tile~=0 and self.map:SetCell("solid",x,y,0) then self.walls_removed=self.walls_removed+1 end
        end
        self:UpdateRemaining()
    end
}
