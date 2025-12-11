#include "AsyncNetworkWorker.h"
#include <iostream>

AsyncNetworkWorker::AsyncNetworkWorker() 
    : running_(true), is_processing_(false) {
    worker_thread_ = std::thread(&AsyncNetworkWorker::workerLoop, this);
}

AsyncNetworkWorker::~AsyncNetworkWorker() {
    stop();
}

void AsyncNetworkWorker::enqueueTask(Task task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push(std::move(task));
    }
    cv_.notify_one();
}

size_t AsyncNetworkWorker::getQueueSize() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return task_queue_.size();
}

void AsyncNetworkWorker::stop() {
    running_.store(false);
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void AsyncNetworkWorker::workerLoop() {
    while (running_.load()) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] {
                return !task_queue_.empty() || !running_.load();
            });

            if (!running_.load() && task_queue_.empty()) {
                break;
            }

            if (!task_queue_.empty()) {
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
        }

        if (task) {
            is_processing_.store(true);
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "[AsyncNetworkWorker] Task exception: " << e.what() << "\n";
            }
            is_processing_.store(false);
        }
    }
}
