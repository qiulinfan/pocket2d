-- Test-only cooperating gameplay callback, enabled in a temporary scene by the
-- regression harness. It performs a real heal through the native source model
-- after UI begin-drag, proving that stale preview revisions cannot transfer it.
InventoryRevisionProbe={
    fired=false,
    OnUpdate=function(self)
        if self.fired then return end
        local state=Actor.Find("inventory"):GetComponent("InventoryState")
        if not state.dragging or state.drag_source~="bag" or state.health>=3 then return end
        local slot=state.bag:GetSlot(state.drag_index)
        if slot.item_id~="herb" then return end
        local result=state.bag:Remove(state.drag_index,1)
        if result.moved==1 then
            state.consumed=state.consumed+1;state.health=state.health+1;self.fired=true;state:Publish()
        end
    end
}
