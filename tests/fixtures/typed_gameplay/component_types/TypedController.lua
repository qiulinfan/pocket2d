-- Handwritten reference, intentionally kept independent of generated TS output.
TypedController = {
    enabled=true,
    health=3,dead=false,paused=false,ready=false,mode="idle",facing=1,
    attacks=0,damage_events=0,heal_events=0,frame_events=0,completed_events=0,hit_events=0,
    attack_finished=false,trace="",save_status="none",load_status="none",save_count=0,load_count=0,
    OnStart=function(self)
        local player=Actor.Find("player")
        if player==nil then return end
        self.body=player:GetComponent("Rigidbody")
        self.anim=player:GetComponent("SpriteAnimation")
        self.sprite=player:GetComponent("SpriteRenderer")
        self.ready=self.body~=nil and self.anim~=nil and self.sprite~=nil
    end,
    OnAnimationEvent=function(self,event)
        self.trace=self.trace..event.type..":"..event.clip..":"..event.frame..":"..event.marker..";"
        if event.type=="frame" then
            self.frame_events=self.frame_events+1
            if event.marker=="hit" then self.hit_events=self.hit_events+1 end
        elseif event.type=="completed" then
            self.completed_events=self.completed_events+1
            if event.clip=="attack" then self.attack_finished=true end
        end
    end,
    ValidSave=function(self,data)
        if type(data)~="table" then return false end
        local count=0
        for key in pairs(data) do
            if key~="health" and key~="position" and key~="facing" then return false end
            count=count+1
        end
        if count~=3 or type(data.health)~="number" or data.health~=math.floor(data.health)
            or data.health<0 or data.health>3 or (data.facing~=1 and data.facing~=-1)
            or type(data.position)~="table" then return false end
        count=0
        for key in pairs(data.position) do if key~="x" and key~="y" then return false end;count=count+1 end
        if count~=2 then return false end
        for _,value in pairs(data.position) do
            if type(value)~="number" or value~=value or math.abs(value)>1000 then return false end
        end
        return true
    end,
    SaveGame=function(self)
        local p=self.body:GetPosition()
        local data={health=self.health,position={x=p.x,y=p.y},facing=self.facing}
        if not self:ValidSave(data) then self.save_status="invalid_game_schema";return end
        local result=Save.Write("main",1,data)
        self.save_status=result.ok and "saved" or result.code
        if result.ok then self.save_count=self.save_count+1 end
    end,
    LoadGame=function(self)
        local result=Save.Read("main")
        if not result.ok then self.load_status=result.code;return end
        if result.schema_version~=1 then self.load_status="unsupported_game_schema";return end
        if not self:ValidSave(result.data) then self.load_status="invalid_game_schema";return end
        local data=result.data
        self.health=data.health;self.dead=self.health==0;self.facing=data.facing
        self.body:SetPosition(Vector2(data.position.x,data.position.y));self.body:SetVelocity(Vector2(0,0))
        self.sprite.scale_x=self.facing*5;self.paused=false;self.attack_finished=false
        self.mode=self.dead and "dead" or "idle"
        if self.dead then self.anim:Stop() else self.anim:Restart("idle") end
        self.load_status="loaded";self.load_count=self.load_count+1
    end,
    OnUpdate=function(self)
        if not self.ready then return end
        if Input.GetKeyDown("f9") then self:LoadGame();return end
        if Input.GetKeyDown("f5") then self.body:SetVelocity(Vector2(0,0));self:SaveGame();return end
        if Input.GetKeyDown("h") and self.health>0 then
            self.health=math.max(0,self.health-1);self.damage_events=self.damage_events+1
        end
        if self.health==0 then
            if not self.dead then self.anim:Stop() end
            self.dead=true;self.mode="dead";self.body:SetVelocity(Vector2(0,0));return
        end
        if Input.GetKeyDown("g") and self.health<3 then
            self.health=math.min(3,self.health+1);self.heal_events=self.heal_events+1
        end
        if Input.GetKeyDown("p") then self.paused=true;self.anim:Pause() end
        if Input.GetKeyDown("r") then self.paused=false;self.anim:Resume() end
        if self.paused then self.body:SetVelocity(Vector2(0,0));return end
        local x,y=0,0
        if Input.GetKey("right") then x=x+1 end
        if Input.GetKey("left") then x=x-1 end
        if Input.GetKey("down") then y=y+1 end
        if Input.GetKey("up") then y=y-1 end
        if x~=0 and y~=0 then x=x/math.sqrt(2);y=y/math.sqrt(2) end
        self.body:SetVelocity(Vector2(x*2,y*2))
        if x~=0 then self.facing=x<0 and -1 or 1;self.sprite.scale_x=self.facing*5 end
        if self.mode=="attack" then
            if not self.attack_finished then return end
            self.attack_finished=false;self.mode="idle"
        end
        if Input.GetKeyDown("space") then
            self.attacks=self.attacks+1;self.mode="attack";self.anim:Restart("attack");return
        end
        self.mode=(x~=0 or y~=0) and "walk" or "idle"
        self.anim:Play(self.mode)
    end
}
