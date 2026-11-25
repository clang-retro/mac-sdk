// Checks the C++ library's threads over the C library's POSIX threads
// (docs/threads.md): std::thread, std::mutex, std::lock_guard,
// std::condition_variable, std::call_once, std::this_thread::yield and
// sleep_for, std::async, and static locals that two threads start
// together. tests/run.sh runs it in the simulators and checks the
// console output: one "ok" line per check, then "all ok".
//
// Copyright (C) clang-retro contributors.
// SPDX-License-Identifier: AGPL-3.0-only
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

static int failures;

// Prints one check's result, and counts the failures.
static void check(const char *what, bool ok) {
  std::printf("%s %s\n", ok ? "ok" : "FAILED", what);
  if (!ok)
    failures++;
}

static int constructions;

// A type whose constructor yields, so that a second thread arrives while
// the first constructs the static local.
struct Slow {
  int value;
  Slow() : value(42) {
    constructions++;
    std::this_thread::yield();
  }
};

// Gives a static local; the first call constructs it.
static int slow_value() {
  static Slow s;
  return s.value;
}

int main() {
  std::mutex m;
  int counter = 0;
  {
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
      ts.emplace_back([&] {
        for (int j = 0; j < 50; j++) {
          std::lock_guard<std::mutex> g(m);
          counter++;
          if (j % 10 == 0)
            std::this_thread::yield();
        }
      });
    for (auto &t : ts)
      t.join();
  }
  check("thread and mutex", counter == 200);

  std::condition_variable cv;
  std::queue<int> q;
  int sum = 0;
  std::thread consumer([&] {
    for (int n = 0; n < 10; n++) {
      std::unique_lock<std::mutex> l(m);
      cv.wait(l, [&] { return !q.empty(); });
      sum += q.front();
      q.pop();
    }
  });
  for (int i = 1; i <= 10; i++) {
    {
      std::lock_guard<std::mutex> g(m);
      q.push(i);
    }
    cv.notify_one();
    std::this_thread::yield();
  }
  consumer.join();
  check("condition_variable", sum == 55);

  std::once_flag once;
  int once_calls = 0;
  std::thread a([&] { std::call_once(once, [&] { once_calls++; }); });
  std::thread b([&] { std::call_once(once, [&] { once_calls++; }); });
  a.join();
  b.join();
  check("call_once", once_calls == 1);

  int v1 = 0, v2 = 0;
  std::thread c([&] { v1 = slow_value(); });
  std::thread d([&] { v2 = slow_value(); });
  c.join();
  d.join();
  check("static local", v1 == 42 && v2 == 42 && constructions == 1);

  auto f = std::async(std::launch::async, [] { return 6 * 7; });
  check("async", f.get() == 42);

  bool woke = false;
  std::thread sleeper([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    woke = true;
  });
  sleeper.join();
  check("sleep_for", woke);
  check("id", std::this_thread::get_id() != std::thread::id());

  std::printf(failures ? "FAILED\n" : "all ok\n");
  return 0;
}
