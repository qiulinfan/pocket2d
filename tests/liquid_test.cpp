#include "scene/LiquidGrid.h"

#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

int Total(const LiquidGrid &grid, LiquidGrid::Kind kind) {
    int total = 0;
    for (int y = 0; y < grid.Height(); ++y)
        for (int x = 0; x < grid.Width(); ++x)
            if (grid.Get(x, y).first == kind) total += grid.Get(x, y).second;
    return total;
}

// Steps until nothing is active (or a limit), returning the steps taken.
int Settle(LiquidGrid &grid, const std::function<bool(int, int)> &solid, int limit = 5000) {
    for (int n = 0; n < limit; ++n) {
        grid.Step(solid, 100000);
        if (grid.ActiveCount() == 0) return n + 1;
    }
    return limit;
}

void FallsAndLevels() {
    // A 10x6 box: walls at x = 0 and 9, floor at y = 5.
    const auto solid = [](int x, int y) { return x == 0 || x == 9 || y == 5; };
    LiquidGrid grid(10, 6);
    grid.Set(4, 0, LiquidGrid::kWater, 255);
    grid.Set(5, 0, LiquidGrid::kWater, 255);
    const int steps = Settle(grid, solid);
    Check(steps < 5000, "water settles");
    Check(Total(grid, LiquidGrid::kWater) == 510, "flow keeps the amount of water");
    for (int x = 1; x <= 8; ++x) {
        Check(grid.Get(x, 4).first == LiquidGrid::kWater, "water reaches every floor cell");
        Check(grid.Get(x, 4).second >= 62 && grid.Get(x, 4).second <= 66, "water levels out on the floor");
        Check(grid.Get(x, 3).second == 0, "levelled water stays one cell deep");
    }
}

void FillsFromTheBottom() {
    const auto solid = [](int x, int y) { return x == 0 || x == 4 || y == 8; };
    LiquidGrid grid(5, 9);
    for (int n = 0; n < 6; ++n) grid.Set(2, n, LiquidGrid::kWater, 255);
    Settle(grid, solid);
    for (int x = 1; x <= 3; ++x) {
        Check(grid.Get(x, 7).second == 255, "the bottom row fills first");
        Check(grid.Get(x, 6).second >= 250, "the second row fills next (levelling stops within a unit or two)");
    }
    Check(Total(grid, LiquidGrid::kWater) == 6 * 255, "filling keeps the amount of water");
}

void LavaIsSlowerAndHardens() {
    const auto open = [](int, int y) { return y == 9; };
    LiquidGrid water(20, 10), lava(20, 10);
    water.Set(10, 0, LiquidGrid::kWater, 255);
    lava.Set(10, 0, LiquidGrid::kLava, 255);
    for (int n = 0; n < 6; ++n) {
        water.Step(open, 1000);
        lava.Step(open, 1000);
    }
    Check(water.Get(10, 6).second > 0 || water.Get(10, 7).second > 0 || water.Get(10, 8).second > 0, "water falls a cell per step");
    Check(lava.Get(10, 3).second > 0 || lava.Get(10, 2).second > 0 || lava.Get(10, 1).second > 0, "lava falls a cell every third step");

    // Water poured beside a lava pool hardens the lava it touches.
    std::set<std::pair<int, int>> hardened;   // the caller places a block on each (here: counts as solid)
    const auto basin = [&](int x, int y) { return x == 0 || x == 9 || y == 5 || hardened.count({x, y}) > 0; };
    LiquidGrid mixed(10, 6);
    for (int x = 1; x <= 4; ++x) mixed.Set(x, 4, LiquidGrid::kLava, 255);
    mixed.Set(8, 0, LiquidGrid::kWater, 255);
    for (int n = 0; n < 200; ++n)
        for (const auto &cell : mixed.Step(basin, 1000)) hardened.insert(cell);
    Check(!hardened.empty(), "water meeting lava hardens it");
    for (const auto &cell : hardened) Check(mixed.Get(cell.first, cell.second).first == LiquidGrid::kNone, "hardened cells are emptied");
}

void SettledLiquidIsIdle() {
    const auto solid = [](int x, int y) { return x == 0 || x == 19 || y == 9; };
    LiquidGrid grid(20, 10);
    for (int y = 5; y <= 8; ++y)
        for (int x = 1; x <= 18; ++x) grid.Set(x, y, LiquidGrid::kWater, 255);
    Settle(grid, solid);
    Check(grid.ActiveCount() == 0, "a full basin goes idle");
    grid.Wake(5, 5);
    Check(grid.ActiveCount() > 0, "a terrain edit wakes the liquid near it");
    Settle(grid, solid);
    Check(Total(grid, LiquidGrid::kWater) == 4 * 18 * 255, "an idle basin keeps its water");
}

void BudgetDefersWork() {
    const auto solid = [](int, int y) { return y == 9; };
    LiquidGrid grid(40, 10);
    for (int x = 0; x < 40; ++x) grid.Set(x, 0, LiquidGrid::kWater, 255);
    grid.Step(solid, 10);
    Check(grid.ActiveCount() >= 30, "cells over the budget wait for the next step");
}

void SavesAndLoads() {
    const auto solid = [](int x, int y) { return x == 0 || x == 11 || y == 7; };
    LiquidGrid grid(12, 8), copy(12, 8);
    grid.Set(3, 0, LiquidGrid::kWater, 200);
    grid.Set(8, 2, LiquidGrid::kLava, 90);
    grid.Set(6, 1, LiquidGrid::kHoney, 255);
    for (int n = 0; n < 7; ++n) grid.Step(solid, 1000);
    const std::string text = grid.Save();
    copy.Load(text);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 12; ++x) Check(copy.Get(x, y) == grid.Get(x, y), "a loaded grid matches the saved one");
    Check(copy.Save() == text, "saving a loaded grid gives the same text");
    bool rejected = false;
    try { copy.Load("5:256"); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "runs that do not cover the grid are rejected");
    rejected = false;
    try { LiquidGrid::KindFromName("oil"); } catch (const std::exception &) { rejected = true; }
    Check(rejected, "unknown liquid kinds are rejected");
}

void Deterministic() {
    const auto solid = [](int x, int y) { return x == 0 || x == 29 || y == 14 || (y == 9 && x % 7 == 3); };
    LiquidGrid a(30, 15), b(30, 15);
    for (int x = 2; x < 28; x += 3) {
        a.Set(x, 0, LiquidGrid::kWater, 255);
        b.Set(x, 0, LiquidGrid::kWater, 255);
    }
    a.Set(14, 12, LiquidGrid::kLava, 255);
    b.Set(14, 12, LiquidGrid::kLava, 255);
    for (int n = 0; n < 120; ++n) {
        a.Step(solid, 1000);
        b.Step(solid, 1000);
    }
    Check(a.Save() == b.Save(), "the same start gives the same flow");
}
} // namespace

int main() {
    try {
        FallsAndLevels();
        FillsFromTheBottom();
        LavaIsSlowerAndHardens();
        SettledLiquidIsIdle();
        BudgetDefersWork();
        SavesAndLoads();
        Deterministic();
    } catch (const std::exception &error) {
        std::cerr << "liquid_test: " << error.what() << "\n";
        return 1;
    }
    std::cout << "liquid_test: ok\n";
    return 0;
}
