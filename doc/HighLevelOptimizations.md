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
