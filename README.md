# Maintained Static Hermes patch history

This orphan branch records the downstream Static Hermes commit train separately
from the source branch. The source branch (`static_h`) starts at the upstream
`static_h` tip and contains the maintained downstream commits; this branch
stores an ordered, applyable patch snapshot for each revision of that train.

Keep the source train organized by subsystem rather than by the embedding that
uses it. Runtime/compiler interfaces and their consumers may share a patch;
unrelated features should not accumulate in a general Wasm patch. Fold fixes,
tests and documentation into the patch that owns the behavior.

The substantial areas are the runtime registry/ABI, C-bundle emission, typed
language support, native value construction, VM property/primitive fast paths,
GC preparation and reclamation, native Promises/async startup, regex execution
and specialization, compiler optimization, retained C layouts/exception dispatch,
and the static ICU provider. Keep small independent correctness changes separate
when that makes their purpose clearer. The maintained patches are ownership
boundaries; upstream pull requests may extract smaller changes from them.

The subsystem reorganization preserves the complete source tree. Its commit
identities change, but the rewrite alone does not require rebuilding artifacts,
changing consumer pins, or publishing a release. `RESULT_TREE` records the source
content independently of those commit identities.

The generated snapshot contains:

- `UPSTREAM`: the exact upstream `static_h` commit on which the series applies;
- `SOURCE`: the last source commit in the exported series;
- `SERIES`: patch filenames paired with their source commit IDs;
- `RESULT_TREE`: the exact tree produced by applying the series; and
- `patches/`: stable `git format-patch` files with commit messages and authors.

Run the canonical updater at `$HOME/.local/lib/patch-history/update.sh` from
this branch before rebasing or amending `static_h`, and again after the rewrite.
The updater requires a clean source worktree from the same repository with
`static_h` checked out and tracking `convex-in-prod/static_h`. It derives the
upstream base from `upstream/static_h`, regenerates the complete series, applies
every patch to a temporary index, and verifies that the resulting tree equals
the source tree. Use `--history-root` when invoking it from another directory:

Configure the `upstream` remote to point at `facebook/hermes` and fetch its
`static_h` branch before updating the snapshot. Configure the `convex-in-prod`
remote to point at this fork and fetch its `static_h` branch.

```sh
"$HOME/.local/lib/patch-history/update.sh" --message "refresh the static_h downstream train" /path/to/hermes-static-h-worktree
"$HOME/.local/lib/patch-history/update.sh" --push convex-in-prod --message "refresh the static_h downstream train" /path/to/hermes-static-h-worktree
```

The required `--message` is a concise human summary of why this snapshot is
being recorded; it becomes the subject of the history commit. Use `--no-commit`
only to inspect generated changes before recording them; it does not accept
`--message`.

To reconstruct the source commits on a clean checkout at `UPSTREAM`:

```sh
./scripts/apply.sh /path/to/upstream-static-h-checkout
```

The generated patch headers use zeroed commit IDs. Source commit IDs remain in
`SERIES`, so unchanged patch content remains stable across a conflict-free
rebase while the upstream and source identities are recorded explicitly.
