#include "audio/SoundLedger.h"

void SoundLedger::BeginTick(std::uint32_t frame, bool resimulating) {
    if (!resimulating) {
        if (started_ && frame <= last_new_frame_) Clear();
        started_ = true;
        last_new_frame_ = frame;
        present_ = frame;
        if (frame > kKeepTicks) {
            const Key oldest{frame - kKeepTicks, std::string(), 0};
            entries_.erase(entries_.begin(), entries_.lower_bound(oldest));
        }
    }
    frame_ = frame;
    resimulating_ = resimulating;
    counts_.clear();
    last_ = nullptr;
}

void SoundLedger::BeginCorrection(std::uint32_t from, std::uint32_t to) {
    correcting_ = true;
    correction_from_ = from;
    correction_to_ = to;
    present_ = to;
    for (auto it = entries_.lower_bound(Key{from, std::string(), 0}); it != entries_.end(); ++it) {
        if (std::get<0>(it->first) >= to) break;
        it->second.confirmed = false;
    }
}

std::vector<SoundLedger::Cancelled> SoundLedger::EndCorrection() {
    std::vector<Cancelled> out;
    if (!correcting_) return out;
    correcting_ = false;
    for (auto it = entries_.lower_bound(Key{correction_from_, std::string(), 0}); it != entries_.end();) {
        if (std::get<0>(it->first) >= correction_to_) break;
        if (it->second.confirmed) { ++it; continue; }
        out.push_back({std::get<0>(it->first), std::get<1>(it->first), std::get<2>(it->first), it->second.voice});
        it = entries_.erase(it);
    }
    last_ = nullptr;
    return out;
}

SoundLedger::Decision SoundLedger::Request(const std::string &clip) {
    Decision d;
    d.frame = frame_;
    d.nth = counts_[clip]++;
    auto [it, inserted] = entries_.try_emplace(Key{frame_, clip, d.nth});
    last_ = &it->second;
    if (!inserted && resimulating_) {
        it->second.confirmed = true; // heard on the first run of this tick
        return d;
    }
    it->second = Entry{};
    d.play = true;
    d.late = resimulating_ && present_ > frame_ ? present_ - frame_ : 0;
    return d;
}

void SoundLedger::SetVoice(std::int64_t voice) {
    if (last_) last_->voice = voice;
}

void SoundLedger::Clear() {
    entries_.clear();
    counts_.clear();
    last_ = nullptr;
    correcting_ = false;
    started_ = false;
}
