// Copyright 2019 The Marl Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef marl_condition_variable_h
#define marl_condition_variable_h

#include "debug.h"
#include "defer.h"
#include "scheduler.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_set>

namespace marl {

// ConditionVariable is a synchronization primitive that can be used to block
// one or more fibers or threads, until another fiber or thread modifies a
// shared variable (the condition) and notifies the ConditionVariable.
//
// If the ConditionVariable is blocked on a thread with a Scheduler bound, the
// thread will work on other tasks until the ConditionVariable is unblocked.
class ConditionVariable {
 public:
  // notify_one() notifies and potentially unblocks one waiting fiber or thread.
  inline void notify_one();

  // notify_all() notifies and potentially unblocks all waiting fibers and/or
  // threads.
  inline void notify_all();

  // wait() blocks the current fiber or thread until the predicate is satisfied
  // and the ConditionVariable is notified.
  template <typename Predicate>
  inline void wait(std::unique_lock<std::mutex>& lock, Predicate&& pred);

  // wait_for() blocks the current fiber or thread until the predicate is
  // satisfied, and the ConditionVariable is notified, or the timeout has been
  // reached. Returns false if pred still evaluates to false after the timeout
  // has been reached, otherwise true.
  template <typename Rep, typename Period, typename Predicate>
  bool wait_for(std::unique_lock<std::mutex>& lock,
                const std::chrono::duration<Rep, Period>& duration,
                Predicate&& pred);

  // wait_until() blocks the current fiber or thread until the predicate is
  // satisfied, and the ConditionVariable is notified, or the timeout has been
  // reached. Returns false if pred still evaluates to false after the timeout
  // has been reached, otherwise true.
  template <typename Clock, typename Duration, typename Predicate>
  bool wait_until(std::unique_lock<std::mutex>& lock,
                  const std::chrono::time_point<Clock, Duration>& timeout,
                  Predicate&& pred);

 private:
  std::mutex mutex;
  std::unordered_set<Scheduler::Fiber*> waiting;
  std::condition_variable condition;
  std::atomic<int> numWaiting = {0};
  std::atomic<int> numWaitingOnCondition = {0};
};

void ConditionVariable::notify_one() {
  if (numWaiting == 0) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex);
  for (auto fiber : waiting) {
    fiber->schedule();
  }
  waiting.clear();
  lock.unlock();

  if (numWaitingOnCondition > 0) {
    condition.notify_one();
  }
}

void ConditionVariable::notify_all() {
  if (numWaiting == 0) {
    return;
  }
  std::unique_lock<std::mutex> lock(mutex);
  for (auto fiber : waiting) {
    fiber->schedule();
  }
  waiting.clear();
  lock.unlock();

  if (numWaitingOnCondition > 0) {
    condition.notify_all();
  }
}

template <typename Predicate>
void ConditionVariable::wait(std::unique_lock<std::mutex>& lock,
                             Predicate&& pred) {
  if (pred()) {
    return;
  }
  numWaiting++;
  if (auto fiber = Scheduler::Fiber::current()) {
    // Currently executing on a scheduler fiber.
    // Yield to let other tasks run that can unblock this fiber.
    while (!pred()) {
      mutex.lock();
      waiting.emplace(fiber);
      mutex.unlock();

      lock.unlock();
      fiber->yield();
      lock.lock();
    }
  } else {
    // Currently running outside of the scheduler.
    // Delegate to the std::condition_variable.
    numWaitingOnCondition++;
    condition.wait(lock, pred);
    numWaitingOnCondition--;
  }
  numWaiting--;
}

template <typename Rep, typename Period, typename Predicate>
bool ConditionVariable::wait_for(
    std::unique_lock<std::mutex>& lock,
    const std::chrono::duration<Rep, Period>& duration,
    Predicate&& pred) {
  return wait_until(lock, std::chrono::system_clock::now() + duration, pred);
}

template <typename Clock, typename Duration, typename Predicate>
bool ConditionVariable::wait_until(
    std::unique_lock<std::mutex>& lock,
    const std::chrono::time_point<Clock, Duration>& timeout,
    Predicate&& pred) {
  if (pred()) {
    return true;
  }
  numWaiting++;
  defer(numWaiting--);

  if (auto fiber = Scheduler::Fiber::current()) {
    // Currently executing on a scheduler fiber.
    // Yield to let other tasks run that can unblock this fiber.
    while (!pred()) {
      mutex.lock();
      waiting.emplace(fiber);
      mutex.unlock();

      lock.unlock();
      fiber->yield_until(timeout);
      lock.lock();

      if (std::chrono::system_clock::now() >= timeout) {
        mutex.lock();
        waiting.erase(fiber);
        mutex.unlock();
        return false;
      }
    }
    return true;
  } else {
    // Currently running outside of the scheduler.
    // Delegate to the std::condition_variable.
    numWaitingOnCondition++;
    defer(numWaitingOnCondition--);
    return condition.wait_until(lock, timeout, pred);
  }
}

}  // namespace marl

#endif  // marl_condition_variable_h
