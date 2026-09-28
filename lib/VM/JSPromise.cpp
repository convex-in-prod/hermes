/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "hermes/VM/JSPromise.h"
#include "hermes/VM/BuildMetadata.h"

namespace hermes {
namespace vm {

const ObjectVTable JSPromise::vt{
    VTable(CellKind::JSPromiseKind, cellSize<JSPromise>()),
    JSPromise::_getOwnIndexedRangeImpl,
    JSPromise::_haveOwnIndexedImpl,
    JSPromise::_getOwnIndexedPropertyFlagsImpl,
    JSPromise::_getOwnIndexedImpl,
    JSPromise::_setOwnIndexedImpl,
    JSPromise::_deleteOwnIndexedImpl,
    JSPromise::_checkAllOwnIndexedImpl,
};

void JSPromiseBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  mb.addJSObjectOverlapSlots(JSObject::numOverlapSlots<JSPromise>());
  JSObjectBuildMeta(cell, mb);
  const auto *self = static_cast<const JSPromise *>(cell);
  mb.setVTable(&JSPromise::vt);
  mb.addField("result", &self->result_);
  mb.addField("firstReaction", &self->first_);
  mb.addField("lastReaction", &self->last_);
}

PseudoHandle<JSPromise> JSPromise::create(
    Runtime &runtime,
    Handle<JSObject> parent) {
  auto *cell = runtime.makeAFixed<JSPromise>(
      runtime,
      parent,
      runtime.getHiddenClassForPrototype(*parent, numOverlapSlots<JSPromise>()));
  return JSObjectInit::initToPseudoHandle(runtime, cell);
}

} // namespace vm
} // namespace hermes
