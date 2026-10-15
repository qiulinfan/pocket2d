Supply = {
    collected = false,
    OnTriggerEnter = function(self, collision)
        if self.collected or collision.other:GetName() ~= "player" then return end
        self.collected = true
        local village = Actor.Find("village"):GetComponent("Village")
        village.gathered = village.gathered + 1
        Actor.Destroy(self.actor)
    end
}
