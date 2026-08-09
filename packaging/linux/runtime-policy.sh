#!/usr/bin/env bash

# These libraries form the Linux kernel, libc, display-server, or graphics-
# driver ABI boundary. Loading the target system's copy is more portable than
# shipping a build-runner copy. Everything else in the runtime dependency
# closure is bundled into the AppImage.
omatrack_linux_system_library() {
  local name=${1##*/}
  case "$name" in
    ld-linux-*.so.* | libc.so.* | libdl.so.* | libm.so.* | libmvec.so.* | \
      libpthread.so.* | libresolv.so.* | librt.so.* | libanl.so.* | \
      libnss_*.so.* | libutil.so.* | libBrokenLocale.so.* | \
      libthread_db.so.* | libEGL.so.* | libGL.so.* | libGLX.so.* | \
      libGLdispatch.so.* | libOpenGL.so.* | libdrm.so.* | libgbm.so.* | \
      libX11.so.* | libX11-xcb.so.* | libXau.so.* | libXdmcp.so.* | \
      libxcb.so.* | libwayland-client.so.*)
      return 0
      ;;
  esac
  return 1
}
