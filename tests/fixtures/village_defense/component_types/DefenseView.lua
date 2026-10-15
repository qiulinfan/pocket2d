-- Diagnostic geometry and bitmap labels; gameplay stays in the same components
-- for graphical play and headless agent stepping. No external font/art needed.
DefenseView = {
    OnStart = function(self)
        Camera.SetPosition(3, 2)
        Camera.SetZoom(0.8)
        self.village = Actor.Find("village"):GetComponent("Village")
    end,
    DrawWorld = function(self, image, x, y, scale, order)
        Image.DrawEx(image, x, y, 0, scale, scale, 0.5, 0.5,
                     255, 255, 255, 255, order)
    end,
    DrawPips = function(self, x, y, value, maximum, r, g, b)
        for i = 1, maximum do
            if i <= value then
                Image.DrawUIEx("segment", x+(i-1)*20, y, r, g, b, 255, 6000)
            else
                Image.DrawUIEx("segment", x+(i-1)*20, y, 48, 63, 67, 255, 6000)
            end
        end
    end,
    OnLateUpdate = function(self)
        if Input.GetKeyDown("r") then Scene.Load("defense") end
        self:DrawWorld("ground", 3, 2, 5, -100)
        self:DrawWorld("altar", 0, 4, 4, 10)
        self:DrawWorld(self.village.tree_health > 0 and "tree" or "stump", 4, 0, 4, 10)
        local supply = Actor.Find("supply")
        if supply ~= nil then self:DrawWorld("supply", 1, 0, 3, 10) end
        local player = Actor.Find("player")
        if player ~= nil then
            local position = player:GetComponent("Rigidbody"):GetPosition()
            local defender = player:GetComponent("Defender")
            self:DrawWorld("player", position.x, position.y, 3, 20)
            if defender.cooldown >= 9 then
                self:DrawWorld("attack", position.x, position.y, 6.25, 15)
            end
        end
        for _, actor in ipairs(Actor.FindAll("invader")) do
            local position = actor:GetComponent("Rigidbody"):GetPosition()
            self:DrawWorld("invader", position.x, position.y, 3, 20)
        end

        Image.DrawUI("hud", 0, 0)
        Image.DrawUI("hud", 0, 560)
        Image.DrawUI("title", 24, 15)
        Image.DrawUI("goal", 24, 44)
        Image.DrawUI("altar_label", 490, 15)
        self:DrawPips(490, 44, self.village.health, 3, 81, 224, 213)
        Image.DrawUI("tree_label", 610, 15)
        self:DrawPips(610, 44, self.village.tree_health, 3, 204, 127, 208)
        Image.DrawUI("wave_label", 730, 15)
        self:DrawPips(730, 44, self.village.wave, 3, 230, 182, 98)
        Image.DrawUI("gather_label", 830, 15)
        self:DrawPips(830, 44, self.village.gathered, 1, 230, 182, 98)
        Image.DrawUI("controls", 24, 580)
        Image.DrawUI("attack_label", 24, 608)
        if self.village.state == "countdown" then
            Image.DrawUI("next_label", 710, 580)
            local remaining = math.max(0, math.ceil((self.village.next_wave-self.village.ticks)/60))
            Image.DrawUI("digit_"..math.min(9,remaining), 850, 580)
        elseif self.village.state == "won" then
            Image.DrawUI("won", 320, 270)
        elseif self.village.state == "lost" then
            Image.DrawUI("lost", 320, 270)
        end
    end
}
