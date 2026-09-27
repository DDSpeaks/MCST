[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$GitExe = 'C:\Users\Administrator\AppData\Local\GitHubDesktop\app-3.6.4\resources\app\git\mingw64\bin\git.exe'
$ReleaseTag = 'v1.20.13'
$CommitMessage = 'Release v1.20.13: MultiCharts 16 and 17 AutoTrading support'

function Assert-ExitCode {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Action,

        [int[]]$Allowed = @(0)
    )

    if ($Allowed -notcontains $LASTEXITCODE) {
        throw "$Action failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $GitExe -PathType Leaf)) {
    throw "Git executable was not found: $GitExe"
}

$StartDirectory = (Get-Location).Path
$RepoRootOutput = @(& $GitExe -C $StartDirectory rev-parse --show-toplevel 2>$null)
Assert-ExitCode -Action 'Repository detection'
$RepoRoot = ($RepoRootOutput | Select-Object -First 1).Trim()
if ([string]::IsNullOrWhiteSpace($RepoRoot)) {
    throw 'Git returned an empty repository path.'
}

Set-Location -LiteralPath $RepoRoot
try {
    Write-Host "Repository: $RepoRoot"
    Write-Host "Release tag: $ReleaseTag"

    $ValidatePath = Join-Path $RepoRoot 'Tools\Validate-Release.ps1'
    if (-not (Test-Path -LiteralPath $ValidatePath -PathType Leaf)) {
        throw "Release validator was not found: $ValidatePath"
    }

    Write-Host 'Running release validation...'
    & $ValidatePath
    if (-not $?) {
        throw 'Validate-Release.ps1 reported a failure.'
    }

    Write-Host 'Staging repository changes...'
    & $GitExe add --all
    Assert-ExitCode -Action 'git add --all'

    & $GitExe --no-pager diff --cached --check
    Assert-ExitCode -Action 'git diff --cached --check'

    & $GitExe --no-pager diff --cached --quiet
    $CachedDiffExitCode = $LASTEXITCODE
    if ($CachedDiffExitCode -eq 1) {
        Write-Host 'Creating release commit...'
        & $GitExe commit -m $CommitMessage
        Assert-ExitCode -Action 'git commit'
    }
    elseif ($CachedDiffExitCode -eq 0) {
        Write-Host 'No new source changes to commit.'
    }
    else {
        throw "git diff --cached --quiet failed with exit code $CachedDiffExitCode."
    }

    $BranchOutput = @(& $GitExe branch --show-current)
    Assert-ExitCode -Action 'Current branch detection'
    $Branch = ($BranchOutput | Select-Object -First 1).Trim()
    if ([string]::IsNullOrWhiteSpace($Branch)) {
        throw 'The repository is in detached HEAD state. Check out the release branch first.'
    }

    Write-Host "Pushing branch '$Branch' to origin..."
    & $GitExe push origin $Branch
    Assert-ExitCode -Action 'Branch push'

    Write-Host 'Refreshing remote tags...'
    & $GitExe fetch --tags origin
    Assert-ExitCode -Action 'Tag fetch'

    & $GitExe show-ref --verify --quiet "refs/tags/$ReleaseTag"
    $LocalTagExitCode = $LASTEXITCODE
    if ($LocalTagExitCode -eq 0) {
        throw "Local tag $ReleaseTag already exists. It was not changed or overwritten."
    }
    elseif ($LocalTagExitCode -ne 1) {
        throw "Local tag check failed with exit code $LocalTagExitCode."
    }

    $RemoteTagOutput = @(& $GitExe ls-remote --tags origin "refs/tags/$ReleaseTag")
    Assert-ExitCode -Action 'Remote tag check'
    if (($RemoteTagOutput -join '').Trim().Length -ne 0) {
        throw "Remote tag $ReleaseTag already exists. It was not changed or overwritten."
    }

    Write-Host "Creating annotated tag $ReleaseTag..."
    & $GitExe tag -a $ReleaseTag -m "MCST-Watchdog $ReleaseTag"
    Assert-ExitCode -Action 'Tag creation'

    Write-Host "Pushing tag $ReleaseTag to origin..."
    & $GitExe push origin $ReleaseTag
    Assert-ExitCode -Action 'Tag push'

    Write-Host ''
    Write-Host 'Publication started successfully.' -ForegroundColor Green
    Write-Host "GitHub Actions will now build and publish $ReleaseTag."
}
finally {
    Set-Location -LiteralPath $StartDirectory
}
