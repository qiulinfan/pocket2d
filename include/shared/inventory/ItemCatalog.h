#ifndef POCKET2D_ITEM_CATALOG_H
#define POCKET2D_ITEM_CATALOG_H

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ItemCatalog {
struct Definition {
    std::string id;
    std::string display_name;
    std::string description;
    std::string kind;
    std::string equipment_slot;
    int max_stack = 99;
    int health_restore = 0;
    int attack = 0;
    int defense = 0;
    double move_speed_bonus = 0;
};
struct Data {
    int version = 1;
    std::vector<Definition> items;
    // Derived by ValidateData; share a validated catalog as const after loading.
    std::unordered_map<std::string, std::size_t> index;
};

void ValidateData(Data &data);
Data LoadFile(const std::filesystem::path &path);
Data LoadResource(const std::string &relative);
const Definition *Find(const Data &data, const std::string &id);
} // namespace ItemCatalog

#endif
