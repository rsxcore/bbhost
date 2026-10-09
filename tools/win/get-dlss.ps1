# Fetches NVIDIA's DLSS library (nvngx_dlss.dll) from NVIDIA's own DLSS SDK
# repository (https://github.com/NVIDIA/DLSS) into the folder this script is
# in, beside bbhost.exe. bbhost cannot ship it: the SDK's license does not let
# it be distributed under the GPL. The file is pinned to one SDK release and
# its SHA-256, and kept only when it matches. By downloading it you accept
# NVIDIA's license: https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt
$ErrorActionPreference = 'Stop'
$version = 'v310.9.1'
$sha256 = '3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983'
$url = "https://raw.githubusercontent.com/NVIDIA/DLSS/$version/lib/Windows_x86_64/rel/nvngx_dlss.dll"
$dest = Join-Path $PSScriptRoot 'nvngx_dlss.dll'

if ((Test-Path $dest) -and ((Get-FileHash $dest -Algorithm SHA256).Hash -eq $sha256)) {
    Write-Host "nvngx_dlss.dll $version is already here."
    exit 0
}
Write-Host "NVIDIA DLSS $version, from $url"
Write-Host "Its license: https://github.com/NVIDIA/DLSS/blob/$version/LICENSE.txt"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$tmp = "$dest.download"
try {
    $ProgressPreference = 'SilentlyContinue'  # Windows PowerShell's progress bar makes the download crawl
    Invoke-WebRequest -Uri $url -OutFile $tmp -UseBasicParsing
    $got = (Get-FileHash $tmp -Algorithm SHA256).Hash
    if ($got -ne $sha256) {
        throw "the download's SHA-256 is $got, not ${sha256}: not kept"
    }
    Move-Item -Force $tmp $dest
    Write-Host "Saved $dest. Turn DLSS on in F10 > Graphics > DLSS."
} finally {
    if (Test-Path $tmp) { Remove-Item -Force $tmp }
}
