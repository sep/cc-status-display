# Maintenance

This is the single living document of every external dependency this
project relies on, where to watch them, how to bump them, and what to
test after. If you're picking this project back up after months away,
**start here** — it's designed to remove the "where do I even look?"
phase of catching up.

The general posture is **proactive over reactive**: we pin tight,
review changes deliberately, and build forward-canaries against
upstream's next release. The CI surfaces drift; humans decide when to
adopt it.

## Active maintenance signals

| Signal | Cadence | Lives in | What it means |
| --- | --- | --- | --- |
| **Dependabot PRs** | Mondays | `.github/dependabot.yml` | A pinned GitHub Action has a new upstream version. Review the diff + release notes; merge if green. |
| **Smoke build** | Mondays | `.github/workflows/smoke.yml` | A weekly rebuild on currently-pinned versions. Red mail = supply-chain rot (managed-component went away, base image broke, etc.). |
| **ESP-IDF canary** | Mondays | `.github/workflows/idf-canary.yml` | Fires only when Espressif tags an ESP-IDF release newer than our pin. Green = safe to bump. Red = migration work needed. |

If Mondays are quiet, the project is healthy. If your inbox lights up,
treat each one as a single ticket and drain the queue before adding
features.

## Dependencies

### ESP-IDF (the build framework)

- **Currently pinned to:** `v6.0`
- **Pinned in:**
  - `eim_config.toml` (developer install)
  - `.github/workflows/release.yml` — `esp_idf_version: v6.0`
  - `.github/workflows/smoke.yml`  — `esp_idf_version: v6.0`
  - `.github/workflows/idf-canary.yml` — `env.PINNED_IDF_VERSION: v6.0`
- **Watch:** <https://github.com/espressif/esp-idf/releases>
- **Bump procedure:**
  1. Wait for the canary workflow to go green against the new version.
     If it's red, address migration items first.
  2. Edit all four locations above to the new version string.
  3. Push a branch, let smoke + a tag-less release dry-run pass.
  4. On a dev board, flash and run the full smoke recipe from each
     per-board page (idle / working / blocked / blocked / configure / identify).
  5. Tag a new semver and push.
- **Migration history:**
  - v5.x → v6.0: cJSON moved from a built-in component to the managed
    component `espressif/cjson`. `nvs.h` moved (still works via
    `<nvs.h>`). `usb_serial_jtag` driver lives in
    `esp_driver_usb_serial_jtag` component.

### `esphome/esp-hub75` (HUB75 panel driver)

- **Currently pinned to:** `^0.3.6` (caret = compatible-update; minor
  bumps come automatically via the component manager, majors don't).
- **Pinned in:** `main/idf_component.yml`
- **Watch:** <https://components.espressif.com/components/esphome/esp-hub75>
  and <https://github.com/esphome-libs/esp-hub75/releases>
- **Risk:** small maintainer pool. If the project goes inactive, we'd
  fork and self-vendor.
- **Bump procedure:** edit `idf_component.yml`, rebuild, smoke-test on
  hardware. Pay attention to any API breaks in `Hub75Config` — that
  struct's fields are the contract surface we touch.

### `espressif/cjson` (JSON parser)

- **Currently pinned to:** `^1.7.19`
- **Pinned in:** `main/idf_component.yml`
- **Watch:** <https://components.espressif.com/components/espressif/cjson>
- **Risk:** very low — cJSON is one of the most stable C libraries
  there is.

### `esp-web-tools` (browser flasher)

- **Currently pinned to:** `10.2.1`
- **Pinned in:** `<script src="...">` tags in
  - `docs/site/index.html`
  - `docs/site/boards/thing_plus.html`
  - `docs/site/boards/lonely_binary_n16r8.html`
- **CDN:** jsDelivr (`cdn.jsdelivr.net/npm/esp-web-tools@<version>/...`).
  jsDelivr respects exact version pins — unlike `unpkg`, which used to
  silently drift. Don't switch back to a floating major.
- **Watch:** <https://github.com/espressif/esp-web-tools/releases>
- **Bump procedure:** edit the version string in all three HTML files,
  load the local preview (`python3 -m http.server` from `docs/site/`),
  click each Flash button to confirm the WebSerial dialog still opens.
  No firmware change needed.

### GitHub Actions

All actions in `.github/workflows/*.yml` are **pinned to commit SHAs**
(not tags) for supply-chain security. Tags are mutable; SHAs aren't.
Dependabot rewrites both the SHA and the trailing version comment when
bumping.

| Action | Pinned at | Watch |
| --- | --- | --- |
| `actions/checkout` | `34e114876b0b11c390a56381ad16ebd13914f8d5` (v4.3.1) | <https://github.com/actions/checkout/releases> |
| `espressif/esp-idf-ci-action` | `e6f5c74232b1ccd4c97ed641f1e48553853f1fd5` (v1.2.0) | <https://github.com/espressif/esp-idf-ci-action/releases> |
| `actions/upload-artifact` | `ea165f8d65b6e75b540449e92b4886f43607fa02` (v4.6.2) | <https://github.com/actions/upload-artifact/releases> |
| `actions/download-artifact` | `d3f86a106a0bac45b974a628896c90dbdf5c8093` (v4.3.0) | <https://github.com/actions/download-artifact/releases> |
| `actions/configure-pages` | `983d7736d9b0ae728b81ab479565c72886d7745b` (v5.0.0) | <https://github.com/actions/configure-pages/releases> |
| `actions/upload-pages-artifact` | `56afc609e74202658d3ffba0e8f6dda462b719fa` (v3.0.1) | <https://github.com/actions/upload-pages-artifact/releases> |
| `actions/deploy-pages` | `d6db90164ac5ed86f2b6aed7e0febac5b3c0c03e` (v4.0.5) | <https://github.com/actions/deploy-pages/releases> |
| `softprops/action-gh-release` | `3bb12739c298aeb8a4eeaf626c5b8d85266b0e65` (v2.6.2) | <https://github.com/softprops/action-gh-release/releases> |

Dependabot opens a PR weekly with any available bumps. Read the
upstream release notes (Dependabot links them in the PR body) and merge
if the changes look benign. If something concerning shows up
(behavioral changes, deprecated outputs we depend on), close the PR
and pin to the previous SHA explicitly.

### Hardware

This is the one dimension we can't pin or canary. Boards do get
discontinued.

- **SparkFun Thing Plus ESP32-S3** — bring-up board, well supported.
  Vendor is reliable. Risk: low.
- **Lonely Binary ESP32-S3 N16R8 Gold Edition** — smaller vendor.
  Risk: medium. If the SKU is discontinued, document a substitute in
  `docs/site/boards/`.
- **WaveShare RGB-Matrix-P2.5-64×32** — generic HUB75, easy to
  substitute with any compatible 64×32 1/16-scan panel.

When adding support for a new board, follow CONTRIBUTING.md.

## Versioning

The firmware version's source of truth is **`version.txt`** at the
repo root. Bumped by hand when starting work toward a new release;
tagged into git when shipping.

**Convention** (matches npm / Cargo / most ecosystems): `version.txt`
holds **bare semver** like `0.3.0`. Git tags use the standard
**`v`-prefixed** form like `v0.3.0`. The CI verification strips a
leading `v` from both sides before comparing, so either form on
either side is accepted — but bare-in-file + v-prefixed-tag is the
documented norm. Either way, the boot-log and tarball filename
always present with the `v` prefix for consistency (CMake prepends
it if missing).

CMake reads `version.txt` and sets `PROJECT_VER`:

- **Release builds** (CI workflow `.github/workflows/release.yml`,
  triggered by tag pushes) export `CC_RELEASE_BUILD=1` in the
  environment. Those builds use the bare `version.txt` string,
  e.g. `v0.1.0`.
- **Everything else** (local dev, smoke, canary) appends a 6-char
  commit SHA as `+abc123`, so the binary always carries the exact
  commit it was built from. Format: `v0.1.0+a1b2c3`.

The version is embedded in the app image header at build time and
read at runtime via `esp_app_get_description()`. The firmware logs
it on boot alongside the board label, e.g.:

```
I (1234) claude-status: firmware: v0.1.0  board: Lonely Binary ESP32-S3 N16R8 Gold Edition
```

A few consequences:

- **The CI release-tarball filename uses `${GITHUB_REF_NAME}`** (the
  tag string). The release workflow also fails the build if
  `version.txt` and the pushed tag disagree, so the boot-log version
  and the artifact filename are guaranteed to match.
- **Local builds may have a slightly stale SHA suffix** if you commit
  without re-running CMake's configure step. Run
  `idf.py reconfigure && idf.py build` (or `idf.py fullclean`) when
  exact-SHA accuracy matters.
- **If git isn't available** (e.g., source-only download with no
  `.git` directory), the SHA falls back to `+nosha`. Still indicates
  "non-release."
