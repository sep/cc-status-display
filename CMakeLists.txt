cmake_minimum_required(VERSION 3.16)

# ----------------------------------------------------------------------------
# Firmware version — derived from version.txt with a 6-char commit SHA
# appended for non-release builds.
#
# Release builds (CI workflow .github/workflows/release.yml, triggered by
# tag pushes) export CC_RELEASE_BUILD=1 in the environment. Those builds
# get the bare version.txt string, e.g. "v0.1.0".
#
# Local / dev / canary / smoke builds don't set CC_RELEASE_BUILD and get
# "<version>+<sha6>", e.g. "v0.1.0+a1b2c3", so the binary always carries
# the exact commit it was built from. (If the working tree changes after
# CMake configures, you'll need `idf.py reconfigure` to pick up the new
# SHA — IDF doesn't re-run this block on every build.)
#
# Bump version.txt manually when starting work toward a new release; tag
# the commit when you ship it. CI verifies version.txt == tag at release
# time and fails the build if they've drifted.
# ----------------------------------------------------------------------------
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" _CC_VERSION_BASE)
string(STRIP "${_CC_VERSION_BASE}" _CC_VERSION_BASE)

if(DEFINED ENV{CC_RELEASE_BUILD} AND "$ENV{CC_RELEASE_BUILD}" STREQUAL "1")
  set(PROJECT_VER "${_CC_VERSION_BASE}")
else()
  execute_process(
    COMMAND git rev-parse --short=6 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE _CC_VERSION_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _CC_GIT_RESULT
    ERROR_QUIET
  )
  if(_CC_GIT_RESULT EQUAL 0 AND _CC_VERSION_SHA)
    set(PROJECT_VER "${_CC_VERSION_BASE}+${_CC_VERSION_SHA}")
  else()
    set(PROJECT_VER "${_CC_VERSION_BASE}+nosha")
  endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(led_panel_hello)
