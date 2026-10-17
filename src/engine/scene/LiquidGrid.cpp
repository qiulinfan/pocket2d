#include "scene/LiquidGrid.h"

#include <algorithm>
#include <stdexcept>

LiquidGrid::LiquidGrid(int width, int height) : width_(width), height_(height) {
    if (width <= 0 || height <= 0 || static_cast<std::int64_t>(width) * height > (std::int64_t{1} << 24))
        throw std::runtime_error("liquid: grid size must be positive and at most 16M cells");
    const auto count = static_cast<std::size_t>(width) * height;
    kind_.assign(count, kNone);
    amount_.assign(count, 0);
    queued_.assign(count, 0);
}

std::pair<LiquidGrid::Kind, std::uint8_t> LiquidGrid::Get(int x, int y) const {
    if (!Inside(x, y)) return {kNone, 0};
    const auto i = Index(x, y);
    return {static_cast<Kind>(kind_[i]), amount_[i]};
}

void LiquidGrid::Set(int x, int y, Kind kind, std::uint8_t amount) {
    if (!Inside(x, y)) return;
    const auto i = Index(x, y);
    if (kind == kNone || amount == 0) { kind = kNone; amount = 0; }
    kind_[i] = kind;
    amount_[i] = amount;
    QueueAround(x, y);
}

void LiquidGrid::Queue(int x, int y) {
    if (!Inside(x, y)) return;
    const auto i = Index(x, y);
    if (queued_[i]) return;
    queued_[i] = 1;
    active_.push_back(static_cast<std::uint32_t>(i));
}

void LiquidGrid::QueueAround(int x, int y) {
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) Queue(x + dx, y + dy);
}

void LiquidGrid::Wake(int x, int y) { QueueAround(x, y); }

std::vector<std::pair<int, int>> LiquidGrid::Step(const std::function<bool(int, int)> &solid, std::size_t budget) {
    ++steps_;
    // Cells are taken in the order they became active, so with more than the
    // budget every cell still gets its turn; the rest stay first in line.
    std::vector<std::uint32_t> work;
    work.swap(active_);
    if (work.size() > budget) {
        active_.assign(work.begin() + static_cast<std::ptrdiff_t>(budget), work.end());
        work.resize(budget);
    }
    for (const auto i : work) queued_[i] = 0;
    // Within a step: bottom rows first, then left to right, as Terraria settles liquid upward.
    std::sort(work.begin(), work.end(), [this](std::uint32_t a, std::uint32_t b) {
        const auto ya = a / static_cast<std::uint32_t>(width_), yb = b / static_cast<std::uint32_t>(width_);
        return ya != yb ? ya > yb : a < b;
    });
    const auto open = [&](int x, int y, Kind kind) {
        if (!Inside(x, y) || solid(x, y)) return false;
        const auto k = kind_[Index(x, y)];
        return k == kNone || k == kind;
    };
    for (const auto i : work) {
        const int x = static_cast<int>(i % width_), y = static_cast<int>(i / width_);
        const auto kind = static_cast<Kind>(kind_[i]);
        int amount = amount_[i];
        if (kind == kNone || amount == 0) continue;
        if (solid(x, y)) {   // a block was placed in the liquid: it is pushed out of existence
            kind_[i] = kNone;
            amount_[i] = 0;
            QueueAround(x, y);
            continue;
        }
        const int every = kind == kLava ? 3 : kind == kHoney ? 4 : 1;
        if (steps_ % every != 0) { Queue(x, y); continue; }
        bool changed = false;
        // Fall into the cell below.
        if (open(x, y + 1, kind)) {
            const auto b = Index(x, y + 1);
            const int move = std::min(amount, static_cast<int>(kFull) - amount_[b]);
            if (move > 0) {
                amount_[b] = static_cast<std::uint8_t>(amount_[b] + move);
                kind_[b] = kind;
                amount -= move;
                changed = true;
                QueueAround(x, y + 1);
            }
        }
        // Level with the open cells beside it that hold at least two less.
        if (amount > 0) {
            int sides[2];
            int count = 0, total = amount;
            for (const int sx : {x - 1, x + 1})
                if (open(sx, y, kind) && amount_[Index(sx, y)] + 1 < amount) {
                    sides[count++] = sx;
                    total += amount_[Index(sx, y)];
                }
            if (count > 0) {
                // The remainder stays here; nothing counts as moved unless an amount changes,
                // so a slope like 7, 9, 7 that cannot level further settles.
                const int level = total / (count + 1);
                const int kept = total - level * count;
                bool moved = kept != amount;
                for (int n = 0; n < count; ++n) moved = moved || amount_[Index(sides[n], y)] != level;
                if (moved) {
                    for (int n = 0; n < count; ++n) {
                        const auto s = Index(sides[n], y);
                        amount_[s] = static_cast<std::uint8_t>(level);
                        kind_[s] = level > 0 ? kind : kNone;
                        QueueAround(sides[n], y);
                    }
                    amount = kept;
                    changed = true;
                }
            }
        }
        // A lone film too thin to flow dries up.
        if (amount > 0 && amount < 3 && !open(x, y + 1, kind)) {
            const auto beside = [&](int sx) { return Inside(sx, y) && kind_[Index(sx, y)] == kind && amount_[Index(sx, y)] > 0; };
            if (!beside(x - 1) && !beside(x + 1)) { amount = 0; changed = true; }
        }
        amount_[i] = static_cast<std::uint8_t>(amount);
        if (amount == 0) kind_[i] = kNone;
        if (changed) QueueAround(x, y);
    }
    // Water touching lava hardens the lava cell.
    std::vector<std::pair<int, int>> hardened;
    for (const auto i : work) {
        const int x = static_cast<int>(i % width_), y = static_cast<int>(i / width_);
        static constexpr int kAround[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto &d : kAround) {
            const int cx = x + d[0], cy = y + d[1];
            if (!Inside(cx, cy) || kind_[Index(cx, cy)] != kLava || amount_[Index(cx, cy)] == 0) continue;
            bool water = false;
            for (int e = 1; e < 5; ++e) {
                const int wx = cx + kAround[e][0], wy = cy + kAround[e][1];
                if (Inside(wx, wy) && kind_[Index(wx, wy)] == kWater && amount_[Index(wx, wy)] > 0) water = true;
            }
            if (!water) continue;
            kind_[Index(cx, cy)] = kNone;
            amount_[Index(cx, cy)] = 0;
            QueueAround(cx, cy);
            hardened.emplace_back(cx, cy);
        }
    }
    std::sort(hardened.begin(), hardened.end());
    hardened.erase(std::unique(hardened.begin(), hardened.end()), hardened.end());
    return hardened;
}

