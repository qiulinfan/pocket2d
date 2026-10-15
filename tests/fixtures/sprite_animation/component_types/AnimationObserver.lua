AnimationObserver = {
    label = "A", reentry = "none", fired = false,
    frame_events = 0, completed_events = 0, hit_events = 0,
    OnAnimationEvent = function(self, event)
        local view=Actor.Find("view"):GetComponent("AnimationView")
        view.trace=view.trace..self.label..":"..event.type..":"..event.clip..":"..event.frame..":"..event.marker..";"
        local controller=self.actor:GetComponent("AnimationController")
        controller.trace=controller.trace..self.label..":"..event.type..":"..event.clip..":"..event.frame..":"..event.marker..";"
        if event.type=="frame" then
            self.frame_events=self.frame_events+1
            if event.marker=="hit" then self.hit_events=self.hit_events+1 end
        elseif event.type=="completed" then self.completed_events=self.completed_events+1 end
        if self.label=="A" and not self.fired and self.reentry~="none" and event.marker=="swing" then
            self.fired=true;controller.auto_select=false;controller.mode="external"
            local anim=self.actor:GetComponent("SpriteAnimation")
            if self.reentry=="pause" then anim:Pause()
            elseif self.reentry=="stop" then anim:Stop()
            elseif self.reentry=="restart" then anim:Restart("walk")
            elseif self.reentry=="remove" then self.actor:RemoveComponent(anim)
            elseif self.reentry=="destroy" then Actor.Destroy(self.actor) end
        end
    end
}
