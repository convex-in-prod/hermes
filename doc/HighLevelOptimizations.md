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

# Record arguments and a restricted Object prototype

Function signature optimization can replace fresh, unaliased record arguments
with their fields, including fields captured in a single initialized closure
environment. Every field read must have an own-data-property or absence proof;
unknown callers, observable record identity, writes and unsupported uses remain
generic. Captured values retain normal environment storage and GC roots.

The optional `-fstatic-object-prototype` flag permits an absence proof for
nonstandard string keys on the intrinsic `Object.prototype`. Before any code
compiled with this flag executes, the embedding must validate that this
prototype is nonextensible, has a null prototype, and has no string keys other
than `constructor`, `toString`, `toLocaleString`, `valueOf`, `hasOwnProperty`,
`isPrototypeOf`, `propertyIsEnumerable`, `__proto__`, `__defineGetter__`,
`__defineSetter__`, `__lookupGetter__`, and `__lookupSetter__`. The compiler does
not perform that runtime validation. The flag is disabled by default.

Nonextensibility keeps other string keys absent even across unknown calls.
Existing standard keys may still change values or descriptors, so missing reads
of those keys remain generic. Custom prototypes remain generic as well. A
literal with a null prototype can supply the same absence proof without this
flag. No property is added to a record to represent absence.

# Guarded method calls

The experimental `-fguarded-method-calls` flag exposes lexically available
method bodies before inlining. It captures a method's original closure when
the IR creates it, without reading the prototype again. A selected call keeps
its original property lookup, receiver and argument evaluation, then checks
that the capture is initialized and equals the actual callee. The matching
branch has a proved function target and closure; the other branch keeps the
original indirect call. Non-inlined matching calls can therefore also use
direct native calls.

The private capture holds only undefined or closures from the same creation
site. Loading it through the caller's lexical environment distinguishes
separate factory invocations. Calls before initialization, replacement methods,
bound functions and proxies retain the generic path. Inlined bodies still
perform their original property reads and writes, including accessor effects.

Selection requires an accessible entry scope and an unambiguous literal method
name. Calls on a member's `this` prefer a method defined on the same object;
that preference selects a candidate and never replaces the identity guard.
Caller membership is retained even when the caller is too large to be a target.
Targets are bounded to 96 initial IR instructions and 24 blocks, with at most
16 guarded sites per function and 512 per module. The flag is off by default;
exposing a body does not itself establish a runtime benefit.

# Guarded primitive string lengths

Optimized Static Hermes lowering can cache a primitive string's length before a
character-scanning loop. A single type guard establishes string immutability;
the cached number remains valid across callbacks and collection. The receiver
must already dominate the loop preheader, and nested loops cannot reuse a cache
across changes to that receiver. No raw character-storage pointer is retained.

Other receivers execute their original property reads at their original points.
This preserves array-length mutations, getters, proxies and exception ordering.
The initial transformation handles one receiver and at most eight read sites per
function, requires a unique preheader and excludes functions containing try/catch.
Single-byte character comparisons select candidates but do not provide the type
proof. This transformation requires no embedding assumptions or runtime ABI change.

# Guarded trim comparisons

The experimental `-fstring-trim-comparisons` flag fuses a `trim()` call used only
to compare its result strictly with its receiver. It is disabled by default.
When the receiver is a primitive string and the actual callee is the original
trim builtin, only the first and last UTF-16 units determine the comparison.
The fast path avoids the JavaScript call frame, coercion and substring operation.

Method lookup and argument evaluation still execute in their original order.
Custom methods, proxies, wrappers and noncallable values retain the original
call and comparison. The call and comparison must be adjacent, and the result
must have no other users. No prototype immutability assumption, retained string
pointer or cross-invocation cache is required. This does not deduplicate enclosing
normalization functions.

# Returned record inlining

The experimental Static Hermes flag `-finline-record-returns` targets a separate
case: a helper returns one fresh record and a caller only reads its existing own
fields. The result must contain at most eight string keys, have one return site,
and have no other uses except initialization stores. This exposes the returned
allocation to ordinary object stack promotion without treating input records as
immutable.

This policy permits cloning exception regions, including iterator cleanup,
only into callers that already contain handlers. Introducing the first handler
would otherwise force all caller temporaries into rooted exception frames.
The existing prohibition on arguments objects and generators remains.
Catch targets are repaired after inlining. It admits at most 256 significant
instructions per producer, 512 added instructions per caller and 128 call sites
per module in each inlining pass. Escaping results and reads of absent fields do
not qualify. The flag is off by default; it can increase exception-frame costs
and requires runtime evaluation before use.

# Guarded record reads

The experimental `-fguarded-record-loads` flag runs before common-subexpression
elimination and requires `-fstatic-object-prototype`. It is disabled by default.
A nonallocating runtime probe accepts exact ordinary objects with data-only,
nondictionary layouts and null or intrinsic Object prototypes. Standard inherited
keys remain excluded. A receiver guard covers multiple fields; child receivers
need independent guards. Original reads remain available at their original points
when any guard fails, without replaying earlier operations.

