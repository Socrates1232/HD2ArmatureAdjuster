[CmdletBinding()]
param(
    [string]$GameRoot = 'F:\Steam\steamapps\common\Helldivers 2',
    [string]$AddonPath,
    [string]$IbProfilePath,
    [ValidateRange(10, 900)]
    [int]$ObserveSeconds = 90,
    [ValidateRange(10, 300)]
    [int]$StartupTimeoutSeconds = 120,
    [switch]$AutomateInput,
    [switch]$EditTest,
    [switch]$EnableInGameCapture,
    [switch]$SteamCapture,
    [switch]$WindowCapture,
    [ValidateRange(0, 120)]
    [int]$InputDelaySeconds = 10,
    [switch]$ShutdownAfterTest,
    [switch]$RecoverOnly,
    [switch]$PreflightOnly,
    [switch]$NoDeploy,
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (!$IbProfilePath) { $IbProfilePath = Join-Path $projectRoot 'profiles\b01_slot7_ab.json' }
$gameBin = Join-Path $GameRoot 'bin'
$gameExe = Join-Path $gameBin 'helldivers2.exe'
$reshadeDll = Join-Path $gameBin 'dxgi.dll'
$reshadeLog = Join-Path $gameBin 'ReShade.log'
$installedAddon = Join-Path $gameBin 'HD2PaletteProbe.addon64'
$runtimeDirectory = Join-Path $env:LOCALAPPDATA 'HD2ArmatureAdjuster'
$telemetryPath = Join-Path $runtimeDirectory 'telemetry.json'
$automationPath = Join-Path $runtimeDirectory 'automation.request'
$captureNames = @(
    'capture-load.bmp',
    'capture-start.bmp',
    'capture-ready.bmp',
    'capture-walk.bmp',
    'capture-stretch.bmp',
    'capture-edit-before.bmp',
    'capture-edit-active.bmp',
    'capture-edit-restored.bmp'
)
$resultRoot = Join-Path $projectRoot 'test-results'
$sessionName = Get-Date -Format 'yyyyMMdd-HHmmss'
$resultDir = Join-Path $resultRoot $sessionName

function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { return 0 }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { return 0 }
        return $reader.ReadUInt16()
    }
    finally {
        $stream.Dispose()
    }
}

function Find-AddonBuild {
    $found = Get-ChildItem -Path (Join-Path $projectRoot 'build*') -Directory -ErrorAction SilentlyContinue |
        Get-ChildItem -Filter 'HD2PaletteProbe.addon64' -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw 'No built HD2PaletteProbe.addon64 was found. Build the Release target first or pass -AddonPath.'
}

function Get-ProfileTripletState([object]$Profile, [string]$DataRoot) {
    if ($Profile.a.file -ne $Profile.b.file) { throw 'The A/B profile uses different patch filenames.' }
    $mainPath = Join-Path $DataRoot $Profile.a.file
    $paths = [ordered]@{
        main = $mainPath
        gpu = "$mainPath.gpu_resources"
        stream = "$mainPath.stream"
    }
    $actual = [ordered]@{}
    foreach ($kind in $paths.Keys) {
        $path = $paths[$kind]
        if (!(Test-Path -LiteralPath $path)) { throw "Profile input is missing: $path" }
        $file = Get-Item -LiteralPath $path
        $actual[$kind] = [ordered]@{
            path = $path
            bytes = $file.Length
            sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $matches = {
        param($variant)
        foreach ($kind in $paths.Keys) {
            if ([int64]$actual[$kind].bytes -ne [int64]$variant.triplet.$kind.bytes -or
                $actual[$kind].sha256 -ne [string]$variant.triplet.$kind.sha256) { return $false }
        }
        return $true
    }
    $state = if (& $matches $Profile.a) { 'A' } elseif (& $matches $Profile.b) { 'B' } else { 'neither' }
    return [pscustomobject]@{ state = $state; files = $actual }
}

function Get-TelemetryValue([object]$Sample, [string]$Name, $Default) {
    if ($Sample.PSObject.Properties[$Name]) { return $Sample.$Name }
    return $Default
}

function Find-Steam {
    $candidate = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $GameRoot))
    $candidate = Join-Path $candidate 'steam.exe'
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    $registryPath = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamExe
    if ($registryPath -and (Test-Path -LiteralPath $registryPath)) { return $registryPath }
    throw 'steam.exe was not found. Start the game yourself and rerun with -NoLaunch.'
}

