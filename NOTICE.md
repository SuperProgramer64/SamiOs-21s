# Origins and publication scope

- Common HakiOS source base: `oosexgang/hakios-xnu-kernel`, commit `87bf485` (full revision is in `docs/source-snapshot.json`). Original copyright notices are preserved.
- A21s device layer: HakiOS A21s sources, MIT except XNU derivatives (APSL 2.0).
- Mali kernel transport and Panfrost userspace integration: HakiOS A32 reference. Exynos power/register handling is distinct from MediaTek handling; the latter must not be applied to the A21s.
- TD4150 and Exynos USB source: simpleXNU/ios-a135f, Schoosh, with the A217F adaptations attributed to Bruhboy666dev in the source headers.
- Mesa 25.0.7: distributed as source patches, retaining Mesa/X.Org notices. Mesa itself is obtained separately from its official archive.
- Samsung's GPL kernel sources were consulted for hardware register locations and sequencing. They are not copied into an APSL/MIT driver by this publication. The wireless work remains research, not a completed port.

The common tree's upstream `NOTICE`, `THIRD_PARTY_LICENSES.md` and license files are retained as historical inventories; references there to `prebuilt/`, embedded FocalTech firmware, and the unrelated `HakiosMetalCA.m` Adreno plugin do not mean those files are distributed here. They are explicitly excluded. The A21s's working Panfrost/OpenGL path and CoreAnimation hooks are included.

The FocalTech driver now explicitly refuses a firmware download if no locally supplied header is available. This preserves compilation without redistributing the vendor blob; the A21s uses TD4150 instead. To build another phone's FocalTech path, generate that ignored header from firmware you supply yourself with `tools/make-focaltech-header.py`.

Publication does not include proprietary firmware, Apple binaries, private device trees, screenshots, partition contents, raw logs, identifiers, user data, credentials, or compiled payloads. Locally recorded component hashes and anonymized technical findings are metadata only.
