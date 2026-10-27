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

#include "logging/log.hpp"
#include "nmt/nmtVMACache.hpp"
#include "nmt/nmtRss.hpp"
#include "procMapsParser.hpp"
#include "runtime/os.hpp"
#include "utilities/globalDefinitions.hpp"

void NMTRssSnapshot::collect() {
  CachedNMTInformation nmt_cache;
  if (!nmt_cache.fill_from_nmt()) {
    return;
  }

  constexpr char filename[] = "/proc/self/smaps";
  FILE* f = os::fopen(filename, "r");
  if (f == nullptr) {
    log_warning(nmt)("Failed to open %s for NMT RSS collection.", filename);
    return;
  }

  ProcSmapsInfo info;
  ProcSmapsParser parser(f);
  while (parser.parse_next(info)) {
    if (info.rss == 0) {
      continue;
    }
    // Count RSS for all VMAs not just ones NMT is tracking.
    _total_rss += info.rss;

    CachedNMTInformation::IntersectionResult result = nmt_cache.find_intersections(info.from, info.to);

    if (result.count == 0) {
      // No NMT regions match this VMA. That's possible and okay.
      continue;
    }
    // Count RSS only for VMAs NMT is tracking.
    //_total_rss += info.rss;

    size_t vma_size = pointer_delta(info.to, info.from, 1);
    if (result.count == 1) {
      size_t nmt_region_size = pointer_delta(result.intersections[0].to, result.intersections[0].from, 1);
      // nmt_region_size is already clamped to the VMA size.
      // Possible overflow risk if regions are very large.
      _rss_by_tag[NMTUtil::tag_to_index(result.intersections[0].tag)] += info.rss * nmt_region_size / vma_size;
      // Only count RSS corresponding to an NMT region.
      //_total_rss += info.rss * nmt_region_size / vma_size;
      continue;
    }

    // The kernel has clumped multiple adjacent regions with identical protections together.
    // Distribute RSS proportionally.
    for (int i = 0; i < result.count; i++) {
      size_t nmt_region_size = pointer_delta(result.intersections[i].to, result.intersections[i].from, 1);
      // Possible overflow risk if regions are very large.
      _rss_by_tag[NMTUtil::tag_to_index(result.intersections[i].tag)] += info.rss * nmt_region_size / vma_size;
      // Only count RSS corresponding to an NMT region.
      //_total_rss += info.rss * nmt_region_size / vma_size;
    }
  }

  ::fclose(f);
  _is_valid = true;
  return;
}
