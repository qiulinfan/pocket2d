AnimationController = {
    mode = "idle", auto_select = true, attack_finished = false,
    frame_events = 0, completed_events = 0, hit_events = 0, rest_events = 0,
    play_calls = 0, updates = 0, trace = "", last_component_key = "",
    OnStart = function(self)
        self.anim = self.actor:GetComponent("SpriteAnimation")
        self.body = self.actor:GetComponent("Rigidbody")
        self.sprite = self.actor:GetComponent("SpriteRenderer")
    end,
    OnAnimationEvent = function(self, event)
        local view=Actor.Find("view"):GetComponent("AnimationView")
        view.trace=view.trace.."C"..":"..event.type..":"..event.clip..":"..event.frame..":"..event.marker..";"
        self.trace = self.trace.."C:"..event.type..":"..event.clip..":"..event.frame..":"..event.marker..";"
        self.last_component_key = event.component_key
        if event.type == "frame" then
            self.frame_events = self.frame_events + 1
            if event.marker == "hit" then self.hit_events = self.hit_events + 1 end
            if event.marker == "rest" then self.rest_events = self.rest_events + 1 end
        elseif event.type == "completed" then
            self.completed_events = self.completed_events + 1
            if event.clip == "attack" then self.attack_finished = true end
        end
    end,
    OnUpdate = function(self)
        self.updates = self.updates + 1
        local x,y=0,0
        if Input.GetKey("right") then x=x+1 end
        if Input.GetKey("left") then x=x-1 end
        if Input.GetKey("down") then y=y+1 end
        if Input.GetKey("up") then y=y-1 end
        if x~=0 and y~=0 then x=x/math.sqrt(2);y=y/math.sqrt(2) end
        self.body:SetVelocity(Vector2(x*2,y*2))
        if x~=0 then self.sprite.scale_x=x<0 and -5 or 5 end
        if Input.GetKeyDown("d") then self.anim.enabled=not self.anim.enabled end
        if Input.GetKeyDown("p") then self.anim:Pause() end
        if Input.GetKeyDown("r") then self.anim:Resume() end
        if Input.GetKeyDown("s") then self.anim:Stop();self.mode="stopped";return end
        if Input.GetKeyDown("x") then
            self.mode="walk";self.auto_select=true;self.anim:Restart("walk");return
        end
        if Input.GetKeyDown("t") and self.anim.frame>0 then
            self.anim:Play(self.anim.current_clip);self.play_calls=self.play_calls+1
        end
        if not self.auto_select or self.mode=="stopped" or self.anim.paused or not self.anim.enabled then return end
        if self.mode=="attack" then
            if not self.attack_finished then return end
            self.attack_finished=false;self.mode="idle"
        end
        if Input.GetKeyDown("space") then
            self.mode="attack";self.anim:Restart("attack");return
        end
        self.mode=(x~=0 or y~=0) and "walk" or "idle"
        self.anim:Play(self.mode);self.play_calls=self.play_calls+1
    end
}
