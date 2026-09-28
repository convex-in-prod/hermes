/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// REQUIRES: linux
// RUN: rm -rf %t.dir
// RUN: %python %S/Inputs/check-c-bundle-layout.py %shermes %t.dir %c_compiler %static_h_config %S/../../include %hermesvm_lib_dir %shermes_console_lib_dir

// The driver checks edited sources, bounded retained slots, function/cache/literal stability,
// typed shape separation, exact surrogate bytes and malformed layout rejection in separate processes.
