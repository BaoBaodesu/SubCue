param(
    [string]$Python = "C:\Users\baobao\AppData\Roaming\uv\python\cpython-3.11.16-windows-x86_64-none\python.exe"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$environment = Join-Path $root ".venv-inference"
$cache = Join-Path $root ".uv-cache"
$packages = Join-Path $root "out\inference-packages"
$torchWheel = Join-Path $packages "torch-2.6.0+cu124-cp311-cp311-win_amd64.whl"
$env:UV_CACHE_DIR = $cache

uv venv $environment --python $Python
New-Item -ItemType Directory -Force -Path $packages | Out-Null
curl.exe -L --fail --retry 20 --retry-all-errors --retry-delay 2 -C - `
    -o $torchWheel `
    "https://download-r2.pytorch.org/whl/cu124/torch-2.6.0%2Bcu124-cp311-cp311-win_amd64.whl"
if ((Get-FileHash -Algorithm SHA256 $torchWheel).Hash -ne
    "6A1FB2714E9323F11EDB6E8ABF7AAD5F79E45AD25C081CDE87681A18D99C29EB") {
    throw "CUDA PyTorch wheel 校验失败"
}
uv pip install --python (Join-Path $environment "Scripts\python.exe") $torchWheel
uv pip install --python (Join-Path $environment "Scripts\python.exe") "qwen-asr==0.0.6"

& (Join-Path $environment "Scripts\python.exe") -c @'
import torch
assert torch.cuda.is_available(), "CUDA 不可用"
x = torch.arange(1024, device="cuda")
print(f"torch={torch.__version__} cuda={torch.version.cuda} device={torch.cuda.get_device_name(0)} sum={x.sum().item()}")
'@
