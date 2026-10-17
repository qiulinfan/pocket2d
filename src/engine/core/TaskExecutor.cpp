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
#ifdef __EMSCRIPTEN__
    return 0;   // the web build has no threads: tasks run inline on the main thread
#endif
    const auto hardware = std::thread::hardware_concurrency();
    return hardware > 1 ? std::min<std::size_t>(hardware - 1, 7) : 0;
}

TaskExecutor &TaskExecutor::Shared() {
    static TaskExecutor executor;
    return executor;
}

TaskExecutor::TaskExecutor(std::size_t worker_count) {
    try {
        workers_.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i)
            workers_.emplace_back([this, i] { WorkerLoop(i + 1); });
    } catch (...) {
        StopWorkers();
        throw;
    }
}

void TaskExecutor::StopWorkers() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    work_ready_.notify_all();
    for (auto &worker : workers_)
        if (worker.joinable()) worker.join();
    workers_.clear();
}

TaskExecutor::~TaskExecutor() {
    // Like a standard container, lifetime must outlive concurrent member calls.
    StopWorkers();
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

struct TaskExecutor::Group {
    IndexedRangeTask task;
    void *context;
    std::size_t remaining; // guarded by the executor's mutex_
};

TaskExecutor::Group *TaskExecutor::Submit(std::size_t count, std::size_t min_range,
                                          IndexedRangeTask task, void *context) {
    if (task == nullptr) throw std::invalid_argument("Submit requires a callback");
    auto *group = new Group{task, context, 0};
    if (count == 0) return group;
    const std::size_t range = std::max<std::size_t>({min_range, 1, (count + workers_.size()) / (workers_.size() + 1)});
    {
        std::lock_guard lock(mutex_);
        for (std::size_t begin = 0; begin < count; begin += range) {
            jobs_.push_back({group, begin, std::min(count, begin + range)});
            ++group->remaining;
        }
    }
    work_ready_.notify_all();
    return group;
}

void TaskExecutor::RunJob(const Job &job, std::size_t worker_index) {
    job.group->task(job.begin, job.end, worker_index, job.group->context);
}

void TaskExecutor::Wait(Group *group) {
    if (group == nullptr) return;
    for (;;) {
        Job job{};
        {
            std::unique_lock lock(mutex_);
            const auto found = std::find_if(jobs_.begin(), jobs_.end(),
                                            [group](const Job &queued) { return queued.group == group; });
            if (found == jobs_.end()) {
                work_finished_.wait(lock, [group] { return group->remaining == 0; });
                break;
            }
            job = *found;
            jobs_.erase(found);
        }
        RunJob(job, 0);
        std::lock_guard lock(mutex_);
        --group->remaining;
    }
    delete group;
}

void TaskExecutor::WorkerLoop(std::size_t worker_index) {
    std::size_t observed_generation = 0;
    for (;;) {
        Job job{};
        bool has_job = false;
        {
            std::unique_lock lock(mutex_);
            work_ready_.wait(lock, [&] {
                return stopping_ || generation_ != observed_generation || !jobs_.empty();
            });
            if (stopping_) return;
            if (generation_ == observed_generation) {
                job = jobs_.front();
                jobs_.pop_front();
                has_job = true;
            } else {
                observed_generation = generation_;
            }
        }
        if (has_job) {
            RunJob(job, worker_index);
            std::lock_guard lock(mutex_);
            if (--job.group->remaining == 0) work_finished_.notify_all();
            continue;
        }
        RunChunks();
        {
            std::lock_guard lock(mutex_);
            if (--pending_workers_ == 0) work_finished_.notify_all();
        }
    }
}
