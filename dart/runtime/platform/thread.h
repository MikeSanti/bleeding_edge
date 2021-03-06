// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#ifndef PLATFORM_THREAD_H_
#define PLATFORM_THREAD_H_

#include "platform/globals.h"

// Declare the OS-specific types ahead of defining the generic classes.
#if defined(TARGET_OS_ANDROID)
#include "platform/thread_android.h"
#elif defined(TARGET_OS_LINUX)
#include "platform/thread_linux.h"
#elif defined(TARGET_OS_MACOS)
#include "platform/thread_macos.h"
#elif defined(TARGET_OS_WINDOWS)
#include "platform/thread_win.h"
#else
#error Unknown target os.
#endif

namespace dart {

class Thread {
 public:
  static ThreadLocalKey kUnsetThreadLocalKey;

  typedef void (*ThreadStartFunction) (uword parameter);

  // Start a thread running the specified function. Returns 0 if the
  // thread started successfuly and a system specific error code if
  // the thread failed to start.
  static int Start(ThreadStartFunction function, uword parameters);

  static ThreadLocalKey CreateThreadLocal();
  static void DeleteThreadLocal(ThreadLocalKey key);
  static uword GetThreadLocal(ThreadLocalKey key) {
    return ThreadInlineImpl::GetThreadLocal(key);
  }
  static void SetThreadLocal(ThreadLocalKey key, uword value);
  static intptr_t GetMaxStackSize();
  static ThreadId GetCurrentThreadId();
  static void GetThreadCpuUsage(ThreadId thread_id, int64_t* cpu_usage);
};


class Mutex {
 public:
  Mutex();
  ~Mutex();

  void Lock();
  bool TryLock();
  void Unlock();

 private:
  MutexData data_;

  DISALLOW_COPY_AND_ASSIGN(Mutex);
};


class Monitor {
 public:
  enum WaitResult {
    kNotified,
    kTimedOut
  };

  static const int64_t kNoTimeout = 0;

  Monitor();
  ~Monitor();

  void Enter();
  void Exit();

  // Wait for notification or timeout.
  WaitResult Wait(int64_t millis);
  WaitResult WaitMicros(int64_t micros);

  // Notify waiting threads.
  void Notify();
  void NotifyAll();

 private:
  MonitorData data_;  // OS-specific data.

  DISALLOW_COPY_AND_ASSIGN(Monitor);
};


class ScopedMutex {
 public:
  explicit ScopedMutex(Mutex* mutex) : mutex_(mutex) {
    ASSERT(mutex_ != NULL);
    mutex_->Lock();
  }

  ~ScopedMutex() {
    mutex_->Unlock();
  }

 private:
  Mutex* const mutex_;

  DISALLOW_COPY_AND_ASSIGN(ScopedMutex);
};


class ScopedMonitor {
 public:
  explicit ScopedMonitor(Monitor* monitor) : monitor_(monitor) {
    ASSERT(monitor_ != NULL);
    monitor_->Enter();
  }

  ~ScopedMonitor() {
    monitor_->Exit();
  }

  Monitor::WaitResult Wait(int64_t millis = dart::Monitor::kNoTimeout) {
    return monitor_->Wait(millis);
  }

  Monitor::WaitResult WaitMicros(int64_t micros = dart::Monitor::kNoTimeout) {
    return monitor_->WaitMicros(micros);
  }

  void Notify() {
    monitor_->Notify();
  }

  void NotifyAll() {
    monitor_->NotifyAll();
  }

 private:
  Monitor* const monitor_;

  DISALLOW_COPY_AND_ASSIGN(ScopedMonitor);
};


}  // namespace dart


#endif  // PLATFORM_THREAD_H_
