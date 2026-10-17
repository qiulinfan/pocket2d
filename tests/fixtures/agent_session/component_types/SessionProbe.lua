-- Records the input a scripted session sends and reports it via OnAgentObserve.
SessionProbe = {
  jumps = 0, held_right = 0, key_presses = 0, moved_x = 0, clicks = 0, click_x = 0, click_y = 0,
  saved = false, saves = 0,
  OnStart = function(self)
    Camera.SetPosition(2, 1)
  end,
  OnUpdate = function(self)
    if Input.GetActionDown("jump") then self.jumps = self.jumps + 1 end
    if Input.GetKey("d") then self.held_right = self.held_right + 1 end
    if Input.GetKeyDown("k") then self.key_presses = self.key_presses + 1 end
    self.moved_x = self.moved_x + Input.GetActionVector("move").x
    if Input.GetMouseButtonDown(1) then
      local mouse = Input.GetMousePosition()
      local world = Camera.ScreenToWorld(mouse.x, mouse.y)
      self.clicks = self.clicks + 1
      self.click_x = world.x
      self.click_y = world.y
    end
    if Input.GetKeyDown("f5") then
      self.saves = self.saves + 1
      self.saved = Save.Write("slot", 1, {jumps = self.jumps}).ok
    end
  end,
  OnAgentObserve = function(self)
    return {jumps = self.jumps, held_right = self.held_right, key_presses = self.key_presses,
            moved_x = self.moved_x, clicks = self.clicks, click_x = self.click_x, click_y = self.click_y,
            saved = self.saved, saves = self.saves}
  end
}
