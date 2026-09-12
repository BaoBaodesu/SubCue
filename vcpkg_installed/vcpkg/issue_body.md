Package: ffmpeg-bin2c:x64-windows@9.0.1

**Host Environment**

- Host: x64-windows
- Compiler: MSVC 19.51.36243.0
- CMake Version: 4.4.2
-    vcpkg-tool version: 2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8
    vcpkg-scripts version: unknown

**To Reproduce**

`vcpkg install ffmpeg[avcodec,avfilter,avformat,swresample,swscale]:x64-windows --clean-after-build`

**Failure logs**

```
Downloading https://github.com/ffmpeg/ffmpeg/archive/n9.0.1.tar.gz -> ffmpeg-ffmpeg-n9.0.1.tar.gz
error: curl operation failed with error code 35 (SSL connect error).
error: Not a transient network error, won't retry download from https://github.com/ffmpeg/ffmpeg/archive/n9.0.1.tar.gz
note: If you are using a proxy, please ensure your proxy settings are correct.
Possible causes are:
1. You are actually using an HTTP proxy, but setting HTTPS_PROXY variable to `https://address:port`.
This is not correct, because `https://` prefix claims the proxy is an HTTPS proxy, while your proxy (v2ray, shadowsocksr, etc...) is an HTTP proxy.
Try setting `http://address:port` to both HTTP_PROXY and HTTPS_PROXY instead.
2. If you are using Windows, vcpkg will automatically use your Windows IE Proxy Settings set by your proxy software. See: https://github.com/microsoft/vcpkg-tool/pull/77
The value set by your proxy might be wrong, or have same `https://` prefix issue.
3. Your proxy's remote server is out of service.
If you believe this is not a temporary download server failure and vcpkg needs to be changed to download this file from a different location, please submit an issue to https://github.com/Microsoft/vcpkg/issues
CMake Error at scripts/cmake/vcpkg_download_distfile.cmake:134 (message):
  Download failed, halting portfile.
Call Stack (most recent call first):
  scripts/cmake/vcpkg_from_github.cmake:120 (z_vcpkg_download_distfile)
  ports/ffmpeg-bin2c/portfile.cmake:4 (vcpkg_from_github)
  scripts/ports.cmake:209 (include)



```

