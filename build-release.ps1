#requires -Version 7.0
# Build and package one clean annotated source tag. Inputs are existing compiler,
# Qt/source and pinned runtime files; output must be a new directory. Host checks
# precede payload assembly, and the receipt records exact bytes and tag identity.
# Guest verification and publication are separate caller responsibilities.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$QtRoot,[Parameter(Mandatory)][string]$QtSourceArchive,[Parameter(Mandatory)][string]$OutputRoot,[Parameter(Mandatory)][string]$RuntimeRedist,[string]$ISCC,[string]$Version="2026.11.26")
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$src=$PSScriptRoot
$commit=(& git -C $src rev-parse HEAD).Trim()
if($Version -notmatch '^(\d{4})\.(\d{1,2})\.(\d{2})(?:\.([1-9]\d*))?$'){throw 'Expected a calendar version.'}
$null=[DateTime]::new([int]$Matches[1],[int]$Matches[2],[int]$Matches[3])
if($Matches[4] -and [int]$Matches[4] -gt 65535){throw 'Version revision exceeds the Windows metadata limit.'}
$sourceTag="v$Version"
$resourceVersion=if(($Version.Split('.')).Count -eq 3){"$Version.0"}else{$Version}

# Verify the annotated object and its peeled commit before creating any output.
# Both identities are retained below; a matching version label alone is insufficient.
$tagObject=(& git -C $src rev-parse $sourceTag).Trim()
if ((& git -C $src cat-file -t $sourceTag) -ne 'tag' -or (& git -C $src rev-parse "$sourceTag^{}") -ne $commit) { throw 'Expected matching annotated release tag.' }
if (& git -C $src status --porcelain) { throw 'Build requires a clean source checkout.' }
if (Test-Path -LiteralPath $OutputRoot) { throw 'Output directory must be new.' }
$run=[IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $run | Out-Null
$build=Join-Path $run 'build'
$corpus=Join-Path $src 'tests/services/fixtures/service-corpus.json'
# Drain stdout and stderr concurrently so a verbose child cannot block on a full
# pipe. Enforce a ten-minute bound and propagate non-zero exits before packaging.
function Invoke-ServiceTestTool([string]$Exe,[string[]]$Arguments) {
    $process=[Diagnostics.Process]::new()
    $process.StartInfo.FileName=$Exe
    foreach($argument in $Arguments){$process.StartInfo.ArgumentList.Add($argument)}
    $process.StartInfo.UseShellExecute=$false
    $process.StartInfo.RedirectStandardOutput=$true
    $process.StartInfo.RedirectStandardError=$true
    if(!$process.Start()){throw "Could not start $Exe"}
    $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
    if(!$process.WaitForExit(600000)){$process.Kill();throw "Timed out: $Exe"}
    $process.WaitForExit()
    $stdout.Result;$stderr.Result
    if($process.ExitCode -ne 0){throw "Exit $($process.ExitCode) from $Exe"}
}
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$sourceName = 'qtbase-everywhere-src-6.10.2.tar.xz'
$qtSource = (Resolve-Path -LiteralPath $QtSourceArchive).Path
$sourceHash = 'aeb78d29291a2b5fd53cb55950f8f5065b4978c25fb1d77f627d695ab9adf21e'
if ((Get-FileHash -LiteralPath $qtSource).Hash -ne $sourceHash) { throw 'Qt corresponding-source archive checksum mismatch.' }
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or !$vs) { throw 'MSVC x64 build tools not found.' }
$vsVersion = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion
if ($LASTEXITCODE -ne 0 -or !$vsVersion) { throw 'Cannot determine the MSVC version.' }
$generator = if ($vsVersion -like '18.*') { 'Visual Studio 18 2026' } else { 'Visual Studio 17 2022' }
$cmake = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (!(Test-Path -LiteralPath $cmake -PathType Leaf) -or
    !(Test-Path -LiteralPath (Join-Path $QtRoot 'lib/cmake/Qt6/Qt6Config.cmake') -PathType Leaf)) { throw 'Expected the existing MSVC CMake and Qt kit.' }
Invoke-ServiceTestTool $cmake @('-S',$src,'-B',$build,'-G',$generator,'-A','x64',
    "-DCMAKE_PREFIX_PATH=$QtRoot",'-DBUILD_TESTING=ON',"-DCC_APP_VERSION=$Version")
