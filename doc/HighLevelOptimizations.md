---
id: high-level-optimizations
title: High Level Optimizations
---

JS authors frequently use high level library functions which they expect to perform efficiently.
They call these in conjunction with other library calls to write code which has similarities to
functional programming languages.

Executing this kind of code in Hermes can be slow due to the compiler not having proper visibility
into the JS library. However, we can get some information about what is being called in certain
scenarios, such as with `-fstatic-builtins` or in actual typed SH code.

This document is a place to put some ideas on how to make specific scenarios more efficient
in the future. It is possible we'll just happen to optimize some cases here as a result of good
type information in SH and a solid optimization pipeline.

# Async startup arguments

The experimental `-fasync-frame-arguments` flag avoids materializing an arguments
object used only by internal async startup. Static Hermes recognizes the private
`spawnAsync` call after inlining, in its original async wrapper, and requires a
directly created generator function. Other uses of the arguments object retain
the ordinary path.

The native entry roots the generator and receiver, creates the result Promise,
then copies arguments from the wrapper's still-live frame into a generator call
frame. Parameter-initialization failures reject that Promise. The existing async
driver handles suspension, rejection, resumption and completion; user-visible
arguments and lexical captures keep their existing representation. No arbitrary
ancestor-frame operation is exposed to JavaScript, and startup does not depend
on replaceable `apply` properties or the optional `Reflect` global.

Existing builtin indices and the old `spawnAsync` entry are preserved. The two
new private JavaScript builtins are appended, and the bytecode version changes
to reject incompatible bytecode. The flag is disabled by default.

# Private async completion records

The experimental `-fasync-generator-results` flag implies async-frame arguments.
After inlining, it jointly changes a private async factory and its driver to
use the lowered state-machine closure directly. The factory and inner function
must have single explicit users, and every inner return must produce a fresh
`{value, done}` literal with a constant boolean completion flag. Loose arguments
objects and direct eval reject the transformation because explicit IR uses alone
do not prove that the factory cannot escape. Public generators are unchanged.

The compiler supplies an ordinary writable, null-prototype completion record.
Literal lowering preserves its two non-index keys in order, allowing direct
stores into slots 0 and 1. The native startup entry roots it across Promise
creation and factory execution. The driver retains the record across suspension
but clears its payload after consumption and abandonment. Its fields never
escape to user code or change descriptors, and the boolean slot never contains
a pointer. This is temporary result transport, not cached computation.

The original startup ABI remains available. The additional native entry and
private builtin form a compiler/runtime contract; the builtin is appended and
the bytecode version rejects incompatible inputs. The flag is disabled by default.

# Native object layout hints

`createObjectFromEntriesWithHint` accepts a non-owning hint for runtime-owned layouts
alongside the same ordered key/value callbacks as `createObjectFromEntries`.
When the actual keys match, it reuses the existing class after one comparison
pass, avoiding repeated key hashing and the layout-cache search. Different keys
or an expired hint use the ordinary bounded cache. A live hint from another
runtime is rejected before any values are decoded.

Hints do not retain objects or extend the 64-entry cache's lifetime. They may
survive runtime destruction; their weak storage then expires. The target object
and class remain rooted independently while value callbacks execute, so recursive
construction, collection and cache eviction preserve all initialized values.
Index-like keys, duplicate names and dictionary layouts retain their existing
definition path. The four layout statistics keep their existing meaning;
`hermes_objectLayoutHintHits` additionally counts the direct hint path.

# Fresh ordinary object spread

`copyDataProperties` has a bounded fast path for a fresh empty ordinary target
and an ordinary source with at most five direct named properties. The source
properties must be default enumerable data properties in direct slots, with no
symbols, index-like properties, accessors, proxies, hosts, indirect storage or
exclusion list. Custom and null prototypes are allowed because only own data
properties are observed. The target adopts the source's data-only hidden class
and copies its slots without an own-key walk or one hidden-class transition per
property.

Every unsupported layout uses the existing generic path. The fast path does not
retain object identity or values across calls, and its slot writes remain rooted
and barriered. Embeddings should validate semantics and dynamic reach before
expecting a runtime benefit; the bounded path is not a general object clone.

# Spreading and modifying existing structures

An example of some sample code found in input to Hermes to clone a Map and add an element:
```
return new Map([...Array.from(origMap), [id, obj]]);
```

1. Iterates `origMap` and creates a new array `%tmp1`, element by element.
2. Creates a new array `%tmp2`, iterates `%tmp1`, and copies the elements.
3. Adds the new array `[id, obj]` to `%tmp2`.
4. Iterates `%tmp2` and populates a newly created `Map`.

Some potential ways to simplify things:

1. `...Array.from(a)` is the same as `...a` provided that the array iterator has not been modified.
2. `new Map([...oldMap, [x, y]])` is the same as `const %tmp = new Map(oldMap); %tmp.set(x, y)`
  provided that the `Map` iterator has not been modified.

If we can enforce that iterators haven't been modified, then we can do step 1 potentially even in
legacy code, and step 2 in typed code.
