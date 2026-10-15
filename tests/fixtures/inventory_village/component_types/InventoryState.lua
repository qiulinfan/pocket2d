-- All item mutations use the native inventory model; UI carries only a preview.
InventoryState={
    health=2,target_health=20,world_remaining=8,consumed=0,selected=1,
    speed=2,attack=1,defense=0,ready=false,
    bag_state="",chest_state="",equipment_state="",hotbar_state="",
    bag_revision=0,chest_revision=0,equipment_revision=0,
    total_herb=0,total_ore=0,total_gear=0,
    dragging=false,drag_source="",drag_index=0,drag_count=0,drag_revision=0,drag_button=0,
    focus="bag",focus_index=1,last_result="none",save_status="none",load_status="none",
    pickup_moved=0,save_count=0,load_count=0,
    OnStart=function(self)
        self.catalog=Inventory.Open("village.items.json")
        self.bag=self.catalog:Create(30);self.chest=self.catalog:Create(12)
        self.equipment=self.catalog:Create(4,{"subweapon","head","chest","legs"})
        self.filters={"subweapon","head","chest","legs"}
        self.hotbar_map={1,2,3,4,5,6,7,8,9}
        self.defs={}
        for _,id in ipairs({"herb","ore","sword","helmet","armor","boots"}) do self.defs[id]=self.catalog:GetItem(id) end
        local function empty(n)
            local slots={};for i=1,n do slots[i]={item_id="",count=0,instance_id=""} end;return slots
        end
        local bag,chest,equip=empty(30),empty(12),empty(4)
        bag[1]={item_id="herb",count=4,instance_id=""};bag[2]={item_id="ore",count=5,instance_id=""}
        bag[3]={item_id="boots",count=1,instance_id="boots-1"}
        for i=5,30 do bag[i]={item_id="ore",count=5,instance_id=""} end
        chest[1]={item_id="sword",count=1,instance_id="sword-1"}
        chest[2]={item_id="helmet",count=1,instance_id="helmet-1"}
        chest[3]={item_id="armor",count=1,instance_id="armor-1"}
        chest[4]={item_id="herb",count=4,instance_id=""};chest[5]={item_id="ore",count=4,instance_id=""}
        local result=Inventory.RestoreGroup({{inventory=self.bag,slots=bag},{inventory=self.chest,slots=chest},{inventory=self.equipment,slots=equip}})
        assert(result.ok,"initial inventory fixture restore failed")
        self.body=Actor.Find("player"):GetComponent("Rigidbody");self.ready=true
        self:Publish()
    end,
    Container=function(self,name)
        if name=="bag" then return self.bag elseif name=="chest" then return self.chest elseif name=="equipment" then return self.equipment end
        return nil
    end,
    Feedback=function(self,result)
        if result.code~="" then return result.code end
        return result.changed and "moved" or "no_change"
    end,
    Publish=function(self)
        local function encode(inv)
            local values={}
            for _,slot in ipairs(inv:Snapshot()) do values[#values+1]=slot.item_id..","..slot.count..","..slot.instance_id end
            return table.concat(values,";")
        end
        self.bag_state=encode(self.bag);self.chest_state=encode(self.chest);self.equipment_state=encode(self.equipment)
        self.hotbar_state=table.concat(self.hotbar_map,",")
        self.bag_revision=self.bag:GetRevision();self.chest_revision=self.chest:GetRevision();self.equipment_revision=self.equipment:GetRevision()
        self.total_herb=self.world_remaining+self.consumed;self.total_ore=0;self.total_gear=0
        for _,inv in ipairs({self.bag,self.chest,self.equipment}) do for _,slot in ipairs(inv:Snapshot()) do
            if slot.item_id=="herb" then self.total_herb=self.total_herb+slot.count
            elseif slot.item_id=="ore" then self.total_ore=self.total_ore+slot.count
            elseif slot.count>0 then self.total_gear=self.total_gear+slot.count end
        end end
        self.speed=2;self.attack=1;self.defense=0
        for _,slot in ipairs(self.equipment:Snapshot()) do
            local item=self.defs[slot.item_id]
            if item then self.speed=self.speed+(item.move_speed_bonus or 0);self.attack=self.attack+(item.attack or 0);self.defense=self.defense+(item.defense or 0) end
        end
    end,
    SyncWorld=function(self,prepared)
        local pickup=prepared or Actor.Find("herb_pickup")
        if self.world_remaining==0 then if pickup then Actor.Destroy(pickup) end
        else
            if not pickup then pickup=Actor.Instantiate("HerbPickup") end
            pickup:GetComponent("InventoryPickup").remaining=self.world_remaining
        end
    end,
    TryPickup=function(self)
        if not self.ready or self.dragging or self.world_remaining==0 then return end
        local result=self.bag:Add("herb",self.world_remaining,"")
        self.world_remaining=self.world_remaining-result.moved;self.pickup_moved=self.pickup_moved+result.moved
        self.last_result=self:Feedback(result);self:SyncWorld();self:Publish()
    end,
    Cancel=function(self,reason)
        self.dragging=false;self.drag_source="";self.drag_index=0;self.drag_count=0;self.drag_revision=0;self.drag_button=0
        self.last_result=reason or "cancelled"
    end,
    Begin=function(self,name,index,half,button)
        if self.dragging then return end
        local inv=self:Container(name);if not inv then return end
        local slot=inv:GetSlot(index);if slot.count==0 then return end
        self.dragging=true;self.drag_source=name;self.drag_index=index
        self.drag_count=half and math.ceil(slot.count/2) or slot.count
        self.drag_revision=inv:GetRevision();self.drag_button=button or 0
        self.focus=name;self.focus_index=index;self.last_result="preview"
    end,
    Drop=function(self,name,index)
        if not self.dragging then return end
        local source=self:Container(self.drag_source)
        if source:GetRevision()~=self.drag_revision then self:Cancel("stale_source");return end
        if name==self.drag_source and index==self.drag_index then self:Cancel("same_slot");return end
        if name=="hotbar" and self.drag_source=="bag" then
            if self.drag_count~=source:GetSlot(self.drag_index).count then self:Cancel("split_hotbar_rejected");return end
            local old=self.hotbar_map[index]
            for i,mapped in ipairs(self.hotbar_map) do if mapped==self.drag_index then self.hotbar_map[i]=old;break end end
            self.hotbar_map[index]=self.drag_index;self.selected=index;self:Cancel("remapped");return
        end
        local target=self:Container(name)
        if not target then self:Cancel("outside");return end
        local slot=source:GetSlot(self.drag_index)
        local result
        if self.drag_count<slot.count then result=source:Split(self.drag_index,target,index,self.drag_count)
        else result=source:Move(self.drag_index,target,index,0,true) end
        self.focus=name;self.focus_index=index;self:Cancel(self:Feedback(result));self:Publish()
    end,
    Hit=function(self,x,y)
        local function grid(name,left,top,cols,rows,pitch)
            local column=math.floor((x-left)/pitch);local row=math.floor((y-top)/pitch)
            if column>=0 and column<cols and row>=0 and row<rows and (x-left)%pitch<52 and (y-top)%pitch<52 then
                return name,row*cols+column+1
            end
        end
        local name,index=grid("bag",40,210,6,5,58);if name then return name,index end
        name,index=grid("chest",500,210,4,3,58);if name then return name,index end
        name,index=grid("equipment",500,430,4,1,58);if name then return name,index end
        return grid("hotbar",40,550,9,1,58)
    end,
    Use=function(self)
        if self.dragging then return end
        local index=self.hotbar_map[self.selected];local slot=self.bag:GetSlot(index);local item=self.defs[slot.item_id]
        if not item then return end
        if item.kind=="food" and self.health<3 then
            local result=self.bag:Remove(index,1)
            if result.moved==1 then self.health=math.min(3,self.health+item.health_restore);self.consumed=self.consumed+1 end
            self.last_result=self:Feedback(result)
        elseif item.equipment_slot then
            for i,filter in ipairs(self.filters) do if filter==item.equipment_slot then
                local result=self.bag:Move(index,self.equipment,i,0,true);self.last_result=self:Feedback(result);break
            end end
        end
        self:Publish()
    end,
    Unequip=function(self)
        if self.dragging or self.focus~="equipment" then return end
        local result=self.equipment:TransferAll(self.focus_index,self.bag)
        self.last_result=self:Feedback(result);self:Publish()
    end,
    ValidateSave=function(self,data)
        local function exact(value,keys)
            if type(value)~="table" then return false end
            local count=0;for key in pairs(value) do if not keys[key] then return false end;count=count+1 end
            local n=0;for key in pairs(keys) do if value[key]==nil then return false end;n=n+1 end
            return count==n
        end
        local function integer(value,min,max) return type(value)=="number" and value==math.floor(value) and value>=min and value<=max end
        local function dense(value,n)
            if type(value)~="table" or #value~=n then return false end
            local count=0;for key in pairs(value) do if not integer(key,1,n) then return false end;count=count+1 end
            return count==n
        end
        if not exact(data,{bag=true,chest=true,equipment=true,hotbar_map=true,selected=true,health=true,target_health=true,player=true,world_remaining=true,consumed=true}) then return false end
        if not integer(data.selected,1,9) or not integer(data.health,0,3) or not integer(data.target_health,0,20)
            or not integer(data.world_remaining,0,8) or not integer(data.consumed,0,16) or not dense(data.hotbar_map,9) then return false end
        local mapped={};for _,index in ipairs(data.hotbar_map) do if not integer(index,1,30) or mapped[index] then return false end;mapped[index]=true end
        if not exact(data.player,{x=true,y=true}) then return false end
        for _,v in pairs(data.player) do if type(v)~="number" or v~=v or math.abs(v)>1000 then return false end end
        local totals={herb=data.world_remaining+data.consumed,ore=0,sword=0,helmet=0,armor=0,boots=0}
        local instances={}
        for _,group in ipairs({{data.bag,30,false},{data.chest,12,false},{data.equipment,4,true}}) do
            if not dense(group[1],group[2]) then return false end
            for index,slot in ipairs(group[1]) do
                if not exact(slot,{item_id=true,count=true,instance_id=true}) or type(slot.item_id)~="string" or type(slot.instance_id)~="string" then return false end
                if slot.item_id=="" then if slot.count~=0 or slot.instance_id~="" then return false end
                else
                    local item=self.defs[slot.item_id]
                    if not item or not integer(slot.count,1,item.max_stack) then return false end
                    if group[3] and item.equipment_slot~=self.filters[index] then return false end
                    if item.max_stack==1 then
                        if slot.instance_id~=slot.item_id.."-1" or instances[slot.instance_id] then return false end
                        instances[slot.instance_id]=true
                    elseif slot.instance_id~="" then return false end
                    totals[slot.item_id]=totals[slot.item_id]+slot.count
                end
            end
        end
        return totals.herb==16 and totals.ore==139 and totals.sword==1 and totals.helmet==1 and totals.armor==1 and totals.boots==1
    end,
    SaveGame=function(self)
        self:Cancel("save")
        local p=self.body:GetPosition()
        local data={bag=self.bag:Snapshot(),chest=self.chest:Snapshot(),equipment=self.equipment:Snapshot(),hotbar_map=self.hotbar_map,
            selected=self.selected,health=self.health,target_health=self.target_health,player={x=p.x,y=p.y},world_remaining=self.world_remaining,consumed=self.consumed}
        if not self:ValidateSave(data) then self.save_status="invalid_game_schema";return end
        local result=Save.Write("main",1,data);self.save_status=result.ok and "saved" or result.code
        if result.ok then self.save_count=self.save_count+1 end
    end,
    LoadGame=function(self)
        local result=Save.Read("main")
        if not result.ok then self.load_status=result.code;return end
        if result.schema_version~=1 then self.load_status="unsupported_game_schema";return end
        if not self:ValidateSave(result.data) then self.load_status="invalid_game_schema";return end
        local data=result.data
        -- Stage the only fallible world reconstruction before inventory/scalars.
        -- The candidate is not published as gameplay state until the native
        -- restore succeeds; a failed restore removes it at the frame boundary.
        local prepared=nil
        if data.world_remaining>0 and not Actor.Find("herb_pickup") then
            local ok,candidate=pcall(function() return Actor.Instantiate("HerbPickup") end)
            if not ok or not candidate then self.load_status="world_prepare_failed";return end
            prepared=candidate
            local valid,complete=pcall(function()
                return candidate:GetName()=="herb_pickup" and candidate:GetComponent("InventoryPickup")~=nil
                    and candidate:GetComponent("Rigidbody")~=nil and candidate:GetComponent("SpriteRenderer")~=nil
            end)
            if not valid or not complete then Actor.Destroy(candidate);self.load_status="world_prepare_failed";return end
        end
        local ok,restored=pcall(function() return Inventory.RestoreGroup({{inventory=self.bag,slots=data.bag},{inventory=self.chest,slots=data.chest},{inventory=self.equipment,slots=data.equipment}}) end)
        if not ok or not restored.ok then if prepared then Actor.Destroy(prepared) end;self.load_status="invalid_inventory";return end
        self.hotbar_map=data.hotbar_map;self.selected=data.selected;self.health=data.health;self.target_health=data.target_health
        self.world_remaining=data.world_remaining;self.consumed=data.consumed
        self.body:SetPosition(Vector2(data.player.x,data.player.y));self.body:SetVelocity(Vector2(0,0))
        self:Cancel("loaded");self:SyncWorld(prepared);self:Publish();self.load_status="loaded";self.load_count=self.load_count+1
    end,
    OnUpdate=function(self)
        if not self.ready then return end
        if Input.GetKeyDown("f9") then self.body:SetVelocity(Vector2(0,0));self:LoadGame();self:Publish();return end
        if Input.GetKeyDown("f5") then self.body:SetVelocity(Vector2(0,0));self:SaveGame();self:Publish();return end
        if Input.GetKeyDown("escape") then self:Cancel("cancelled") end
        local mouse=Input.GetMousePosition();local hit,index=self:Hit(mouse.x,mouse.y)
        if self.dragging then
            if self.drag_button~=0 and Input.GetMouseButtonUp(self.drag_button) then self:Drop(hit,index) end
        else
            for _,button in ipairs({1,3}) do if Input.GetMouseButtonDown(button) and hit then
                if hit=="hotbar" then self.selected=index
                else self.focus=hit;self.focus_index=index;self:Begin(hit,index,button==3,button) end
            end end
        end
        if Input.GetKeyDown("tab") then
            self.focus=self.focus=="bag" and "chest" or (self.focus=="chest" and "equipment" or "bag");self.focus_index=1
        end
        local cols=self.focus=="bag" and 6 or 4
        local capacity=self:Container(self.focus):GetCapacity()
        if Input.GetKeyDown("left") then self.focus_index=math.max(1,self.focus_index-1) end
        if Input.GetKeyDown("right") then self.focus_index=math.min(capacity,self.focus_index+1) end
        if Input.GetKeyDown("up") then self.focus_index=math.max(1,self.focus_index-cols) end
        if Input.GetKeyDown("down") then self.focus_index=math.min(capacity,self.focus_index+cols) end
        if Input.GetKeyDown("return") then
            if self.dragging then self:Drop(self.focus,self.focus_index)
            else self:Begin(self.focus,self.focus_index,Input.GetKey("left shift") or Input.GetKey("right shift"),0) end
        end
        if not self.dragging then
            for i=1,9 do if Input.GetKeyDown(tostring(i)) then self.selected=i end end
            local wheel=Input.GetMouseScrollDelta()
            if wheel~=0 then self.selected=((self.selected-1+(wheel>0 and 1 or -1))%9)+1 end
            if Input.GetKeyDown("u") then self:Use() end
            if Input.GetKeyDown("q") then self:Unequip() end
            local p=self.body:GetPosition()
            if Input.GetKeyDown("e") and (p.x-1)^2+p.y^2<=1.5625 then self:TryPickup() end
            if Input.GetKeyDown("h") then self.health=math.max(0,self.health-math.max(1,2-self.defense)) end
            if Input.GetKeyDown("space") and (p.x+0.5)^2+p.y*p.y<=2.25 then self.target_health=math.max(0,self.target_health-self.attack) end
        end
        local x,y=0,0
        if not self.dragging then
            if Input.GetKey("d") then x=x+1 end;if Input.GetKey("a") then x=x-1 end
            if Input.GetKey("s") then y=y+1 end;if Input.GetKey("w") then y=y-1 end
            if x~=0 and y~=0 then x=x/math.sqrt(2);y=y/math.sqrt(2) end
        end
        self.body:SetVelocity(Vector2(x*self.speed,y*self.speed));self:Publish()
    end
}
