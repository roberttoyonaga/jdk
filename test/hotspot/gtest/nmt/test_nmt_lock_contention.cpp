/*
 * Copyright (c) 2026, Red Hat, Inc. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

//#include "precompiled.hpp"
#include "memory/allocation.hpp"
#include "nmt/memTracker.hpp"
#include "runtime/atomic.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"
#include "threadHelper.inline.hpp"
#include "unittest.hpp"

/*
 * Microbenchmark: NMT lock contention in os::reserve_memory / os::release_memory.
 *
 * Multiple threads concurrently do reserve+release cycles. When NMT is enabled,
 * these operations contend on NmtVirtualMemoryLocker. Moving pd_release_memory
 * outside the lock scope should reduce hold time and improve throughput.
 *
 * Build & run (from JDK root):
 *
 *   # 1. Build with your change (PR branch):
 *   make CONF=<your-config> test-image
 *
 *   # 2. Run just these tests with NMT enabled:
 *   build/<config>/images/test/hotspot/gtest/gtestLauncher \
 *     -jdk:build/<config>/jdk \
 *     -- --gtest_filter='nmt.release_memory_contention*' \
 *     -vmoption:-XX:NativeMemoryTracking=summary
 *
 *   # 3. Repeat on baseline (master) and compare wall time & throughput.
 */

static const int ITERATIONS_PER_THREAD = 2000;

static Atomic<int64_t> total_nanos{0};

static void bench_reserve_release(Thread* current, int id) {
  const size_t alloc_size = 4 * os::vm_page_size();
  const jlong start = os::javaTimeNanos();

  for (int i = 0; i < ITERATIONS_PER_THREAD; i++) {
    char* mem = os::reserve_memory(alloc_size, mtTest);
    ASSERT_NE(mem, nullptr) << "reserve_memory failed on thread " << id << " iteration " << i;
    os::release_memory(mem, alloc_size);
  }

  const jlong elapsed = os::javaTimeNanos() - start;
  total_nanos.add_then_fetch((int64_t)elapsed);
}

static void run_bench(int num_threads) {
  if (!MemTracker::enabled()) {
    tty->print_cr("NMT is disabled -- results won't reflect NMT lock contention.");
    tty->print_cr("Re-run with -XX:NativeMemoryTracking=summary for meaningful results.");
  }

  total_nanos.store_relaxed(0);

  auto fn = [](Thread* t, int id) {
    bench_reserve_release(t, id);
  };

  const jlong wall_start = os::javaTimeNanos();

  {
    TestThreadGroup<decltype(fn)> threads{fn, num_threads};
    threads.doit();
    threads.join();
  }

  const jlong wall_elapsed = os::javaTimeNanos() - wall_start;
  const int64_t sum_thread_nanos = total_nanos.load_relaxed();
  const int total_ops = num_threads * ITERATIONS_PER_THREAD;

  tty->print_cr("=== NMT reserve+release benchmark ===");
  tty->print_cr("  Threads:            %d", num_threads);
  tty->print_cr("  Iterations/thread:  %d", ITERATIONS_PER_THREAD);
  tty->print_cr("  Total ops:          %d", total_ops);
  tty->print_cr("  Wall time:          %.3f ms", wall_elapsed / 1e6);
  tty->print_cr("  Sum of thread time: %.3f ms", sum_thread_nanos / 1e6);
  tty->print_cr("  Throughput:         %.0f ops/sec", total_ops / (wall_elapsed / 1e9));
  tty->print_cr("  Avg latency:        %.3f us/op", (sum_thread_nanos / (double)total_ops) / 1e3);
  tty->print_cr("=====================================");
}

TEST_VM(nmt, release_memory_contention_1_thread) {
  run_bench(1);
}

TEST_VM(nmt, release_memory_contention_4_threads) {
  run_bench(4);
}

TEST_VM(nmt, release_memory_contention_8_threads) {
  run_bench(8);
}

TEST_VM(nmt, release_memory_contention_16_threads) {
  run_bench(16);
}

TEST_VM(nmt, release_memory_contention_32_threads) {
  run_bench(32);
}
