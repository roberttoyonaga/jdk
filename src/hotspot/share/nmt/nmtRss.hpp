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

#ifndef SHARE_NMT_NMTRSS_HPP
#define SHARE_NMT_NMTRSS_HPP

#include "nmt/nmtCommon.hpp"
#include "nmt/memTag.hpp"
#include "utilities/globalDefinitions.hpp"

class NMTRssSnapshot {
  size_t _rss_by_tag[mt_number_of_tags];
  size_t _total_rss;
  bool _is_valid;

public:
  NMTRssSnapshot() : _rss_by_tag{}, _total_rss(0), _is_valid(false) {}
  // Gathers instantaneous RSS data.
  LINUX_ONLY(void collect();)
  NOT_LINUX(void collect() { })

  // collect() has successfully completed on this snapshot.
  bool is_valid() const { return _is_valid; }

  size_t rss(MemTag mem_tag) const { return _rss_by_tag[NMTUtil::tag_to_index(mem_tag)]; }
  size_t total_rss() const { return _total_rss; }
};
#endif //SHARE_NMT_NMTRSS_HPP
