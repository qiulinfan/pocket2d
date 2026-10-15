#include "shared/inventory/InventoryModel.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace PocketInventory {
namespace {
Result Failure(const char *code, int remaining = 0) { return {false, code, 0, remaining, false}; }
Result Success(int moved = 0, int remaining = 0, bool changed = false) { return {true, "", moved, remaining, changed}; }
bool Identifier(const std::string &value) {
    if (value.empty() || value.size() > 64) return false;
    const auto alphanumeric = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (!alphanumeric(static_cast<unsigned char>(value.front()))) return false;
    return std::all_of(value.begin(), value.end(), [&](unsigned char c) { return alphanumeric(c) || c == '_' || c == '-' || c == '.'; });
}
bool Filter(const std::string &value) {
    return value.empty() || value == "subweapon" || value == "head" || value == "chest" || value == "legs";
}
bool Empty(const Slot &slot) { return slot.item_id.empty() && slot.count == 0 && slot.instance_id.empty(); }
Result ValidateEntries(const std::vector<Model::RestoreEntry> &entries) {
    std::unordered_set<const Model *> models;
    std::unordered_set<std::string> instances;
    const ItemCatalog::Data *catalog = nullptr;
    for (const auto &[model, snapshot] : entries) {
        if (!model) return Failure("invalid_argument");
        if (!models.insert(model).second) return Failure("duplicate_model");
        if (catalog && catalog != model->Catalog().get()) return Failure("incompatible_catalog");
        catalog = model->Catalog().get();
        const auto valid = model->ValidateSnapshot(snapshot);
        if (!valid.ok) return valid;
        for (const auto &slot : snapshot)
            if (!slot.instance_id.empty() && !instances.insert(slot.instance_id).second)
                return Failure("duplicate_instance");
    }
    return Success();
}
}

