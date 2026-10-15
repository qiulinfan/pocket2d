#ifndef POCKET2D_TASK_EXECUTOR_H
#define POCKET2D_TASK_EXECUTOR_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

// Synchronous bulk work: callbacks own disjoint [begin,end) ranges, may not
// touch Lua, SDL, Box2D, or resize their source storage. The caller participates
// and returns only after every worker has finished, including on exceptions.
class TaskExecutor {
public:
    using RangeTask = std::function<void(std::size_t, std::size_t)>;
    static std::size_t DefaultWorkerCount();
    explicit TaskExecutor(std::size_t worker_count = DefaultWorkerCount());
    ~TaskExecutor();
    TaskExecutor(const TaskExecutor &) = delete;
    TaskExecutor &operator=(const TaskExecutor &) = delete;
    std::size_t WorkerCount() const { return workers_.size(); }
    void ParallelFor(std::size_t count, std::size_t grain, const RangeTask &task);

private:
    void WorkerLoop();
    void RunChunks();
    // submissions serialize; recursive calls on the executing thread run inline.
    std::mutex submission_mutex_;
    std::mutex mutex_;
    std::condition_variable work_ready_;
    std::condition_variable work_finished_;
    bool stopping_ = false;
    std::size_t generation_ = 0;
    std::size_t pending_workers_ = 0;
    std::size_t count_ = 0;
    std::size_t grain_ = 1;
    std::atomic<std::size_t> next_{0};
    RangeTask task_;
    std::exception_ptr failure_;
    // Explicitly joined in the destructor before synchronization state dies.
    std::vector<std::jthread> workers_;
};

#endif
