# Generates build_info.hpp for `cx --version`. Run in script mode
# (cmake -P) by the cx_build_info target on every build, so the commit
# and its dirty state are current even when CMake does not reconfigure.
#
# Inputs (-D):
#   SRC_DIR  source tree, for git
#   GEN_DIR  generated-file directory; holds build_flags.cmake, written
#            at configure time with CX_COMPILER / CX_BUILD_TYPE /
#            CX_BUILD_FLAGS, and receives build_info.hpp

include(${GEN_DIR}/build_flags.cmake)

execute_process(
    COMMAND git describe --always --dirty --abbrev=12
    WORKING_DIRECTORY ${SRC_DIR}
    OUTPUT_VARIABLE CX_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE git_result)
if(NOT git_result EQUAL 0 OR CX_GIT_COMMIT STREQUAL "")
    # Not a git checkout (e.g. a source tarball) or git is missing.
    set(CX_GIT_COMMIT "unknown commit")
endif()

# A date rather than a time stamp, so the header (and the cx link) only
# change once a day without a commit. Honours SOURCE_DATE_EPOCH for
# reproducible builds.
string(TIMESTAMP CX_BUILD_DATE "%Y-%m-%d" UTC)

# configure_file leaves the output untouched when the content is unchanged.
configure_file(${CMAKE_CURRENT_LIST_DIR}/build_info.hpp.in ${GEN_DIR}/build_info.hpp @ONLY)
