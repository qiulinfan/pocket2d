-- Deterministic village-defense fixture: three waves or destroy their source.
Village = {
    health = 3, wave = 0, active = 0, spawned = 0, defeated = 0,
    gathered = 0, tree_health = 3, state = "countdown", ticks = 0,
    next_wave = 120, interval = 90, total_waves = 3,
    OnUpdate = function(self)
        self.ticks = self.ticks + 1
        if self.state == "won" or self.state == "lost" then return end
        if self.tree_health <= 0 then
            self.state = "won"
            for _, actor in ipairs(Actor.FindAll("invader")) do
                Actor.Destroy(actor)
            end
            self.active = 0
            return
        end
        if self.health <= 0 then self.state = "lost"; return end
        if self.state == "wave" and self.active == 0 then
            if self.wave == self.total_waves then self.state = "won"; return end
            self.state = "countdown"
            self.next_wave = self.ticks + self.interval
        end
        if self.state == "countdown" and self.ticks >= self.next_wave then
            self.wave = self.wave + 1
            self.state = "wave"
            for i = 1, 2 do
                local actor = Actor.Instantiate("Invader")
                local transform = actor:GetComponent("Transform")
                transform.x = 6 + i
                transform.y = 4
                actor:GetComponent("Rigidbody"):SetPosition(Vector2(6 + i, 4))
                self.active = self.active + 1
                self.spawned = self.spawned + 1
            end
        end
    end
}
