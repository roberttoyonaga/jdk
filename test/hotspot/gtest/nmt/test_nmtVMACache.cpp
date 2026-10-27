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

#include "nmt/nmtVMACache.hpp"
#include "memory/resourceArea.hpp"
#include "utilities/bitMap.inline.hpp"
#include "unittest.hpp"

TEST_VM(NMTVMACacheTest, test_lookup_split_region) {
  CachedNMTInformation cache;
  // Add a couple NMT regions to the cache. The mtThreadStack NMT region spans 2 VMAs.
  cache.add((const void*)0x1000, (const void*)0x101000, mtThreadStack);
  cache.add((const void*)0x200000, (const void*)0x300000, mtGC);

  ResourceMark rm;

  // Simulate inspection of guard page VMA
  {
    ResourceBitMap bm(mt_number_of_tags);
    cache.lookup((const void*)0x1000, (const void*)0x2000, bm);
    EXPECT_TRUE(bm.at((BitMap::idx_t)mtThreadStack));
  }
  // Simulate inspection of thread stack VMA
  {
    ResourceBitMap bm(mt_number_of_tags);
    cache.lookup((const void*)0x2000, (const void*)0x101000, bm);
    EXPECT_TRUE(bm.at((BitMap::idx_t)mtThreadStack)) << "VMA should still be tagged mtThreadStack";
  }
  // Simulate inspection of VMA used by GC corresponding to the next NMT region in the cache.
  {
    ResourceBitMap bm(mt_number_of_tags);
    cache.lookup((const void*)0x200000, (const void*)0x300000, bm);
    EXPECT_TRUE(bm.at((BitMap::idx_t)mtGC));
  }
}