function Read-Telemetry {
    if (!(Test-Path -LiteralPath $telemetryPath)) { return $null }
    try {
        return Get-Content -LiteralPath $telemetryPath -Raw | ConvertFrom-Json
    }
    catch {
        return $null
    }
}

function Get-GameProcess {
    $live = @()
    foreach ($process in @(Get-Process -Name helldivers2 -ErrorAction SilentlyContinue)) {
        try {
            $process.Refresh()
            if (!$process.HasExited) { $live += $process }
        }
        catch {}
    }
    return $live | Sort-Object StartTime -Descending | Select-Object -First 1
}

function Get-ZombieGameProcesses {
    $zombies = @()
    foreach ($process in @(Get-Process -Name helldivers2 -ErrorAction SilentlyContinue)) {
        try {
            $process.Refresh()
            if ($process.HasExited) { $zombies += $process }
        }
        catch {}
    }
    return $zombies
}

function Test-AddonUnlocked {
    if (!(Test-Path -LiteralPath $installedAddon)) { return $true }
    try {
        $stream = [System.IO.File]::Open($installedAddon, 'Open', 'ReadWrite', 'None')
        $stream.Dispose()
        return $true
    }
    catch {
        return $false
    }
}

function Recover-ZombieGameProcesses {
    $zombies = @(Get-ZombieGameProcesses)
    foreach ($zombie in $zombies) {
        Write-Host "Recovering half-terminated helldivers2.exe PID $($zombie.Id)..."
        Stop-Process -Id $zombie.Id -Force -ErrorAction SilentlyContinue
    }
    if ($zombies.Count) { Start-Sleep -Seconds 2 }

    $remaining = @(Get-ZombieGameProcesses)
    if ($remaining.Count) {
        $ids = ($remaining.Id -join ', ')
        throw "Half-terminated helldivers2.exe PID(s) $ids remain after exact-PID cleanup. Windows still owns the process state; restart Windows before another deploy or launch."
    }
}

function Confirm-GameProcess($Expected, [string]$Stage) {
    $current = Get-GameProcess
    if (!$current) { throw "helldivers2.exe stopped before $Stage." }
    if ($current.Id -ne $Expected.Id -or $current.StartTime -ne $Expected.StartTime) {
        throw "helldivers2.exe changed before $Stage; refusing to use stale process state."
    }
    return $current
}

function Save-GameWindowCapture([int]$ProcessId, [int64]$WindowHandle, [string]$Path) {
    $captureTarget = Get-Process -Id $ProcessId -ErrorAction Stop
    $captureTarget.Refresh()
    $handle = [IntPtr]::new($WindowHandle)
    if ($handle -eq [IntPtr]::Zero) { $handle = $captureTarget.MainWindowHandle }
    if ($handle -eq [IntPtr]::Zero) {
        $handle = [WindowCaptureNative]::FindLargestWindow([uint32]$ProcessId)
    }
    if ($handle -eq [IntPtr]::Zero) { throw 'No visible game window was found for the exact PID.' }
    $rect = [WindowCaptureNative+RECT]::new()
    if (![WindowCaptureNative]::GetClientRect($handle, [ref]$rect)) { throw 'GetClientRect failed.' }
    $origin = [WindowCaptureNative+POINT]::new()
    if (![WindowCaptureNative]::ClientToScreen($handle, [ref]$origin)) { throw 'ClientToScreen failed.' }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) { throw "Invalid game client size $width x $height." }

    $full = [System.Drawing.Bitmap]::new($width, $height)
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($full)
        try {
            $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $full.Size)
        }
        finally { $graphics.Dispose() }
        $outputWidth = [Math]::Min(480, $width)
        $outputHeight = [Math]::Max(1, [int]($height * $outputWidth / $width))
        $small = [System.Drawing.Bitmap]::new($full, $outputWidth, $outputHeight)
        try { $small.Save($Path, [System.Drawing.Imaging.ImageFormat]::Jpeg) }
        finally { $small.Dispose() }
    }
    finally { $full.Dispose() }
}

