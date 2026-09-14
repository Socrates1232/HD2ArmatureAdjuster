[CmdletBinding()]
param(
    [string]$GameRoot = 'F:\Steam\steamapps\common\Helldivers 2',
    [string]$AddonPath,
    [string]$ProfileDirectory,
    [ValidateRange(10, 900)]
    [int]$ObserveSeconds = 90,
    [ValidateRange(10, 300)]
    [int]$StartupTimeoutSeconds = 120,
    [switch]$AutomateInput,
    [switch]$EditTest,
    [string]$EditUnit,
    [ValidateRange(-1, 4095)]
    [int]$EditSlot = -1,
    [ValidateRange(-10.0, 10.0)]
    [double]$EditX = 0.0,
    [ValidateRange(-10.0, 10.0)]
    [double]$EditY = 0.0,
    [ValidateRange(-10.0, 10.0)]
    [double]$EditZ = 0.0,
    [switch]$SteamCapture,
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
if (!$ProfileDirectory) { $ProfileDirectory = Join-Path $projectRoot 'profiles\runtime' }
$gameBin = Join-Path $GameRoot 'bin'
$gameData = Join-Path $GameRoot 'data'
$gameExe = Join-Path $gameBin 'helldivers2.exe'
$reshadeDll = Join-Path $gameBin 'dxgi.dll'
$reshadeLog = Join-Path $gameBin 'ReShade.log'
$installedAddon = Join-Path $gameBin 'HD2PaletteProbe.addon64'
$installedProfileDirectory = Join-Path $gameBin 'HD2ArmatureProfiles'
$activeProfilesPath = Join-Path $installedProfileDirectory 'active_profiles.txt'
$runtimeDirectory = Join-Path $env:LOCALAPPDATA 'HD2ArmatureAdjuster'
$telemetryPath = Join-Path $runtimeDirectory 'telemetry.json'
$automationPath = Join-Path $runtimeDirectory 'automation.request'
$resultRoot = Join-Path $projectRoot 'test-results'
$resultDir = Join-Path $resultRoot (Get-Date -Format 'yyyyMMdd-HHmmss')

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
    finally { $stream.Dispose() }
}

function Find-AddonBuild {
    $found = Get-ChildItem -Path (Join-Path $projectRoot 'build*') -Directory -ErrorAction SilentlyContinue |
        Get-ChildItem -Filter 'HD2PaletteProbe.addon64' -File -Recurse -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { return $found.FullName }
    throw 'No built HD2PaletteProbe.addon64 was found. Build Release first or pass -AddonPath.'
}

function Read-ProfileHeader([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64) { throw "Profile header is truncated: $Path" }
    $magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 7)
    if ($magic -ne 'HD2IBP1' -or $bytes[7] -ne 0) { throw "Unsupported profile magic: $Path" }
    if ([BitConverter]::ToUInt32($bytes, 8) -ne 64) { throw "Unsupported profile header size: $Path" }
    $records = [BitConverter]::ToUInt32($bytes, 12)
    $nameBytes = [BitConverter]::ToUInt32($bytes, 16)
    if (!$records -or !$nameBytes -or 64 + $nameBytes -gt $bytes.Length) {
        throw "Invalid profile counts: $Path"
    }
    $patchName = [System.Text.Encoding]::UTF8.GetString($bytes, 64, $nameBytes)
    if ([System.IO.Path]::GetFileName($patchName) -ne $patchName) {
        throw "Profile patch name is not a basename: $Path"
    }
    $patchHash = -join $bytes[24..55].ForEach({ $_.ToString('x2') })
    [pscustomobject]@{
        file = (Get-Item -LiteralPath $Path).Name
        path = $Path
        bytes = $bytes.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
        patch = $patchName
        patch_sha256 = $patchHash
        records = $records
    }
}

function Get-TelemetryValue([object]$Sample, [string]$Name, $Default) {
    if ($Sample -and $Sample.PSObject.Properties[$Name]) { return $Sample.$Name }
    return $Default
}