std::string LiquidGrid::Save() const {
    std::string out;
    const auto count = kind_.size();
    std::size_t n = 0;
    while (n < count) {
        const int code = kind_[n] * 256 + amount_[n];
        std::size_t run = 1;
        while (n + run < count && kind_[n + run] * 256 + amount_[n + run] == code) ++run;
        if (!out.empty()) out += ',';
        out += std::to_string(run) + ':' + std::to_string(code);
        n += run;
    }
    return out;
}

void LiquidGrid::Load(const std::string &runs) {
    const auto count = kind_.size();
    std::vector<std::uint8_t> kinds(count, kNone), amounts(count, 0);
    std::size_t at = 0, pos = 0;
    while (pos < runs.size()) {
        const auto colon = runs.find(':', pos);
        auto comma = runs.find(',', pos);
        if (comma == std::string::npos) comma = runs.size();
        if (colon == std::string::npos || colon > comma) throw std::runtime_error("liquid: bad saved runs");
        const auto run = std::stoull(runs.substr(pos, colon - pos));
        const auto code = std::stoi(runs.substr(colon + 1, comma - colon - 1));
        if (code < 0 || code >= 4 * 256 || at + run > count) throw std::runtime_error("liquid: saved runs do not fit the grid");
        std::fill(kinds.begin() + static_cast<std::ptrdiff_t>(at), kinds.begin() + static_cast<std::ptrdiff_t>(at + run),
                  static_cast<std::uint8_t>(code / 256));
        std::fill(amounts.begin() + static_cast<std::ptrdiff_t>(at), amounts.begin() + static_cast<std::ptrdiff_t>(at + run),
                  static_cast<std::uint8_t>(code % 256));
        at += run;
        pos = comma + 1;
    }
    if (at != count) throw std::runtime_error("liquid: saved runs do not cover the grid");
    kind_.swap(kinds);
    amount_.swap(amounts);
    active_.clear();
    std::fill(queued_.begin(), queued_.end(), 0);
    for (std::size_t i = 0; i < count; ++i)
        if (amount_[i] > 0) Queue(static_cast<int>(i % width_), static_cast<int>(i / width_));
}

LiquidGrid::Kind LiquidGrid::KindFromName(const std::string &name) {
    if (name == "water") return kWater;
    if (name == "lava") return kLava;
    if (name == "honey") return kHoney;
    if (name.empty()) return kNone;
    throw std::runtime_error("liquid: unknown kind '" + name + "' (water, lava, honey or \"\")");
}

const char *LiquidGrid::KindName(Kind kind) {
    switch (kind) {
    case kWater: return "water";
    case kLava: return "lava";
    case kHoney: return "honey";
    default: return "";
    }
}
