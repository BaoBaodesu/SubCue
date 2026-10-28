param([switch]$Apply, [switch]$TestCredentials)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name SubCue -ErrorAction SilentlyContinue) { throw '请先关闭全部 SubCue 实例，保留工程恢复草稿后再执行。' }
if ($TestCredentials -and (Get-Process -Name 'SubCue*Tests' -ErrorAction SilentlyContinue)) { throw '请先等待 SubCue 测试退出，避免删除使用中的测试凭据。' }
# 只枚举目标名称，不读取或输出 CredentialBlob。
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class SubCueCredentialReset {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    public struct Header { public uint Flags; public uint Type; public IntPtr Target; }
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool CredEnumerateW(string filter, uint flags, out uint count, out IntPtr credentials);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern bool CredDeleteW(string target, uint type, uint flags);
    [DllImport("advapi32.dll")] public static extern void CredFree(IntPtr buffer);
}
'@
$count = [uint32]0
$buffer = [IntPtr]::Zero
$filter = if ($TestCredentials) { 'SubCue/Test/*' } else { 'SubCue/*' }
if (-not [SubCueCredentialReset]::CredEnumerateW($filter, 0, [ref]$count, [ref]$buffer)) {
    if ([Runtime.InteropServices.Marshal]::GetLastWin32Error() -eq 1168) { Write-Output 'SubCue 凭据数量：0'; exit 0 }
    throw '无法枚举 SubCue 凭据；未执行删除。'
}
try {
    for ($i = 0; $i -lt $count; ++$i) {
        $pointer = [Runtime.InteropServices.Marshal]::ReadIntPtr($buffer, $i * [IntPtr]::Size)
        $header = [Runtime.InteropServices.Marshal]::PtrToStructure($pointer, [type][SubCueCredentialReset+Header])
        $target = [Runtime.InteropServices.Marshal]::PtrToStringUni($header.Target)
        if (-not $target.StartsWith('SubCue/', [StringComparison]::Ordinal) -or $header.Type -ne 1) { continue }
        # 测试模式只清理按临时文件路径哈希隔离的命名空间，不接受正式账号目标。
        if ($TestCredentials -and $target -cnotmatch '^SubCue/Test/[0-9a-f]{16}/SubCue/(Cloud|ASR|AIReview|AI|VoiceStudio)/') { continue }
        if ($Apply) {
            if (-not [SubCueCredentialReset]::CredDeleteW($target, $header.Type, 0)) { throw "凭据删除失败：$target" }
            Write-Output "已删除：$target"
        } else { Write-Output "预览：$target" }
    }
} finally { [SubCueCredentialReset]::CredFree($buffer) }
