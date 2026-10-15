FarmMovement = {
    speed = 6, facing_x = 1, facing_y = 0,
    OnStart = function(self) self.body = self.actor:GetComponent("Rigidbody") end,
    OnUpdate = function(self)
        local x, y = 0, 0
        if Input.GetKey("right") then x = x + 1 end
        if Input.GetKey("left") then x = x - 1 end
        if Input.GetKey("down") then y = y + 1 end
        if Input.GetKey("up") then y = y - 1 end
        if x ~= 0 then self.facing_x = x; self.facing_y = 0
        elseif y ~= 0 then self.facing_x = 0; self.facing_y = y end
        if x ~= 0 and y ~= 0 then x=x/math.sqrt(2); y=y/math.sqrt(2) end
        self.body:SetVelocity(Vector2(x*self.speed, y*self.speed))
    end
}
