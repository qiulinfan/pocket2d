CollisionEditProbe = {
    requested = false, before = -1, during = -1, request_frame = -1, requests = 0,
    OnCollisionEnter = function(self, collision)
        if self.requested or collision.other:GetName() ~= "terrain" then return end
        local map = Actor.Find("terrain"):GetComponent("Tilemap")
        self.before = map:GetCell("solid", 16, 9)
        map:SetCell("solid", 16, 9, 0)
        self.during = map:GetCell("solid", 16, 9)
        self.requested = true
        self.requests = self.requests + 1
        self.request_frame = Application.GetFrame()
    end
}
