[CmdletBinding()]
param(
    [string]$BinaryRoot = "",
    [int]$ReadyTimeoutSeconds = 8,
    [switch]$IncludeGlobal
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($BinaryRoot)) {
    $BinaryRoot = Join-Path $repoRoot "x64\Release"
}
$BinaryRoot = (Resolve-Path -LiteralPath $BinaryRoot).Path
$targetBinary = Join-Path $BinaryRoot "bb_ok_gui_hook_target.exe"
$windowsHookBinary = Join-Path $BinaryRoot "bb_det_setwindows_hookex.exe"
$winEventBinary = Join-Path $BinaryRoot "bb_det_setwinevent_hook.exe"
foreach ($path in @($targetBinary, $windowsHookBinary, $winEventBinary, (Join-Path $BinaryRoot "bb_unsigned_plugin.dll"))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required AES artifact is missing: $path"
    }
}

$artifactRoot = Join-Path $repoRoot "artifacts\gui-process-scope-pivots"
New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

function Start-GuiTarget {
    param([string]$Label)

    $nonce = [Guid]::NewGuid().ToString("N")
    $infoPath = Join-Path $artifactRoot "$Label-$nonce.txt"
    $eventName = "Local\BkaesGuiScope-$nonce"
    $process = Start-Process -FilePath $targetBinary -ArgumentList @(
        "--info-file", $infoPath,
        "--trigger-event", $eventName
    ) -PassThru -WindowStyle Hidden

    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSeconds)
    while (-not (Test-Path -LiteralPath $infoPath -PathType Leaf) -and (Get-Date) -lt $deadline) {
        if ($process.HasExited) {
            throw "GUI target exited before publishing its identity (exit=$($process.ExitCode))."
        }
        Start-Sleep -Milliseconds 50
    }
    if (-not (Test-Path -LiteralPath $infoPath -PathType Leaf)) {
        throw "Timed out waiting for GUI target identity: $infoPath"
    }

    $values = @{}
    Get-Content -LiteralPath $infoPath | ForEach-Object {
        $pair = $_ -split "=", 2
        if ($pair.Count -eq 2) {
            $values[$pair[0]] = $pair[1]
        }
    }
    if ([uint32]$values["pid"] -ne [uint32]$process.Id -or [uint32]$values["tid"] -eq 0) {
        throw "GUI target identity mismatch."
    }

    return [pscustomobject]@{
        Process = $process
        Pid = [uint32]$values["pid"]
        Tid = [uint32]$values["tid"]
        EventName = $eventName
        InfoPath = $infoPath
    }
}

function Stop-GuiTarget {
    param($Target)

    if ($null -ne $Target -and $null -ne $Target.Process -and -not $Target.Process.HasExited) {
        Stop-Process -Id $Target.Process.Id -Force
        $Target.Process.WaitForExit()
    }
}

$windowsTarget = $null
$windowsAnsiTarget = $null
$winEventTarget = $null
$globalWinEventTarget = $null
try {
    $windowsTarget = Start-GuiTarget -Label "setwindowshookex"
    & $windowsHookBinary --target-tid $windowsTarget.Tid
    if ($LASTEXITCODE -ne 0) {
        throw "SetWindowsHookEx sample failed with exit code $LASTEXITCODE."
    }
    Stop-GuiTarget $windowsTarget

    $windowsAnsiTarget = Start-GuiTarget -Label "setwindowshookex-ansi"
    & $windowsHookBinary --ansi --target-tid $windowsAnsiTarget.Tid
    if ($LASTEXITCODE -ne 0) {
        throw "SetWindowsHookExA sample failed with exit code $LASTEXITCODE."
    }
    Stop-GuiTarget $windowsAnsiTarget

    $winEventTarget = Start-GuiTarget -Label "setwineventhook"
    & $winEventBinary --target-pid $winEventTarget.Pid --target-tid $winEventTarget.Tid `
        --trigger-event $winEventTarget.EventName
    if ($LASTEXITCODE -ne 0) {
        throw "SetWinEventHook sample failed with exit code $LASTEXITCODE."
    }

    if ($IncludeGlobal) {
        & $windowsHookBinary --global
        if ($LASTEXITCODE -ne 0) {
            throw "Global SetWindowsHookEx sample failed with exit code $LASTEXITCODE."
        }

        $globalWinEventTarget = Start-GuiTarget -Label "setwineventhook-global"
        & $winEventBinary --global --trigger-event $globalWinEventTarget.EventName
        if ($LASTEXITCODE -ne 0) {
            throw "Global SetWinEventHook sample failed with exit code $LASTEXITCODE."
        }
    }

    Write-Host "[PASS] GUI process-scope pivot samples completed."
}
finally {
    Stop-GuiTarget $windowsTarget
    Stop-GuiTarget $windowsAnsiTarget
    Stop-GuiTarget $winEventTarget
    Stop-GuiTarget $globalWinEventTarget
}
