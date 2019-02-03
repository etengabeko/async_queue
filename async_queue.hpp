#ifndef ASYNC_QUEUE_HPP
#define ASYNC_QUEUE_HPP

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>

namespace details {

template <typename R, typename F>
void Do(std::promise<R>& p, F&& f) {
  p.set_value(f());
}

template <typename F>
void Do(std::promise<void>& p, F&& f) {
  f();
  p.set_value();
}

} // details

template <typename Arg, typename Result>
class AsyncHandledQueue {
public:
  explicit AsyncHandledQueue(std::function<Result(Arg &&)> &&handler)
      : handler_(std::forward<decltype(handler)>(handler)),
        is_interrupted_(false),
        work_thread_(std::bind(&AsyncHandledQueue::Run, this)) {}

  ~AsyncHandledQueue() {
    is_interrupted_ = true;
    cv_.notify_one();
    work_thread_.join();
  }

  std::future<Result> Add(Arg &&element) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::promise<Result> promise;
    std::future<Result> result = promise.get_future();

    queue_.emplace(std::forward<Arg>(element), std::move(promise));
    cv_.notify_one();

    return result;
  }

  size_t Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  bool IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

private:
  void Run() {
    do {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return (is_interrupted_ || !queue_.empty()); });

      if (is_interrupted_)
        break;

      std::queue<std::pair<Arg, std::promise<Result>>> copy;
      copy.swap(queue_);
      lock.unlock();

      while (!copy.empty()) {
        Arg arg = std::move(copy.front().first);
        std::promise<Result> promise = std::move(copy.front().second);
        copy.pop();

        try {
          details::Do(promise, [this, &arg]() { return handler_(std::move(arg)); });
        } catch (std::exception&) {
          promise.set_exception(std::current_exception());
        }
      }
    } while (true);
  }

private:
  std::function<Result(Arg&&)> handler_;
  std::queue<std::pair<Arg, std::promise<Result>>> queue_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> is_interrupted_;

  std::thread work_thread_;
};

#endif // ASYNC_QUEUE_HPP
