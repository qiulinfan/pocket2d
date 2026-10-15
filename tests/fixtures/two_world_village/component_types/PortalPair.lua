PortalPair={
    armed=true,
    Init=function(self,state,world,target_name,target_world,x,y)
        self.state=state;self.world_id=world;self.target_name=target_name;self.target_world=target_world
        self.arrival_x=x;self.arrival_y=y;self.armed=true
        self.transform=self.actor:GetComponent('Transform')
    end,
    Tick=function(self)
        local state=self.state;local p=state.player_body:GetPosition()
        local near=(p.x-self.transform.x)^2+(p.y-self.transform.y)^2<=0.36
        if not near then self.armed=true end
        if not self.armed or not near or state.world_id~=self.world_id or state.transition_remaining>0
            or state.portal_grace>0 or state.phase~='playing' then return end
        self.armed=false
        local target=Actor.Find(self.target_name)
        if not target or not target:GetComponent('PortalPair') then state.last_result='missing_portal';return end
        state.transition={elapsed=0,world=self.target_world,x=self.arrival_x,y=self.arrival_y}
        state.transition_remaining=42;state.transition_alpha=0;state.last_result='portal_entered'
        state.player_combat:Stop()
    end
}
