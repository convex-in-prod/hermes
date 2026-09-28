/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#ifndef HERMES_VMLAYOUTS_PROPERTYCACHE_H
#define HERMES_VMLAYOUTS_PROPERTYCACHE_H

// Ordinary SH named reads reserve two adjacent entries. Other users, including
// the JIT, retain the single-entry layout and API.
#define SH_NAMED_READ_CACHE_WAYS 2

// Four sets of two shape/key pairs keep both receiver diversity and key
// diversity bounded without increasing the per-site allocation.
#define SH_COMPUTED_READ_CACHE_WAYS 8
#define SH_COMPUTED_READ_CACHE_ASSOCIATIVITY 2

#endif
