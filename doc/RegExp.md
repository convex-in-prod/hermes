---
id: regexp
title: RegExp
---

## RegExp

The general Hermes regexp engine uses a backtracking stack and executes regexp bytecode. For regexp literals like `/abc/`, compilation occurs ahead of execution: HBC embeds the regexp bytecode in its bytecode file, and Static Hermes emits static byte arrays in generated C. Regexp bytecode is distinct from Hermes bytecode.

The regexp engine proceeds as follows:

1. *Parse phase.* The regexp parser emits a tree of nodes, effectively an IR.
1. *Optimization phase.* The node tree is traversed and optimized in various ways.
1. *Emitting phase.* The node tree is traversed and emits regexp bytecode.
1. *Execution phase.* The bytecode is executed against an input string.

## Static Hermes compiled matchers

Static Hermes can emit pattern-specific C control flow from validated regexp
bytecode. The ordinary C toolchain compiles these functions to native or Wasm
code. Eligible concatenations contain literals, classes, dots, captures, word
boundaries, ordinary or multiline anchors, and greedy or lazy width-one
repetition, including counted repetition. Captures may surround a repetition but
cannot occur inside its body. Patterns with alternatives must be capture-free
throughout; their branches may contain the other terms and width-one repetitions.
Empty branches and matches are supported.
Global, sticky and indices flags retain their ordinary builtin behavior.

The matcher emits direct character predicates and repetition loops, with direct
branches for backtracking. It scans candidate starts using the possible first
characters and, where available, a leading word boundary. Captures and loop
positions use bounded call-local storage. Alternatives preserve their original
order, including retries when a common suffix fails. Forward branches target
validated operation boundaries; they cannot enter a literal payload or a
repetition body. A bounded stack holds one pending choice per alternative or
repetition site. Fixed-width concatenations with an ordinary end anchor check
only their final possible start, subject to sticky and start
anchors. Character classes derive from the interpreter's tables. Matchers accept
ASCII and UTF-16 input without conversion, including lone surrogates.
Non-Unicode case-insensitive literals and runs expand canonical equivalents
using the same Hermes tables as the interpreter and character-class compiler.
Case-insensitive class bytecode already contains those equivalents. In particular,
non-Unicode folding does not equate long s or the Kelvin sign with ASCII letters.
Local modifiers are interpreted through the actual bytecode instructions.

Unicode mode, patterns combining alternatives and captures, repeated alternatives,
lookarounds, backreferences, repeated captures and other unsupported instructions
retain the interpreter. This is not a DFA or a
linear-time matching guarantee. A generated search has a work budget of 262,144
candidate attempts, repetition and backtracking steps; exhaustion restarts the complete
original bytecode search without publishing partial captures. The interpreter
retains its existing resource errors. Guest execution metering still applies to
the generated code and to the fallback.

First-character and boundary rejection do not consume that backtracking budget:
they advance through the subject once. A single character predicate uses a
direct linear scan without a speculative work cap or duplicate predicate check.

Generation examines at most 2 KiB of bytecode per pattern and accepts at most 64
operations, 64 explicit class ranges, eight captures, eight repetition sites and
eight alternative sites. Branching matchers have at most sixteen pending choices;
their scratch storage is independent of the subject length.
Generated C is limited to 24 KiB per pattern and 512 KiB per logical module, with
at most 256 distinct patterns analyzed for compiled matchers. Identical compiled
patterns share one function across literal sites and bundle shards. These are
source-generation limits, not promises about final machine-code size. Repetition
is never unrolled, and alternatives are not expanded into separate full paths.

If compiled-function generation declines, Static Hermes may derive the existing
fixed-width descriptor for one shared matcher. That tier supports non-Unicode,
capture-free concatenations of case-sensitive literals, classes and dots, optionally
with ordinary start/end anchors. It examines at most 1 KiB of bytecode, accepts
16 consuming terms and 32 ranges, and emits at most 64 KiB of descriptors per
logical module. All remaining patterns use the bytecode interpreter.
Case-insensitive non-Unicode classes also qualify because their bytecode ranges
already include case equivalents; folded literal instructions do not qualify.

## Whole-operation lowering

Static Hermes recognizes a fresh literal used only by `.test()` and an
unanchored, capture-free, non-Unicode character predicate. It also recognizes
global character filtering with an empty replacement, optionally preceded by
`toLowerCase()` or consumed by strict equality with the original input. These
operations can omit the RegExp object and generic builtin call frames. They use
the validated bytecode descriptor, not source-pattern heuristics.
Its descriptors share the existing 64 KiB per-module auxiliary-data budget;
exhaustion selects the retained original expression.

