#include "shared/inventory/InventoryModel.h"
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>

namespace {
using namespace PocketInventory;
void Check(bool value,const std::string &message) { if(!value) throw std::runtime_error(message); }
std::shared_ptr<const ItemCatalog::Data> Catalog() {
    ItemCatalog::Data data;
    data.items={{"ore","Ore","","material","",10},
                {"fruit","Fruit","","food","",5,3},
                {"sword","Sword","","weapon","subweapon",1,0,10},
                {"bow","Bow","","weapon","subweapon",1,0,8},
                {"hat","Hat","","armor","head",1,0,0,2},
                {"vest","Vest","","armor","chest",1,0,0,3},
                {"boots","Boots","","armor","legs",1,0,0,1,0.5}};
    ItemCatalog::ValidateData(data);
    return std::make_shared<const ItemCatalog::Data>(std::move(data));
}
void RejectConstructor(const std::function<void()> &operation) {
    bool rejected=false; try { operation(); } catch(const std::invalid_argument &) { rejected=true; }
    Check(rejected,"invalid model constructor accepted");
}
struct Snapshot {
    std::vector<Slot> slots;
    std::uint64_t revision;
    explicit Snapshot(const Model &model):slots(model.Slots()),revision(model.Revision()) {}
    void Unchanged(const Model &model) const {
        Check(slots==model.Slots() && revision==model.Revision(),"failed/no-op operation changed inventory or revision");
    }
};
using Totals=std::map<std::string,std::int64_t>;
Totals Count(const Model &a,const Model &b) {
    Totals counts;
    for(const auto *model:{&a,&b}) for(const auto &slot:model->Slots())
        if(slot.count) counts[slot.item_id]+=slot.count;
    return counts;
}
void ConstructionAndValidation() {
    auto catalog=Catalog();
    RejectConstructor([&]{Model bad(catalog,0);}); RejectConstructor([&]{Model bad(catalog,101);});
    RejectConstructor([&]{Model bad(nullptr,1);});
    RejectConstructor([&]{Model bad(catalog,2,{"head"});});
    RejectConstructor([&]{Model bad(catalog,1,{"unknown"});});
    Model dotted(catalog,1); Check(dotted.AddUpTo({"sword",1,"instance.1"}).ok,"catalog-compatible dotted instance ID rejected");
    Model bag(catalog,100); Check(bag.Capacity()==100 && bag.Filters().size()==100 && bag.Revision()==0,"initial model shape");
    for(const Slot &request:std::vector<Slot>{{"missing",1,""},{"ore",0,""},{"ore",-1,""},
            {"ore",1,"unexpected"},{"sword",1,""},{"sword",2,"unique"},{"sword",1,"bad/path"},
            {"sword",1,"_leading"},{"sword",1,std::string(65,'a')},{"sword",1,std::string("bad\0id",6)}}) {
        Snapshot before(bag); Check(!bag.AddUpTo(request).ok,"invalid add accepted"); before.Unchanged(bag);
    }
    auto malformed=bag.Slots(); malformed[99]={"ore",11,""};
    Check(!bag.ValidateSnapshot(malformed).ok,"overfull restored stack accepted");
    malformed[99]={"",0,"orphan"}; Check(!bag.ValidateSnapshot(malformed).ok,"noncanonical empty accepted");
    malformed.pop_back(); Check(!bag.ValidateSnapshot(malformed).ok,"short snapshot accepted");
    ItemCatalog::Data empty; ItemCatalog::ValidateData(empty);
    Model empty_catalog(std::make_shared<const ItemCatalog::Data>(empty),1);
    Check(!empty_catalog.AddUpTo({"ore",1,""}).ok,"empty catalog accepted an unknown item");
}
void AdditionAndPartialPickup() {
    auto catalog=Catalog(); Model bag(catalog,2),world(catalog,1);
    Check(bag.AddUpTo({"ore",8,""}).moved==8,"seed initial stack");
    auto result=bag.AddUpTo({"ore",15,""});
    Check(result.ok && result.changed && result.moved==12 && result.remaining==3,"partial add receipt");
    Check(bag.Slots()[0].count==10 && bag.Slots()[1].count==10 && bag.Revision()==2,"stack-first add order/revision");
    Snapshot before(bag); result=bag.AddUpTo({"ore",4,""});
    Check(!result.ok && !result.changed && result.moved==0 && result.remaining==4,"full add receipt"); before.Unchanged(bag);
    Check(bag.RemoveExact(1,4).ok && world.AddUpTo({"ore",9,""}).ok,"prepare world pickup");
    const auto total=Count(bag,world); const auto bag_revision=bag.Revision(),world_revision=world.Revision();
    result=world.TransferAll(0,bag);
    Check(result.ok && result.moved==4 && result.remaining==5 && world.Slots()[0].count==5,"partial pickup lost remainder");
    Check(Count(bag,world)==total && bag.Revision()==bag_revision+1 && world.Revision()==world_revision+1,"pickup conservation/one commit");
    Snapshot full(bag),remainder(world); result=world.TransferAll(0,bag);
    Check(!result.ok && result.remaining==5,"full pickup must not erase world stack"); full.Unchanged(bag); remainder.Unchanged(world);
    Check(!world.TransferAll(0,world).ok,"same-container TransferAll must explicitly reject"); remainder.Unchanged(world);
    Model capacity(catalog,2); result=capacity.AddUpTo({"ore",std::numeric_limits<int>::max(),""});
    Check(result.moved==20 && result.remaining==std::numeric_limits<int>::max()-20,"large request overflow");
}
void MoveSplitAndSwap() {
    auto catalog=Catalog(); Model bag(catalog,4),chest(catalog,3);
    Snapshot empty_before(bag);
    const auto empty_noop=bag.Move(0,bag,0);
    Check(empty_noop.ok && !empty_noop.changed && empty_noop.moved==0 && empty_noop.remaining==0,
          "empty same-slot whole move must be a successful no-op");
    Check(!bag.Move(0,bag,0,1).ok && !bag.Move(0,bag,0,-1).ok && !bag.Move(4,bag,4).ok,
          "same-slot operation bypassed count/bounds validation");
    empty_before.Unchanged(bag);
    Check(bag.AddUpTo({"ore",10,""}).ok && chest.AddUpTo({"ore",8,""}).ok,"seed merge");
    const auto total=Count(bag,chest);
    auto result=bag.Move(0,chest,0,5);
    Check(result.ok && result.moved==2 && result.remaining==3 && bag.Slots()[0].count==8 && chest.Slots()[0].count==10,"partial merge request semantics");
    Check(Count(bag,chest)==total,"merge conservation");
    Snapshot unchanged(bag); result=bag.Move(0,bag,0);
    Check(result.ok && !result.changed && result.moved==0,"same-slot no-op"); unchanged.Unchanged(bag);
    Check(bag.Split(0,bag,1,3).ok && bag.Slots()[0].count==5 && bag.Slots()[1].count==3,"split stack");
    Snapshot after_split(bag);
    Check(!bag.Split(0,bag,2,5).ok && !bag.Split(0,bag,1,1).ok && !bag.Split(0,bag,2,0).ok,"invalid split accepted");
    after_split.Unchanged(bag);
    result=bag.Move(0,chest,0); // same item, destination full: whole stack swap
    Check(result.ok && result.moved==5 && bag.Slots()[0].count==10 && chest.Slots()[0].count==5,"full same-item swap contract");
    Check(Count(bag,chest)==total,"split/swap conservation");
    Check(chest.AddUpTo({"fruit",3,""}).ok,"seed different-item swap");
    const auto both=Count(bag,chest);
    Snapshot source(bag),destination(chest);
    Check(!bag.Move(0,chest,1,4).ok,"partial unlike-item swap accepted"); source.Unchanged(bag);destination.Unchanged(chest);
    Check(!bag.Move(0,chest,1,0,false).ok,"disabled swap accepted");source.Unchanged(bag);destination.Unchanged(chest);
    Check(bag.Move(0,chest,1).ok && bag.Slots()[0].item_id=="fruit" && chest.Slots()[1].item_id=="ore","different-item swap");
    Check(Count(bag,chest)==both,"swap conservation");
    Model identical(catalog,2); Check(identical.AddUpTo({"ore",20,""}).ok,"seed identical stacks");
    Snapshot identical_before(identical); result=identical.Move(0,identical,1);
    Check(result.ok && !result.changed && result.moved==0,"identical-value swap must be no-op");identical_before.Unchanged(identical);
    Snapshot bounds(bag);
    Check(!bag.Move(-1,chest,0).ok && !bag.Move(0,chest,3).ok && !bag.Move(0,chest,0,-1).ok &&
          !bag.RemoveExact(0,999).ok && !bag.RemoveExact(99,1).ok && !bag.RemoveExact(0,0).ok,"invalid bounds/count accepted");
    bounds.Unchanged(bag);
}
void EquipmentAndIdentity() {
    auto catalog=Catalog(); Model bag(catalog,4),equipment(catalog,4,{"subweapon","head","chest","legs"});
    Check(bag.AddUpTo({"sword",1,"sword-A"}).ok && bag.AddUpTo({"ore",5,""}).ok,"seed equipment");
    Check(bag.Move(0,equipment,0).ok && equipment.Slots()[0].instance_id=="sword-A" && bag.Slots()[0]==Slot{},"equip preserves instance");
    Snapshot equip_before(equipment),bag_before(bag);
    auto result=equipment.Move(0,bag,1); // reverse swap would put ore in weapon slot
    Check(!result.ok && result.code=="slot_filter","reverse equipment swap bypassed source filter");
    equip_before.Unchanged(equipment);bag_before.Unchanged(bag);
    Check(bag.AddUpTo({"bow",1,"bow-A"}).ok,"seed second weapon");
    Check(bag.Move(0,equipment,0).ok && equipment.Slots()[0].instance_id=="bow-A" && bag.Slots()[0].instance_id=="sword-A","equipment swap preserved both identities");
    Snapshot before(bag);
    Check(!bag.AddUpTo({"hat",1,"sword-A"}).ok,"duplicate identity across item IDs accepted");before.Unchanged(bag);
    Check(!bag.Split(0,bag,2,1).ok,"unique item split accepted");before.Unchanged(bag);
    Check(!bag.Move(0,equipment,1).ok,"weapon inserted in head slot");before.Unchanged(bag);
    Check(equipment.AddUpTo({"boots",1,"boots-A"}).ok && equipment.Slots()[3].instance_id=="boots-A","automatic equipment filter routing");
    Model copy(catalog,1);Check(copy.AddUpTo({"bow",1,"bow-A"}).ok,"local IDs may exist until group validation");
    Snapshot copy_before(copy),equip_current(equipment);
    const auto duplicate=copy.Move(0,equipment,0);
    Check(!duplicate.ok && duplicate.code=="duplicate_instance","cross-container duplicate identity accepted");copy_before.Unchanged(copy);equip_current.Unchanged(equipment);
    Check(bag.RemoveExact(0,1).ok && bag.Slots()[0]==Slot{},"unique removal must clear ID and count");
}
void AtomicRestore() {
    auto catalog=Catalog();Model bag(catalog,2),chest(catalog,2),equipment(catalog,1,{"head"});
    Check(bag.AddUpTo({"ore",4,""}).ok && chest.AddUpTo({"fruit",2,""}).ok,"seed group restore");
    const Snapshot bag_before(bag),chest_before(chest),equipment_before(equipment);
    std::vector<Slot> next_bag{{"ore",9,""},{}},next_chest{{"fruit",4,""},{}},bad_equipment{{"sword",1,"sword-A"}};
    auto result=Model::RestoreGroup({{&bag,next_bag},{&chest,next_chest},{&equipment,bad_equipment}});
    Check(!result.ok,"late invalid equipment accepted");bag_before.Unchanged(bag);chest_before.Unchanged(chest);equipment_before.Unchanged(equipment);
    next_chest[1]={"unknown",1,""};
    Check(!Model::RestoreGroup({{&bag,next_bag},{&chest,next_chest}}).ok,"late invalid slot accepted");bag_before.Unchanged(bag);chest_before.Unchanged(chest);
    next_bag[0]={"hat",1,"hat-A"};next_chest[0]={"hat",1,"hat-A"};next_chest[1]={};
    Check(!Model::RestoreGroup({{&bag,next_bag},{&chest,next_chest}}).ok,"cross-group duplicate instance accepted");bag_before.Unchanged(bag);chest_before.Unchanged(chest);
    Check(!Model::RestoreGroup({{&bag,bag.Slots()},{&bag,bag.Slots()}}).ok,"duplicate model accepted");
    Check(!Model::RestoreGroup({{nullptr,{}}}).ok,"null model accepted");
    Model foreign(Catalog(),2);Snapshot foreign_before(foreign);
    Check(!bag.Move(0,foreign,0).ok && !Model::RestoreGroup({{&bag,bag.Slots()},{&foreign,foreign.Slots()}}).ok,"separate catalog identities accepted");
    bag_before.Unchanged(bag);foreign_before.Unchanged(foreign);
    result=Model::RestoreGroup({{&bag,{{"ore",8,""},{}}},{&chest,chest.Slots()},{&equipment,{{"hat",1,"hat-A"}}}});
    Check(result.ok && result.changed && bag.Revision()==bag_before.revision+1 && chest.Revision()==chest_before.revision && equipment.Revision()==1,"restore increments only changed models once");
    Snapshot restored_bag(bag),restored_chest(chest),restored_equipment(equipment);
    result=Model::RestoreGroup({{&bag,bag.Slots()},{&chest,chest.Slots()},{&equipment,equipment.Slots()}});
    Check(result.ok && !result.changed,"identical restore no-op");restored_bag.Unchanged(bag);restored_chest.Unchanged(chest);restored_equipment.Unchanged(equipment);
    Check(Model::RestoreGroup({}).ok,"empty restore is a valid no-op");
}
void DeterministicConservation() {
    auto catalog=Catalog();Model bag(catalog,100),chest(catalog,100);
    Check(bag.AddUpTo({"ore",175,""}).ok && bag.AddUpTo({"fruit",53,""}).ok &&
          chest.AddUpTo({"ore",261,""}).ok && chest.AddUpTo({"fruit",91,""}).ok,"seed conservation workload");
    const auto total=Count(bag,chest);std::mt19937 random(71421);
    for(int step=0;step<5000;++step) {
        Model &source=random()%2 ? bag : chest;
        Model &destination=random()%2 ? bag : chest;
        const int from=static_cast<int>(random()%100),to=static_cast<int>(random()%100);
        const Snapshot before_source(source),before_destination(destination);
        Result result;
        switch(random()%3) {
            case 0:result=source.Move(from,destination,to,random()%3==0 ? static_cast<int>(1+random()%5) : 0,random()%2);break;
            case 1:result=source.Split(from,destination,to,static_cast<int>(1+random()%5));break;
            default:result=source.TransferAll(from,destination);break;
        }
        Check(Count(bag,chest)==total,"generated transfer lost or duplicated items");
        Check(bag.ValidateSnapshot(bag.Slots()).ok && chest.ValidateSnapshot(chest.Slots()).ok,"generated operation broke invariant");
        if(!result.ok || !result.changed) {before_source.Unchanged(source);before_destination.Unchanged(destination);}
        else {
            const bool source_changed=source.Slots()!=before_source.slots;
            const bool destination_changed=destination.Slots()!=before_destination.slots;
            Check(source.Revision()==before_source.revision+source_changed && destination.Revision()==before_destination.revision+destination_changed,
                  "generated transaction revision count mismatch");
        }
    }
}
}
int main() {
    try {
        ConstructionAndValidation();AdditionAndPartialPickup();MoveSplitAndSwap();
        EquipmentAndIdentity();AtomicRestore();DeterministicConservation();
        std::cout << "PASS inventory model: transactional conservation/partial pickup/split/swap/equipment/identity/restore\n";
        return 0;
    } catch(const std::exception &error) {std::cerr << "FAIL inventory model: " << error.what() << '\n';return 1;}
}
