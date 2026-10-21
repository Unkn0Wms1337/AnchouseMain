param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$ProtoDir,
    [Parameter(Mandatory = $true)][string]$Protoc,
    [Parameter(Mandatory = $true)][string]$ProtobufSrc,
    [string]$Sha = "a742e214618bfb0d4668e1b7a022b8db4d0cbe08"
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$files = @(
    "base_gcmessages.proto",
    "gcsdk_gcmessages.proto",
    "econ_gcmessages.proto",
    "steammessages.proto",
    "steammessages_steamlearn.steamworkssdk.proto",
    "steammessages_unified_base.steamworkssdk.proto",
    "valveextensions.proto",
    "econ_shared_enums.proto",
    "netmessages.proto",
    "networkbasetypes.proto",
    "source2_steam_stats.proto",
    "dota_usermessages.proto",
    "dota_shared_enums.proto",
    "dota_commonmessages.proto",
    "usermessages.proto",
    "network_connection.proto",
    "event_gcmessages_common.proto",
    "events.proto"
)

$baseUrl = "https://raw.githubusercontent.com/SteamTracking/GameTracking-Dota2/$Sha/Protobufs"
$utf8 = New-Object System.Text.UTF8Encoding($false)

New-Item -ItemType Directory -Force -Path $ProtoDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($file in $files) {
    $path = Join-Path $ProtoDir $file
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "  downloading $file"
        try {
            Invoke-WebRequest -Uri "$baseUrl/$file" -OutFile $path -UseBasicParsing
        }
        catch {
            throw "Failed to download ${file}: $($_.Exception.Message)"
        }
    }

    $text = [IO.File]::ReadAllText($path)
    if ($text -notmatch '(?m)^\s*syntax\s*=') {
        $text = "syntax = `"proto2`";`r`n" + $text
    }

    # Valve's protoc has built-in options that the public protoc does not know.
    # They only affect Valve's generated code, so drop them before generation.
    $text = [regex]::Replace($text, '(?m)^[ \t]*option[ \t]+additional_includes[ \t]*=[^;]*;[ \t]*\r?\n?', '')
    $text = [regex]::Replace($text, '\s*,\s*(boxed_type|synthetic_default)\s*=\s*"[^"]*"', '')
    $text = [regex]::Replace($text, '(boxed_type|synthetic_default)\s*=\s*"[^"]*"\s*,\s*', '')
    $text = [regex]::Replace($text, '\s*\[\s*(boxed_type|synthetic_default)\s*=\s*"[^"]*"\s*\]', '')
    $text = [regex]::Replace($text, '\[\s*\]', '')
    $text = [regex]::Replace($text, '\[\s*,', '[')
    $text = [regex]::Replace($text, ',\s*\]', ']')

    [IO.File]::WriteAllText($path, $text, $utf8)
}

Get-ChildItem -LiteralPath $OutDir -Filter "*.pb.cc" -File | Remove-Item -Force
Get-ChildItem -LiteralPath $OutDir -Filter "*.pb.h" -File | Remove-Item -Force

$arguments = @(
    "-I$ProtoDir",
    "-I$ProtobufSrc",
    "--cpp_out=$OutDir"
) + $files

$oldErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$output = & $Protoc @arguments 2>&1
$protocExit = $LASTEXITCODE
$ErrorActionPreference = $oldErrorAction
if ($protocExit -ne 0) {
    $output | ForEach-Object { Write-Host $_ }
    throw "protoc failed with exit code $protocExit"
}

if (-not (Test-Path -LiteralPath (Join-Path $OutDir "base_gcmessages.pb.cc"))) {
    throw "protoc did not generate base_gcmessages.pb.cc"
}

Write-Host "  generated $($files.Count) protobuf files"
exit 0
