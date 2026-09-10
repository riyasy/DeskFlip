# How to make a GitHub release

Written after cutting v1.0.1. Follow it top to bottom and no decisions are left over.

The house style is DeskTick's releases — same headings, same badges, same asset shape. Read the
most recent one before writing notes:

```powershell
gh release view v1.0.1 --repo riyasy/DeskTick --json body --jq .body
```

## 0. The version lives in one file

`src\DeskFlip\resource.h` — `VER_DISPLAY` (`v1.0.1`) is what the tag, the asset names and the
notes all use. Bump it there, plus the two copies XML cannot include (`Package.appxmanifest`'s
`Identity Version` and `DeskFlip.manifest`'s `assemblyIdentity version`), and commit before
building. The workflow greps `VER_DISPLAY` out of the header, so the artifact names follow on
their own.

## 1. Build all three arches in CI

```powershell
gh workflow run build.yml --ref main
gh run list --limit 1                      # wait for "completed success", ~2 min
```

The matrix is x64 and x86 on `windows-latest`, ARM64 on `windows-11-arm`. Artifacts come out named
`DeskFlip_v<version>_<arch>` and already contain everything: `DeskFlip.exe`, `LICENSE`, `lang\` and
`assets\` (font plus its OFL). Nothing is added by hand any more.

Do not build locally for a release. The point of CI is that ARM64 is built on ARM64.

## 2. Download and zip

```powershell
$s = "<scratchpad>"                        # anywhere temporary
gh run download <run-id> -D "$s\rel"

foreach ($a in 'x64','ARM64','x86') {
  $d = "$s\rel\DeskFlip_v1.0.1_$a"
  Compress-Archive -Path "$d\*" -DestinationPath "$s\rel\DeskFlip_v1.0.1_$a.zip" -Force
}
```

Zips, not bare exes: DeskFlip reads `lang\*.ini` and the `.ttf` from disk at runtime, so a lone exe
would ship English-only in Cascadia Mono. (Let It Rain attaches bare exes; do not copy that here.)

Sanity check before publishing — the arch you cannot run is the one worth checking:

```powershell
(Get-Item "$s\rel\DeskFlip_v1.0.1_x64\DeskFlip.exe").VersionInfo.FileVersion   # 1.0.1.0
```

## 3. Write the notes

Sections, in this order, matching DeskTick:

1. `# DeskFlip <version> Release Notes`
2. `## ❤️ You can support **DeskFlip** by sponsoring or by downloading from Microsoft Store`,
   followed by the two badges below
3. `## 🚀 What's New` (`## 🚀 First Release` the first time) — a one-line description, then bold-lead bullets
4. `## 📦 Installing` — it is a zip, there is no installer, keep `lang\` and `assets\` beside the exe,
   settings live in `%LOCALAPPDATA%\DeskFlip.ini`
5. `## 🪟 First Launch (Unsigned App)` — SmartScreen, More info, Run anyway; the Store build is signed
6. `## 📥 Downloads` — the three-row table, then the x64/ARM64/x86 hint line
7. `## 📌 Requirements` — Windows 10 1809 or later, or Windows 11. Nothing else to install.

`---` between sections.

The badges, verbatim (the Store id `9MSXBKV3295F` is DeskFlip's; `cid=from_github` is the Partner
Center campaign id):

```markdown
[![Microsoft Store](https://get.microsoft.com/images/en-us%20light.svg)](https://apps.microsoft.com/detail/9MSXBKV3295F?referrer=appbadge&mode=full&cid=from_github)
[![Sponsor](https://img.shields.io/badge/Sponsor-%E2%9D%A4-fe8e86?logo=github)](https://github.com/sponsors/riyasy)
```

**No em dashes.** Hyphens only, everywhere in the notes. Check before publishing:

```powershell
if ((Get-Content "$s\notes.md" -Raw) -match '[–—]') { "FOUND" } else { "clean" }
```

Show the notes to the user and get a yes before creating the release.

## 4. Publish

```powershell
gh release create v1.0.1 --title "DeskFlip v1.0.1" --notes-file "$s\notes.md" `
   --target <commit-sha> `
   "$s\rel\DeskFlip_v1.0.1_x64.zip" "$s\rel\DeskFlip_v1.0.1_ARM64.zip" "$s\rel\DeskFlip_v1.0.1_x86.zip"
gh release view v1.0.1
```

`--target` pins the tag to the commit CI actually built; without it the tag lands on whatever `main`
points at by then. Notes can be revised afterwards with `gh release edit v1.0.1 --notes-file ...`.

## Gotchas

- **The tool may refuse to publish.** `gh release create` is outward-facing and the permission
  classifier can block it. Everything else is safe to do unattended; if the create is denied, hand
  the user the exact command rather than working around it.
- **`gh ... --jq '<expr with spaces>'` fails under Windows PowerShell** with "accepts 1 arg(s),
  received 2". Use `gh release view <tag>` plainly, or `--json x --jq .x` with no spaces.
- **The Store build is a separate artifact.** A GitHub release does not touch it; the `.msixupload`
  goes to Partner Center by hand (see CLAUDE.md, "The MSIX package").
- **The first release had no prior tag.** `gh release list` empty is normal for a first cut, not a
  sign that something failed.
