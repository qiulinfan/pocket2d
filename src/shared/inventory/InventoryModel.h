#ifndef POCKET2D_INVENTORY_MODEL_H
#define POCKET2D_INVENTORY_MODEL_H

#include "shared/inventory/ItemCatalog.h"
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace PocketInventory {
struct Slot {
    std::string item_id;
    int count = 0;
    std::string instance_id;
    bool operator==(const Slot &) const = default;
};
struct Result {
    bool ok = false;
    std::string code;
    int moved = 0;
    int remaining = 0;
    bool changed = false;
};

// Pure owner-thread state: no Lua, SDL, callbacks, I/O or observer publication.
// Catalogs are validated once by ItemCatalog and shared immutably. Cross-model
// operations require the same catalog object, avoiding ambiguous item rules.
class Model {
public:
    using RestoreEntry = std::pair<Model *, std::vector<Slot>>;
    Model(std::shared_ptr<const ItemCatalog::Data> catalog, int capacity,
          std::vector<std::string> filters = {});

    int Capacity() const { return static_cast<int>(slots_.size()); }
    std::uint64_t Revision() const { return revision_; }
    const std::vector<Slot> &Slots() const { return slots_; }
    const std::shared_ptr<const ItemCatalog::Data> &Catalog() const { return catalog_; }
    const std::vector<std::string> &Filters() const { return filters_; }

    Result ValidateSnapshot(const std::vector<Slot> &snapshot) const;
    Result AddUpTo(const Slot &request);
    Result RemoveExact(int index, int count);
    Result Move(int source_index, Model &destination, int destination_index,
                int count = 0, bool allow_swap = true);
    Result Split(int source_index, Model &destination, int destination_index, int count);
    Result TransferAll(int source_index, Model &destination);
    static Result RestoreGroup(const std::vector<RestoreEntry> &entries);

private:
    bool InBounds(int index) const;
    bool Accepts(int index, const Slot &slot) const;
    Result ValidateRequest(const Slot &request) const;
    int Fill(std::vector<Slot> &snapshot, const Slot &request) const;
    Result Commit(std::vector<Slot> source, Model &destination,
                  std::vector<Slot> target, int moved, int remaining);

    std::shared_ptr<const ItemCatalog::Data> catalog_;
    std::vector<Slot> slots_;
    std::vector<std::string> filters_;
    std::uint64_t revision_ = 0;
};
} // namespace PocketInventory
#endif
