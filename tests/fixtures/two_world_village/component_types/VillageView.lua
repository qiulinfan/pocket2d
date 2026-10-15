-- Presentation uses the real native inventory and public simulation state.
-- Both regions remain alive; the camera only selects which region is visible.
VillageView={
    OnStart=function(self)
        self.state=self.actor:GetComponent("VillageState")
        local source=Actor.Find("source")
        self.source_sprite=source and source:GetComponent("SpriteRenderer") or nil
        local player=Actor.Find("player")
        self.player_body=player and player:GetComponent("Rigidbody") or nil
    end,
    Text=function(self,value,x,y,order)
        local text=string.upper(tostring(value))
        local punctuation={[":"]="colon",["/"]="slash",["+"]="plus",["."]="dot",["-"]="dash",["_"]="under"}
        for i=1,#text do
            local char=string.sub(text,i,i)
            local glyph=punctuation[char] or (string.match(char,"^[A-Z0-9]$") and char)
            if glyph then Image.DrawUIEx("glyph_"..glyph..".png",x+(i-1)*12,y,255,255,255,255,order or 1100) end
        end
    end,
    Counts=function(self,state)
        local counts={seed=0,food=0,sword=0};local equipped=false
        for _,inventory in ipairs({state.bag,state.equip}) do
            for _,slot in ipairs(inventory:Snapshot()) do
                if counts[slot.item_id]~=nil then counts[slot.item_id]=counts[slot.item_id]+slot.count end
                if inventory==state.equip and slot.item_id=="sword" and slot.count>0 then equipped=true end
            end
        end
        return counts,equipped
    end,
    Card=function(self,id,label,detail,x)
        Image.DrawUIEx("item_card.png",x,626,255,255,255,255,1010)
        Image.DrawUIEx(id..".png",x+12,646,255,255,255,255,1050)
        self:Text(label,x+48,638)
        self:Text(detail,x+48,658)
    end,
    Status=function(self,value)
        return string.sub(string.gsub(tostring(value or "none"),"_"," "),1,24)
    end,
    OnLateUpdate=function(self)
        local state=self.state
        if not state or not state.ready or not state.bag or not state.equip then return end
        local underworld=state.world_id=="underworld"
        Camera.SetPosition(underworld and 24 or 0,0);Camera.SetZoom(0.5)
        if self.source_sprite then
            local image=(state.source_health or 0)<=0 and "source_closed.png" or "source.png"
            if self.source_sprite.sprite~=image then self.source_sprite.sprite=image end
            self.source_sprite.enabled=true
        end
        if self.player_body and (state.attack_cooldown or 0)>0 and state.phase~="failed" and (state.transition_remaining or 0)==0 then
            local point=self.player_body:GetPosition()
            Image.DrawEx("attack.png",point.x,point.y,0,6.25,6.25,0.5,0.5,239,190,89,
                math.min(150,40+state.attack_cooldown*6),25)
        end
        Image.DrawUIEx("hud_top.png",0,0,255,255,255,255,1000)
        Image.DrawUIEx("hud_bottom.png",0,612,255,255,255,255,1000)
        Image.DrawUIEx(underworld and "world_underworld.png" or "world_village.png",18,14,255,255,255,255,1020)
        self:Text(underworld and "UNDERWORLD" or "VILLAGE",30,21)
        self:Text("HP "..math.max(0,state.player_health or 0).."/6",202,21)
        self:Text("ALTAR "..math.max(0,state.altar_health or 0).."/8",370,21)
        local route=state.route_closed and "closed" or (state.route_phase=="active" and "active" or "countdown")
        Image.DrawUIEx("route_"..route..".png",598,14,255,255,255,255,1020)
        self:Text(route=="closed" and "ROUTE CLOSED" or (route=="active" and "INVASION ACTIVE" or "ROUTE OPEN"),610,22)
        local seconds=math.max(0,math.ceil((state.countdown_remaining or 0)/60))
        self:Text("WAVE "..(state.wave or 0).." NEXT "..seconds,18,48)
        self:Text("INVADERS "..(state.active_invaders or 0),286,48)
        self:Text("SOURCE "..math.max(0,state.source_health or 0).."/9",490,48)
        self:Text("ATTACK "..(state.attack or 1),742,48)
        self:Text(route=="closed" and "ROUTE CLOSED - F5 TO KEEP THIS CHECKPOINT" or
            "PLANT FOOD - EQUIP SWORD - CROSS PORTAL - DESTROY SOURCE",18,74)
        local counts,equipped=self:Counts(state)
        self:Card("seed","SEED",counts.seed.." E FARM",20)
        self:Card("food","FOOD",counts.food.." Q EAT",200)
        self:Card("sword","SWORD",counts.sword..(equipped and " ON" or " BAG"),380)
        self:Text("SAVE "..string.sub(self:Status(state.save_status),1,24),572,628)
        self:Text("LOAD "..string.sub(self:Status(state.load_status),1,24),572,648)
        self:Text(string.sub(self:Status(state.last_result),1,30),572,672)
        self:Text("ARROWS MOVE  E/RMB FARM  SPACE/LMB HIT  Q EAT  1 SWORD  F5 SAVE  F9 LOAD",20,698)
        if state.phase=="failed" then
            Image.DrawUIEx("fade.png",0,0,48,8,16,180,8000)
            self:Text("FAILED - F9 LOAD CHECKPOINT",324,350,8100)
        end
        local alpha=math.max(0,math.min(1,state.transition_alpha or 0))
        if alpha>0 then Image.DrawUIEx("fade.png",0,0,0,0,0,math.floor(alpha*255+0.5),10000) end
    end
}
