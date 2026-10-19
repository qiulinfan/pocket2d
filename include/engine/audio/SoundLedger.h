#ifndef POCKET2D_SOUND_LEDGER_H
#define POCKET2D_SOUND_LEDGER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

// The sounds of a rollback netplay session, kept as GGPO games keep them. A
// tick that a correction simulates again asks for its sounds again; each
// request is keyed by its tick, its clip and how many requests of that clip
// came before it in the tick. So a sound already heard is not played twice,
// a sound that first appears in a correction (a hit the prediction missed)
// is played then, late by the ticks the correction reached back, and a sound
// of a corrected tick that the correction did not ask for again was
// mispredicted: EndCorrection hands it back to be stopped. Pure bookkeeping;
// the audio API plays and stops the voices.
class SoundLedger {
public:
    struct Decision {
        bool play = false;       // false: heard already (the same tick asked for it before)
        std::uint32_t frame = 0; // the tick
        int nth = 0;             // earlier requests of the clip in the tick
        std::uint32_t late = 0;  // ticks between the request's tick and the present one
    };
    struct Cancelled {
        std::uint32_t frame = 0;
        std::string clip;
        int nth = 0;
        std::int64_t voice = -1;
    };

    // A tick starts. A tick simulated for the first time at or before the
    // last one (a new session) forgets everything.
    void BeginTick(std::uint32_t frame, bool resimulating);
    // A correction simulates ticks [from, to) again; `to` is the present tick.
    void BeginCorrection(std::uint32_t from, std::uint32_t to);
    // The requests of the corrected ticks that were not asked for again.
    std::vector<Cancelled> EndCorrection();
    Decision Request(const std::string &clip);
    // The voice that plays the last request, so a cancellation can stop it.
    void SetVoice(std::int64_t voice);
    void Clear();
    std::size_t Size() const { return entries_.size(); }

private:
    using Key = std::tuple<std::uint32_t, std::string, int>;
    struct Entry {
        bool confirmed = true;
        std::int64_t voice = -1;
    };
    // Corrections reach back at most the rollback window (15 ticks) plus the
    // input delay; older requests can no longer change.
    static constexpr std::uint32_t kKeepTicks = 64;

    std::map<Key, Entry> entries_;
    std::unordered_map<std::string, int> counts_; // requests per clip in the current tick
    std::uint32_t frame_ = 0, last_new_frame_ = 0, present_ = 0;
    std::uint32_t correction_from_ = 0, correction_to_ = 0;
    bool resimulating_ = false, correcting_ = false, started_ = false;
    Entry *last_ = nullptr;
};

#endif
