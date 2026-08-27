# Maintained Static Hermes patch history

This orphan branch records the downstream Static Hermes commit train separately
from the source branch. The source branch (`main`) starts at the upstream
`static_h` tip and contains the maintained downstream commits; this branch
stores an ordered, applyable patch snapshot for each revision of that train.

The generated snapshot contains:

- `UPSTREAM`: the exact upstream `static_h` commit on which the series applies;
- `SOURCE`: the last source commit in the exported series;
- `SERIES`: patch filenames paired with their source commit IDs;
- `RESULT_TREE`: the exact tree produced by applying the series; and
- `patches/`: stable `git format-patch` files with commit messages and authors.

Run `scripts/update.sh` from this branch before rebasing or amending `main`,
and again after the rewrite. The updater requires a clean source worktree from
the same repository, derives the upstream base from `upstream/static_h`,
regenerates the complete series, applies every patch to a temporary index, and
verifies that the resulting tree equals the source tree:

Configure the `upstream` remote to point at `facebook/hermes` and fetch its
`static_h` branch before updating the snapshot.

```sh
./scripts/update.sh /path/to/hermes-main-worktree
./scripts/update.sh --push convex-in-prod /path/to/hermes-main-worktree
```

To reconstruct the source commits on a clean checkout at `UPSTREAM`:

```sh
./scripts/apply.sh /path/to/upstream-static-h-checkout
```

The generated patch headers use zeroed commit IDs. Source commit IDs remain in
`SERIES`, so unchanged patch content remains stable across a conflict-free
rebase while the upstream and source identities are recorded explicitly.
