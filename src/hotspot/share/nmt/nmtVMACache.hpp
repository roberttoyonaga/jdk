/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_NMT_NMTVMACACHE_HPP
#define SHARE_NMT_NMTVMACACHE_HPP

#include "nmt/memTag.hpp"
#include "nmt/memTracker.hpp"
#include "nmt/virtualMemoryTracker.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/permitForbiddenFunctions.hpp"
#include "utilities/bitMap.hpp"
#include "memory/resourceArea.hpp"

/// NMT virtual memory

inline bool range_intersects(const void* from1, const void* to1, const void* from2, const void* to2) {
  return MAX2(from1, from2) < MIN2(to1, to2);
}

// A Cache that correlates range with MemTag, optimized to be iterated quickly
// (cache friendly).
class CachedNMTInformation : public VirtualMemoryWalker {
  struct Range { const void* from; const void* to; };
  // We keep ranges apart from flags since that prevents the padding a combined
  // structure would have, and it allows for faster iteration of ranges since more
  // of them fit into a cache line.
  Range* _ranges;
  MemTag* _mem_tags;
  size_t _count, _capacity;
  mutable size_t _last;//** consider renaming this

public:

  static constexpr int max_intersections = 8; // It's highly unlikely this needs to be higher than 2

  struct Intersection {
    const void* from;
    const void* to;
    MemTag tag;
  };

  struct IntersectionResult {
    Intersection intersections[max_intersections];
    int count;
  };

  CachedNMTInformation() : _ranges(nullptr), _mem_tags(nullptr),
                           _count(0), _capacity(0), _last(0) {}

  ~CachedNMTInformation() {
    permit_forbidden_function::free(_ranges);
    permit_forbidden_function::free(_mem_tags);
  }

  IntersectionResult find_intersections(const void* from, const void* to) const {
    IntersectionResult result;
    result.count = 0;

    if (_count <= _last) { // TODO remove this. lookup() doesn't do this.
      return result;
    }

    if (to <= _ranges[_last].from) {
      _last = 0;
    }

    for (uintx i = _last; i < _count; i++) {
      if (_ranges[i].to <= from) {
        // We really shouldn't reach here, but check anyway just in case there are accounting inconsistencies.
        continue;
      }
      if (_ranges[i].from >= to) {
        // *** stopping condition. We've hit the end of this VMA, time to parse the next line
        break;
      }
      // *** intersection
      if (result.count < max_intersections) {
        result.intersections[result.count].from = MAX2(from, _ranges[i].from);
        result.intersections[result.count].to = MIN2(to, _ranges[i].to);
        result.intersections[result.count].tag = _mem_tags[i];
        result.count++;
      } else {
        //TODO log a warning
      }
      if (_ranges[i].to <= to) {
        // Only bump _last when we know the NMT region does not overlap with the next VMA.
        // This can happen when a range is reserved then os::protect_memory is called on part of it (ie thread stacks).
        _last = i + 1;
      }
    }
    return result;
  }

  bool add(const void* from, const void* to, MemTag mem_tag) {
    // We rely on NMT regions being sorted by base
    assert(_count == 0 || (from >= _ranges[_count - 1].to), "NMT regions unordered?");
    // we can just fold two regions if they are adjacent and have the same mem_tag.
    if (_count > 0 && from == _ranges[_count - 1].to && mem_tag == _mem_tags[_count - 1]) {
      _ranges[_count - 1].to = to;
      return true;
    }
    if (_count == _capacity) {
      // Enlarge if needed
      const size_t new_capacity = MAX2((size_t)4096, 2 * _capacity);
      // Unfortunately, we need to allocate manually, raw, since we must prevent NMT deadlocks.
      _ranges = (Range*)permit_forbidden_function::realloc(_ranges, new_capacity * sizeof(Range));
      _mem_tags = (MemTag*)permit_forbidden_function::realloc(_mem_tags, new_capacity * sizeof(MemTag));
      if (_ranges == nullptr || _mem_tags == nullptr) {
        // In case of OOM lets make no fuss. Just return.
        return false;
      }
      _capacity = new_capacity;
    }
    assert(_capacity > _count, "Sanity");
    _ranges[_count] = Range { from, to };
    _mem_tags[_count] = mem_tag;
    _count++;
    return true;
  }

  // Given a vma [from, to), find all regions that intersect with this vma and
  // fill out their collective flags into bm.
  void lookup(const void* from, const void* to, ResourceBitMap& bm) const {
    assert(from <= to, "Sanity");
    // We optimize for sequential lookups. Since this class is used when a list
    // of OS mappings is scanned (VirtualQuery, /proc/pid/maps), and these lists
    // are usually sorted in order of addresses, ascending.
    if (to <= _ranges[_last].from) {
      // the range is to the right of the given section, we need to re-start the search
      _last = 0;
    }
    for(uintx i = _last; i < _count; i++) {
      if (range_intersects(from, to, _ranges[i].from, _ranges[i].to)) {
        bm.set_bit((BitMap::idx_t)_mem_tags[i]);
      } else if (to <= _ranges[i].from) {
        _last = i;
        break;
      }
    }
  }

  bool do_allocation_site(const VirtualMemoryRegion* rgn) override {
    // Cancel iteration if we run out of memory (add returns false);
    return add(rgn->base(), rgn->end(), rgn->mem_tag());
  }

  // Iterate all NMT virtual memory regions and fill this cache.
  bool fill_from_nmt() {
    return MemTracker::walk_virtual_memory(this);
  }
};

#endif // SHARE_NMT_NMTVMACACHE_HPP