$buildLog = Join-Path $run 'build.log'
& $cmake --build $build --config Release --target cc_app cc_window_tests cc_native_tests cc_update_tests cc_https_tests 2>&1 | Tee-Object -FilePath $buildLog
if ($LASTEXITCODE -ne 0) { throw "Build failed. First compiler error is in $buildLog." }
$release = Join-Path $build 'Release'
foreach ($module in 'Core','Gui','Widgets','Test','Network') {
    Copy-Item -LiteralPath (Join-Path $QtRoot "bin/Qt6$module.dll") -Destination $release
}
$plugins = Join-Path $release 'platforms'
New-Item -ItemType Directory -Path $plugins | Out-Null
Copy-Item -LiteralPath (Join-Path $QtRoot 'plugins/platforms/qwindows.dll') -Destination $plugins
$fixture = Join-Path $release 'cc_window_tests.exe'
$application = Join-Path $release 'MabiTools.ChannelChecker.exe'
$tls = Join-Path $release 'tls'
$backend = Join-Path $QtRoot 'plugins/tls/qschannelbackend.dll'
$hasTls = Test-Path -LiteralPath $backend -PathType Leaf
if ($hasTls) {
    New-Item -ItemType Directory -Path $tls | Out-Null
    Copy-Item -LiteralPath $backend -Destination $tls
}
$log = Join-Path $run 'verification.log'
Invoke-ServiceTestTool (Join-Path $release 'cc_https_tests.exe') @('--configuration-check') | Tee-Object -FilePath $log
Invoke-ServiceTestTool (Join-Path $release 'cc_update_tests.exe') @() | Tee-Object -FilePath $log -Append
Invoke-ServiceTestTool (Join-Path $build 'src/measurement/Release/cc_native_tests.exe') @() | Tee-Object -FilePath $log -Append
Invoke-ServiceTestTool $application @('--self-check') | Tee-Object -FilePath $log -Append
Invoke-ServiceTestTool $application @('--migration-self-check') | Tee-Object -FilePath $log -Append
Invoke-ServiceTestTool $fixture @($corpus,$run) | Tee-Object -FilePath $log -Append
# Qt scale factors only affect the isolated self-check invocations. Restore
# the caller's setting, including absence, before making the package.
$originalScale = $env:QT_SCALE_FACTOR
try {
    foreach ($scale in '1','1.25','1.5') {
        $env:QT_SCALE_FACTOR = $scale
        Write-Host "Layout fixture at Qt scale factor $scale"
        Invoke-ServiceTestTool $application @('--layout-self-check') | Tee-Object -FilePath $log -Append
        Invoke-ServiceTestTool $application @('--settings-capture',(Join-Path $run "settings-$scale.png")) | Tee-Object -FilePath $log -Append
    }
} finally { $env:QT_SCALE_FACTOR = $originalScale }

# Checks use disposable profiles and do not read existing application data.
# Only redistribute the exact Qt binaries matched to the included source.
$kitInputs = Join-Path $src 'licenses/qt'
$known = Get-Content -LiteralPath (Join-Path $kitInputs 'official-inventory.json') -Raw | ConvertFrom-Json -AsHashtable
$qtFiles = [ordered]@{
    'Qt6Core.dll' = 'bin/Qt6Core.dll'; 'Qt6Gui.dll' = 'bin/Qt6Gui.dll'
    'Qt6Network.dll' = 'bin/Qt6Network.dll'; 'Qt6Widgets.dll' = 'bin/Qt6Widgets.dll'
    'platforms/qwindows.dll' = 'plugins/platforms/qwindows.dll'
    'tls/qschannelbackend.dll' = 'plugins/tls/qschannelbackend.dll'
}
foreach ($relative in $qtFiles.Keys) {
    if ((Get-FileHash -LiteralPath (Join-Path $release $relative)).Hash -ne $known[$qtFiles[$relative]]) {
        throw "The local Qt kit does not match the audited Qt build: $relative"
    }
}
$projectLicense = Join-Path $src 'LICENSE'
$projectNotices = Join-Path $src 'licenses'

