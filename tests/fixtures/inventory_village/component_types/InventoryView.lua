InventoryView={
    OnStart=function(self) Camera.SetPosition(0,2.5);Camera.SetZoom(1) end,
    Text=function(self,text,x,y)
        text=string.upper(tostring(text))
        for i=1,#text do local char=string.sub(text,i,i)
            if char=="." then Image.DrawUI("glyph_dot.png",x+(i-1)*12,y)
            elseif string.match(char,"[A-Z0-9_-]") then Image.DrawUI("glyph_"..char..".png",x+(i-1)*12,y) end
        end
    end,
    Slot=function(self,state,container,index,x,y)
        Image.DrawUI("slot.png",x,y)
        local slot=state:Container(container):GetSlot(index)
        if slot.count>0 then
            local alpha=(state.dragging and state.drag_source==container and state.drag_index==index) and 100 or 255
            Image.DrawUIEx(slot.item_id..".png",x+2,y+2,255,255,255,alpha,1)
            if slot.count>1 then self:Text(slot.count,x+38,y+35) end
        end
        if state.focus==container and state.focus_index==index then Image.DrawUIEx("frame.png",x,y,240,195,77,255,2) end
        if container=="bag" and state.hotbar_map[state.selected]==index then Image.DrawUIEx("frame.png",x,y,70,207,225,255,3) end
    end,
    OnLateUpdate=function(self)
        local state=self.actor:GetComponent("InventoryState");if not state.ready then return end
        Image.DrawEx("floor.png",0,0,0,5,5,0.5,0.5,255,255,255,255,-100)
        self:Text("BAG",40,182);self:Text("CHEST",500,182)
        for i=1,30 do self:Slot(state,"bag",i,40+(i-1)%6*58,210+math.floor((i-1)/6)*58) end
        for i=1,12 do self:Slot(state,"chest",i,500+(i-1)%4*58,210+math.floor((i-1)/4)*58) end
        for i,label in ipairs({"HAND","HEAD","BODY","LEGS"}) do self:Text(label,500+(i-1)*58,406);self:Slot(state,"equipment",i,500+(i-1)*58,430) end
        self:Text("HOTBAR",40,526)
        for i=1,9 do
            self:Slot(state,"bag",state.hotbar_map[i],40+(i-1)*58,550)
            if i==state.selected then Image.DrawUIEx("frame.png",40+(i-1)*58,550,70,207,225,255,4) end
            self:Text(i,60+(i-1)*58,606)
        end
        self:Text("HP",20,20);for i=1,state.health do Image.DrawUIEx("bar.png",60+i*16,20,224,82,91,255,1) end
        self:Text("SPEED "..state.speed.." ATTACK "..state.attack.." DEFENSE "..state.defense,250,20)
        self:Text("PILE "..state.world_remaining.." TARGET "..state.target_health,20,48)
        self:Text("SAVE "..state.save_status,500,508);self:Text("LOAD "..state.load_status,500,528)
        self:Text("WASD MOVE  E PICKUP  U USE  Q UNEQUIP  H DAMAGE  SPACE HIT",20,646)
        self:Text("TAB ARROWS ENTER KEYBOARD  SHIFT SPLIT  ESC CANCEL  F5 F9 SAVE LOAD",20,670)
        self:Text(string.sub(state.last_result,1,18),740,210)
        self:Text(string.sub(state.last_result,19,36),740,228)
        if state.dragging then
            local slot=state:Container(state.drag_source):GetSlot(state.drag_index)
            if slot.count>0 then local mouse=Input.GetMousePosition()
                Image.DrawUIEx(slot.item_id..".png",mouse.x-24,mouse.y-24,255,255,255,190,20)
                self:Text(state.drag_count,mouse.x+12,mouse.y+12)
            end
        end
    end
}
