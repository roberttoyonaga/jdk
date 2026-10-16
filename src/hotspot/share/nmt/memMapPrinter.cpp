/*
 * Copyright (c) 2023, 2026, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2023, 2024, Red Hat, Inc. and/or its affiliates.
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
 *
 */

#if defined(LINUX) || defined(_WIN64) || defined(__APPLE__)

#include "gc/shared/collectedHeap.hpp"
#include "logging/logAsyncWriter.hpp"
#include "memory/allocation.hpp"
#include "memory/resourceArea.hpp"
#include "memory/universe.hpp"
#include "nmt/memMapPrinter.hpp"
#include "nmt/memTag.hpp"
#include "nmt/memTracker.hpp"
#include "nmt/nmtVMACache.hpp"
#include "nmt/virtualMemoryTracker.hpp"
#include "runtime/nonJavaThread.hpp"
#include "runtime/osThread.hpp"
#include "runtime/thread.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vmThread.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ostream.hpp"
#include "utilities/permitForbiddenFunctions.hpp"

// Note: throughout this code we will use the term "VMA" for OS system level memory mapping

/// NMT mechanics

// Short, clear, descriptive names for all possible markers. Note that we only expect to see
// those that have been used with mmap. Flags left out are printed with their nmt tags name.
#define NMT_FLAGS_DO(f) \
  /* mem_tag, short, description */ \
  f(mtGCCardSet,      "CARDTBL", "GC Card table") \
  f(mtClassShared,    "CDS", "CDS archives") \
  f(mtClass,          "CLASS", "Class Space") \
  f(mtCode,           "CODE", "Code Heap") \
  f(mtGC,             "GC", "GC support data (e.g. bitmaps)") \
  f(mtInternal,       "INTERN", "Internal") \
  f(mtJavaHeap,       "JAVAHEAP", "Java Heap") \
  f(mtOther,          "JDK", "allocated by JDK libraries other than VM") \
  f(mtMetaspace,      "META", "Metaspace nodes (non-class)") \
  f(mtSafepoint,      "POLL", "Polling pages") \
  f(mtThreadStack,    "STACK", "(known) Thread Stack") \
  f(mtTest,           "TEST", "JVM internal test mappings")
  //end

static const char* get_shortname_for_mem_tag(MemTag mem_tag) {
#define DO(t, shortname, text) if (t == mem_tag) return shortname;
  NMT_FLAGS_DO(DO)
#undef DO
  return NMTUtil::tag_to_enum_name(mem_tag);
}

/////// Thread information //////////////////////////

// Given a VMA [from, to) and a thread, check if vma intersects with thread stack
static bool vma_touches_thread_stack(const void* from, const void* to, const Thread* t) {
  // Java thread stacks (and sometimes also other threads) have guard pages. Therefore they typically occupy
  // at least two distinct neighboring VMAs. Therefore we typically have a 1:n relationshipt between thread
  // stack and vma.
  // Very rarely however is a VMA backing a thread stack folded together with another adjacent VMA by the
  // kernel. That can happen, e.g., for non-java threads that don't have guard pages.
  // Therefore we go for the simplest way here and check for intersection between VMA and thread stack.
  // Note it is possible to encounter a brand new thread that has not yet initialized its stack fields.
  return t->stack_base_or_null() != nullptr && range_intersects(from, to, (const void*)t->stack_end(), (const void*)t->stack_base());
}

struct GCThreadClosure : public ThreadClosure {
  bool _found;
  uintx _tid;
  const void* const _from;
  const void* const _to;
  GCThreadClosure(const void* from, const void* to) : _found(false), _tid(0), _from(from), _to(to) {}
  void do_thread(Thread* t) override {
    if (_tid == 0 && t != nullptr && vma_touches_thread_stack(_from, _to, t)) {
      _found = true;
      _tid = t->osthread()->thread_id();
      // lemme stooop! No way to signal stop :(
    }
  }
};

static void print_thread_details(uintx thread_id, const char* name, outputStream* st) {
  // avoid commas and spaces in output to ease post-processing via awk
  char tmp[64];
  stringStream ss(tmp, sizeof(tmp));
  ss.print(":%zu-%s", (uintx)thread_id, name);
  for (int i = 0; tmp[i] != '\0'; i++) {
    if (!isalnum(tmp[i])) {
      tmp[i] = '-';
    }
  }
  st->print_raw(tmp);
}

// Given a region [from, to), if it intersects a known thread stack, print detail infos about that thread.
static void print_thread_details_for_supposed_stack_address(const void* from, const void* to, outputStream* st) {

  ResourceMark rm;

#define HANDLE_THREAD(T)                                                        \
  if (T != nullptr && vma_touches_thread_stack(from, to, T)) {                  \
    print_thread_details((uintx)(T->osthread()->thread_id()), T->name(), st);   \
    return;                                                                     \
  }
  for (JavaThreadIteratorWithHandle jtiwh; JavaThread* t = jtiwh.next(); ) {
    HANDLE_THREAD(t);
  }
  HANDLE_THREAD(VMThread::vm_thread());
  HANDLE_THREAD(WatcherThread::watcher_thread());
  HANDLE_THREAD(AsyncLogWriter::instance());
#undef HANDLE_THREAD

  if (Universe::heap() != nullptr) {
    GCThreadClosure cl(from, to);
    Universe::heap()->gc_threads_do(&cl);
    if (cl._found) {
      print_thread_details(cl._tid, "GC Thread", st);
    }
  }
}

///////////////

MappingPrintSession::MappingPrintSession(outputStream* st, const CachedNMTInformation& nmt_info) :
    _out(st), _nmt_info(nmt_info)
{}

void MappingPrintSession::print_nmt_flag_legend() const {
#define DO(flag, shortname, text) _out->print_cr("%10s: %s", shortname, text);
  NMT_FLAGS_DO(DO)
#undef DO
}

bool MappingPrintSession::print_nmt_info_for_region(const void* vma_from, const void* vma_to) const {
  int num_printed = 0;
  // print NMT information, if available
  if (MemTracker::enabled()) {
    // Correlate vma region (from, to) with NMT region(s) we collected previously.
    ResourceMark rm;
    ResourceBitMap flags(mt_number_of_tags);
    _nmt_info.lookup(vma_from, vma_to, flags);
    if (!flags.is_empty()) {
      for (int i = 0; i < mt_number_of_tags; i++) {
        const MemTag mem_tag = (MemTag)i;
        if (flags.at((BitMap::idx_t)mem_tag)) {
          if (num_printed > 0) {
            _out->put(',');
          }
          _out->print("%s", get_shortname_for_mem_tag(mem_tag));
          if (mem_tag == mtThreadStack) {
            print_thread_details_for_supposed_stack_address(vma_from, vma_to, _out);
          }
          num_printed++;
        }
      }
    }
  }
  return num_printed > 0;
}

void MemMapPrinter::print_all_mappings(outputStream* st) {
  CachedNMTInformation nmt_info;
  st->print_cr("Memory mappings:");
  // Prepare NMT info cache. But only do so if we print individual mappings,
  // otherwise, we won't need it and can save that work.
  if (MemTracker::enabled()) {
    nmt_info.fill_from_nmt();
  } else {
    st->print_cr("NMT is disabled. VM info not available.");
  }
  MappingPrintSession session(st, nmt_info);
  pd_print_all_mappings(session);
}

#endif // LINUX