$payload = Join-Path $run 'payload'
$dist = Join-Path $run 'packages'
$notices = Join-Path $payload 'notices'
$null = New-Item -ItemType Directory -Path $payload,$dist,$notices
$runtimePin = Get-Content -LiteralPath (Join-Path $src 'installer/runtime.json') -Raw | ConvertFrom-Json
$RuntimeRedist = (Resolve-Path -LiteralPath $RuntimeRedist).Path
$runtimeFile = Get-Item -LiteralPath $RuntimeRedist
$signature = Get-AuthenticodeSignature -LiteralPath $RuntimeRedist
if ($runtimeFile.Length -ne $runtimePin.size -or (Get-FileHash -LiteralPath $RuntimeRedist).Hash -ne $runtimePin.sha256 -or
    $signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,') {
    throw 'The redistributable must match the pinned Microsoft-signed x64 runtime.'
}
# Assemble the shared portable/Setup payload only after checking dependency and
# runtime identity. Record hashes of the copied bytes, not their source paths.
$entries = @(
    foreach ($relative in (@('MabiTools.ChannelChecker.exe') + @($qtFiles.Keys))) {
        $dest = Join-Path $payload $relative
        $directory = Split-Path -Parent $dest
        if (!(Test-Path -LiteralPath $directory)) { $null = New-Item -ItemType Directory -Path $directory }
        Copy-Item -LiteralPath (Join-Path $release $relative) -Destination $dest
        [ordered]@{ path=$relative; sha256=(Get-FileHash -LiteralPath $dest).Hash }
    }
)
'[Paths]', 'Prefix=.', 'Plugins=.' | Set-Content -LiteralPath (Join-Path $payload 'qt.conf') -Encoding ascii
Copy-Item -LiteralPath $projectLicense -Destination (Join-Path $payload 'LICENSE')
Copy-Item -LiteralPath $projectNotices -Destination (Join-Path $notices 'project') -Recurse
foreach ($file in 'THIRD-PARTY-NOTICES.txt','GPL-3.0-only.txt','LGPL-3.0-only.txt','qtbase-6.10.2.spdx.json','QT-SOURCE-AND-REPLACEMENT.txt') {
    Copy-Item -LiteralPath (Join-Path $kitInputs $file) -Destination $notices
}
[ordered]@{
    # CandidateSha256 is the stable manifest key for the application EXE hash.
    Architecture='Windows x64'; Version=$Version; UpdatesEnabled=$true; UpdateTransport='native-https'; SourceCommit=$commit; SourceTag=$sourceTag; TagObject=$tagObject; QtVersion='6.10.2'; CandidateSha256=(Get-FileHash -LiteralPath $application).Hash
    RuntimeVersion=$runtimePin.version; RuntimeMinimumVersion=$runtimePin.minimumVersion; RuntimeSha256=$runtimePin.sha256; files=$entries; QtSourceFile=$sourceName; QtSourceSha256=$sourceHash
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $payload 'manifest.json') -Encoding utf8

if (!$ISCC) {
    $command = Get-Command ISCC.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $ISCC = $command.Source }
    else {
        foreach ($path in @($env:LOCALAPPDATA, $env:ProgramFiles, ${env:ProgramFiles(x86)})) {
            if (!$path) { continue }
            foreach ($relative in 'Programs/Inno Setup 6/ISCC.exe','Inno Setup 6/ISCC.exe') {
                $p = Join-Path $path $relative
                if (!$ISCC -and (Test-Path -LiteralPath $p -PathType Leaf)) { $ISCC = $p }
            }
        }
    }
}
if (!$ISCC -or !(Test-Path -LiteralPath $ISCC -PathType Leaf)) { throw 'Existing Inno Setup compiler not found. Pass -ISCC with its full path.' }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$portable = Join-Path $dist "MabiTools-Channel-Checker-$Version-win-x64.zip"
New-Item -ItemType Directory -Path (Join-Path $payload 'sources') | Out-Null
Copy-Item -LiteralPath $qtSource -Destination (Join-Path $payload "sources/$sourceName")
[IO.Compression.ZipFile]::CreateFromDirectory($payload, $portable)
Copy-Item -LiteralPath $qtSource -Destination (Join-Path $dist $sourceName)
$definition = Join-Path $src 'installer/ChannelChecker.iss'
Invoke-ServiceTestTool $ISCC @("/DAppVersion=$Version","/DResourceVersion=$resourceVersion","/DVerifiedPayload=$payload","/DPackageOutput=$dist","/DRuntimeRedist=$RuntimeRedist","/DRuntimeSha256=$($runtimePin.sha256)",$definition)
$installer = Join-Path $dist "MabiTools-Channel-Checker-$Version-Setup-x64.exe"
# Hash the finished containers independently. Host checks do not establish guest
# installation results, so the receipt deliberately leaves cleanGuest pending.
$assets = @(foreach ($file in @($portable,$installer)) { $item=Get-Item -LiteralPath $file; [ordered]@{ path=$item.FullName; size=$item.Length; sha256=(Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() } })
[ordered]@{builtAtUtc=[DateTime]::UtcNow.ToString('o'); sourceCommit=$commit; version=$Version; updatesEnabled=$true; updateTransport='native-https'; sourceTag=$sourceTag; tagObject=$tagObject; qtSourceSha256=$sourceHash; runtimeVersion=$runtimePin.version; runtimeMinimumVersion=$runtimePin.minimumVersion; runtimeSha256=$runtimePin.sha256; assets=$assets; hostChecks='passed'; cleanGuest='pending'} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding utf8
Write-Host "Packages: $dist"