The compiler keeps the complete original expression behind a runtime guard.
Only contiguous expression instructions qualify: moving method lookup across
argument coercion or other user effects would change behavior. The runtime
requires a primitive string and exact own builtin methods on the intrinsic
prototypes, including the global/unicode flag getters for replacement. Changed
methods, accessors, coercible objects, sticky matching and unsupported regex
shapes retain the original path. A directly indexed string argument can be
consumed as a code unit only when its numeric index is integral and in bounds.

Lowercase/filter fusion currently admits strings unchanged by root lowercasing.
It checks Hermes's full Unicode casing tables for BMP characters; expansions,
context-sensitive conversions and surrogate pairs retain normal conversion.
Platforms with external casing tables use the fused lowercase path only for
ASCII. The scan constructs output lazily after the first removed character;
an equality consumer needs no output string. Even that consumer scans to the
end so legacy match state reflects the final successful replacement match.

Successful indexed tests retain a single code unit for legacy `RegExp.input`
and match/context getters, materializing its string only if observed. They do
not retain the containing source string. Failed tests preserve previous state;
ordinary successful matches replace the lazy input. Captures, contexts and
input remain observable across GC. No cross-request normalization cache is used.

## Matching state and ownership

`RegExp.prototype.test` coerces its input and resolves `exec` once. Custom callable
methods still run and must return an object or null. When the resolved operation
is the builtin matcher on a real RegExp, or the specified non-callable fallback
selects it, `test` omits the JavaScript match array, substring results, groups and
indices. Matching still retains capture ranges, updates legacy RegExp properties
on success, and performs the ordinary `lastIndex` conversions and writes. Failure
leaves previous legacy match state intact; stack overflow remains an error.
Object consumers such as `exec` and string matching operations still receive
ordinary match objects.

Boolean tests can resolve an ordinary builtin `exec` data property without
allocating lookup handles when existing property maps establish its value.
Missing property maps, accessors and proxy/host prototype chains retain ordinary
lookup.
Global replacement with an empty string and ordinary builtin `exec` copies
unmatched spans directly, omitting match arrays, capture substrings and the list
of results. Input/replacement coercion, the observable global/unicode reads and
the initial `lastIndex` write occur before the builtin `exec` guard. The fast path
requires an internally global RegExp, maintains
`lastIndex`, empty-match advancement and legacy capture state, and never invokes
custom `exec` or a replacement callback. Other replacements use the ordinary
object-result protocol. These paths do not eliminate the RegExp object itself.

Each literal evaluation whose object can be observed creates a distinct mutable
RegExp object. Only generated
Static Hermes bytecode, descriptors and matcher functions are borrowed: their
immutable storage must remain alive for the runtime's full lifetime under the existing persistent SHUnit
contract, including when supplied by an embedding loader. Every RegExp carries
both a descriptor pointer and a generated matcher pointer, plus the bytecode
ownership tag, including instances that use the interpreter. Generated code is
shared by the module rather than copied into a per-instance executable cache.
Dynamic compilation,
HBC literals and same-flags copies own their bytecode. Finalization and per-object
native-size/snapshot accounting include only owned bytecode; borrowed static data
still belongs to the module and its memory accounting.

The executor's synchronous capture sink borrows ranges only during its call and
must not reenter JavaScript or trigger GC while raw input pointers are live. The
VM copies those ranges directly into its existing native match storage. This
removes an intermediate output vector, not the interpreter's capture and
backtracking state. The existing stack guard and embedding interruption contract
remain in force; there is no new cancellation protocol. Prepared runtime images
still require the exact compatible runtime, module graph, memory layout and
function-table reconstruction. Generated matchers are linked into their owning
module and passed through the existing specialized-literal helper; they add no
host import or separate memory. The helper signature and RegExp layout require
matching generated code and runtime artifacts.

## Supported Syntax

As of this writing, Hermes regexp supports

1. All of ES6, including global, case-insensitive, multiline, sticky, and Unicode (and legacy).
1. ES9 lookbehinds.
1. Named capture groups.
1. Unicode property escapes.
1. Match indices (`d`), dotAll (`s`), and local `i`, `m`, and `s` modifiers.
