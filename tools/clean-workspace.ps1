param(
    [switch]$Apply,
    [string[]]$Only = @()
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$modelRoot = (Resolve-Path (Join-Path $projectRoot 'models')).Path
$targets = [System.Collections.Generic.List[string]]::new()

function Add-CleanupTarget([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (-not $resolved.StartsWith($projectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理项目目录之外的路径：$resolved"
    }
    if ($resolved.StartsWith($modelRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理模型目录：$resolved"
    }
    $targets.Add($resolved)
}

if ($Only.Count -gt 0) {
    foreach ($item in $Only) {
        $candidate = if ([System.IO.Path]::IsPathRooted($item)) {
            $item
        } else {
            Join-Path $projectRoot $item
        }
        Add-CleanupTarget $candidate
    }
} else {
    Add-CleanupTarget (Join-Path $projectRoot 'build')
    Add-CleanupTarget (Join-Path $projectRoot '.test_tmp')
    Add-CleanupTarget (Join-Path $projectRoot '.venv-ml')
    Add-CleanupTarget (Join-Path $projectRoot 'out\build\_probe')
    Add-CleanupTarget (Join-Path $projectRoot 'out\build\audio-fix')
    Add-CleanupTarget (Join-Path $projectRoot 'out\build\verify-release')
    Add-CleanupTarget (Join-Path $projectRoot 'dist\subcue-0.1.1-editor-fix')
    Add-CleanupTarget (Join-Path $projectRoot '.git\lfs\tmp')

    $buildRoot = Join-Path $projectRoot 'out\build'
    if (Test-Path -LiteralPath $buildRoot) {
        Get-ChildItem -LiteralPath $buildRoot -Directory -Force -ErrorAction SilentlyContinue |
            ForEach-Object {
                Add-CleanupTarget (Join-Path $_.FullName 'package')
            }
    }

    $lfsFiles = & git -C $projectRoot lfs ls-files --all 2>$null
    if (-not $lfsFiles) {
        Add-CleanupTarget (Join-Path $projectRoot '.git\lfs\objects')
    } elseif (Test-Path -LiteralPath (Join-Path $projectRoot '.git\lfs\objects')) {
        Write-Host '[保留] .git\lfs\objects（检测到 Git 历史中的 LFS 引用）'
    }

    Get-ChildItem -LiteralPath (Join-Path $projectRoot '.git\objects') -File -Recurse -Force `
        -Filter 'tmp_obj_*' -ErrorAction SilentlyContinue | ForEach-Object {
            Add-CleanupTarget $_.FullName
        }

    Get-ChildItem -LiteralPath $projectRoot -File -Force -ErrorAction SilentlyContinue | Where-Object {
        $_.Extension -in @('.log', '.err') -or $_.Name -in @('$log', 'dryrun.txt')
    } | ForEach-Object {
        Add-CleanupTarget $_.FullName
    }
}

$bytes = 0
foreach ($target in $targets) {
    if (Test-Path -LiteralPath $target -PathType Container) {
        $bytes += (Get-ChildItem -LiteralPath $target -File -Recurse -Force `
            -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
    } else {
        $bytes += (Get-Item -LiteralPath $target -Force).Length
    }
    $label = '[预览]'
    if ($Apply) {
        $label = '[清理]'
    }
    Write-Host ($label + " $target")
}

Write-Host ('预计释放：{0:N2} GiB' -f ($bytes / 1GB))
if (-not $Apply) {
    Write-Host '当前为预览模式；确认后使用 -Apply 执行。'
    exit 0
}

if (Get-Process -Name git, git-lfs -ErrorAction SilentlyContinue) {
    throw '检测到 Git 或 Git LFS 进程，已停止清理。'
}

foreach ($target in $targets) {
    Remove-Item -LiteralPath $target -Recurse -Force
}
Write-Host '保守清理完成；models 目录未被操作。'
