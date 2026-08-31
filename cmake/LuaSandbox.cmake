if(TARGET lua_sandbox)
  return()
endif()

enable_language(C)

# Lua 5.4 + sol2 for the USB rename sandbox in src/app.
#
# Lua is compiled without io/os/package/debug so those libraries cannot be
# opened even if a script asked. sol2 is header-only and used only here.
# Neither upstream archive is added as a CMake subproject: lua has no
# CMakeLists, and sol2's would try to find a system Lua.
include(FetchContent)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

set(LUA_VERSION 5.4.7)
FetchContent_Declare(
  lua_src
  URL https://www.lua.org/ftp/lua-${LUA_VERSION}.tar.gz
  URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(
  sol2
  # d805d02: optional<T&>::emplace fix for GCC 15 / Clang 19
  GIT_REPOSITORY https://github.com/ThePhD/sol2.git
  GIT_TAG d805d027e0a0a7222e936926139f06e23828ce9f)
FetchContent_GetProperties(lua_src)
if(NOT lua_src_POPULATED)
  FetchContent_Populate(lua_src)
endif()
FetchContent_GetProperties(sol2)
if(NOT sol2_POPULATED)
  FetchContent_Populate(sol2)
endif()

set(LUA_CORE
  ${lua_src_SOURCE_DIR}/src/lapi.c
  ${lua_src_SOURCE_DIR}/src/lcode.c
  ${lua_src_SOURCE_DIR}/src/lctype.c
  ${lua_src_SOURCE_DIR}/src/ldebug.c
  ${lua_src_SOURCE_DIR}/src/ldo.c
  ${lua_src_SOURCE_DIR}/src/ldump.c
  ${lua_src_SOURCE_DIR}/src/lfunc.c
  ${lua_src_SOURCE_DIR}/src/lgc.c
  ${lua_src_SOURCE_DIR}/src/llex.c
  ${lua_src_SOURCE_DIR}/src/lmem.c
  ${lua_src_SOURCE_DIR}/src/lobject.c
  ${lua_src_SOURCE_DIR}/src/lopcodes.c
  ${lua_src_SOURCE_DIR}/src/lparser.c
  ${lua_src_SOURCE_DIR}/src/lstate.c
  ${lua_src_SOURCE_DIR}/src/lstring.c
  ${lua_src_SOURCE_DIR}/src/ltable.c
  ${lua_src_SOURCE_DIR}/src/ltm.c
  ${lua_src_SOURCE_DIR}/src/lundump.c
  ${lua_src_SOURCE_DIR}/src/lvm.c
  ${lua_src_SOURCE_DIR}/src/lzio.c)
set(LUA_LIBS
  ${lua_src_SOURCE_DIR}/src/lauxlib.c
  ${lua_src_SOURCE_DIR}/src/lbaselib.c
  ${lua_src_SOURCE_DIR}/src/lmathlib.c
  ${lua_src_SOURCE_DIR}/src/lstrlib.c
  ${lua_src_SOURCE_DIR}/src/ltablib.c
  ${lua_src_SOURCE_DIR}/src/lutf8lib.c
  # os.time/os.date/os.clock for plugins; PluginHost strips execute, exit,
  # getenv, remove, rename, setlocale and tmpname before any script runs.
  ${lua_src_SOURCE_DIR}/src/loslib.c)

add_library(lua_sandbox STATIC ${LUA_CORE} ${LUA_LIBS})
set_target_properties(lua_sandbox PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
  # Keep the embedded 5.4 ABI private. libmpv can use LuaJIT 5.1; exporting
  # these same lua_* symbols from the executable interposes its interpreter
  # and crashes mpv's built-in script threads at player initialization.
  C_VISIBILITY_PRESET hidden)
target_include_directories(lua_sandbox PUBLIC ${lua_src_SOURCE_DIR}/src)
# The interpreter, compiler, io, os, package, coroutine, and debug libraries
# stay out of this target on purpose.
if(NOT WIN32)
  target_compile_definitions(lua_sandbox PRIVATE LUA_USE_POSIX)
  target_link_libraries(lua_sandbox PUBLIC m)
endif()

add_library(sol2_headers INTERFACE)
target_include_directories(sol2_headers INTERFACE ${sol2_SOURCE_DIR}/include)
target_link_libraries(sol2_headers INTERFACE lua_sandbox)
