param(
    [switch]$Apply,
    [string[]]$Only = @(),
    [string]$KeepBuild = 'windows-release-cuda'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$protectedRoots = [System.Collections.Generic.List[string]]::new()
foreach ($candidate in @(
        (Join-Path $projectRoot 'models'),
        $env:SUBCUE_MODELS_ROOT,
        'E:\AIModels\ASR\models'
    )) {
    if ($candidate -and (Test-Path -LiteralPath $candidate)) {
        $protectedRoots.Add((Resolve-Path -LiteralPath $candidate).Path)
    }
}
$targets = [System.Collections.Generic.List[string]]::new()

function Add-CleanupTarget([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if (-not $resolved.StartsWith($projectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "拒绝清理项目目录之外的路径：$resolved"
    }
    foreach ($protected in $protectedRoots) {
        if ($resolved.StartsWith($protected, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "拒绝清理模型目录：$resolved"
        }
    }
    $keepPath = Join-Path $projectRoot "out\build\$KeepBuild"
    if ((Test-Path -LiteralPath $keepPath)) {
        $keepResolved = (Resolve-Path -LiteralPath $keepPath).Path
        if ($resolved.Equals($keepResolved, [System.StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "[保留] 当前构建 $resolved"
            return
        }
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
    Add-CleanupTarget (Join-Path $projectRoot '.test_appdata')
    Add-CleanupTarget (Join-Path $projectRoot '.venv-ml')
    Add-CleanupTarget (Join-Path $projectRoot 'out\build\_probe')
    Add-CleanupTarget (Join-Path $projectRoot 'out\build\audio-fix')
    Add-CleanupTarget (Join-Path $projectRoot 'out\build\verify-release')
    Add-CleanupTarget (Join-Path $projectRoot 'dist\subcue-0.1.1-editor-fix')
    Add-CleanupTarget (Join-Path $projectRoot '.git\lfs\tmp')

    $outRoot = Join-Path $projectRoot 'out'
    if (Test-Path -LiteralPath $outRoot) {
        Get-ChildItem -LiteralPath $outRoot -Force -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -like 'phase*' -or $_.Name -like 'tmp*' -or $_.Name -eq 'inference-packages'
        } | ForEach-Object { Add-CleanupTarget $_.FullName }
        Get-ChildItem -LiteralPath $outRoot -File -Force -ErrorAction SilentlyContinue | Where-Object {
            $_.Extension -in @('.txt', '.log', '.json', '.csv', '.docx', '.zip', '.html')
        } | ForEach-Object { Add-CleanupTarget $_.FullName }
    }

    $buildRoot = Join-Path $projectRoot 'out\build'
    if (Test-Path -LiteralPath $buildRoot) {
        Get-ChildItem -LiteralPath $buildRoot -Directory -Force -ErrorAction SilentlyContinue |
            ForEach-Object {
                Add-CleanupTarget (Join-Path $_.FullName 'package')
                if ($_.Name -ne $KeepBuild) {
                    Add-CleanupTarget $_.FullName
                } else {
                    Write-Host "[保留] 当前构建 $($_.FullName)"
                }
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
        $_.Extension -in @('.log', '.err') -or $_.Name -in @('$log', 'dryrun.txt', 'main.obj')
    } | ForEach-Object {
        Add-CleanupTarget $_.FullName
    }
}

$unique = [System.Collections.Generic.List[string]]::new()
foreach ($target in $targets) {
    if ($unique -notcontains $target) { $unique.Add($target) }
}
$targets = $unique

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
Write-Host ("保留当前构建：$KeepBuild")
if (-not $Apply) {
    Write-Host '当前为预览模式；确认后使用 -Apply 执行。'
    exit 0
}

if (Get-Process -Name git, git-lfs -ErrorAction SilentlyContinue) {
    throw '检测到 Git 或 Git LFS 进程，已停止清理。'
}

foreach ($target in $targets) {
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
    }
}
Write-Host '保守清理完成；模型目录未被操作。'
