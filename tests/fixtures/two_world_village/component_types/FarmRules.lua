FarmRules={
    Init=function(self,state)
        self.state=state;self.map=Actor.Find('village_map'):GetComponent('Tilemap')
        self.crops={};self.pickups={}
        for i=1,16 do
            self.crops[i]={stage=2,remaining_ticks=0,drop_count=0}
            local actor=Actor.Find('crop_pickup_'..i);assert(actor,'missing fixed crop pickup')
            self.pickups[i]={actor=actor,sprite=actor:GetComponent('SpriteRenderer')}
            self.pickups[i].sprite.enabled=false
        end
    end,
    Cell=function(self,index) return 3+(index-1)%4,2+math.floor((index-1)/4) end,
    Position=function(self,index) local x,y=self:Cell(index);return self.map:GetCellCenter(x,y) end,
    Queue=function(self,index)
        local x,y=self:Cell(index);self.map:SetCell('ground',x,y,self.crops[index].stage)
        self.pickups[index].sprite.enabled=self.crops[index].drop_count>0
    end,
    Tick=function(self)
        for i,crop in ipairs(self.crops) do
            if crop.stage==3 then
                crop.remaining_ticks=crop.remaining_ticks-1
                if crop.remaining_ticks==0 then crop.stage=4;self:Queue(i) end
            end
        end
        if self.state.world_id~='village' then return end
        local p=self.state.player_body:GetPosition()
        for i,crop in ipairs(self.crops) do
            if crop.drop_count>0 then
                local q=self:Position(i)
                if (p.x-q.x)^2+(p.y-q.y)^2<=0.64 then
                    local result=self.state.bag:Add('food',crop.drop_count)
                    crop.drop_count=crop.drop_count-result.moved
                    self.pickups[i].sprite.enabled=crop.drop_count>0
                    if result.moved>0 then self.state.last_result='food_collected'
                    elseif result.code=='no_space' then self.state.last_result='bag_full' end
                end
            end
        end
    end,
    Interact=function(self)
        if self.state.world_id~='village' then self.state.last_result='not_farmland';return false end
        local p=self.state.player_body:GetPosition();local cell=self.map:WorldToCell(p.x,p.y)
        if cell.x<3 or cell.x>6 or cell.y<2 or cell.y>5 then self.state.last_result='not_farmland';return false end
        local index=(cell.y-2)*4+cell.x-3+1;local crop=self.crops[index]
        -- Lua crop state is authoritative: native SetCell commits next boundary.
        if crop.stage==2 then
            local slot=self.state:FindItem('seed')
            if not slot then self.state.last_result='no_seed';return false end
            local result=self.state.bag:Remove(slot,1)
            if not result.ok then self.state.last_result=result.code;return false end
            crop.stage=3;crop.remaining_ticks=120;self.state.planted=self.state.planted+1
            self:Queue(index);self.state.last_result='planted';return true
        elseif crop.stage==4 then
            crop.stage=2;crop.remaining_ticks=0;crop.drop_count=crop.drop_count+1
            self.state.harvested=self.state.harvested+1;self:Queue(index)
            self.state.last_result='harvested';return true
        end
        self.state.last_result='still_growing';return false
    end,
    NearestThreat=function(self,x,y)
        local index,position,best
        for i,crop in ipairs(self.crops) do if crop.stage==3 or crop.stage==4 then
            local p=self:Position(i);local distance=(p.x-x)^2+(p.y-y)^2
            if not best or distance<best then index=i;position=p;best=distance end
        end end
        return index,position
    end,
    DestroyCrop=function(self,index)
        local crop=self.crops[index]
        if not crop or crop.stage==2 then return false end
        crop.stage=2;crop.remaining_ticks=0;self.state.crops_destroyed=self.state.crops_destroyed+1
        self:Queue(index);self.state.last_result='crop_lost';return true
    end,
    Capture=function(self)
        local crops={}
        for i,crop in ipairs(self.crops) do
            crops[i]={stage=crop.stage,remaining_ticks=crop.remaining_ticks,drop_count=crop.drop_count}
        end
        return crops
    end,
    Apply=function(self,crops)
        for i,crop in ipairs(crops) do
            self.crops[i]={stage=crop.stage,remaining_ticks=crop.remaining_ticks,drop_count=crop.drop_count};self:Queue(i)
        end
    end
}
