# Common-tree patch

`a21s-common.patch` contains the public-source changes against the HakiOS base recorded in `docs/source-snapshot.json`. Apply it from a matching common-tree checkout with `git apply --check` before applying. New sources are included in the patch as well as in `kernel/`.

The source-only publication omits prebuilt artifacts, vendor firmware, an unrelated plugin with unresolved upstream licensing, and non-English historical material. These omitted distributions are not embedded in deletion hunks. The full `kernel/` snapshot is authoritative.

`kernel/xnu/hakios.patch` is the separate patch against Apple XNU 8796.121.2. It was regenerated from the actual compiled source tree, including nine files whose local changes differed from the earlier export. Mesa/Panfrost has its own complete patch under `graphics/`.
