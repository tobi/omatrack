#!/usr/bin/env bash

# Windows ships these stable system interfaces. All other imported DLLs must
# be present in the release tree, including compiler runtimes and media codecs.
omatrack_windows_system_library() {
  local name=${1,,}
  case "$name" in
    api-ms-win-*.dll | ext-ms-win-*.dll | advapi32.dll | authz.dll | \
      avicap32.dll | avrt.dll | bcrypt.dll | bcryptprimitives.dll | \
      cfgmgr32.dll | comdlg32.dll | crypt32.dll | d3d11.dll | d3d12.dll | \
      d3d9.dll | dnsapi.dll | dwmapi.dll | dwrite.dll | dxgi.dll | \
      gdi32.dll | gdiplus.dll | imm32.dll | iphlpapi.dll | kernel32.dll | \
      mpr.dll | msimg32.dll | ncrypt.dll | netapi32.dll | ntdll.dll | \
      ole32.dll | oleaut32.dll | opengl32.dll | rpcrt4.dll | secur32.dll | \
      setupapi.dll | shcore.dll | shell32.dll | shlwapi.dll | user32.dll | \
      userenv.dll | usp10.dll | uxtheme.dll | version.dll | winhttp.dll | \
      winmm.dll | ws2_32.dll | wsock32.dll | wtsapi32.dll)
      return 0
      ;;
  esac
  return 1
}
