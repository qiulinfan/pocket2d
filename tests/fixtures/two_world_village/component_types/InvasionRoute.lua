InvasionRoute={
    Init=function(self,state)
        self.state=state;state.route_id='island_1';state.route_phase='countdown'
        state.route_closed=false;state.wave=1;state.countdown_remaining=900;state.active_invaders=0
    end,
    Close=function(self)
        local state=self.state
        if state.route_closed then return end
        state.route_closed=true;state.route_phase='closed';state.countdown_remaining=0
        for _,enemy in ipairs(state.invaders) do enemy:SetActive(false) end
        state.active_invaders=0;state.last_result='route_closed'
    end,
    Tick=function(self)
        local state=self.state
        if state.phase~='playing' or state.route_closed then return end
        if state.route_phase=='countdown' then
            state.countdown_remaining=state.countdown_remaining-1
            if state.countdown_remaining<=0 then
                state.countdown_remaining=0;state.route_phase='active'
                for _,enemy in ipairs(state.invaders) do enemy:Spawn() end
                state.active_invaders=3;state.last_result='wave_started'
            end
        elseif state.route_phase=='active' then
            local count=0;for _,enemy in ipairs(state.invaders) do if enemy.active then count=count+1 end end
            state.active_invaders=count
            if count==0 then
                state.wave=math.min(1000000,state.wave+1);state.countdown_remaining=600
                state.route_phase='countdown';state.last_result='wave_cleared'
            end
        end
    end,
    Apply=function(self,data)
        local state=self.state
        state.route_closed=data.closed;state.wave=data.wave;state.countdown_remaining=data.remaining_ticks
        state.route_phase=data.closed and 'closed' or 'countdown';state.altar_health=data.altar_health
        state.source_combat.health=data.source_health;state.source_combat:SetActive(data.source_health>0)
        for _,enemy in ipairs(state.invaders) do enemy:SetActive(false);enemy.health=0;enemy.attack_remaining=0 end
        state.active_invaders=0
    end
}
