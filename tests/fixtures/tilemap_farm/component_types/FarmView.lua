FarmView = {
    OnStart = function(self) Camera.SetPosition(4.5,2); Camera.SetZoom(1) end,
    OnLateUpdate = function(self)
        if Input.GetKeyDown("r") then Scene.Load("farm") end
        -- Native tile rendering provides the map; labels are authored geometry.
        local farm = self.actor:GetComponent("FarmLogic")
        Image.DrawUI("hud",0,0)
        Image.DrawUI("controls",16,14)
        Image.DrawUI("harvest_label",16,48)
        Image.DrawUI("planted_label",470,48)
        for i=1,math.min(farm.harvested,40) do
            Image.DrawUIEx("counter",116+(i-1)*8,50,70,220,205,255,1)
        end
        for i=1,math.min(farm.planted_count,16) do
            Image.DrawUIEx("counter",570+(i-1)*8,50,220,175,80,255,1)
        end
    end
}
