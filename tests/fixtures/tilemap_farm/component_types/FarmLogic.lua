FarmLogic = {
    tick = 0, harvested = 0, planted_count = 0, mature_count = 0,
    planted_tick = -1, mature_at_tick = -1, walls_removed = 0,
    OnStart = function(self)
        self.map = Actor.Find("terrain"):GetComponent("Tilemap")
        self.player = Actor.Find("player")
        self.growing = {}
        self.mature = {}
    end,
    OnUpdate = function(self)
        self.tick = self.tick + 1
        for index, deadline in pairs(self.growing) do
            if self.tick >= deadline then
                local x, y = index % 64, math.floor(index / 64)
                self.map:SetCell("ground", x, y, 4)
                self.growing[index] = nil
                self.mature[index] = true
                self.mature_count = self.mature_count + 1
            end
        end
        local position = self.player:GetComponent("Rigidbody"):GetPosition()
        local cell = self.map:WorldToCell(position.x, position.y)
        local index = cell.y * 64 + cell.x
        if Input.GetKeyDown("e") then
            local tile = self.map:GetCell("ground", cell.x, cell.y)
            if tile == 2 then
                if self.map:SetCell("ground", cell.x, cell.y, 3) then
                    self.growing[index] = self.tick + 120
                    self.planted_tick = self.tick
                    self.mature_at_tick = self.tick + 120
                    self.planted_count = self.planted_count + 1
                end
            elseif tile == 4 or (tile == 3 and self.mature[index]) then
                self.map:SetCell("ground", cell.x, cell.y, 2)
                self.mature[index] = nil
                self.mature_count = self.mature_count - 1
                self.planted_count = self.planted_count - 1
                self.harvested = self.harvested + 1
            end
        end
        if Input.GetKeyDown("space") then
            local movement = self.player:GetComponent("FarmMovement")
            local x, y = cell.x + movement.facing_x, cell.y + movement.facing_y
            local tile = self.map:GetCell("solid", x, y)
            if tile ~= nil and tile ~= 0 and self.map:SetCell("solid", x, y, 0) then
                self.walls_removed = self.walls_removed + 1
            end
        end
    end
}
