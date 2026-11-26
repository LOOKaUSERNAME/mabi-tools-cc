#requires -Version 7.0
# Convert a production build receipt into native feed metadata after rehashing
# its local packages. This writes a file only: derived release URLs are expected
# destinations, not evidence of upload. Publication must verify their bytes.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$BuildResult,[Parameter(Mandatory)][string]$Output)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$result=Get-Content -LiteralPath $BuildResult -Raw | ConvertFrom-Json
if(!$result.updatesEnabled -or $result.updateTransport -cne 'native-https'){
    throw 'A native HTTPS production build is required.'
}
$version=$result.version
if($version -notmatch '^\d{4}\.[1-9]\d?\.\d{2}(?:\.[1-9]\d*)?$' -or
   $result.sourceTag -cne "v$version" -or $result.sourceCommit -cnotmatch '^[0-9a-f]{40}$' -or
   $result.tagObject -cnotmatch '^[0-9a-f]{40}$'){throw 'Invalid source provenance'}
$hub='https://github.com/LOOKaUSERNAME/mabi-tools-releases/releases'
$tag="cc-v$version"
# Require exactly one asset per role, then rehash the files named by the receipt.
# A stale receipt or renamed/replaced package must not produce trusted feed entries.
$assets=@(foreach($role in 'installer','portable') {
    $suffix=if($role -eq 'installer'){'Setup-x64.exe'}else{'win-x64.zip'}
    $name="MabiTools-Channel-Checker-$version-$suffix"
    $matches=@($result.assets | Where-Object {(Split-Path $_.path -Leaf) -ceq $name})
    if($matches.Count -ne 1){throw "Missing or duplicate asset: $name"}
    $asset=$matches[0];$file=Get-Item -LiteralPath $asset.path
    $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if($file.Length -le 0 -or $file.Length -gt 256MB -or $file.Length -ne $asset.size -or $hash -cne $asset.sha256){throw "Asset verification failed: $name"}
    [ordered]@{role=$role;name=$name;size=$file.Length;sha256=$hash;downloadUrl="$hub/download/$tag/$name"}
})
[ordered]@{schemaVersion=1;productId='cc';channel='stable';feedId='cc-native-stable-v1';architecture='windows-x64';
    installerAppId='{B81A179F-E1D0-471B-B274-B521D71C8088}';release=[ordered]@{
        version=$version;centralTag=$tag;releaseUrl="$hub/tag/$tag";assets=$assets;
        source=[ordered]@{repository='LOOKaUSERNAME/mabi-tools-cc';tag=$result.sourceTag;commit=$result.sourceCommit;tagObject=$result.tagObject}
    }} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8NoBOM