The pass retains common values across forward joins only when every predecessor
has been analyzed. Guard validity is merged independently from field values.
Writes, unknown calls, unsupported effects and loop backedges end propagation.
Supported arithmetic and selected Math builtins retain value facts when operands
are numbers; runtime type checks disable reuse before potentially effectful
coercions. No object layout or value is assumed immutable across arbitrary calls.

Known helpers can preserve facts when a bounded transitive inspection proves
that they only perform supported parameter field reads and effect-free work.
Receiver probes cover those actual arguments before the original helper executes;
a failed probe invalidates earlier values. Parameter forwarding is supported,
but loaded children are not inferred immutable from their parents. Recursive
calls, writes, unsupported coercions and unknown targets remain excluded.
The proof examines at most four nested functions and 4,096 helper instructions
per caller, caching call-site results across candidate regions.

Analysis considers at most 64 starts, 256 instructions, 32 reads, 16 blocks,
eight receiver guards and sixteen numeric guards per candidate region. It selects
one region per function and excludes functions containing try/catch. Static
repetition alone does not establish useful dynamic reach or performance benefit.

# Invocation-scoped string computation reuse

The experimental `-fscoped-normalization-reuse` pass summarizes a bounded helper
whose successful return is a string derived from named record fields, immediate
lexical captures, supported string operations and dense string-array joins.
It requires `-fstatic-object-prototype` and is disabled by default. Successful
paths cannot write external state, call unknown code or contain loops. Throw-only
paths keep their original operations and exceptions; failures are never cached.

At the proved helper entry, non-observable probes require an ordinary data-only
record and the original own `trim`, `includes` and `join` methods. The current
field and capture values must all be strings. A previous result is reusable only
when those values match; receiver identity alone is insufficient. Getters,
proxies, custom methods and unsupported inputs execute the original call. The
intrinsic check distinguishes native methods sharing a code pointer but using
different contexts. Reads never move across an observable validation operation.

The embedding creates a `HermesRuntime::StringReuseScope` for one invocation.
It owns sixteen fixed entries rooted as ordinary VM locals. The runtime holds a
non-owning active-scope pointer; nested scopes are independent and restore the
outer scope on destruction. Scopes must be destroyed in reverse creation order,
before destroying their runtime. Embeddings must cover async work belonging to
the invocation and end the scope before cleanup can retire the runtime.

Keys combine native function metadata identity and up to twelve current primitive
strings. No closure or environment is retained. Instrumented helpers cannot be
inlined because their native frame identifies the proof. Successful string
results can be reused across independent helper and callback activations while
the scope exists; no entries survive its destruction. Without an active scope,
the guard selects the original body. Each stored tuple admits at most 32,768
UTF-16 code units across inputs and result; oversized values execute normally
without storing a result. Round-robin replacement clears unused roots.
After thirty-two consecutive misses for a helper, the scope bypasses further
reuse attempts for that helper. A hit resets the miss count, and a fresh scope
starts with no disabled helpers. This bounds lookup/store overhead when the
working set exceeds cache capacity; it does not change the original computation.
The scope tracks at most sixteen helper identities. Its admission decision is
checked before intrinsic descriptors or record fields are inspected.

The pass admits sixteen helpers per module, with eight fields, four captures and
384 instructions per helper. Dynamic reach and useful runtime benefit must be
established before enabling it for a workload.

# Known array predicate callbacks

The experimental `-farray-callbacks` flag exposes supported `some` callbacks as
ordinary IR calls before inlining. It is disabled by default. The initial pass
considers at most eight sites per function and accepts directly created normal
functions with at most 128 IR instructions.

The fast path requires the captured original `Array.prototype.some` callee and
an actual array receiver. Proxies, array-like objects and replaced methods keep
the original call. The method lookup and argument evaluations remain in order;
only the nonconfigurable array length is snapshotted. Each iteration performs
its original presence test and value read, so holes, inherited indices, getters,
and changes made by callbacks retain their behavior. Callback arguments,
`thisArg`, early exit and exceptions are preserved. No element-storage pointer
or field-value fact is retained across user code.

This exposes callback work to existing optimization passes. It does not itself
prove element fields immutable or guarantee that a callback will be inlined.

# Native integer temporaries

The experimental `-fnative-integer-registers` flag separates proved integer
lifetimes from other nonpointer temporaries during Static Hermes allocation.
Before emission, every definition and consumer of a reused register is checked.
Supported registers use `int32_t`, `uint32_t`, or `int64_t` when both signed and
unsigned ranges occur. Unsupported registers retain tagged storage. Functions
with try/catch and outlined functions retain their original representation.

Bitwise operations and numeric `Math.imul` keep intermediate results in integer
storage, including supported loop phis. Numeric `imul` also avoids the outgoing
argument-frame moves normally introduced for builtin calls. Generic coercions
still execute through their original runtime operations; JavaScript calls,
returns and captured fields receive boxed numbers. Negative zero is excluded
from the integer proof, including mixed phi inputs. Integer temporaries are
nonpointers and require no GC roots. The flag is disabled by default.

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