- **The wire-protocol version is independent.** Wire protocol lives
  in the bridge repo and is at v1.2; firmware versions are unrelated
  numbers. Don't conflate.

### When to bump

Loose semver, scoped to firmware behavior end users will notice:

- **Major (vX.0.0)** — a behavior change a user has to know about.
  E.g., the meaning of a state visual changes; a board preset's
  GPIOs change in a way that breaks existing wiring; the wire
  contract this firmware speaks bumps to a new major. Rare.
- **Minor (vX.Y.0)** — a new state in the lexicon, a new dispatch
  type, a new board preset, a new visual feature (like the ack
  button or task counting). Additive and backward-compatible.
- **Patch (vX.Y.Z)** — bug fixes, panel-quirk compensations, doc
  edits, dependency bumps that don't change behavior.

### Pre-1.0

While we're below v1.0.0, the bar is looser — minor bumps are fine
for behavior changes, patch for everything else. Promotion to v1.0.0
should signal "this is the firmware contract end users can rely on
not to change underneath them." We're not there yet.

## Cutting a release

1. Confirm the latest smoke build was green.
2. Confirm the latest IDF canary is either green or not running
   (canary skips when our pin matches latest).
3. **Bump `version.txt`** to the version you're about to release
   (e.g., `v0.2.0`), commit on `main`. The CI release workflow
   verifies that the pushed tag matches `version.txt` and fails
   loudly if they drift.
4. On a real ESP32 board, flash from the just-bumped `main` and
   walk through each per-board page's "verify it works" recipe.
   Confirm the boot log's `firmware:` line shows the right base
   version (with a `+sha6` suffix on local builds).
5. Update CHANGELOG (when one exists — currently relying on
   GitHub's auto-generated release notes).
6. `git tag vX.Y.Z && git push --tags`. CI does the rest. The
   resulting binary will boot logging `firmware: vX.Y.Z` (no SHA
   suffix, since CI sets `CC_RELEASE_BUILD=1`).
7. Bump `version.txt` to the next planned version (e.g., `v0.3.0`)
   and commit on `main` so subsequent dev builds carry the
   forward-looking version + SHA suffix.

## When a Monday is loud

If multiple signals fire at once (Dependabot PRs, smoke red, canary
red), drain in this order:

1. **Smoke red first.** Something concrete is broken right now.
2. **Dependabot PRs second.** They might fix the smoke red, or they
   might be the cause of it; review the diff carefully.
3. **Canary red last.** It's a heads-up about a future bump, not an
   immediate issue.
