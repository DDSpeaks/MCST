# First GitHub publication checklist

This source package is the repository input. It intentionally contains no
prebuilt EXE or DLL: the GitHub Windows workflow builds those binaries from the
committed source, runs LogicTests, and then creates the portable user ZIP.

## 1. Create the repository

1. Extract the source ZIP.
2. Use the extracted `MCST-src` directory as the repository root. Confirm that
   `.github/workflows/release.yml`, `.gitignore`, `MCST.sln`, and `README.md`
   are directly beneath that root.
3. Add the directory in GitHub Desktop. If it is not yet a Git repository, use
   GitHub Desktop's offered **Create a repository** action.
4. Commit all source-package files and publish the repository.
5. Choose a private repository until you are ready for public distribution.

## 2. Configure Actions

In the GitHub repository, open **Settings → Actions → General**:

- enable Actions;
- allow the actions used by the workflow;
- set Workflow permissions to **Read and write permissions**.

The write permission allows the tag-triggered workflow to create a Release and
attach its assets. Do not add populated INI files, SMTP passwords, account data,
private keys, or certificates to the repository. `.gitignore` blocks the normal
names, but the committer must still inspect every staged file.

## 3. Run the non-publishing build first

Open **Actions → Build Windows release → Run workflow**. A successful manual
run must:

1. validate the source;
2. build `Release|x64`;
3. run `MCST-LogicTests.exe` successfully;
4. create a workflow artifact containing the portable ZIP and SHA-256 file;
5. create no GitHub Release.

Download and inspect that artifact before tagging the release.

## 4. Publish version 1.20.14

From a terminal opened in the repository root:

```powershell
.\Publish-GitHub-Release.ps1
```

The helper runs the release validator, commits and pushes any pending release
source changes, verifies that `v1.20.14` does not already exist, and then
creates and pushes the annotated tag. It stops without overwriting an existing
local or remote tag.

Do not create the Release manually in the GitHub web interface. The tag starts
the validated workflow, which creates the Release and attaches:

```text
MCST-Watchdog-1.20.14-Windows-x64.zip
MCST-Watchdog-1.20.14-SHA256SUMS.txt
```

## 5. Confirm the MIT License

The repository root contains `LICENSE` with:

```text
MIT License
Copyright (c) 2026 Mika Tättäläinen
```

Keep this file in the first commit and in later releases. The GitHub workflow
also includes it in the portable user ZIP. The license covers MCST, not
MultiCharts, Saxo, Windows, or any other third-party product or service.

## 6. Verify the published user package

The portable ZIP must contain:

```text
MCExtras/MCST-Watchdog.exe
MCExtras/MCST-TrackerBridge.dll
PowerLanguage/MCST_Tracker_Bridge_Host.txt
PowerLanguage/MCST_Tracker_Bridge_Stop.txt
Examples/MCST-Watchdog.ini.example
Examples/MCST-Compatibility.ini.example
LICENSE
INSTALL.md
RELEASE_NOTES.md
PACKAGE_MANIFEST.txt
```

It must not contain an active `.ini`, credentials, `MCST-LogicTests.exe`, PDB,
LIB, OBJ, source file, or private key.
