#include "core/TaskExecutor.h"
#include <algorithm>
#include <stdexcept>

namespace {
thread_local TaskExecutor *executing = nullptr;
class ExecutionScope {
public:
    explicit ExecutionScope(TaskExecutor *executor) : previous_(executing) { executing = executor; }
    ~ExecutionScope() { executing = previous_; }
private:
    TaskExecutor *previous_;
};
}

std::size_t TaskExecutor::DefaultWorkerCount() {
    const auto hardware = std::thread::hardware_concurrency();
    return hardware > 1 ? std::min<std::size_t>(hardware - 1, 7) : 0;
}

TaskExecutor::TaskExecutor(std::size_t worker_count) {
    try {
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i)
            workers_.emplace_back([this] { WorkerLoop(); });
    } catch (...) {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        work_ready_.notify_all();
        workers_.clear();
        throw;
    }
}

TaskExecutor::~TaskExecutor() {
    // Like a standard container, lifetime must outlive concurrent member calls.
    { std::lock_guard lock(mutex_); stopping_ = true; }
    work_ready_.notify_all();
    workers_.clear();
}

void TaskExecutor::ParallelFor(std::size_t count, std::size_t grain,
                              const RangeTask &task) {
    if (!task) throw std::invalid_argument("ParallelFor requires a callback");
    if (grain == 0) throw std::invalid_argument("ParallelFor grain must be positive");
    if (count == 0) return;
    if (executing == this || workers_.empty() || count <= grain) {
        ExecutionScope scope(this);
        task(0, count);
        return;
    }

    std::lock_guard submission_lock(submission_mutex_);
    {
        std::lock_guard lock(mutex_);
        task_ = task;
        count_ = count;
        grain_ = grain;
        next_.store(0, std::memory_order_relaxed);
        failure_ = nullptr;
        pending_workers_ = workers_.size();
        ++generation_;
    }
    work_ready_.notify_all();
    RunChunks();
    std::exception_ptr failure;
    {
        std::unique_lock lock(mutex_);
        work_finished_.wait(lock, [this] { return pending_workers_ == 0; });
        failure = failure_;
        task_ = {};
    }
    if (failure) std::rethrow_exception(failure);
}

void TaskExecutor::RunChunks() {
    ExecutionScope scope(this);
    for (;;) {
        const auto begin = next_.fetch_add(grain_, std::memory_order_relaxed);
        if (begin >= count_) return;
        try {
            task_(begin, begin + std::min(grain_, count_ - begin));
        } catch (...) {
            std::lock_guard lock(mutex_);
            if (!failure_) failure_ = std::current_exception();
            // Other chunks already running still complete before the caller exits.
            next_.store(count_, std::memory_order_relaxed);
            return;
        }
    }
}

void TaskExecutor::WorkerLoop() {
    std::size_t observed_generation = 0;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            work_ready_.wait(lock, [&] {
                return stopping_ || generation_ != observed_generation;
            });
            if (stopping_) return;
            observed_generation = generation_;
        }
        RunChunks();
        {
            std::lock_guard lock(mutex_);
            if (--pending_workers_ == 0) work_finished_.notify_one();
        }
    }
}
