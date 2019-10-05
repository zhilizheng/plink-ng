#ifndef __PLINK2_THREAD_H__
#define __PLINK2_THREAD_H__

// This library is part of PLINK 2.00, copyright (C) 2005-2019 Shaun Purcell,
// Christopher Chang.
//
// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// This library is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


// Basic multithreading code.  Uses native Win32 API instead of pthreads
// emulation on Windows.
#include "plink2_base.h"

#ifdef _WIN32
#  include <process.h>
#else
#  include <pthread.h>
#endif

// Thread functions are expected to be of the form
//   THREAD_FUNC_DECL function_name(void* raw_arg) {
//     ThreadGroupFuncArg* arg = S_CAST(ThreadGroupFuncArg*, raw_arg);
//     uint32_t tidx = arg->tidx;
//     ...
//     do {
//       ... // process current block
//     } while (!THREAD_BLOCK_FINISH(arg));
//     THREAD_RETURN;
//   }
#ifdef _WIN32
#  define pthread_t HANDLE
#  define THREAD_FUNC_DECL unsigned __stdcall
#  define THREAD_FUNCPTR_T(func_ptr) unsigned (__stdcall *func_ptr)(void*)
  // #define THREAD_FUNCPP_T(func_pp) unsigned (__stdcall **func_pp)(void*)
#  define THREAD_RETURN return 0
#else
#  define THREAD_FUNC_DECL void*
#  define THREAD_FUNCPTR_T(func_ptr) void* (*func_ptr)(void*)
  // #define THREAD_FUNCPP_T(func_pp) void* (**func_pp)(void*)
#  define THREAD_RETURN return nullptr
#endif

#ifdef __cplusplus
namespace plink2 {
#endif

#ifdef _WIN32
// If kMaxThreads > 64, single WaitForMultipleObjects calls must be converted
// into loops.  Which isn't a big deal, but let's keep things simpler for now.
CONSTI32(kMaxThreads, 64);
#else
// currently assumed to be less than 2^16 (otherwise some multiply overflows
// are theoretically possible, at least in the 32-bit build)
CONSTI32(kMaxThreads, 512);
#endif

#ifdef __APPLE__
// cblas_dgemm may fail with 128k
CONSTI32(kDefaultThreadStack, 524288);
#else
// asserts didn't seem to work properly with a setting much smaller than this
CONSTI32(kDefaultThreadStack, 131072);
#endif

typedef struct ThreadGroupControlBlockStruct {
  // Neither thread-functions nor the thread-group owner should touch these
  // variables directly.
  uintptr_t spawn_ct;
#ifdef _WIN32
  HANDLE* start_next_events;
  HANDLE* cur_block_done_events;
#else
  pthread_mutex_t sync_mutex;
  pthread_cond_t cur_block_done_condvar;
  pthread_cond_t start_next_condvar;
  uint32_t active_ct;
#endif

  // Thread-functions can safely read from these.
  uint32_t thread_ct;

  // 1 = process last block and exit; 2 = immediate termination requested
  uint32_t is_last_block;
} ThreadGroupControlBlock;

typedef struct ThreadGroupSharedStruct {
  void* context;
  ThreadGroupControlBlock cb;
} ThreadGroupShared;

typedef struct ThreadGroupFuncArgStruct {
  ThreadGroupShared* sharedp;
  uint32_t tidx;
} ThreadGroupFuncArg;

typedef struct ThreadGroupStruct {
  ThreadGroupShared shared;
  THREAD_FUNCPTR_T(thread_func_ptr);
  pthread_t* threads;
  ThreadGroupFuncArg* thread_args;
  // Generally favor uint16_t/uint32_t over unsigned char/uint8_t for isolated
  // bools, since in the latter case the compiler is fairly likely to generate
  // worse code due to aliasing paranoia; see e.g.
  //   https://travisdowns.github.io/blog/2019/08/26/vector-inc.html
  uint16_t is_unjoined;
  uint16_t is_active;

#ifndef _WIN32
  // 1 = sync_mutex, 2 = cur_block_done_condvar, 4 = start_next_condvar
  uint32_t sync_init_bits;
#endif
} ThreadGroup;

void PreinitThreads(ThreadGroup* tgp);

// Return value is clipped to 1..kMaxThreads.
// If known_procs_ptr is non-null, it's set to the raw unclipped value (which
// can theoretically be -1 if the sysconf call fails)
uint32_t NumCpu(int32_t* known_procs_ptr);

// Also allocates, returning 1 on failure.
BoolErr SetThreadCt(uint32_t thread_ct, ThreadGroup* tgp);

HEADER_INLINE uint32_t GetThreadCt(const ThreadGroup* tgp) {
  return tgp->shared.cb.thread_ct;
}

HEADER_INLINE void SetThreadFuncAndData(THREAD_FUNCPTR_T(start_routine), void* shared_context, ThreadGroup* tgp) {
  assert(!tgp->is_active);
  tgp->shared.context = shared_context;
  tgp->shared.cb.is_last_block = 0;
  tgp->thread_func_ptr = start_routine;
}

// Equivalent to SetThreadFuncAndData() with unchanged
// start_routine/shared_context.  Ok to call this "unnecessarily".
HEADER_INLINE void ReinitThreads(ThreadGroup* tgp) {
  assert(!tgp->is_active);
  tgp->shared.cb.is_last_block = 0;
}

// Note that, if there's only one block of work-shards, this should be called
// before the first SpawnThreads() call.
HEADER_INLINE void DeclareLastThreadBlock(ThreadGroup* tgp) {
  assert(!tgp->is_unjoined);
  tgp->shared.cb.is_last_block = 1;
}

#if defined(__cplusplus) && !defined(_WIN32)
class Plink2ThreadStartup {
public:
  pthread_attr_t smallstack_thread_attr;
  Plink2ThreadStartup() {
#  ifdef NDEBUG
    // we'll error out for another reason soon enough if there's insufficient
    // memory...
    pthread_attr_init(&smallstack_thread_attr);
#  else
    assert(!pthread_attr_init(&smallstack_thread_attr));
#  endif
    // if this fails due to kDefaultThreadStack being smaller than the system
    // page size, no need to error out
    pthread_attr_setstacksize(&smallstack_thread_attr, kDefaultThreadStack);
  }

  ~Plink2ThreadStartup() {
    pthread_attr_destroy(&smallstack_thread_attr);
  }
};

extern Plink2ThreadStartup g_thread_startup;
#endif

BoolErr SpawnThreads(ThreadGroup* tgp);

void JoinThreads(ThreadGroup* tgp);

void CleanupThreads(ThreadGroup* tgp);

#ifdef _WIN32
HEADER_INLINE BoolErr THREAD_BLOCK_FINISH(ThreadGroupFuncArg* tgfap) {
  ThreadGroupControlBlock* cbp = &(tgfap->sharedp->cb);
  if (cbp->is_last_block) {
    return 1;
  }
  const uint32_t tidx = tgfap->tidx;
  SetEvent(cbp->cur_block_done_events[tidx]);
  WaitForSingleObject(cbp->start_next_events[tidx], INFINITE);
  return (cbp->is_last_block == 2);
}
#else
BoolErr THREAD_BLOCK_FINISH(ThreadGroupFuncArg* tgfap);
#endif

#ifdef __cplusplus
}  // namespace plink2
#endif

#endif  // __PLINK2_THREAD_H__
