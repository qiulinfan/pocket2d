AnimationView = {
    trace = "",
    OnStart = function(self) Camera.SetPosition(0,0);Camera.SetZoom(1) end,
    OnLateUpdate = function(self)
        Image.DrawEx("floor.png",0,0,0,5,5,0.5,0.5,255,255,255,255,-100)
    end
}
