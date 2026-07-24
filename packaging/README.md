# ARPBOX packaging

## `ArpBox.entitlements` — Hardened Runtime entitlements (docs/ARCHITECTURE.md §3.1)

ARPBOX loads third-party VST3 / AudioUnit binaries into its own process. Under
the macOS Hardened Runtime these entitlements are what make that possible;
without them Gatekeeper / library validation refuse to map the plugin code.

Applied at sign time via `codesign --options runtime --entitlements`
(see `app/CMakeLists.txt`). Keep the list MINIMAL — every relaxation is attack
surface and every one is scrutinised at notarization.

> NOTE: the entitlements plist contains **no XML comments**. Apple's AMFI
> entitlement parser (`AMFIUnserializeXML`) rejects `<!-- ... -->`, failing the
> sign with `syntax error`. Documentation lives here instead.

| Entitlement | Why |
|---|---|
| `com.apple.security.cs.disable-library-validation` | **Mandatory.** Lets the process load libraries (plugins) not signed by ARPBOX's own Team ID. Third-party plugins are vendor- or ad-hoc-signed, so without this every non-first-party VST3/AU fails to load. This is the entitlement Phase 1 exists to wire in. |
| `com.apple.security.cs.allow-unsigned-executable-memory` | Many instrument/effect plugins JIT-compile DSP or script engines and map writable+executable memory. |
| `com.apple.security.cs.allow-jit` | Explicit `MAP_JIT` permission for plugins using the toggle-able W^X JIT path. |

`com.apple.security.cs.disable-executable-page-protection` intentionally omitted; add only if a specific hosted plugin demonstrably fails without it (revisit in the Phase 22 compatibility lab).

## Signing knobs (CMake cache variables)

| Variable | Default | Meaning |
|---|---|---|
| `ARPBOX_CODESIGN_IDENTITY` | `-` (ad-hoc) | `codesign --sign` identity. Ad-hoc is correct for local dev and CI without secrets. Release: `-DARPBOX_CODESIGN_IDENTITY="Developer ID Application: <Name> (<TEAMID>)"`. |
| `ARPBOX_ENTITLEMENTS` | `packaging/ArpBox.entitlements` | Entitlements plist applied under `--options runtime`. |

The app is re-signed as a `POST_BUILD` step because JUCE emits it ad-hoc /
linker-signed with no Hardened Runtime and no entitlements.

## Verify locally

```sh
APP=build/app/ARPBOX_artefacts/RelWithDebInfo/ARPBOX.app
codesign -dv --verbose=4 "$APP"          # expect flags=...(runtime)
codesign -d --entitlements - "$APP"      # expect disable-library-validation
```

## Notarization (release only)

Notarization is stubbed in `.github/workflows/ci.yml` (`notarize` job, gated on
tags / `release/*`). It requires signing secrets not yet configured; the job is
a documented placeholder until a Developer ID identity + notary credentials are
added to the repo secrets.
