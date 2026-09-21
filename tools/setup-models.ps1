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
    $config = Join-Path $Directory 'config.json'
    $safetensors = Join-Path $Directory 'model.safetensors'
    $pt = Join-Path $Directory 'model.pt'
    return (Test-Path -LiteralPath $config) -and ((Test-Path -LiteralPath $safetensors) -or (Test-Path -LiteralPath $pt))
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
    if ((Test-Path -LiteralPath $destination) -and -not $Force) {
        throw "目标已存在但权重不完整：$destination。确认后使用 -Force。"
    }

    $staging = Join-Path $Root (Join-Path '.download' $entry.folder)
    if (Test-Path -LiteralPath $staging) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $staging | Out-Null
    Write-Host "[下载] $($entry.repo) -> $staging"
    & uvx --from modelscope modelscope download --model $entry.repo --local_dir $staging
    if ($LASTEXITCODE -ne 0) {
        throw "modelscope 下载失败：$($entry.repo)"
    }

    foreach ($file in $entry.files) {
        $path = Join-Path $staging $file.path
        if (-not (Test-Path -LiteralPath $path)) {
            throw "下载缺少文件：$($file.path)"
        }
        $actual = Get-Sha256 $path
        if ($actual -ne $file.sha256) {
            throw "SHA-256 不匹配：$($file.path)`n期望 $($file.sha256)`n实际 $actual"
        }
    }

    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    Move-Item -LiteralPath $staging -Destination $destination
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
