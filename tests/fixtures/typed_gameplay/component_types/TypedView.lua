TypedView = {
    OnStart=function(self) Camera.SetPosition(0,0);Camera.SetZoom(1) end,
    OnLateUpdate=function(self)
        Image.DrawEx("floor.png",0,0,0,5,5,0.5,0.5,255,255,255,255,-100)
        local player=Actor.Find("player")
        local controller=player:GetComponent("TypedController")
        for i=1,controller.health do
            for x=0,17 do for y=0,5 do Image.DrawPixel(20+i*24+x,20+y,220,80,90,255) end end
        end
    end
}
