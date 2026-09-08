$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts\Install-Jukebox.ps1'
$tokens=$null
$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($scriptPath,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors | Out-String) }
$resetFunction=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Reset-MediaSources'},$false)
if (-not $resetFunction) { throw 'Missing source reset function.' }
# Load only this data-file function. No accounts, elevation or registry changes.
. ([ScriptBlock]::Create($resetFunction.Extent.Text))

function Check([bool]$condition,[string]$message) { if (-not $condition) { throw $message } }
$tempBase=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$testRoot=Join-Path $tempBase ('NeonJukeboxKioskTest_' + [Guid]::NewGuid().ToString('N'))
try {
    $profile=Join-Path $testRoot 'profile'
    $app=Join-Path $testRoot 'app'
    $preferences=Join-Path $profile 'AppData\Roaming\NeonJukebox\NeonJukebox'
    $library=Join-Path $app 'library'
    New-Item -ItemType Directory -Path $preferences,$library -Force | Out-Null
    $settingsPath=Join-Path $preferences 'settings.json'
    $queuePath=Join-Path $preferences 'queue.json'
    $libraryPath=Join-Path $library 'library.json'
    [ordered]@{
        musicRoots=@('Z:\music');videoRoots=@('Z:\videos');currentTrackId='old-track'
        playbackPositionMs=12345;playbackWasActive=$true;currentTrackManual=$true
        ambientMediaKind='video';theme='retro';volume=0.6
        adminPin=@{hash='test-only-pin-hash';salt='test-only-salt'}
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $settingsPath -Encoding UTF8
    '[{"trackId":"old-track"}]' | Set-Content -LiteralPath $queuePath -Encoding UTF8
    '{"musicRoots":["Z:\\music"],"videoRoots":["Z:\\videos"],"tracks":[{"id":"old-track"}]}' | Set-Content -LiteralPath $libraryPath -Encoding UTF8
    $originalContents=@{}
    foreach ($path in @($settingsPath,$queuePath,$libraryPath)) { $originalContents[(Split-Path -Leaf $path)]=[Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) }
    $backup=Reset-MediaSources $profile $app
    foreach ($name in $originalContents.Keys) { Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $backup $name))) -eq $originalContents[$name]) "Backup changed $name" }
    $saved=Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
    Check (@($saved.musicRoots).Count -eq 0 -and @($saved.videoRoots).Count -eq 0) 'Sources were not cleared.'
    Check ($saved.currentTrackId -eq '' -and $saved.playbackPositionMs -eq 0 -and -not $saved.playbackWasActive -and -not $saved.currentTrackManual) 'Playback was not reset.'
    Check ($saved.theme -eq 'retro' -and $saved.volume -eq 0.6 -and $saved.adminPin.hash -eq 'test-only-pin-hash' -and $saved.adminPin.salt -eq 'test-only-salt') 'Unrelated settings changed.'
    $savedLibrary=Get-Content -LiteralPath $libraryPath -Raw | ConvertFrom-Json
    Check (@($savedLibrary.tracks).Count -eq 0 -and @($savedLibrary.videoRoots).Count -eq 0 -and @($savedLibrary.musicRoots).Count -eq 0) 'Catalogue was not cleared.'
    Check ((Get-Content -LiteralPath $queuePath -Raw | ConvertFrom-Json).Count -eq 0) 'Queue was not cleared.'
    $secondBackup=Reset-MediaSources $profile $app
    Check ($backup -ne $secondBackup -and (Test-Path -LiteralPath $backup)) 'Repeated reset overwrote its backup.'
    $emptyProfile=Join-Path $testRoot 'fresh-profile'
    $emptyApp=Join-Path $testRoot 'fresh-app'
    $null=Reset-MediaSources $emptyProfile $emptyApp
    $fresh=Get-Content -LiteralPath (Join-Path $emptyProfile 'AppData\Roaming\NeonJukebox\NeonJukebox\settings.json') -Raw | ConvertFrom-Json
    Check (@($fresh.musicRoots).Count -eq 0 -and @($fresh.videoRoots).Count -eq 0) 'Fresh-profile reset failed.'
    Write-Output 'Kiosk source reset: backups, empty sources, queue, catalogue, preserved PIN/theme, repeated reset and fresh profile passed.'
} finally {
    $resolved=[IO.Path]::GetFullPath($testRoot)
    if (-not $resolved.StartsWith($tempBase + '\NeonJukeboxKioskTest_',[StringComparison]::OrdinalIgnoreCase)) { throw 'Unexpected test cleanup target.' }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
