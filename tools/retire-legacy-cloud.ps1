param([switch]$Apply, [string]$PreviewToken = '')
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$running = @(Get-Process -Name SubCue,AutoSubtitleAligner -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) { throw '请先正常关闭全部 SubCue 和旧版应用，保留恢复草稿后再清理。' }
$exe = Join-Path $projectRoot 'out/build/windows-release-cuda/SubCue.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw '请先完成 CUDA 构建。' }
if ($Apply -and $PreviewToken -notmatch '^[0-9a-f]{64}$') { throw '执行清理必须传入本次预览的 -PreviewToken。' }
$output = Join-Path $projectRoot ('.test_tmp/legacy-cloud-' + [guid]::NewGuid().ToString('N') + '.json')
$arguments = @('--preview-legacy-cloud')
if ($Apply) { $arguments = @('--retire-legacy-cloud', $PreviewToken) }
$arguments += @('--legacy-cloud-output', ('"' + $output + '"'))
$qtEntry = Select-String -LiteralPath (Join-Path $projectRoot 'out/build/windows-release-cuda/CMakeCache.txt') -Pattern '^Qt6_DIR:PATH='
if (-not $qtEntry) { throw '构建记录缺少 Qt 运行时位置。' }
$qtRuntime = [IO.Path]::GetFullPath((Join-Path ($qtEntry.Line.Split('=',2)[1]) '../../..'))
$previousPath = $env:PATH
try {
    $env:PATH = (Join-Path $qtRuntime 'bin') + ';' + (Join-Path $projectRoot 'vcpkg_installed/x64-windows/bin') + ';' + $previousPath
    if ($env:CUDA_PATH) { $env:PATH = (Join-Path $env:CUDA_PATH 'bin/x64') + ';' + (Join-Path $env:CUDA_PATH 'bin') + ';' + $env:PATH }
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -Wait
} finally { $env:PATH = $previousPath }
if (-not (Test-Path -LiteralPath $output)) { throw '清理程序未返回完整记录，未宣称成功。' }
$result = Get-Content -LiteralPath $output -Raw | ConvertFrom-Json
foreach ($preview in $result.previews) {
    Write-Output ('配置：' + $preview.path)
    Write-Output ('旧字段：' + ($preview.fields -join '、'))
    Write-Output ('旧连接数：' + $preview.connections + '；旧凭据数：' + @($preview.credentials).Count)
    foreach ($target in $preview.credentials) { Write-Output ('凭据目标：' + $target) }
}
Write-Output ('记录：' + $output)
if ($process.ExitCode -ne 0 -or -not $result.success) { throw ('清理未完成：' + $result.error) }
if ($Apply) { Write-Output '已清除预览确认的旧入口数据；统一账号、模型、密钥和工程保留。' }
else { Write-Output ('预览 Token：' + $result.token); Write-Output '当前仅预览；使用 -Apply -PreviewToken <本次 Token> 执行。' }
