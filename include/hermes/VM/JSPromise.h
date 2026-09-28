/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "hermes/VM/Callable.h"
#include "hermes/VM/JSObject.h"

namespace hermes {
namespace vm {

/// Experimental Promise storage. State is independent of ordinary properties
/// and the object's prototype; only the native resolution operations mutate it.
class JSPromise final : public JSObject {
  friend void JSPromiseBuildMeta(const GCCell *, Metadata::Builder &);
  friend struct NativePromiseCore;
  friend CallResult<HermesValue> nativePromiseOperation(void *, Runtime &);

 public:
  static const ObjectVTable vt;
  static constexpr CellKind getCellKind() {
    return CellKind::JSPromiseKind;
  }
  static bool classof(const GCCell *cell) {
    return cell->getKind() == getCellKind();
  }
  static PseudoHandle<JSPromise> create(
      Runtime &runtime,
      Handle<JSObject> parent);

  JSPromise(Runtime &runtime, Handle<JSObject> parent, Handle<HiddenClass> clazz)
      : JSObject(runtime, *parent, *clazz) {}

 private:
  enum class State : uint8_t { Pending, Fulfilled, Rejected, Adopted };
  State state_{State::Pending};
  bool handled_{false};
  GCHermesValue result_{};
  GCPointer<NativeFunction> first_{};
  GCPointer<NativeFunction> last_{};
};

} // namespace vm
} // namespace hermes
