/*
 * Copyright (C) 2019 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "heap_tagging.h"
#include "malloc_common.h"
#include "malloc_tagged_pointers.h"

#include <bionic/pthread_internal.h>
#include <platform/bionic/malloc.h>

extern "C" void scudo_malloc_disable_memory_tagging();
extern "C" void scudo_malloc_set_track_allocation_stacks(int);

// Protected by `g_heap_tagging_lock`.
static HeapTaggingLevel heap_tagging_level = M_HEAP_TAGGING_LEVEL_NONE;

void SetDefaultHeapTaggingLevel() {
#if defined(__aarch64__)
#if !__has_feature(hwaddress_sanitizer)
  heap_tagging_level = __libc_shared_globals()->initial_heap_tagging_level;
#endif
  switch (heap_tagging_level) {
    case M_HEAP_TAGGING_LEVEL_TBI:
      __libc_globals.mutate([](libc_globals* globals) {
        // Arrange for us to set pointer tags to POINTER_TAG, check tags on
        // deallocation and untag when passing pointers to the allocator.
        globals->heap_pointer_tag = (reinterpret_cast<uintptr_t>(POINTER_TAG) << TAG_SHIFT) |
                                    (0xffull << CHECK_SHIFT) | (0xffull << UNTAG_SHIFT);
      });
      break;
#if defined(USE_SCUDO)
    case M_HEAP_TAGGING_LEVEL_SYNC:
      scudo_malloc_set_track_allocation_stacks(1);
      break;

    case M_HEAP_TAGGING_LEVEL_NONE:
      scudo_malloc_disable_memory_tagging();
      break;
#endif  // USE_SCUDO
    default:
      break;
  }
#endif  // aarch64
}

static bool set_tcf_on_all_threads(int tcf) {
  static int g_tcf;
  g_tcf = tcf;

  return android_run_on_all_threads(
      [](void*) {
        int tagged_addr_ctrl = prctl(PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0);
        if (tagged_addr_ctrl < 0) {
          return false;
        }

        tagged_addr_ctrl = (tagged_addr_ctrl & ~PR_MTE_TCF_MASK) | g_tcf;
        if (prctl(PR_SET_TAGGED_ADDR_CTRL, tagged_addr_ctrl, 0, 0, 0) < 0) {
          return false;
        }
        return true;
      },
      nullptr);
}

pthread_mutex_t g_heap_tagging_lock = PTHREAD_MUTEX_INITIALIZER;

// Requires `g_heap_tagging_lock` to be held.
HeapTaggingLevel GetHeapTaggingLevel() {
  return heap_tagging_level;
}

// Requires `g_heap_tagging_lock` to be held.
bool SetHeapTaggingLevel(void* arg, size_t arg_size) {
  if (arg_size != sizeof(HeapTaggingLevel)) {
    return false;
  }

  auto tag_level = *reinterpret_cast<HeapTaggingLevel*>(arg);
  if (tag_level == heap_tagging_level) {
    return true;
  }

  switch (tag_level) {
    case M_HEAP_TAGGING_LEVEL_NONE:
      if (heap_tagging_level == M_HEAP_TAGGING_LEVEL_TBI) {
        __libc_globals.mutate([](libc_globals* globals) {
          // Preserve the untag mask (we still want to untag pointers when passing them to the
          // allocator), but clear the fixed tag and the check mask, so that pointers are no longer
          // tagged and checks no longer happen.
          globals->heap_pointer_tag = static_cast<uintptr_t>(0xffull << UNTAG_SHIFT);
        });
      } else if (!set_tcf_on_all_threads(PR_MTE_TCF_NONE)) {
        error_log("SetHeapTaggingLevel: set_tcf_on_all_threads failed");
        return false;
      }
#if defined(USE_SCUDO)
      scudo_malloc_disable_memory_tagging();
#endif
      break;
    case M_HEAP_TAGGING_LEVEL_TBI:
    case M_HEAP_TAGGING_LEVEL_ASYNC:
    case M_HEAP_TAGGING_LEVEL_SYNC:
      if (heap_tagging_level == M_HEAP_TAGGING_LEVEL_NONE) {
        error_log(
            "SetHeapTaggingLevel: re-enabling tagging after it was disabled is not supported");
        return false;
      } else if (tag_level == M_HEAP_TAGGING_LEVEL_TBI ||
                 heap_tagging_level == M_HEAP_TAGGING_LEVEL_TBI) {
        error_log("SetHeapTaggingLevel: switching between TBI and ASYNC/SYNC is not supported");
        return false;
      }

      if (tag_level == M_HEAP_TAGGING_LEVEL_ASYNC) {
        set_tcf_on_all_threads(PR_MTE_TCF_ASYNC);
#if defined(USE_SCUDO)
        scudo_malloc_set_track_allocation_stacks(0);
#endif
      } else if (tag_level == M_HEAP_TAGGING_LEVEL_SYNC) {
        set_tcf_on_all_threads(PR_MTE_TCF_SYNC);
#if defined(USE_SCUDO)
        scudo_malloc_set_track_allocation_stacks(1);
#endif
      }
      break;
    default:
      error_log("SetHeapTaggingLevel: unknown tagging level");
      return false;
  }

  heap_tagging_level = tag_level;
  info_log("SetHeapTaggingLevel: tag level set to %d", tag_level);

  return true;
}