if (!(Test-Path -LiteralPath $gameExe)) { throw "Game executable not found: $gameExe" }
if (!(Test-Path -LiteralPath $reshadeDll)) { throw "ReShade dxgi.dll not found: $reshadeDll" }
Recover-ZombieGameProcesses
if ($RecoverOnly) {
    $live = Get-GameProcess
    if ($live) { throw "helldivers2.exe PID $($live.Id) is still running; recovery-only mode will not terminate a live game." }
    if (!(Test-AddonUnlocked)) { throw "No live game remains, but $installedAddon is still locked. Restart Windows to release it." }
    Write-Host 'Recovery passed: no live or half-terminated game process remains, and the add-on DLL is unlocked.'
    exit 0
}
$IbProfilePath = (Resolve-Path -LiteralPath $IbProfilePath).Path
$ibProfile = Get-Content -LiteralPath $IbProfilePath -Raw | ConvertFrom-Json
if ([int]$ibProfile.schema -ne 1) { throw "Unsupported inverse-bind profile schema: $($ibProfile.schema)" }
$installedProfile = Get-ProfileTripletState -Profile $ibProfile -DataRoot (Join-Path $GameRoot 'data')
if ($installedProfile.state -eq 'neither') {
    throw "The installed B-01 patch is neither exact profile A nor B. Refusing an ambiguous run. See $IbProfilePath"
}
if (!$AddonPath) { $AddonPath = Find-AddonBuild }
$AddonPath = (Resolve-Path -LiteralPath $AddonPath).Path

$reshadeVersion = (Get-Item -LiteralPath $reshadeDll).VersionInfo.ProductVersion
if ((Get-PeMachine $AddonPath) -ne 0x8664) { throw 'The add-on is not an x64 PE binary.' }
if ($AutomateInput -and $ObserveSeconds -lt $InputDelaySeconds + 8) {
    throw '-ObserveSeconds must allow at least eight seconds after -InputDelaySeconds.'
}
if ($EditTest) { throw '-EditTest is disabled in the read-only converted_ib_scan stage.' }
if ($EnableInGameCapture -or $WindowCapture) {
    throw 'Only -SteamCapture is allowed in this stage; the other capture paths are excluded from the crash-sensitive test.'
}
if (@($EnableInGameCapture, $SteamCapture, $WindowCapture).Where({ $_ }).Count -gt 1) {
    throw 'Choose only one screenshot method.'
}
if ($WindowCapture) {
    Add-Type -AssemblyName System.Drawing
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WindowCaptureNative {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr state);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr state);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, ref RECT rect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    public static IntPtr FindLargestWindow(uint wantedProcessId) {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows((window, state) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            RECT rect = new RECT();
            if (processId == wantedProcessId && IsWindowVisible(window) && GetClientRect(window, ref rect)) {
                long area = (long)(rect.Right - rect.Left) * (rect.Bottom - rect.Top);
                if (area > bestArea) { best = window; bestArea = area; }
            }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
'@
    [WindowCaptureNative]::SetProcessDpiAwarenessContext([IntPtr]::new(-4)) | Out-Null
}
$steamScreenshotDirectory = $null
$steamScreenshotsBefore = @{}
if ($SteamCapture) {
    $steamRoot = Split-Path -Parent (Find-Steam)
    $userdata = Join-Path $steamRoot 'userdata'
    $screenshotDirectories = @(foreach ($account in @(Get-ChildItem -LiteralPath $userdata -Directory)) {
        $path = Join-Path $account.FullName '760\remote\553850\screenshots'
        if (Test-Path -LiteralPath $path) { $path }
    })
    if ($screenshotDirectories.Count -ne 1) {
        throw "Expected one Helldivers 2 Steam screenshot directory, found $($screenshotDirectories.Count)."
    }
    $steamScreenshotDirectory = $screenshotDirectories[0]
    foreach ($file in @(Get-ChildItem -LiteralPath $steamScreenshotDirectory -File)) {
        $steamScreenshotsBefore[$file.Name] = $true
    }
}
$addonHash = (Get-FileHash -LiteralPath $AddonPath -Algorithm SHA256).Hash
$running = Get-GameProcess
if (!$running -and !(Test-AddonUnlocked)) {
    throw "No live helldivers2.exe process was found, but the installed add-on is still locked: $installedAddon"
}

if (!$NoDeploy) {
    $installedHash = if (Test-Path -LiteralPath $installedAddon) {
        (Get-FileHash -LiteralPath $installedAddon -Algorithm SHA256).Hash
    }
    if ($installedHash -ne $addonHash) {
        if ($running) { throw 'The game is running and the installed add-on differs. Close the game and rerun.' }
        if (!(Test-AddonUnlocked)) { throw "The installed add-on is still locked: $installedAddon" }
        Copy-Item -LiteralPath $AddonPath -Destination $installedAddon -Force
    }
}

if ($PreflightOnly) {
    Write-Host "Preflight passed: ReShade $reshadeVersion, x64 add-on, profile $($installedProfile.state), SHA256 $addonHash"
    exit 0
}

$previousSession = if ($running) { $null } else { (Read-Telemetry).session_id }
$logOffset = if (Test-Path -LiteralPath $reshadeLog) { (Get-Item -LiteralPath $reshadeLog).Length } else { 0 }
$launchedByRunner = $false

if (!$running) {
    if ($NoLaunch) { throw 'Helldivers 2 is not running and -NoLaunch was specified.' }
    $running = Get-GameProcess
}

if (!$running) {
    $steam = Find-Steam
    Write-Host "Launching Helldivers 2 through Steam..."
    Start-Process -FilePath $steam -ArgumentList '-applaunch', '553850'
    $launchedByRunner = $true
    $startupDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Seconds 1
        $running = Get-GameProcess
    } while (!$running -and [DateTime]::UtcNow -lt $startupDeadline)
    if (!$running) { throw "helldivers2.exe did not start within $StartupTimeoutSeconds seconds." }
}
else {
    Write-Host "Attaching to running process $($running.Id)."
}

