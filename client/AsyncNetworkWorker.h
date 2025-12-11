#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

class AsyncNetworkWorker {
public:
    using Task = std::function<void()>;

    AsyncNetworkWorker();
    ~AsyncNetworkWorker();

    // Queue a task to be executed on the background thread
    void enqueueTask(Task task);

    // Check if worker is running
    bool isRunning() const { return running_.load(); }

    // Get thread activity status
    bool isProcessing() const { return is_processing_.load(); }
    
    // Get queue size
    size_t getQueueSize() const;

    // Stop the worker thread
    void stop();

private:
    void workerLoop();

    std::thread worker_thread_;
    std::queue<Task> task_queue_;
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_;
    std::atomic<bool> is_processing_;
};
