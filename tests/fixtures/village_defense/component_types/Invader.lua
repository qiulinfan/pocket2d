Invader = {
    dead = false,
    OnStart = function(self)
        self.rb = self.actor:GetComponent("Rigidbody")
        self.village = Actor.Find("village"):GetComponent("Village")
    end,
    OnUpdate = function(self)
        if self.dead then return end
        if self.village.state == "won" or self.village.state == "lost" then
            self.rb:SetVelocity(Vector2(0, 0)); return
        end
        local p = self.rb:GetPosition()
        local dx, dy = -p.x, 4-p.y
        local length = math.sqrt(dx*dx + dy*dy)
        if length > 0 then self.rb:SetVelocity(Vector2(dx/length*2, dy/length*2)) end
    end,
    Defeat = function(self)
        if self.dead then return end
        self.dead = true
        self.village.active = self.village.active - 1
        self.village.defeated = self.village.defeated + 1
        Actor.Destroy(self.actor)
    end,
    OnCollisionEnter = function(self, collision)
        if collision.other:GetName() ~= "altar" or self.dead then return end
        self.village.health = self.village.health - 1
        self:Defeat()
    end
}
