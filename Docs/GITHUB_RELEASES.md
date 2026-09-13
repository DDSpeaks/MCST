# GitHub build and release automation

MCST uses `.github/workflows/release.yml` to build and test the Windows
application on GitHub's Windows runner. A user who only wants to run MCST can
download the resulting portable ZIP and does not need Visual Studio.

For the first repository upload, follow `FIRST_GITHUB_PUBLICATION.md`. The
source package includes `.gitignore` to block active INI files, credentials,
build output, runtime reports, and research captures, plus `.gitattributes` for
stable Windows project and script line endings.

## Manual test build

Open the repository's **Actions** page, choose **Build Windows release**, and
select **Run workflow**. The job validates the source, builds `Release|x64`,
runs `MCST-LogicTests.exe`, and uploads a 30-day workflow artifact. A manual
run does not create a public GitHub Release.

## Publish a release

Commit the release source and push a tag that exactly matches the version:

```powershell
git tag v1.20.1
git push origin v1.20.1
```

The tag starts the same validated build and then creates a GitHub Release with:

- `MCST-Watchdog-1.20.1-Windows-x64.zip`
- `MCST-Watchdog-1.20.1-SHA256SUMS.txt`
- the repository's `RELEASE_NOTES.md` as the Release description

`Build-PortableRelease.ps1` rejects a tag whose name does not match the source
version. `gh release create --verify-tag` also requires the tag to exist.

## Portable package contract

The user package contains only the required runtime and integration files:

```text
MCST-Watchdog-1.20.1-Windows-x64/
  MCExtras/
    MCST-Watchdog.exe
    MCST-TrackerBridge.dll
  PowerLanguage/
    MCST_Tracker_Bridge_Host.txt
    MCST_Tracker_Bridge_Stop.txt
  Examples/
    MCST-Watchdog.ini.example
    MCST-Compatibility.ini.example
  LICENSE
  INSTALL.md
  RELEASE_NOTES.md
  PACKAGE_MANIFEST.txt
```

It intentionally excludes active INI settings, credentials, PDB/LIB/OBJ files,
source code, and `MCST-LogicTests.exe`. The two `.ini.example` files are inert
documentation templates and are never active under those names. The test
executable is run in CI but is not a runtime component.

The packaged `LICENSE` is the same MIT License committed at the repository
root. It must retain `Copyright (c) 2026 Mika Tättäläinen`.

## Required repository setting

GitHub Actions must be enabled. The workflow declares `contents: write` so the
repository's automatically created `GITHUB_TOKEN` can add Release assets. If an
organization restricts workflow token permissions, allow this workflow to
write repository contents or the build will succeed but the Release step will
be denied.

Do not store SMTP passwords, account credentials, signing keys, or populated
INI files in the repository or portable package. Code signing, if introduced
later, should use protected GitHub Actions secrets and must not print secret
values to the build log.
