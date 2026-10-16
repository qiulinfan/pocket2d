// Log records are per World, bounded, and counted including evictions.
#include "core/Log.h"
#include "core/World.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void Check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}
} // namespace

int main() {
    try {
        World a, b;
        {
            WorldScope scope(a);
            a.frame_number = 7;
            Log::Warning("test", "quote \" and newline \n kept");
            const auto recent = Log::Recent();
            Check(recent.size() == 1 && recent[0].frame == 7 && recent[0].world == a.GetId() &&
                      recent[0].level == Log::Level::Warning && recent[0].category == "test",
                  "record must carry level, category, frame and World");
        }
        {
            WorldScope scope(b);
            Check(Log::Recent().empty() && Log::WrittenCount() == 0, "Worlds must not see each other's records");
            for (std::size_t i = 0; i < Log::kRecentCapacity + 10; ++i) Log::Info("bulk", std::to_string(i));
            const auto recent = Log::Recent();
            Check(recent.size() == Log::kRecentCapacity && recent.front().message == "10" &&
                      Log::WrittenCount() == Log::kRecentCapacity + 10,
                  "buffer must keep the newest records and count evicted ones");
        }
        std::cout << "log_test: per-World bounded records passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "log_test: " << error.what() << '\n';
        return 1;
    }
}
