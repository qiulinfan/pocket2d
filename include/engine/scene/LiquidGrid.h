#ifndef POCKET2D_ENGINE_LIQUIDGRID_H
#define POCKET2D_ENGINE_LIQUIDGRID_H

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Terraria-style flowing liquids on a cell grid: each cell holds one kind
// (water, lava or honey) in an amount from 0 to 255. A step moves liquid down
// into the cell below, then levels it with the open cells beside it; lava
// moves every third step and honey every fourth, as they are thicker. Only
// active cells (changed recently, or woken by a terrain edit) are processed,
// in a fixed order, so a settled lake costs nothing and results are
// deterministic. Water touching lava turns the lava cell into a block.
class LiquidGrid {
public:
    enum Kind : std::uint8_t { kNone = 0, kWater = 1, kLava = 2, kHoney = 3 };
    static constexpr std::uint8_t kFull = 255;

    LiquidGrid(int width, int height);

    int Width() const { return width_; }
    int Height() const { return height_; }
    // Kind and amount of a cell (kNone and 0 outside the grid).
    std::pair<Kind, std::uint8_t> Get(int x, int y) const;
    // Sets a cell (amount 0 or kind kNone empties it) and wakes it.
    void Set(int x, int y, Kind kind, std::uint8_t amount);
    // Wakes the 3x3 cells around a cell, after the terrain there changed.
    void Wake(int x, int y);
    std::size_t ActiveCount() const { return active_.size(); }

    // One step over at most `budget` active cells (the rest wait for the next
    // step). `solid` says whether a cell blocks liquid. Returns the cells where
    // water met lava: the caller turns them into a block; they are emptied.
    std::vector<std::pair<int, int>> Step(const std::function<bool(int, int)> &solid, std::size_t budget);

    // Run lengths of cells in row-major order, "count:code" pairs joined by
    // commas, where code is kind * 256 + amount ("1200:0,40:511,...").
    std::string Save() const;
    void Load(const std::string &runs);

    static Kind KindFromName(const std::string &name);
    static const char *KindName(Kind kind);

private:
    int width_;
    int height_;
    std::uint64_t steps_ = 0;
    std::vector<std::uint8_t> kind_;
    std::vector<std::uint8_t> amount_;
    std::vector<std::uint32_t> active_;      // cell indices to process next step
    std::vector<std::uint8_t> queued_;       // 1 while a cell is in active_

    bool Inside(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    std::size_t Index(int x, int y) const { return static_cast<std::size_t>(y) * width_ + x; }
    void Queue(int x, int y);
    void QueueAround(int x, int y);
};

#endif