function Read-Telemetry {
    if (!(Test-Path -LiteralPath $telemetryPath)) { return $null }
    try { return Get-Content -LiteralPath $telemetryPath -Raw | ConvertFrom-Json }
    catch { return $null }
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
    $live | Sort-Object StartTime -Descending | Select-Object -First 1
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
    $zombies
}

function Test-FileUnlocked([string]$Path) {
    if (!(Test-Path -LiteralPath $Path)) { return $true }
    try {
        $stream = [System.IO.File]::Open($Path, 'Open', 'ReadWrite', 'None')
        $stream.Dispose()
        return $true
    }
    catch { return $false }
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
        throw "Half-terminated helldivers2.exe PID(s) $($remaining.Id -join ', ') remain. Restart Windows before deployment."
    }
}

function Confirm-GameProcess($Expected, [string]$Stage) {
    $current = Get-GameProcess
    if (!$current) { throw "helldivers2.exe stopped before $Stage." }
    if ($current.Id -ne $Expected.Id -or $current.StartTime -ne $Expected.StartTime) {
        throw "helldivers2.exe changed before $Stage; refusing stale process state."
    }
    $current
}

function Find-Steam {
    $candidate = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $GameRoot))) 'steam.exe'
    if (Test-Path -LiteralPath $candidate) { return $candidate }
    $registryPath = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamExe
    if ($registryPath -and (Test-Path -LiteralPath $registryPath)) { return $registryPath }
    throw 'steam.exe was not found. Start the game yourself and rerun with -NoLaunch.'
}

if (!(Test-Path -LiteralPath $gameExe)) { throw "Game executable not found: $gameExe" }
if (!(Test-Path -LiteralPath $reshadeDll)) { throw "ReShade dxgi.dll not found: $reshadeDll" }
Recover-ZombieGameProcesses
if ($RecoverOnly) {
    $live = Get-GameProcess
    if ($live) { throw "helldivers2.exe PID $($live.Id) is still running; recovery will not stop a live game." }
    if (!(Test-FileUnlocked $installedAddon)) { throw "The installed add-on is still locked: $installedAddon" }
    Write-Host 'Recovery passed: no live or half-terminated game process remains, and the add-on is unlocked.'
    exit 0
}

$ProfileDirectory = (Resolve-Path -LiteralPath $ProfileDirectory).Path
$profileFiles = @(Get-ChildItem -LiteralPath $ProfileDirectory -Filter '*.hd2profile' -File | Sort-Object Name)
if (!$profileFiles.Count) { throw "No .hd2profile files found: $ProfileDirectory" }
$profileMetadata = @($profileFiles | ForEach-Object { Read-ProfileHeader $_.FullName })
if (@($profileMetadata.file | Select-Object -Unique).Count -ne $profileMetadata.Count) {
    throw 'Runtime profile filenames must be unique.'
}
foreach ($profile in $profileMetadata) {
    $installedPatch = Join-Path $gameData $profile.patch
    if (!(Test-Path -LiteralPath $installedPatch)) { throw "Profile patch is not installed: $installedPatch" }
    $actual = (Get-FileHash -LiteralPath $installedPatch -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $profile.patch_sha256) {
        throw "Installed patch does not match profile $($profile.file): $($profile.patch)"
    }
}

if (!$AddonPath) { $AddonPath = Find-AddonBuild }
$AddonPath = (Resolve-Path -LiteralPath $AddonPath).Path
if ((Get-PeMachine $AddonPath) -ne 0x8664) { throw 'The add-on is not an x64 PE binary.' }
if ($AutomateInput -and $ObserveSeconds -lt $InputDelaySeconds + 8) {
    throw '-ObserveSeconds must allow at least eight seconds after -InputDelaySeconds.'
}
if ($EditTest) {
    if (!$AutomateInput) { throw '-EditTest requires -AutomateInput.' }
    if ($EditUnit -notmatch '^[0-9A-Fa-f]{16}$') { throw '-EditUnit must be a 16-digit hexadecimal unit ID.' }
    if ($EditSlot -lt 0) { throw '-EditSlot is required for -EditTest.' }
    if ($EditX -eq 0.0 -and $EditY -eq 0.0 -and $EditZ -eq 0.0) {
        throw 'At least one of -EditX, -EditY, or -EditZ must be nonzero.'
    }
}

