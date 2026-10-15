FarmView = {
    OnStart=function(self) Camera.SetPosition(4.5,2);Camera.SetZoom(1) end,
    OnLateUpdate=function(self)
        local farm=self.actor:GetComponent("FarmLogic")
        Image.DrawUI("hud",0,0);Image.DrawUI("controls",16,14)
        Image.DrawUI("harvest_label",16,48)
        for i=1,math.min(farm.harvested,20) do Image.DrawUIEx("counter",116+(i-1)*8,50,70,220,205,255,1) end
        Image.DrawUI("save_label",300,48);Image.DrawUI("load_label",420,48);Image.DrawUI("irrigation_label",550,48)
        local saved=farm.save_status=="saved"
        local loaded=farm.load_status=="loaded"
        Image.DrawUIEx("counter",370,50,saved and 70 or 200,saved and 220 or 70,70,255,1)
        Image.DrawUIEx("counter",490,50,loaded and 70 or 200,loaded and 220 or 70,70,255,1)
        Image.DrawUIEx("counter",690,50,farm.irrigation_unlocked and 70 or 200,farm.irrigation_unlocked and 220 or 70,70,255,1)
    end
}
