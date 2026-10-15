Combatant={
    role='',world_id='',health=0,active=false,target_kind='none',attack_remaining=0,
    Init=function(self,state,role,world,health,active)
        self.state=state;self.role=role;self.world_id=world;self.health=health;self.max_health=health
        self.body=self.actor:GetComponent('Rigidbody');self.transform=self.actor:GetComponent('Transform')
        self.sprite=self.actor:GetComponent('SpriteRenderer');self.attack_remaining=0
        self.home_x=self.transform.x;self.home_y=self.transform.y;self:SetActive(active)
    end,
    Position=function(self)
        if self.body then return self.body:GetPosition() end
        return Vector2(self.transform.x,self.transform.y)
    end,
    Stop=function(self) if self.body then self.body:SetVelocity(Vector2(0,0)) end end,
    Teleport=function(self,x,y)
        self.transform.x=x;self.transform.y=y
        if self.body then self.body:SetPosition(Vector2(x,y));self:Stop() end
    end,
    SetActive=function(self,active)
        self.active=active;self.target_kind='none'
        if self.body then self.body.enabled=active;self:Stop() end
        if self.sprite then self.sprite.enabled=active or self.role=='source' or self.role=='player' end
    end,
    Spawn=function(self)
        self.health=self.max_health;self.attack_remaining=0;self:Teleport(self.home_x,self.home_y);self:SetActive(true)
    end,
    Damage=function(self,amount)
        if not self.active or self.health<=0 then return false end
        if self.role=='player' and self.state.hurt_remaining>0 then return false end
        self.health=math.max(0,self.health-amount)
        if self.role=='player' then self.state.hurt_remaining=30 end
        if self.health==0 then
            self:SetActive(false)
            if self.role=='source' then self.state.route:Close()
            elseif self.role=='player' then self.state:Fail('player_defeated')
            else self.state.defeated=self.state.defeated+1;self.state.last_result=self.role..'_defeated' end
        end
        return true
    end,
    Tick=function(self)
        if not self.active or self.state.phase~='playing' then self:Stop();return end
        self.attack_remaining=math.max(0,self.attack_remaining-1)
        if self.role~='guardian' and self.role~='invader' then return end
        local p=self:Position();local target,crop_index
        if self.role=='guardian' then
            local player=self.state.player_body:GetPosition()
            if self.state.world_id~='underworld' or (p.x-player.x)^2+(p.y-player.y)^2>12.25 then
                self.target_kind='none';self:Stop();return
            end
            self.target_kind='player';target=player
        else
            crop_index,target=self.state.farm:NearestThreat(p.x,p.y)
            if crop_index then self.target_kind='crop'
            else self.target_kind='altar';target=Vector2(2.5,0.5) end
        end
        local dx,dy=target.x-p.x,target.y-p.y;local distance=math.sqrt(dx*dx+dy*dy)
        if distance>0.8 then
            local speed=self.role=='guardian' and 1.2 or 1.3
            self.body:SetVelocity(Vector2(dx/distance*speed,dy/distance*speed));return
        end
        self:Stop()
        if self.attack_remaining>0 then return end
        self.attack_remaining=60
        if self.target_kind=='player' then self.state.player_combat:Damage(2)
        elseif self.target_kind=='crop' then self.state.farm:DestroyCrop(crop_index)
        else
            self.state.altar_health=math.max(0,self.state.altar_health-2);self.state.last_result='altar_hit'
            if self.state.altar_health==0 then
                self.state:Fail('altar_destroyed')
            end
        end
    end
}