if ($AutomateInput) {
    $running = Confirm-GameProcess $running 'the input request'
    New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
    Remove-Item -LiteralPath $automationPath -Force -ErrorAction SilentlyContinue
    foreach ($name in $captureNames) {
        Remove-Item -LiteralPath (Join-Path $runtimeDirectory $name) -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $runtimeDirectory -Filter 'capture-edit-c*.bmp' -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
    $skipIntro = if ($launchedByRunner) { 1 } else { 0 }
    $editRequested = if ($EditTest) { 1 } else { 0 }
    $captureRequested = if ($EnableInGameCapture) { 1 } else { 0 }
    $steamCaptureRequested = if ($SteamCapture) { 1 } else { 0 }
    "$(1000 * $InputDelaySeconds) $skipIntro $editRequested $captureRequested $steamCaptureRequested" | Set-Content -LiteralPath $automationPath -Encoding ascii
    Write-Host "Requested input and edit test $([bool]$EditTest) (skip intro: $([bool]$skipIntro), ReShade capture: $([bool]$captureRequested), Steam capture: $([bool]$steamCaptureRequested))."
}

Write-Host "Observing process $($running.Id) for $ObserveSeconds seconds. Move and turn the character now."
$latestTelemetry = $null
$strongestTelemetry = $null
$maxSelectedChanges = -1
$maxCandidates = 0
$maxMovingCandidates = 0
$maxRingTargets = 0
$maxMovingRingTargets = 0
$maxConvertedIbHits = 0
$maxConvertedIbAHits = 0
$maxConvertedIbBHits = 0
$maxConvertedPartialCandidates = 0
$maxConvertedBestPartialEntries = 0
$experimentMode = $null
$lastReportedFrame = -1
$lastReportedAutomation = -1
$lastReportAt = [DateTime]::MinValue
$inputStarted = $false
$inputCompleted = $false
$inputError = $null
$automationStage = 0
$introAttempts = 0
$editCompleted = $false
$editTargetHadMotion = $false
$editWriteSuccesses = 0
$editImmediateReadbacks = 0
$editPresentReadbacks = 0
$editRefillsObserved = 0
$editStaleSkips = 0
$editTargetsSelected = 0
$editTargetsWritten = 0
$editTargetsRestored = 0
$editRestoreSucceeded = $false
$editOverwriteObserved = $false
$editError = 0
$editRound = 0
$editTotalRounds = 0
$completedAt = $null
$lastProgressAt = [DateTime]::UtcNow
$lastProgressFrame = -1
$activeStageSeenAt = $null
$captureFiles = @()
$windowCaptured = @{}
New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
$deadline = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
while ([DateTime]::UtcNow -lt $deadline) {
    $currentProcess = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
    if (!$currentProcess) { break }
    $currentProcess.Refresh()
    if ($currentProcess.HasExited) { break }
    $sample = Read-Telemetry
    if ($sample -and $sample.process_id -eq $running.Id -and
        ($null -eq $previousSession -or $sample.session_id -ne $previousSession)) {
        $latestTelemetry = $sample
        if ([int64]$sample.frame -gt $lastProgressFrame) {
            $lastProgressFrame = [int64]$sample.frame
            $lastProgressAt = [DateTime]::UtcNow
        }
        $maxCandidates = [Math]::Max($maxCandidates, [int]$sample.candidates)
        $maxMovingCandidates = [Math]::Max($maxMovingCandidates, [int]$sample.moving_candidates)
        $maxRingTargets = [Math]::Max($maxRingTargets, [int]$sample.ring_targets)
        $maxMovingRingTargets = [Math]::Max($maxMovingRingTargets, [int]$sample.moving_ring_targets)
        $experimentMode = [string](Get-TelemetryValue $sample 'experiment_mode' '')
        $maxConvertedIbHits = [Math]::Max($maxConvertedIbHits, [int](Get-TelemetryValue $sample 'converted_ib_hits' 0))
        $maxConvertedIbAHits = [Math]::Max($maxConvertedIbAHits, [int](Get-TelemetryValue $sample 'converted_ib_a_hits' 0))
        $maxConvertedIbBHits = [Math]::Max($maxConvertedIbBHits, [int](Get-TelemetryValue $sample 'converted_ib_b_hits' 0))
        $maxConvertedPartialCandidates = [Math]::Max($maxConvertedPartialCandidates, [int64](Get-TelemetryValue $sample 'converted_ib_partial_candidates' 0))
        $maxConvertedBestPartialEntries = [Math]::Max($maxConvertedBestPartialEntries, [int](Get-TelemetryValue $sample 'converted_ib_best_partial_entries' 0))
        $automationStage = [Math]::Max($automationStage, [int]$sample.automation_stage)
        $introAttempts = [Math]::Max($introAttempts, [int]$sample.automation_intro_attempts)
        $inputStarted = $inputStarted -or [bool]$sample.automation_input_started
        $inputCompleted = $inputCompleted -or [bool]$sample.automation_completed
        $editCompleted = $editCompleted -or [bool]$sample.edit_test_completed
        $editTargetHadMotion = $editTargetHadMotion -or [bool]$sample.edit_target_had_motion
        $editWriteSuccesses = [Math]::Max($editWriteSuccesses, [int64]$sample.edit_write_successes)
        $editImmediateReadbacks = [Math]::Max($editImmediateReadbacks, [int64]$sample.edit_immediate_readbacks)
        $editPresentReadbacks = [Math]::Max($editPresentReadbacks, [int64]$sample.edit_present_readbacks)
        $editRefillsObserved = [Math]::Max($editRefillsObserved, [int64](Get-TelemetryValue $sample 'edit_refills_observed' 0))
        $editStaleSkips = [Math]::Max($editStaleSkips, [int64]$sample.edit_stale_skips)
        $editTargetsSelected = [Math]::Max($editTargetsSelected, [int]$sample.edit_targets_selected)
        $editTargetsWritten = [Math]::Max($editTargetsWritten, [int]$sample.edit_targets_written)
        $editTargetsRestored = [Math]::Max($editTargetsRestored, [int]$sample.edit_targets_restored)
        $editRestoreSucceeded = $editRestoreSucceeded -or [bool]$sample.edit_restore_succeeded
        $editOverwriteObserved = $editOverwriteObserved -or [bool]$sample.edit_overwrite_observed
        $editError = [Math]::Max($editError, [int]$sample.edit_error)
        $editRound = [Math]::Max($editRound, [int]$sample.edit_round)
        $editTotalRounds = [Math]::Max($editTotalRounds, [int]$sample.edit_total_rounds)
        if ($WindowCapture) {
            $stage = [int]$sample.automation_stage
            if ($stage -eq 10 -and !$activeStageSeenAt) { $activeStageSeenAt = [DateTime]::UtcNow }
            $windowName = switch ($stage) {
                3 { 'capture-ready-window.jpg' }
                4 { 'capture-before-walk-window.jpg' }
                6 { 'capture-walk-window.jpg' }
                7 { 'capture-stretch-window.jpg' }
                12 { 'capture-stretch-window.jpg' }
                13 { 'capture-edit-before-window.jpg' }
                10 {
                    if (([DateTime]::UtcNow - $activeStageSeenAt).TotalSeconds -ge 3) {
                        'capture-edit-active-window.jpg'
                    }
                }
                11 { 'capture-edit-restored-window.jpg' }
            }
            if ($windowName -and !$windowCaptured.ContainsKey($windowName)) {
                try {
                    $captureProcess = Confirm-GameProcess $running "window capture $windowName"
                    Save-GameWindowCapture -ProcessId $captureProcess.Id -WindowHandle ([int64]$sample.window_handle) `
                        -Path (Join-Path $resultDir $windowName)
                    $windowCaptured[$windowName] = $true
                    $captureFiles += $windowName
                    Write-Host "Captured $windowName."
                }
                catch { Write-Warning "Window capture failed: $($_.Exception.Message)" }
            }
        }
        if ([int]$sample.automation_error -ne 0) {
            $inputError = "In-game automation error $($sample.automation_error)."
        }
        if ([int]$sample.selected_changed_slots -gt $maxSelectedChanges -or
            ([int]$sample.selected_changed_slots -eq $maxSelectedChanges -and
             (!$strongestTelemetry -or [int]$sample.candidates -gt [int]$strongestTelemetry.candidates))) {
            $maxSelectedChanges = [int]$sample.selected_changed_slots
            $strongestTelemetry = $sample
        }
        $now = [DateTime]::UtcNow
        if ([int64]$sample.frame -ne $lastReportedFrame -and
            ([int]$sample.automation_stage -ne $lastReportedAutomation -or
             ($now - $lastReportAt).TotalSeconds -ge 5)) {
            $lastReportedFrame = [int64]$sample.frame
            $lastReportedAutomation = [int]$sample.automation_stage
            $lastReportAt = $now
            Write-Host ("frame={0} mode={1} converted={2} A/B={3}/{4} partial={5} automation={6} edit={7}/{8} writes={9}" -f
                $sample.frame, $experimentMode, $maxConvertedIbHits, $maxConvertedIbAHits,
                $maxConvertedIbBHits, $maxConvertedBestPartialEntries, $sample.automation_stage,
                $sample.edit_round, $sample.edit_total_rounds, $sample.edit_write_successes)
        }
    }
    if ($latestTelemetry -and ([DateTime]::UtcNow - $lastProgressAt).TotalSeconds -ge 20) {
        Write-Warning "Telemetry stopped advancing at frame $lastProgressFrame."
        break
    }
    if ($AutomateInput -and ($inputCompleted -or $inputError)) {
        if (!$completedAt) { $completedAt = [DateTime]::UtcNow }
        elseif (([DateTime]::UtcNow - $completedAt).TotalSeconds -ge 5) { break }
    }
    Start-Sleep -Seconds 1
}

if ($AutomateInput -and !$inputStarted -and !$inputError) {
    $inputError = 'The in-game automation did not reach the walking stage.'
}
elseif ($AutomateInput -and !$inputCompleted -and !$inputError) {
    $inputError = 'The in-game automation did not complete.'
}
Remove-Item -LiteralPath $automationPath -Force -ErrorAction SilentlyContinue

$logLines = if (Test-Path -LiteralPath $reshadeLog) { @(Get-Content -LiteralPath $reshadeLog -Tail 600) } else { @() }
$interestingLog = @($logLines | Where-Object {
    $_ -match 'HD2PaletteProbe|HD2 Palette Probe|requested API version|Failed to (load|register) add-on'
})
$registerPattern = '\|\s+Registered add-on "HD2 Palette Probe"'
$unregisterPattern = '\|\s+Unregistered add-on "HD2 Palette Probe"'
$registered = [bool]($interestingLog | Where-Object { $_ -match $registerPattern })
$unregistered = [bool]($interestingLog | Where-Object { $_ -match $unregisterPattern })
$lastAddonEvent = $interestingLog | Where-Object { $_ -match $registerPattern -or $_ -match $unregisterPattern } |
    Select-Object -Last 1
$registeredAtEnd = $registered -and $lastAddonEvent -match $registerPattern
$loadError = [bool]($interestingLog | Where-Object { $_ -match 'ERROR.*(HD2PaletteProbe|requested API version|Failed to (load|register) add-on)' })
$telemetrySeen = $null -ne $latestTelemetry
$heartbeatAgeSeconds = if ($telemetrySeen) {
    [Math]::Max(0.0, ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - [double]$latestTelemetry.updated_unix_ms) / 1000.0)
} else { $null }
$heartbeatFresh = $telemetrySeen -and $heartbeatAgeSeconds -le 5.0
$currentProcess = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
if ($currentProcess) { $currentProcess.Refresh() }
$processAlive = $null -ne $currentProcess -and !$currentProcess.HasExited
$runtimeActive = $heartbeatFresh -and $processAlive -and $latestTelemetry.frame -gt 0
$motionDetected = $telemetrySeen -and ($maxMovingCandidates -gt 0 -or $maxSelectedChanges -gt 0)
$convertedScanVerified = $experimentMode -eq 'converted_ib_scan' -and $maxConvertedIbHits -gt 0
$editVerified = !$EditTest -or ($editCompleted -and $editWriteSuccesses -gt 0 -and
    $editImmediateReadbacks -gt 0 -and $editRestoreSucceeded -and $editError -eq 0)
$status = if ($loadError) { 'failed-load' } elseif (!$registered -and !$telemetrySeen) { 'inconclusive-load' } elseif (!$runtimeActive) { 'failed-runtime' } elseif ($experimentMode -ne 'converted_ib_scan') { 'failed-experiment-mode' } elseif ($AutomateInput -and ($inputError -or !$inputCompleted)) { 'failed-input' } elseif ($EditTest -and !$editVerified) { 'failed-edit' } elseif (!$convertedScanVerified) { 'failed-no-converted-match' } else { 'passed-converted-scan' }

New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
$interestingLog | Set-Content -LiteralPath (Join-Path $resultDir 'reshade-tail.log') -Encoding utf8
if ($telemetrySeen) {
    $latestTelemetry | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $resultDir 'telemetry.json') -Encoding utf8
}
if ($SteamCapture) {
    $newScreenshots = @(Get-ChildItem -LiteralPath $steamScreenshotDirectory -File |
        Where-Object { !$steamScreenshotsBefore.ContainsKey($_.Name) } |
        Sort-Object LastWriteTime)
    $steamCaptureNames = if ($EditTest) {
        @('capture-edit-before-steam.jpg', 'capture-edit-active-steam.jpg', 'capture-edit-restored-steam.jpg')
    }
    else {
        @('capture-load-steam.jpg', 'capture-ready-steam.jpg', 'capture-walk-steam.jpg', 'capture-stretch-steam.jpg')
    }
    for ($i = 0; $i -lt [Math]::Min($newScreenshots.Count, $steamCaptureNames.Count); ++$i) {
        Copy-Item -LiteralPath $newScreenshots[$i].FullName -Destination (Join-Path $resultDir $steamCaptureNames[$i])
        $captureFiles += $steamCaptureNames[$i]
    }
}
foreach ($name in $captureNames) {
    $source = Join-Path $runtimeDirectory $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $resultDir $name)
        $captureFiles += $name
    }
}
foreach ($source in @(Get-ChildItem -LiteralPath $runtimeDirectory -Filter 'capture-edit-c*.bmp' -File -ErrorAction SilentlyContinue | Sort-Object Name)) {
    Copy-Item -LiteralPath $source.FullName -Destination (Join-Path $resultDir $source.Name)
    $captureFiles += $source.Name
}

$shutdownSucceeded = $false
$shutdownError = $null
if ($ShutdownAfterTest) {
    try {
        $target = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
        if ($target) {
            $target.Refresh()
            if (!$target.HasExited) {
                if ($target.StartTime -ne $running.StartTime) { throw 'The process ID was reused; shutdown was refused.' }
                Stop-Process -Id $running.Id -Force -ErrorAction Stop
                Wait-Process -Id $running.Id -Timeout 10 -ErrorAction SilentlyContinue
            }
        }
        $remaining = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
        if ($remaining) { $remaining.Refresh() }
        if ($remaining -and $remaining.HasExited) {
            $clearDeadline = [DateTime]::UtcNow.AddSeconds(3)
            do {
                Start-Sleep -Milliseconds 250
                $remaining = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
                if ($remaining) { $remaining.Refresh() }
            } while ($remaining -and $remaining.HasExited -and [DateTime]::UtcNow -lt $clearDeadline)
        }
        $shutdownSucceeded = $null -eq $remaining
        if (!$shutdownSucceeded) {
            $shutdownError = if ($remaining.HasExited) {
                'The tested process is half-terminated and still present after shutdown.'
            }
            else {
                'The tested process did not exit within ten seconds.'
            }
        }
    }
    catch {
        $shutdownError = $_.Exception.Message
    }
}
if ($ShutdownAfterTest -and !$shutdownSucceeded -and $status -like 'passed-*') {
    $status = 'failed-shutdown'
}

$summary = [ordered]@{
    status = $status
    timestamp = (Get-Date).ToString('o')
    game_process_id = $running.Id
    game_root = $GameRoot
    ib_profile_path = $IbProfilePath
    installed_patch_state = $installedProfile.state
    installed_patch_files = $installedProfile.files
    reshade_version = $reshadeVersion
    required_reshade_api = 17
    addon_sha256 = $addonHash
    addon_registered = $registered
    addon_unregistered = $unregistered
    addon_registered_at_end = $registeredAtEnd
    addon_load_error = $loadError
    log_start_offset = $logOffset
    log_lines_examined = $logLines.Count
    telemetry_seen = $telemetrySeen
    heartbeat_fresh = $heartbeatFresh
    heartbeat_age_seconds = $heartbeatAgeSeconds
    process_alive = $processAlive
    runtime_active = $runtimeActive
    input_requested = [bool]$AutomateInput
    input_started = $inputStarted
    input_completed = $inputCompleted
    input_error = $inputError
    edit_test_requested = [bool]$EditTest
    edit_test_completed = $editCompleted
    edit_target_had_motion = $editTargetHadMotion
    edit_write_successes = $editWriteSuccesses
    edit_immediate_readbacks = $editImmediateReadbacks
    edit_present_readbacks = $editPresentReadbacks
    edit_refills_observed = $editRefillsObserved
    edit_stale_skips = $editStaleSkips
    edit_targets_selected = $editTargetsSelected
    edit_targets_written = $editTargetsWritten
    edit_targets_restored = $editTargetsRestored
    edit_restore_succeeded = $editRestoreSucceeded
    edit_overwrite_observed = $editOverwriteObserved
    edit_error = $editError
    edit_channel_verified = $editVerified
    edit_rounds_completed = $editRound
    edit_total_rounds = $editTotalRounds
    automation_stage = $automationStage
    intro_attempts = $introAttempts
    capture_files = $captureFiles
    steam_capture_requested = [bool]$SteamCapture
    window_capture_requested = [bool]$WindowCapture
    shutdown_requested = [bool]$ShutdownAfterTest
    shutdown_succeeded = $shutdownSucceeded
    shutdown_error = $shutdownError
    motion_detected = $motionDetected
    max_candidates = $maxCandidates
    max_moving_candidates = $maxMovingCandidates
    max_ring_targets = $maxRingTargets
    max_moving_ring_targets = $maxMovingRingTargets
    experiment_mode = $experimentMode
    converted_scan_verified = $convertedScanVerified
    max_converted_ib_hits = $maxConvertedIbHits
    max_converted_ib_a_hits = $maxConvertedIbAHits
    max_converted_ib_b_hits = $maxConvertedIbBHits
    max_converted_partial_candidates = $maxConvertedPartialCandidates
    max_converted_best_partial_entries = $maxConvertedBestPartialEntries
    max_selected_changed_slots = [Math]::Max(0, $maxSelectedChanges)
    telemetry = $latestTelemetry
    strongest_telemetry = $strongestTelemetry
}
$summaryPath = Join-Path $resultDir 'summary.json'
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8

Write-Host "Result: $status"
Write-Host "Report: $summaryPath"
if ($status -like 'failed-*' -or $status -like 'inconclusive-*') { exit 1 }
