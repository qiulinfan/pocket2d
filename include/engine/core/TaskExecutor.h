#ifndef POCKET2D_TASK_EXECUTOR_H
#define POCKET2D_TASK_EXECUTOR_H

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
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
    // The process-wide pool shared by all Worlds (particles, physics). It lives
    // until process exit; concurrent submitters are supported.
    static TaskExecutor &Shared();
    explicit TaskExecutor(std::size_t worker_count = DefaultWorkerCount());
    ~TaskExecutor();
    TaskExecutor(const TaskExecutor &) = delete;
    TaskExecutor &operator=(const TaskExecutor &) = delete;
    std::size_t WorkerCount() const { return workers_.size(); }
    void ParallelFor(std::size_t count, std::size_t grain, const RangeTask &task);

    // Asynchronous parallel-for for task systems such as Box2D's. Submit
    // splits [0,count) into at most WorkerCount()+1 ranges of at least
    // min_range and queues them. Wait runs the group's still-queued ranges on
    // the calling thread, then waits for ranges that workers took, so a waiter
    // never depends on a free worker. Callbacks get a worker index that is 0
    // inside Wait and 1..WorkerCount() on pool workers; two ranges of one
    // group never run with the same index at once. Callbacks must not throw.
    using IndexedRangeTask = void (*)(std::size_t begin, std::size_t end, std::size_t worker, void *context);
    struct Group;
    Group *Submit(std::size_t count, std::size_t min_range, IndexedRangeTask task, void *context);
    void Wait(Group *group); // also releases the group

private:
    struct Job {
        Group *group;
        std::size_t begin, end;
    };
    void WorkerLoop(std::size_t worker_index);
    void RunChunks();
    static void RunJob(const Job &job, std::size_t worker_index);
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
    std::deque<Job> jobs_; // guarded by mutex_
    // Explicitly joined in the destructor before synchronization state dies.
    std::vector<std::jthread> workers_;
};

#endif