$reshadeVersion = (Get-Item -LiteralPath $reshadeDll).VersionInfo.ProductVersion
$addonHash = (Get-FileHash -LiteralPath $AddonPath -Algorithm SHA256).Hash
$running = Get-GameProcess
if (!$running -and !(Test-FileUnlocked $installedAddon)) {
    throw "No live game was found, but the installed add-on is locked: $installedAddon"
}

$activeNames = @($profileMetadata.file | Sort-Object)
if (!$NoDeploy) {
    $installedHash = if (Test-Path -LiteralPath $installedAddon) {
        (Get-FileHash -LiteralPath $installedAddon -Algorithm SHA256).Hash
    }
    if ($installedHash -ne $addonHash) {
        if ($running) { throw 'The game is running and the installed add-on differs. Close the game and rerun.' }
        Copy-Item -LiteralPath $AddonPath -Destination $installedAddon -Force
    }
    if ($running) {
        foreach ($profile in $profileMetadata) {
            $destination = Join-Path $installedProfileDirectory $profile.file
            if (!(Test-Path -LiteralPath $destination) -or
                (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $profile.sha256) {
                throw "The game is running and installed profile differs: $($profile.file)"
            }
        }
        $installedActive = if (Test-Path -LiteralPath $activeProfilesPath) {
            @(Get-Content -LiteralPath $activeProfilesPath | Where-Object { $_ }) | Sort-Object
        } else { @() }
        if (($installedActive -join "`n") -ne ($activeNames -join "`n")) {
            throw 'The game is running and active_profiles.txt differs. Close the game and rerun.'
        }
    }
    else {
        New-Item -ItemType Directory -Force -Path $installedProfileDirectory | Out-Null
        foreach ($profile in $profileMetadata) {
            Copy-Item -LiteralPath $profile.path -Destination (Join-Path $installedProfileDirectory $profile.file) -Force
        }
        [System.IO.File]::WriteAllLines($activeProfilesPath, $activeNames, [System.Text.UTF8Encoding]::new($false))
    }
}
else {
    if (!(Test-Path -LiteralPath $installedAddon) -or
        (Get-FileHash -LiteralPath $installedAddon -Algorithm SHA256).Hash -ne $addonHash) {
        throw 'The installed add-on differs from -AddonPath while -NoDeploy is active.'
    }
    foreach ($profile in $profileMetadata) {
        $destination = Join-Path $installedProfileDirectory $profile.file
        if (!(Test-Path -LiteralPath $destination) -or
            (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $profile.sha256) {
            throw "The installed profile differs while -NoDeploy is active: $($profile.file)"
        }
    }
    $installedActive = if (Test-Path -LiteralPath $activeProfilesPath) {
        @(Get-Content -LiteralPath $activeProfilesPath | Where-Object { $_ }) | Sort-Object
    } else { @() }
    if (($installedActive -join "`n") -ne ($activeNames -join "`n")) {
        throw 'The installed active_profiles.txt differs while -NoDeploy is active.'
    }
}

if ($PreflightOnly) {
    Write-Host "Preflight passed: ReShade $reshadeVersion, x64 add-on, $($profileMetadata.Count) profile file(s), SHA256 $addonHash"
    exit 0
}

$steamScreenshotDirectory = $null
$steamScreenshotsBefore = @{}
if ($SteamCapture) {
    $steamRoot = Split-Path -Parent (Find-Steam)
    $screenshotDirectories = @(foreach ($account in @(Get-ChildItem -LiteralPath (Join-Path $steamRoot 'userdata') -Directory)) {
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

$previousSession = if ($running) { $null } else { (Read-Telemetry).session_id }
$launchedByRunner = $false
if (!$running) {
    if ($NoLaunch) { throw 'Helldivers 2 is not running and -NoLaunch was specified.' }
    $steam = Find-Steam
    Write-Host 'Launching Helldivers 2 through Steam...'
    Start-Process -FilePath $steam -ArgumentList '-applaunch', '553850'
    $launchedByRunner = $true
    $startupDeadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
    do {
        Start-Sleep -Seconds 1
        $running = Get-GameProcess
    } while (!$running -and [DateTime]::UtcNow -lt $startupDeadline)
    if (!$running) { throw "helldivers2.exe did not start within $StartupTimeoutSeconds seconds." }
}
else { Write-Host "Attaching to running process $($running.Id)." }

if ($AutomateInput) {
    $running = Confirm-GameProcess $running 'the automation request'
    New-Item -ItemType Directory -Force -Path $runtimeDirectory | Out-Null
    Remove-Item -LiteralPath $automationPath -Force -ErrorAction SilentlyContinue
    $skipIntro = if ($launchedByRunner) { 1 } else { 0 }
    $editRequested = if ($EditTest) { 1 } else { 0 }
    $steamRequested = if ($SteamCapture) { 1 } else { 0 }
    $unit = if ($EditTest) { $EditUnit.ToLowerInvariant() } else { '0' }
    $slot = if ($EditTest) { $EditSlot } else { 0 }
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $request = '{0} {1} {2} 0 {3} {4} {5} {6} {7} {8}' -f `
        (1000 * $InputDelaySeconds), $skipIntro, $editRequested, $steamRequested, $unit, $slot,
        $EditX.ToString('R', $culture), $EditY.ToString('R', $culture), $EditZ.ToString('R', $culture)
    [System.IO.File]::WriteAllText($automationPath, $request, [System.Text.Encoding]::ASCII)
    Write-Host "Requested automation (edit: $([bool]$EditTest), unit: $unit, slot: $slot, translation: [$EditX,$EditY,$EditZ])."
}

New-Item -ItemType Directory -Force -Path $resultDir | Out-Null
Write-Host "Observing process $($running.Id) for $ObserveSeconds seconds."
$latestTelemetry = $null
$maxHits = 0
$maxPartial = 0
$maxBestPartial = 0
$profileFilesLoaded = 0
$profileTableRecords = 0
$profileDuplicateRecords = 0
$profileTablesLoaded = 0
$profileLoadErrors = 0
$experimentMode = $null
$inputStarted = $false
$inputCompleted = $false
$inputError = $null
$editCompleted = $false
$editWriteSuccesses = 0
$editImmediateReadbacks = 0
$editPresentReadbacks = 0
$editRefills = 0
$editStaleSkips = 0
$editTargetsSelected = 0
$editTargetsWritten = 0
$editTargetsRestored = 0
$editRestoreSucceeded = $false
$editError = 0
$automationStage = 0
$introAttempts = 0
$completedAt = $null
$lastFrame = -1
$lastProgressAt = [DateTime]::UtcNow
$captureFiles = @()
$deadline = [DateTime]::UtcNow.AddSeconds($ObserveSeconds)
while ([DateTime]::UtcNow -lt $deadline) {
    $current = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
    if (!$current) { break }
    $current.Refresh()
    if ($current.HasExited) { break }
    $sample = Read-Telemetry
    if ($sample -and $sample.process_id -eq $running.Id -and
        ($null -eq $previousSession -or $sample.session_id -ne $previousSession)) {
        $latestTelemetry = $sample
        if ([int64]$sample.frame -gt $lastFrame) {
            $lastFrame = [int64]$sample.frame
            $lastProgressAt = [DateTime]::UtcNow
        }
        $experimentMode = [string](Get-TelemetryValue $sample 'experiment_mode' '')
        $profileFilesLoaded = [Math]::Max($profileFilesLoaded, [int](Get-TelemetryValue $sample 'profile_files_loaded' 0))
        $profileTableRecords = [Math]::Max($profileTableRecords, [int](Get-TelemetryValue $sample 'profile_table_records' 0))
        $profileDuplicateRecords = [Math]::Max($profileDuplicateRecords, [int](Get-TelemetryValue $sample 'profile_duplicate_records' 0))
        $profileTablesLoaded = [Math]::Max($profileTablesLoaded, [int](Get-TelemetryValue $sample 'profile_tables_loaded' 0))
        $profileLoadErrors = [Math]::Max($profileLoadErrors, [int](Get-TelemetryValue $sample 'profile_load_errors' 0))
        $maxHits = [Math]::Max($maxHits, [int](Get-TelemetryValue $sample 'converted_ib_hits' 0))
        $maxPartial = [Math]::Max($maxPartial, [int64](Get-TelemetryValue $sample 'converted_ib_partial_candidates' 0))
        $maxBestPartial = [Math]::Max($maxBestPartial, [int](Get-TelemetryValue $sample 'converted_ib_best_partial_entries' 0))
        $automationStage = [Math]::Max($automationStage, [int](Get-TelemetryValue $sample 'automation_stage' 0))
        $introAttempts = [Math]::Max($introAttempts, [int](Get-TelemetryValue $sample 'automation_intro_attempts' 0))
        $inputStarted = $inputStarted -or [bool](Get-TelemetryValue $sample 'automation_input_started' $false)
        $inputCompleted = $inputCompleted -or [bool](Get-TelemetryValue $sample 'automation_completed' $false)
        $editCompleted = $editCompleted -or [bool](Get-TelemetryValue $sample 'edit_test_completed' $false)
        $editWriteSuccesses = [Math]::Max($editWriteSuccesses, [int64](Get-TelemetryValue $sample 'edit_write_successes' 0))
        $editImmediateReadbacks = [Math]::Max($editImmediateReadbacks, [int64](Get-TelemetryValue $sample 'edit_immediate_readbacks' 0))
        $editPresentReadbacks = [Math]::Max($editPresentReadbacks, [int64](Get-TelemetryValue $sample 'edit_present_readbacks' 0))
        $editRefills = [Math]::Max($editRefills, [int64](Get-TelemetryValue $sample 'edit_refills_observed' 0))
        $editStaleSkips = [Math]::Max($editStaleSkips, [int64](Get-TelemetryValue $sample 'edit_stale_skips' 0))
        $editTargetsSelected = [Math]::Max($editTargetsSelected, [int](Get-TelemetryValue $sample 'edit_targets_selected' 0))
        $editTargetsWritten = [Math]::Max($editTargetsWritten, [int](Get-TelemetryValue $sample 'edit_targets_written' 0))
        $editTargetsRestored = [Math]::Max($editTargetsRestored, [int](Get-TelemetryValue $sample 'edit_targets_restored' 0))
        $editRestoreSucceeded = $editRestoreSucceeded -or [bool](Get-TelemetryValue $sample 'edit_restore_succeeded' $false)
        $editError = [Math]::Max($editError, [int](Get-TelemetryValue $sample 'edit_error' 0))
        if ([int](Get-TelemetryValue $sample 'automation_error' 0) -ne 0) {
            $inputError = "In-game automation error $($sample.automation_error)."
        }
        if ($inputCompleted -or $inputError) {
            if (!$completedAt) { $completedAt = [DateTime]::UtcNow }
            elseif (([DateTime]::UtcNow - $completedAt).TotalSeconds -ge 5) { break }
        }
    }
    if ($latestTelemetry -and ([DateTime]::UtcNow - $lastProgressAt).TotalSeconds -ge 60) {
        Write-Warning "Telemetry stopped advancing at frame $lastFrame."
        break
    }
    Start-Sleep -Seconds 1
}

if ($AutomateInput -and !$inputStarted -and !$inputError) { $inputError = 'Automation did not start.' }
elseif ($AutomateInput -and !$inputCompleted -and !$inputError) { $inputError = 'Automation did not complete.' }
Remove-Item -LiteralPath $automationPath -Force -ErrorAction SilentlyContinue

$logLines = if (Test-Path -LiteralPath $reshadeLog) { @(Get-Content -LiteralPath $reshadeLog -Tail 600) } else { @() }
$interestingLog = @($logLines | Where-Object {
    $_ -match 'HD2PaletteProbe|HD2 Armature Profile Runtime|requested API version|Failed to (load|register) add-on'
})
$registerPattern = '\|\s+Registered add-on "HD2 Armature Profile Runtime"'
$unregisterPattern = '\|\s+Unregistered add-on "HD2 Armature Profile Runtime"'
$registered = [bool]($interestingLog | Where-Object { $_ -match $registerPattern })
$unregistered = [bool]($interestingLog | Where-Object { $_ -match $unregisterPattern })
$loadError = [bool]($interestingLog | Where-Object { $_ -match 'ERROR.*(HD2PaletteProbe|requested API version|Failed to (load|register) add-on)' })
$telemetrySeen = $null -ne $latestTelemetry
$heartbeatAge = if ($telemetrySeen) {
    [Math]::Max(0.0, ([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() - [double]$latestTelemetry.updated_unix_ms) / 1000.0)
} else { $null }
$currentProcess = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
if ($currentProcess) { $currentProcess.Refresh() }
$processAlive = $null -ne $currentProcess -and !$currentProcess.HasExited
$runtimeActive = $telemetrySeen -and $heartbeatAge -le 5.0 -and $processAlive -and $latestTelemetry.frame -gt 0
$expectedMode = if ($EditTest) { 'converted_ib_edit' } else { 'converted_ib_scan' }
$editVerified = !$EditTest -or ($editCompleted -and $editWriteSuccesses -gt 0 -and
    $editImmediateReadbacks -gt 0 -and $editRestoreSucceeded -and $editError -eq 0)
$status = if ($loadError) { 'failed-load' }
    elseif (!$registered -and !$telemetrySeen) { 'inconclusive-load' }
    elseif (!$runtimeActive) { 'failed-runtime' }
    elseif ($profileFilesLoaded -eq 0 -or $profileTablesLoaded -eq 0 -or $profileLoadErrors -ne 0) { 'failed-profile-load' }
    elseif ($experimentMode -ne $expectedMode) { 'failed-experiment-mode' }
    elseif ($AutomateInput -and ($inputError -or !$inputCompleted)) { 'failed-input' }
    elseif ($maxHits -eq 0) { 'failed-no-converted-match' }
    elseif ($EditTest -and !$editVerified) { 'failed-edit' }
    elseif ($EditTest) { 'passed-converted-edit' }
    else { 'passed-converted-scan' }

$interestingLog | Set-Content -LiteralPath (Join-Path $resultDir 'reshade-tail.log') -Encoding utf8
if ($telemetrySeen) {
    $latestTelemetry | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $resultDir 'telemetry.json') -Encoding utf8
}
if ($SteamCapture) {
    $newScreenshots = @(Get-ChildItem -LiteralPath $steamScreenshotDirectory -File |
        Where-Object { !$steamScreenshotsBefore.ContainsKey($_.Name) } | Sort-Object LastWriteTime)
    $names = if ($EditTest) {
        @('capture-edit-before-steam.jpg', 'capture-edit-active-steam.jpg', 'capture-edit-restored-steam.jpg')
    } else {
        @('capture-load-steam.jpg', 'capture-ready-steam.jpg', 'capture-walk-steam.jpg', 'capture-stretch-steam.jpg')
    }
    for ($index = 0; $index -lt [Math]::Min($newScreenshots.Count, $names.Count); ++$index) {
        Copy-Item -LiteralPath $newScreenshots[$index].FullName -Destination (Join-Path $resultDir $names[$index])
        $captureFiles += $names[$index]
    }
}

$shutdownSucceeded = $false
$shutdownError = $null
if ($ShutdownAfterTest) {
    try {
        $target = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
        if ($target) {
            $target.Refresh()
            if (!$target.HasExited) {
                if ($target.StartTime -ne $running.StartTime) { throw 'PID reuse detected; shutdown refused.' }
                Stop-Process -Id $running.Id -Force -ErrorAction Stop
                Wait-Process -Id $running.Id -Timeout 10 -ErrorAction SilentlyContinue
            }
        }
        $cleanupDeadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $remaining = Get-Process -Id $running.Id -ErrorAction SilentlyContinue
            if (!$remaining) { break }
            $remaining.Refresh()
            Start-Sleep -Milliseconds 500
        } while ([DateTime]::UtcNow -lt $cleanupDeadline)
        $shutdownSucceeded = $null -eq $remaining
        if (!$shutdownSucceeded) { $shutdownError = 'The tested process remains after shutdown.' }
    }
    catch { $shutdownError = $_.Exception.Message }
}
if ($ShutdownAfterTest -and !$shutdownSucceeded -and $status -like 'passed-*') { $status = 'failed-shutdown' }

$summary = [ordered]@{
    status = $status
    timestamp = (Get-Date).ToString('o')
    game_process_id = $running.Id
    game_root = $GameRoot
    reshade_version = $reshadeVersion
    required_reshade_api = 17
    addon_sha256 = $addonHash
    addon_registered = $registered
    addon_unregistered = $unregistered
    addon_load_error = $loadError
    runtime_profiles = $profileMetadata
    profile_files_loaded = $profileFilesLoaded
    profile_table_records = $profileTableRecords
    profile_duplicate_records = $profileDuplicateRecords
    profile_tables_loaded = $profileTablesLoaded
    profile_load_errors = $profileLoadErrors
    telemetry_seen = $telemetrySeen
    heartbeat_age_seconds = $heartbeatAge
    process_alive = $processAlive
    runtime_active = $runtimeActive
    experiment_mode = $experimentMode
    expected_experiment_mode = $expectedMode
    max_converted_ib_hits = $maxHits
    max_converted_partial_candidates = $maxPartial
    max_converted_best_partial_entries = $maxBestPartial
    input_requested = [bool]$AutomateInput
    input_started = $inputStarted
    input_completed = $inputCompleted
    input_error = $inputError
    edit_test_requested = [bool]$EditTest
    edit_unit = $EditUnit
    edit_slot = $EditSlot
    edit_world_translation = @($EditX, $EditY, $EditZ)
    edit_test_completed = $editCompleted
    edit_write_successes = $editWriteSuccesses
    edit_immediate_readbacks = $editImmediateReadbacks
    edit_present_readbacks = $editPresentReadbacks
    edit_refills_observed = $editRefills
    edit_stale_skips = $editStaleSkips
    edit_targets_selected = $editTargetsSelected
    edit_targets_written = $editTargetsWritten
    edit_targets_restored = $editTargetsRestored
    edit_restore_succeeded = $editRestoreSucceeded
    edit_error = $editError
    edit_channel_verified = $editVerified
    automation_stage = $automationStage
    intro_attempts = $introAttempts
    capture_files = $captureFiles
    steam_capture_requested = [bool]$SteamCapture
    shutdown_requested = [bool]$ShutdownAfterTest
    shutdown_succeeded = $shutdownSucceeded
    shutdown_error = $shutdownError
    telemetry = $latestTelemetry
}
$summaryPath = Join-Path $resultDir 'summary.json'
$summary | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $summaryPath -Encoding utf8
Write-Host "Result: $status"
Write-Host "Report: $summaryPath"
if ($status -like 'failed-*' -or $status -like 'inconclusive-*') { exit 1 }
