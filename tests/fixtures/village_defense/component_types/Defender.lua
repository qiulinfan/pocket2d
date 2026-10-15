Defender = {
    speed = 6, held_ticks = 0, presses = 0, releases = 0, attacks = 0,
    cooldown = 0,
    OnStart = function(self)
        self.rb = self.actor:GetComponent("Rigidbody")
        self.village = Actor.Find("village"):GetComponent("Village")
    end,
    OnUpdate = function(self)
        if Input.GetKeyDown("right") then self.presses = self.presses + 1 end
        if Input.GetKeyUp("right") then self.releases = self.releases + 1 end
        if Input.GetKey("right") then self.held_ticks = self.held_ticks + 1 end
        local x, y = 0, 0
        if Input.GetKey("right") then x = x + 1 end
        if Input.GetKey("left") then x = x - 1 end
        if Input.GetKey("down") then y = y + 1 end
        if Input.GetKey("up") then y = y - 1 end
        if x ~= 0 and y ~= 0 then x = x / math.sqrt(2); y = y / math.sqrt(2) end
        self.rb:SetVelocity(Vector2(x * self.speed, y * self.speed))
        self.cooldown = math.max(0, self.cooldown - 1)
        if not Input.GetKey("space") or self.cooldown > 0 then return end
        if self.village.state == "won" or self.village.state == "lost" then return end
        self.cooldown = 12
        self.attacks = self.attacks + 1
        local p = self.rb:GetPosition()
        for _, actor in ipairs(Actor.FindAll("invader")) do
            local enemy = actor:GetComponent("Invader")
            local q = actor:GetComponent("Rigidbody"):GetPosition()
            if (p.x-q.x)^2 + (p.y-q.y)^2 <= 4 then enemy:Defeat() end
        end
        if (p.x-4)^2 + p.y^2 <= 2.25 then
            self.village.tree_health = math.max(0, self.village.tree_health - 1)
        end
    end
}
