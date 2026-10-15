InventoryPickup={
    remaining=8,
    OnTriggerEnter=function(self,collision)
        if collision.other:GetName()=="player" then
            Actor.Find("inventory"):GetComponent("InventoryState"):TryPickup()
        end
    end
}
