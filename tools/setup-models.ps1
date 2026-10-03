param(
    [string]$Root = $env:SUBCUE_MODELS_ROOT,
    [string[]]$Models = @(),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $PSCommandPath
$manifestPath = Join-Path $scriptDir 'models-manifest.json'
if (-not $Root) {
    if (Test-Path -LiteralPath 'E:\AIModels\ASR\models') {
        $Root = 'E:\AIModels\ASR\models'
    } else {
        $Root = Join-Path (Split-Path -Parent $scriptDir) 'models'
    }
}
$Root = [System.IO.Path]::GetFullPath($Root)
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "缺少模型清单：$manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $Root | Out-Null
$selected = if ($Models.Count -gt 0) { $Models } else { @($manifest.models.id) }

function Test-ModelReady([string]$Directory) {
    $hasConfig = (Test-Path -LiteralPath (Join-Path $Directory 'config.json')) -or
        (Test-Path -LiteralPath (Join-Path $Directory 'configuration.json')) -or
        (Test-Path -LiteralPath (Join-Path $Directory 'config.yaml'))
    $hasWeights = (Test-Path -LiteralPath (Join-Path $Directory 'model.safetensors')) -or
        (Test-Path -LiteralPath (Join-Path $Directory 'model.pt'))
    return $hasConfig -and $hasWeights
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

foreach ($id in $selected) {
    $entry = $manifest.models | Where-Object { $_.id -eq $id } | Select-Object -First 1
    if (-not $entry) {
        throw "清单中没有模型：$id"
    }
    $destination = Join-Path $Root $entry.folder
    if ((Test-ModelReady $destination) -and -not $Force) {
        Write-Host "[跳过] $($entry.id) 已完整，使用 -Force 才会覆盖"
        continue
    }
    $resume = (Test-Path -LiteralPath $destination) -and -not $Force

    $staging = Join-Path $Root (Join-Path '.download' $entry.folder)
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $staging) {
            $staging = Join-Path $Root (Join-Path '.download' ($entry.folder + '-' + [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')))
        }
    }
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    Write-Host "[下载] $($entry.repo) -> $staging"
    & uvx --from modelscope modelscope download --model $entry.repo --local_dir $staging
    if ($LASTEXITCODE -ne 0) {
        throw "modelscope 下载失败：$($entry.repo)"
    }

    foreach ($file in @($entry.files)) {
        $relative = [string]$file.path
        $expected = [string]$file.sha256
        $path = Join-Path $staging $relative
        if (-not (Test-Path -LiteralPath $path)) {
            $path = Join-Path $destination $relative
        }
        if (-not (Test-Path -LiteralPath $path)) {
            throw "下载缺少文件：$relative"
        }
        $actual = Get-Sha256 $path
        if ($actual -ne $expected) {
            throw "SHA-256 不匹配：$relative 期望 $expected 实际 $actual"
        }
    }

    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if ($resume) {
        Get-ChildItem -LiteralPath $staging -Recurse -File | ForEach-Object {
            $relative = $_.FullName.Substring($staging.Length).TrimStart('\')
            $target = Join-Path $destination $relative
            if (Test-Path -LiteralPath $target) { return }
            $targetParent = Split-Path -Parent $target
            if (-not (Test-Path -LiteralPath $targetParent)) {
                New-Item -ItemType Directory -Force -Path $targetParent | Out-Null
            }
            Copy-Item -LiteralPath $_.FullName -Destination $target
        }
        Remove-Item -LiteralPath $staging -Recurse -Force
        $incomplete = Join-Path $destination 'model.pt.incomplete'
        if ((Test-Path -LiteralPath (Join-Path $destination 'model.pt')) -and (Test-Path -LiteralPath $incomplete)) {
            Remove-Item -LiteralPath $incomplete -Force
        }
    } else {
        if (Test-Path -LiteralPath $destination) {
            Remove-Item -LiteralPath $destination -Recurse -Force
        }
        Move-Item -LiteralPath $staging -Destination $destination
    }
    if (-not (Test-ModelReady $destination)) {
        throw "下载后权重仍不完整：$destination"
    }
    Write-Host "[完成] $($entry.id) -> $destination"
}

$downloadRoot = Join-Path $Root '.download'
if (Test-Path -LiteralPath $downloadRoot) {
    $leftover = Get-ChildItem -LiteralPath $downloadRoot -Force -ErrorAction SilentlyContinue
    if (-not $leftover) {
        Remove-Item -LiteralPath $downloadRoot -Force -ErrorAction SilentlyContinue
    }
}
Write-Host "模型根目录：$Root"