Model::Model(std::shared_ptr<const ItemCatalog::Data> catalog, int capacity, std::vector<std::string> filters)
    : catalog_(std::move(catalog)) {
    if (capacity < 1 || capacity > 100) throw std::invalid_argument("inventory capacity must be 1..100");
    if (!catalog_ || catalog_->version != 1 || catalog_->index.size() != catalog_->items.size())
        throw std::invalid_argument("inventory requires a validated item catalog");
    for (const auto &item : catalog_->items) {
        if (!Identifier(item.id) || item.max_stack < 1 || item.max_stack > 9999 || !Filter(item.equipment_slot) ||
            ItemCatalog::Find(*catalog_, item.id) != &item)
            throw std::invalid_argument("inventory requires valid catalog IDs, stack limits and index");
    }
    if (filters.empty()) filters.resize(static_cast<std::size_t>(capacity));
    if (filters.size() != static_cast<std::size_t>(capacity) ||
        !std::all_of(filters.begin(), filters.end(), Filter))
        throw std::invalid_argument("inventory filters must match capacity and use known equipment slots");
    filters_ = std::move(filters);
    slots_.resize(static_cast<std::size_t>(capacity));
}
bool Model::InBounds(int index) const { return index >= 0 && index < Capacity(); }
Result Model::ValidateRequest(const Slot &request) const {
    if (request.count <= 0) return Failure("invalid_count");
    const auto *definition = ItemCatalog::Find(*catalog_, request.item_id);
    if (!definition) return Failure("unknown_item", request.count);
    if (definition->max_stack == 1) {
        if (request.count != 1 || !Identifier(request.instance_id)) return Failure("invalid_instance", request.count);
    } else if (!request.instance_id.empty()) return Failure("invalid_instance", request.count);
    return Success();
}
bool Model::Accepts(int index, const Slot &slot) const {
    if (Empty(slot)) return true;
    const auto &filter = filters_[static_cast<std::size_t>(index)];
    if (filter.empty()) return true;
    const auto *definition = ItemCatalog::Find(*catalog_, slot.item_id);
    return definition && definition->equipment_slot == filter && definition->max_stack == 1 && slot.count == 1;
}
Result Model::ValidateSnapshot(const std::vector<Slot> &snapshot) const {
    if (snapshot.size() != slots_.size()) return Failure("capacity_mismatch");
    std::unordered_set<std::string> instances;
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        const auto &slot = snapshot[index];
        if (Empty(slot)) continue;
        const auto valid = ValidateRequest(slot);
        if (!valid.ok) return valid;
        const auto *definition = ItemCatalog::Find(*catalog_, slot.item_id);
        if (slot.count > definition->max_stack) return Failure("stack_limit");
        if (!Accepts(static_cast<int>(index), slot)) return Failure("slot_filter");
        if (!slot.instance_id.empty() && !instances.insert(slot.instance_id).second) return Failure("duplicate_instance");
    }
    return Success();
}
int Model::Fill(std::vector<Slot> &snapshot, const Slot &request) const {
    const auto *definition = ItemCatalog::Find(*catalog_, request.item_id);
    int remaining = request.count;
    if (definition->max_stack > 1) {
        for (auto &slot : snapshot) {
            if (remaining == 0) break;
            if (slot.item_id != request.item_id || slot.count >= definition->max_stack) continue;
            const int moved = std::min(remaining, definition->max_stack - slot.count);
            slot.count += moved;
            remaining -= moved;
        }
    }
    for (std::size_t index = 0; index < snapshot.size() && remaining > 0; ++index) {
        if (!Empty(snapshot[index])) continue;
        Slot candidate{request.item_id, std::min(remaining, definition->max_stack), request.instance_id};
        if (!Accepts(static_cast<int>(index), candidate)) continue;
        remaining -= candidate.count;
        snapshot[index] = std::move(candidate);
    }
    return request.count - remaining;
}
Result Model::RestoreGroup(const std::vector<RestoreEntry> &entries) {
    const auto valid = ValidateEntries(entries);
    if (!valid.ok) return valid;
    bool changed = false;
    for (const auto &[model, snapshot] : entries) {
        if (snapshot == model->slots_) continue;
        if (model->revision_ == std::numeric_limits<std::uint64_t>::max()) return Failure("revision_overflow");
        changed = true;
    }
    if (!changed) return Success();
    // Complete all potentially throwing allocations before changing any model.
    std::vector<std::vector<Slot>> prepared;
    prepared.reserve(entries.size());
    for (const auto &entry : entries) prepared.push_back(entry.second);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto *model = entries[index].first;
        if (prepared[index] == model->slots_) continue;
        model->slots_.swap(prepared[index]);
        ++model->revision_;
    }
    return Success(0, 0, true);
}
Result Model::AddUpTo(const Slot &request) {
    const auto valid = ValidateRequest(request);
    if (!valid.ok) return valid;
    if (!request.instance_id.empty())
        for (const auto &slot : slots_)
            if (slot.instance_id == request.instance_id) return Failure("duplicate_instance", request.count);
    auto next = slots_;
    const int moved = Fill(next, request);
    if (moved == 0) return Failure("no_space", request.count);
    auto result = RestoreGroup({{this, std::move(next)}});
    if (!result.ok) { result.remaining = request.count; return result; }
    result.moved = moved;
    result.remaining = request.count - moved;
    return result;
}
Result Model::RemoveExact(int index, int count) {
    if (!InBounds(index)) return Failure("invalid_slot", std::max(count, 0));
    if (count <= 0) return Failure("invalid_count");
    if (slots_[index].count < count) return Failure("insufficient_items", count);
    auto next = slots_;
    next[index].count -= count;
    if (next[index].count == 0) next[index] = {};
    auto result = RestoreGroup({{this, std::move(next)}});
    if (!result.ok) { result.remaining = count; return result; }
    result.moved = count;
    return result;
}
Result Model::Commit(std::vector<Slot> source, Model &destination, std::vector<Slot> target,
                     int moved, int remaining) {
    auto result = &destination == this ? RestoreGroup({{this, std::move(source)}})
        : RestoreGroup({{this, std::move(source)}, {&destination, std::move(target)}});
    if (!result.ok) { result.remaining = moved + remaining; return result; }
    // A swap of identical stack values has no externally observable movement.
    result.moved = result.changed ? moved : 0;
    result.remaining = result.changed ? remaining : moved + remaining;
    return result;
}
Result Model::Move(int source_index, Model &destination, int destination_index, int count, bool allow_swap) {
    if (!InBounds(source_index) || !destination.InBounds(destination_index)) return Failure("invalid_slot", std::max(count, 0));
    if (count < 0) return Failure("invalid_count");
    if (&destination == this && source_index == destination_index && count == 0)
        return Success(0, slots_[source_index].count);
    if (Empty(slots_[source_index])) return Failure("empty_source", count);
    const Slot source_slot = slots_[source_index];
    const int requested = count == 0 ? source_slot.count : count;
    if (requested > source_slot.count) return Failure("insufficient_items", requested);
    if (catalog_.get() != destination.catalog_.get()) return Failure("incompatible_catalog", requested);
    if (&destination == this && source_index == destination_index) return Success(0, requested);
    auto source = slots_;
    auto other = &destination == this ? std::vector<Slot>{} : destination.slots_;
    auto &target = &destination == this ? source : other;
    const Slot destination_slot = target[destination_index];
    const auto *definition = ItemCatalog::Find(*catalog_, source_slot.item_id);
    int moved = 0;
    if (Empty(destination_slot)) {
        moved = requested;
        target[destination_index] = {source_slot.item_id, moved, source_slot.instance_id};
    } else if (destination_slot.item_id == source_slot.item_id && definition->max_stack > 1 &&
               destination_slot.count < definition->max_stack) {
        moved = std::min(requested, definition->max_stack - destination_slot.count);
        target[destination_index].count += moved;
    } else {
        if (!allow_swap || requested != source_slot.count) return Failure("swap_not_allowed", requested);
        target[destination_index] = source_slot;
        source[source_index] = destination_slot;
        return Commit(std::move(source), destination, std::move(other), requested, 0);
    }
    source[source_index].count -= moved;
    if (source[source_index].count == 0) source[source_index] = {};
    return Commit(std::move(source), destination, std::move(other), moved, requested - moved);
}
Result Model::Split(int source_index, Model &destination, int destination_index, int count) {
    if (!InBounds(source_index) || !destination.InBounds(destination_index)) return Failure("invalid_slot", std::max(count, 0));
    if (count <= 0) return Failure("invalid_count");
    const auto &slot = slots_[source_index];
    if (Empty(slot)) return Failure("empty_source", count);
    const auto *definition = ItemCatalog::Find(*catalog_, slot.item_id);
    if (definition->max_stack == 1 || count >= slot.count) return Failure("invalid_split", count);
    if (!Empty(destination.slots_[destination_index])) return Failure("destination_occupied", count);
    return Move(source_index, destination, destination_index, count, false);
}
Result Model::TransferAll(int source_index, Model &destination) {
    if (!InBounds(source_index)) return Failure("invalid_slot");
    if (Empty(slots_[source_index])) return Failure("empty_source");
    const Slot request = slots_[source_index];
    if (&destination == this) return Failure("same_container", request.count);
    if (catalog_.get() != destination.catalog_.get()) return Failure("incompatible_catalog", request.count);
    auto source = slots_;
    auto target = destination.slots_;
    const int moved = destination.Fill(target, request);
    if (moved == 0) return Failure("no_space", request.count);
    source[source_index].count -= moved;
    if (source[source_index].count == 0) source[source_index] = {};
    return Commit(std::move(source), destination, std::move(target), moved, request.count - moved);
}
} // namespace PocketInventory
