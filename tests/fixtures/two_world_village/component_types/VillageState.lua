VillageState={
    ready=false,tick=0,world_id='village',route_id='island_1',phase='playing',route_phase='countdown',
    countdown_remaining=900,wave=1,active_invaders=0,route_closed=false,
    player_health=6,altar_health=8,source_health=9,attack=1,attack_cooldown=0,hurt_remaining=0,
    transition_remaining=0,transition_alpha=0,portal_grace=0,selected=3,
    last_result='ready',save_status='none',load_status='none',last_error='',
    bag_state='',equipment_state='',crop_state='',seed_count=0,food_count=0,
    crops_growing=0,crops_mature=0,planted=0,harvested=0,consumed=0,crops_destroyed=0,defeated=0,attacks=0,
    OnStart=function(self)
        self.catalog=Inventory.Open('village.items.json');self.bag=self.catalog:Create(30)
        self.equip=self.catalog:Create(4,{'subweapon','head','chest','legs'})
        self.food_def=self.catalog:GetItem('food');self.sword_def=self.catalog:GetItem('sword')
        local function empty(n)
            local slots={};for i=1,n do slots[i]={item_id='',count=0,instance_id=''} end;return slots
        end
        local bag,equipment=empty(30),empty(4)
        bag[1]={item_id='seed',count=8,instance_id=''}
        bag[3]={item_id='sword',count=1,instance_id='sword-island-1'}
        assert(Inventory.RestoreGroup({{inventory=self.bag,slots=bag},{inventory=self.equip,slots=equipment}}).ok)
        self.player=Actor.Find('player');self.player_body=self.player:GetComponent('Rigidbody')
        self.player_combat=self.player:GetComponent('Combatant');self.player_combat:Init(self,'player','village',6,true)
        self.source=Actor.Find('source');self.source_combat=self.source:GetComponent('Combatant')
        self.source_combat:Init(self,'source','underworld',9,true)
        self.guardian_actor=Actor.Find('guardian');self.guardian=self.guardian_actor:GetComponent('Combatant')
        self.guardian:Init(self,'guardian','underworld',6,true)
        self.invaders={};self.combatants={self.player_combat,self.source_combat,self.guardian}
        for i=1,3 do
            local enemy=Actor.Find('invader_'..i):GetComponent('Combatant');enemy:Init(self,'invader','village',3,false)
            self.invaders[i]=enemy;self.combatants[#self.combatants+1]=enemy
        end
        self.map_actors={Actor.Find('village_map'),Actor.Find('underworld_map')}
        self.maps={self.map_actors[1]:GetComponent('Tilemap'),self.map_actors[2]:GetComponent('Tilemap')}
        self.farm=self.actor:GetComponent('FarmRules');self.farm:Init(self)
        self.route=self.actor:GetComponent('InvasionRoute');self.route:Init(self)
        self.portal_actors={Actor.Find('village_portal'),Actor.Find('underworld_portal')}
        self.portals={self.portal_actors[1]:GetComponent('PortalPair'),self.portal_actors[2]:GetComponent('PortalPair')}
        self.portals[1]:Init(self,'village','underworld_portal','underworld',19.75,2.5)
        self.portals[2]:Init(self,'underworld','village_portal','village',4.25,2.5)
        self.save=self.actor:GetComponent('VillageSave');assert(self.save,'missing save component')
        self.ready=true;self:Publish()
    end,
    Fail=function(self,reason)
        self.phase='failed';self.transition=nil;self.transition_remaining=0;self.transition_alpha=0
        self.last_result=reason;self:StopAllBodies()
    end,
    FindItem=function(self,id)
        for i,slot in ipairs(self.bag:Snapshot()) do if slot.item_id==id and slot.count>0 then return i end end
        return nil
    end,
    StopAllBodies=function(self)
        if not self.combatants then return end
        for _,combat in ipairs(self.combatants) do if combat.actor:IsValid() then combat:Stop() end end
    end,
    UpdateAttack=function(self)
        local slot=self.equip:GetSlot(1)
        self.attack=1+(slot.item_id=='sword' and self.sword_def.attack or 0)
    end,
    ToggleSword=function(self)
        local result
        if self.equip:GetSlot(1).count>0 then result=self.equip:TransferAll(1,self.bag)
        else
            local index=self:FindItem('sword')
            if not index then self.last_result='no_sword';return end
            result=self.bag:Move(index,self.equip,1,0,true)
        end
        self.last_result=result.ok and 'equipment_changed' or result.code;self:UpdateAttack()
    end,
    Eat=function(self)
        if self.player_combat.health>=6 then self.last_result='health_full';return end
        local index=self:FindItem('food')
        if not index then self.last_result='no_food';return end
        local result=self.bag:Remove(index,1)
        if result.ok then
            self.player_combat.health=math.min(6,self.player_combat.health+self.food_def.health_restore)
            self.consumed=self.consumed+1;self.last_result='food_used'
        else self.last_result=result.code end
    end,
    Melee=function(self)
        if self.attack_cooldown>0 then return end
        self.attack_cooldown=12;self.attacks=self.attacks+1
        local p=self.player_body:GetPosition();local hit=false
        for _,target in ipairs(self.combatants) do
            if target.role~='player' and target.active and target.world_id==self.world_id then
                local q=target:Position()
                if (p.x-q.x)^2+(p.y-q.y)^2<=1.96 then hit=target:Damage(self.attack) or hit end
            end
        end
        if not self.route_closed then self.last_result=hit and 'attack_hit' or 'attack_miss' end
    end,
    MouseInWorld=function(self)
        local mouse=Input.GetMousePosition()
        return mouse.x>=0 and mouse.x<960 and mouse.y>=96 and mouse.y<612
    end,
    MovePlayer=function(self)
        if self.transition_remaining>0 or self.phase~='playing' then self.player_combat:Stop();return end
        local x,y=0,0
        if Input.GetKey('right') then x=x+1 end;if Input.GetKey('left') then x=x-1 end
        if Input.GetKey('down') then y=y+1 end;if Input.GetKey('up') then y=y-1 end
        if x~=0 and y~=0 then x=x/math.sqrt(2);y=y/math.sqrt(2) end
        self.player_body:SetVelocity(Vector2(x*4,y*4))
    end,
    TickTransition=function(self)
        if not self.transition then return end
        local transition=self.transition;transition.elapsed=transition.elapsed+1
        local elapsed=transition.elapsed;self.transition_remaining=42-elapsed
        if elapsed<=18 then self.transition_alpha=elapsed/18
        elseif elapsed<=24 then self.transition_alpha=1
        else self.transition_alpha=(42-elapsed)/18 end
        if elapsed==24 then
            self.world_id=transition.world;self.player_combat.world_id=transition.world
            self.player_combat:Teleport(transition.x,transition.y)
        end
        if elapsed==42 then
            self.transition=nil;self.transition_remaining=0;self.transition_alpha=0;self.portal_grace=9
            for _,portal in ipairs(self.portals) do portal.armed=false end
            self.last_result='portal_arrived'
        end
    end,
    IsCheckpointStable=function(self,for_load)
        if not self.ready then return false,'not_ready' end
        if self.transition_remaining>0 then return false,'busy' end
        if for_load and self.phase=='failed' then return true,'' end
        if self.phase~='playing' or self.route_phase=='active' then return false,'busy' end
        return true,''
    end,
    CaptureSaveState=function(self)
        local p=self.player_body:GetPosition();local guardian=self.guardian:Position()
        return {world_id=self.world_id,player={x=p.x,y=p.y,health=self.player_combat.health},
            bag=self.bag:Snapshot(),equipment=self.equip:Snapshot(),crops=self.farm:Capture(),
            route={closed=self.route_closed,wave=self.wave,remaining_ticks=self.countdown_remaining,
                altar_health=self.altar_health,source_health=self.source_combat.health},
            guardian={x=guardian.x,y=guardian.y,health=self.guardian.health},
            stats={planted=self.planted,harvested=self.harvested,consumed=self.consumed,
                crops_destroyed=self.crops_destroyed,defeated=self.defeated}}
    end,
    CanApplySaveState=function(self,data)
        local stable,code=self:IsCheckpointStable(true);if not stable then return false,code end
        local function current(actor,kind,component)
            return actor and actor:IsValid() and actor:GetComponent(kind)==component
        end
        if not current(self.player,'Rigidbody',self.player_body)
            or not current(self.player,'Combatant',self.player_combat)
            or not current(self.source,'Combatant',self.source_combat) then return false,'world_unavailable' end
        for i,actor in ipairs(self.portal_actors) do
            if not current(actor,'PortalPair',self.portals[i]) then return false,'missing_portal' end
        end
        for i,actor in ipairs(self.map_actors) do
            if not current(actor,'Tilemap',self.maps[i]) or self.maps[i].width~=16 or self.maps[i].height~=10
                or self.maps[i].cell_size~=1 then return false,'world_unavailable' end
        end
        for _,combat in ipairs(self.combatants) do
            if not current(combat.actor,'Combatant',combat) then return false,'world_unavailable' end
            if combat.body and not current(combat.actor,'Rigidbody',combat.body) then return false,'world_unavailable' end
        end
        for _,pickup in ipairs(self.farm.pickups) do
            if not current(pickup.actor,'SpriteRenderer',pickup.sprite) then return false,'world_unavailable' end
        end
        return true,''
    end,
    ApplySaveState=function(self,data)
        local ready,code=self:CanApplySaveState(data);if not ready then return false,code end
        local restored=Inventory.RestoreGroup({{inventory=self.bag,slots=data.bag},{inventory=self.equip,slots=data.equipment}})
        if not restored.ok then return false,'invalid_inventory' end
        -- Save validated every field before this boundary. Only existing fixed
        -- handles/data are used below; unexpected native failure is not rollback.
        self.world_id=data.world_id;self.phase='playing';self.transition=nil;self.transition_remaining=0;self.transition_alpha=0
        self.attack_cooldown=0;self.hurt_remaining=0;self.portal_grace=9
        self.player_combat.health=data.player.health;self.player_combat.world_id=data.world_id
        self.player_combat:SetActive(true);self.player_combat:Teleport(data.player.x,data.player.y)
        self.guardian.health=data.guardian.health;self.guardian.attack_remaining=0
        self.guardian:SetActive(data.guardian.health>0);self.guardian:Teleport(data.guardian.x,data.guardian.y)
        self.route:Apply(data.route);self.farm:Apply(data.crops)
        self.planted=data.stats.planted;self.harvested=data.stats.harvested;self.consumed=data.stats.consumed
        self.crops_destroyed=data.stats.crops_destroyed;self.defeated=data.stats.defeated
        for _,portal in ipairs(self.portals) do portal.armed=false end
        self:UpdateAttack();self:StopAllBodies();self.last_result='checkpoint_loaded';self:Publish();return true,''
    end,
    Publish=function(self)
        local function encode(inventory)
            local parts={};for _,slot in ipairs(inventory:Snapshot()) do
                parts[#parts+1]=slot.item_id..','..slot.count..','..slot.instance_id
            end;return table.concat(parts,';')
        end
        self.bag_state=encode(self.bag);self.equipment_state=encode(self.equip)
        self.seed_count=0;self.food_count=0
        for _,slot in ipairs(self.bag:Snapshot()) do
            if slot.item_id=='seed' then self.seed_count=self.seed_count+slot.count end
            if slot.item_id=='food' then self.food_count=self.food_count+slot.count end
        end
        self.player_health=self.player_combat.health;self.source_health=self.source_combat.health
        self.crops_growing=0;self.crops_mature=0;local crops={}
        for _,crop in ipairs(self.farm.crops) do
            if crop.stage==3 then self.crops_growing=self.crops_growing+1 end
            if crop.stage==4 then self.crops_mature=self.crops_mature+1 end
            crops[#crops+1]=crop.stage..'/'..crop.remaining_ticks..'/'..crop.drop_count
        end
        self.crop_state=table.concat(crops,';')
        local active=0;for _,enemy in ipairs(self.invaders) do if enemy.active then active=active+1 end end
        self.active_invaders=active
    end,
    OnUpdate=function(self)
        if not self.ready then return end
        -- These are the only save/load input consumers. Motion is explicitly
        -- quiesced even on rejection; no gameplay clock/actions run this tick.
        if Input.GetKeyDown('f9') then self:StopAllBodies();self.save:LoadGame(self);self:Publish();return end
        if Input.GetKeyDown('f5') then self:StopAllBodies();self.save:SaveGame(self);self:Publish();return end
        if self.phase~='playing' then self:StopAllBodies();self:Publish();return end
        self.tick=self.tick+1;self.attack_cooldown=math.max(0,self.attack_cooldown-1)
        self.hurt_remaining=math.max(0,self.hurt_remaining-1);self.portal_grace=math.max(0,self.portal_grace-1)
        self.farm:Tick()
        for _,portal in ipairs(self.portals) do if portal.actor:IsValid() then portal:Tick() end end
        self:TickTransition();self:MovePlayer()
        if self.transition_remaining==0 then
            if Input.GetKeyDown('1') then self:ToggleSword() end
            if Input.GetKeyDown('q') then self:Eat() end
            if Input.GetKeyDown('e') or (self:MouseInWorld() and Input.GetMouseButtonDown(3)) then self.farm:Interact() end
            if Input.GetKey('space') or (self:MouseInWorld() and Input.GetMouseButton(1)) then self:Melee() end
        end
        self.guardian:Tick();for _,enemy in ipairs(self.invaders) do enemy:Tick() end
        self.route:Tick();self:Publish()
    end
}
