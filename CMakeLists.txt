# CPU-Power — a small GUI for CPU frequency scaling and power settings on Linux.
#
# Two installed binaries, deliberately separated by privilege, plus the rule that connects them:
#
#   cpu-power           the GUI. Never root, never setuid.
#   cpu-power-helper    the ONLY thing that runs as root. Plain C, libc only.
#   <action>.policy     the polkit action saying who may run the helper. Part of the security
#                       boundary, not configuration.
#
# and the never-installed test drivers, which is where everything else lives:
#
#   cpu-power-cli           headless driver for the helper. Used by both mandatory gates.
#   cpu-power-helper-test   the same helper source with sysroot support compiled IN and the
#                           privileged paths compiled OUT. For the fixtures.
#   coretest, uirender      the offline model and layout audits.
#   fdprobe                 measures pkexec's descriptor and process shape.
#
# There is no password prompt binary. There used to be; it was deleted rather than hardened, which
# is why this project now handles no secret at all.

cmake_minimum_required(VERSION 3.16)
project(cpu-power VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

# ---------------------------------------------------------------------------------------------
# Hardening. Applied to everything, not just the privileged helper: the GUI parses a config file
# and hands an argv to a setuid-root binary, so there is no target here that does not want these.
# scripts/sec-gate.sh asserts that the RESULTING BINARIES actually have these properties, because
# a flag that is silently dropped by a toolchain is a flag that is not doing anything.
# ---------------------------------------------------------------------------------------------
include(CheckCCompilerFlag)

set(CPUPOWER_HARDEN_FLAGS "")
foreach(_flag
        -fstack-protector-strong
        -fstack-clash-protection
        -fcf-protection=full)
    string(MAKE_C_IDENTIFIER "HAVE${_flag}" _var)
    check_c_compiler_flag("${_flag}" ${_var})
    if(${_var})
        list(APPEND CPUPOWER_HARDEN_FLAGS "${_flag}")
    endif()
endforeach()

# _FORTIFY_SOURCE needs optimisation to do anything, and warns noisily without it.
add_compile_options(
    $<$<NOT:$<CONFIG:Debug>>:-D_FORTIFY_SOURCE=3>
    ${CPUPOWER_HARDEN_FLAGS}
)
add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_LINK_WHAT_YOU_USE ON)

# PIE. Checked rather than assumed, since a toolchain may have it off by default.
include(CheckPIESupported)
check_pie_supported()

# ---------------------------------------------------------------------------------------------
# The model. No X11, no Cairo, no privilege, and it cannot write anything. Everything else is
# built on top of it, and tools/coretest exercises it against captured sysfs trees.
# ---------------------------------------------------------------------------------------------
add_library(cpupower-core STATIC
    src/core/sysfs.cpp
    src/core/topology.cpp
    src/core/backend.cpp
    src/core/plan.cpp
    src/core/profile.cpp
    src/core/idle.cpp
)
target_include_directories(cpupower-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(cpupower-core PRIVATE -Wall -Wextra -Werror)

# ---------------------------------------------------------------------------------------------
# Offline gates.
# ---------------------------------------------------------------------------------------------
add_executable(coretest tools/coretest.cpp)
target_link_libraries(coretest PRIVATE cpupower-core)
target_compile_options(coretest PRIVATE -Wall -Wextra -Werror)

enable_testing()
# coretest resolves tests/fixtures and tests/golden relative to the working directory, so it is
# run from the source tree rather than from the build directory.
add_test(NAME coretest
         COMMAND coretest
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------------------------
# Where things get installed. The HELPER PATH IS BAKED INTO THE GUI AND THE CLI at build time and
# is never derived at runtime -- not from argv[0], not from /proc/self/exe, not from $PATH, not
# from any environment variable. That is the single difference between a tool and a
# privilege-escalation vector, and scripts/sec-gate.sh asserts it.
# ---------------------------------------------------------------------------------------------
set(CPUPOWER_LIBEXECDIR "${CMAKE_INSTALL_FULL_LIBEXECDIR}/cpu-power")
set(CPUPOWER_HELPER_PATH "${CPUPOWER_LIBEXECDIR}/cpu-power-helper")

# ---------------------------------------------------------------------------------------------
# The polkit action. THESE THREE VARIABLES FEED BOTH THE .policy FILE AND THE C++ that execs
# pkexec, from one place, because polkit compares the action's exec.path annotation against the
# program path BYTE FOR BYTE with no canonicalisation. If the two ever drift nothing crashes: the
# action stops matching, pkexec falls back to org.freedesktop.policykit.exec, and the tool starts
# asking for an administrator password it was never meant to ask for.
#
# The action id is a fixed literal, never constructed at run time from a path, a user name or a
# hostname -- an id an attacker gets a say in is not a pin.
# ---------------------------------------------------------------------------------------------
set(CPUPOWER_POLKIT_ACTION_ID "io.github.rations.cpu-power"
    CACHE STRING "polkit action id for the privileged helper")
#
# THE ACTIONDIR IS NOT DERIVED FROM CMAKE_INSTALL_PREFIX, and that is deliberate.
#
# polkitd reads action files from exactly two fixed directories, /usr/share/polkit-1/actions and
# /etc/polkit-1/actions, and from nowhere else. That search path belongs to polkitd; it is not
# relative to where this project happens to be installed. A prefix-relative actiondir is therefore
# wrong for every prefix except /usr -- including this project's own default of /usr/local, which
# is how the mistake was caught: the first build with a prefix-derived actiondir warned about
# itself. And the failure is SILENT, because pkexec falls back to org.freedesktop.policykit.exec
# and merely starts asking for an administrator password.
#
# So: ask polkit where it wants the file, and fall back to the documented location. Distributions
# that put it elsewhere override the cache variable, and DESTDIR still applies on top.
find_package(PkgConfig REQUIRED)
pkg_get_variable(CPUPOWER_POLKIT_ACTIONDIR_DEFAULT polkit-gobject-1 actiondir)
if(NOT CPUPOWER_POLKIT_ACTIONDIR_DEFAULT)
    set(CPUPOWER_POLKIT_ACTIONDIR_DEFAULT "/usr/share/polkit-1/actions")
endif()
set(CPUPOWER_POLKIT_ACTIONDIR "${CPUPOWER_POLKIT_ACTIONDIR_DEFAULT}"
    CACHE PATH "Where polkit action (.policy) files are installed")

# argv[1] of the helper, pinned by the action's exec.argv1 annotation. Pins argv[1] AND NOTHING
# AFTER IT -- the helper rejects unrecognised arguments itself, and sec-gate.sh asserts that.
set(CPUPOWER_PKEXEC_ARGV1 "--serve")

# ---------------------------------------------------------------------------------------------
# The privileged helper. Plain C, libc only, no libraries to link.
#
# TWO TARGETS FROM ONE SOURCE:
#   cpu-power-helper       installed. --sysroot IS NOT COMPILED IN AT ALL.
#   cpu-power-helper-test  never installed, refuses to run as root, --sysroot only.
# An option that does not exist cannot be abused, which is stronger than validating it.
# ---------------------------------------------------------------------------------------------
# -fstack-protector-ALL, not -strong, for the helper. Under -strong GCC instruments only what it
# judges worth instrumenting, which is a defensible optimisation and the wrong answer for the one
# program in this tree that runs as root. It is a few hundred lines and is not on a hot path, so
# the cost is nil and the guarantee is unconditional rather than at the compiler's discretion.
# sec-gate.sh asserts the canary is actually present in the built binary.
add_executable(cpu-power-helper helper/cpu-power-helper.c)
target_compile_options(cpu-power-helper PRIVATE -Wall -Wextra -Werror -fstack-protector-all)

add_executable(cpu-power-helper-test helper/cpu-power-helper.c)
target_compile_definitions(cpu-power-helper-test PRIVATE CPUPOWER_TEST_BUILD=1)
target_compile_options(cpu-power-helper-test PRIVATE -Wall -Wextra -Werror)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo cairo-ft freetype2 fontconfig)
pkg_check_modules(CAIROX11 REQUIRED IMPORTED_TARGET cairo-xlib x11)

# ---------------------------------------------------------------------------------------------
# The GUI layer: Canvas, FontStack and the X11 window, ported from the rations-amp plug-in editor
# rather than rewritten. Nothing here is privileged and nothing here can write.
#
# The resource directory is a compile-time default with a run-time environment override. That
# override is safe only because it exists in an UNPRIVILEGED process: a substituted font is a
# FreeType attack surface. The helper links none of this, reads no font, and is exec'd with an
# empty environment, so the override cannot reach anything with privilege to lose.
# ---------------------------------------------------------------------------------------------
set(CPUPOWER_RESOURCE_DIR "${CMAKE_INSTALL_FULL_DATADIR}/cpu-power")

add_library(cpupower-gfx STATIC
    src/gfx/canvas.cpp
    src/gfx/fontstack.cpp
    src/gfx/widgets.cpp
    src/panel.cpp
    src/platform/respath.cpp
    src/platform/xerror.cpp
    src/platform/x11window.cpp)
target_include_directories(cpupower-gfx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(cpupower-gfx PUBLIC
    CPUPOWER_RESOURCE_DIR_DEFAULT="${CPUPOWER_RESOURCE_DIR}")
target_link_libraries(cpupower-gfx PUBLIC PkgConfig::CAIRO PkgConfig::CAIROX11)
target_compile_options(cpupower-gfx PRIVATE -Wall -Wextra -Werror)

# The window itself. UNPRIVILEGED AND NEVER SETUID: it links the spawn code, which reaches the
# root helper through pkexec, and that is the only privilege anywhere near it.
add_executable(cpu-power src/main.cpp src/app.cpp src/config.cpp)
target_link_libraries(cpu-power PRIVATE cpupower-gfx cpupower-core cpupower-spawn)
target_compile_options(cpu-power PRIVATE -Wall -Wextra -Werror)

# The layout audit. geometry.h asserts the rectangles at compile time; this measures the text that
# goes in them, with the real fonts, at every scale the window claims to work at.
add_executable(uirender tools/uirender.cpp)
target_link_libraries(uirender PRIVATE cpupower-gfx)
target_compile_options(uirender PRIVATE -Wall -Wextra -Werror)

# ---------------------------------------------------------------------------------------------
# Spawning the helper, and the headless driver.
# ---------------------------------------------------------------------------------------------
add_library(cpupower-spawn STATIC src/platform/spawn.cpp)
target_include_directories(cpupower-spawn PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(cpupower-spawn PUBLIC
    CPUPOWER_HELPER_PATH="${CPUPOWER_HELPER_PATH}"
    CPUPOWER_POLKIT_ACTION_ID="${CPUPOWER_POLKIT_ACTION_ID}"
    CPUPOWER_PKEXEC_ARGV1="${CPUPOWER_PKEXEC_ARGV1}")
target_compile_options(cpupower-spawn PRIVATE -Wall -Wextra -Werror)

add_executable(cpu-power-cli tools/cpu-power-cli.cpp)
target_link_libraries(cpu-power-cli PRIVATE cpupower-core cpupower-spawn)
target_compile_options(cpu-power-cli PRIVATE -Wall -Wextra -Werror)

# fdprobe: measures whether pkexec hands fd 0 and fd 1 through unmodified, and whether it stays
# resident. The protocol channel is a socketpair dup2'd onto those two descriptors and the helper
# refuses a tty, so if pkexec touched them the polkit privilege path could not work at all. Reading
# the answer out of the shipped binary is not measuring it. NEVER INSTALLED.
add_executable(fdprobe tools/fdprobe.c)
target_compile_options(fdprobe PRIVATE -Wall -Wextra -Werror)

# ---------------------------------------------------------------------------------------------
# Install. NO SETUID BIT IS EVER SET, by this or by anything else; sec-gate.sh
# asserts that too. The helper reaches root through polkit at run time, never through a mode bit.
#
# The installed surface is deliberately three things: the GUI, the helper, and the action that
# says who may run the helper. cpu-power-cli is NOT installed -- it stays in-tree as a
# never-installed test driver, the same pattern as coretest, uirender and cpu-power-helper-test,
# because it is how both mandatory gates drive the helper.
# ---------------------------------------------------------------------------------------------
install(TARGETS cpu-power
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS cpu-power-helper
        RUNTIME DESTINATION ${CPUPOWER_LIBEXECDIR})

# The polkit action. Generated, never hand-written, so its exec.path cannot drift from the path
# the program actually execs.
configure_file(packaging/cpu-power.policy.in
               "${CMAKE_CURRENT_BINARY_DIR}/${CPUPOWER_POLKIT_ACTION_ID}.policy" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${CPUPOWER_POLKIT_ACTION_ID}.policy"
        DESTINATION "${CPUPOWER_POLKIT_ACTIONDIR}")

# The two faces the window is lettered in. Michroma is OFL-1.1, so its licence travels with it.
install(DIRECTORY resources/fonts
        DESTINATION ${CPUPOWER_RESOURCE_DIR})

# The application icon, in the freedesktop.org icon theme layout. These PNGs are DERIVED from the
# master artwork by scripts/make-icons.sh and are committed rather than generated at build time,
# so that building this project needs a compiler and not an image toolchain.
#
# The desktop entry names the icon as "cpu-power", with no path and no extension, and the icon
# theme lookup finds it by that name under hicolor/<size>x<size>/apps/. That indirection is the
# reason the window sets WM_CLASS: it is what lets a taskbar match a live window to this entry.
#
# This is the one installed directory whose location genuinely is prefix-relative, unlike the
# polkit actiondir above -- an icon theme is searched under every XDG data directory, so a
# ~/.local prefix works here even though it does not work for the action.
install(DIRECTORY packaging/icons/hicolor
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
        DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                              GROUP_READ GROUP_EXECUTE
                              WORLD_READ WORLD_EXECUTE)

# The desktop entry. Generated so that Exec carries the same absolute bindir the binary is
# installed to: a bare "cpu-power" would be resolved against the desktop session's PATH, which
# routinely does not contain /usr/local/bin.
configure_file(packaging/cpu-power.desktop.in
               "${CMAKE_CURRENT_BINARY_DIR}/cpu-power.desktop" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cpu-power.desktop"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# The manual. Also generated, so that the paths it documents are the paths this build uses --
# a manual that describes a different layout from the binary is worse than no manual.
configure_file(packaging/cpu-power.1.in "${CMAKE_CURRENT_BINARY_DIR}/cpu-power.1" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/cpu-power.1"
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)


message(STATUS "cpu-power ${PROJECT_VERSION}")
message(STATUS "  build type   ${CMAKE_BUILD_TYPE}")
message(STATUS "  hardening    ${CPUPOWER_HARDEN_FLAGS}")
message(STATUS "  helper       ${CPUPOWER_HELPER_PATH}")
message(STATUS "  polkit id    ${CPUPOWER_POLKIT_ACTION_ID}")
message(STATUS "  polkit dir   ${CPUPOWER_POLKIT_ACTIONDIR}")
message(STATUS "  resources    ${CPUPOWER_RESOURCE_DIR}")
message(STATUS "  icons        ${CMAKE_INSTALL_FULL_DATADIR}/icons/hicolor")
message(STATUS "  desktop      ${CMAKE_INSTALL_FULL_DATADIR}/applications")
message(STATUS "  manual       ${CMAKE_INSTALL_FULL_MANDIR}/man1")

# ---------------------------------------------------------------------------------------------
# The packaging trap worth catching at configure time rather than at first run.
#
# polkitd reads action files from exactly two directories and nowhere else. A prefix like
# ~/.local produces an actiondir polkitd will never look in -- and the failure is SILENT, because
# pkexec then falls back to org.freedesktop.policykit.exec and merely asks for an administrator
# password. The same prefix also puts the helper somewhere not owned by root, which the GUI's own
# checkHelperBinary() refuses outright. Both are correct behaviour and neither explains itself, so
# say it here, once, in the place where the choice was actually made.
# ---------------------------------------------------------------------------------------------
if(NOT CPUPOWER_POLKIT_ACTIONDIR STREQUAL "/usr/share/polkit-1/actions"
   AND NOT CPUPOWER_POLKIT_ACTIONDIR STREQUAL "/etc/polkit-1/actions")
    message(WARNING
        "polkitd only reads /usr/share/polkit-1/actions and /etc/polkit-1/actions.\n"
        "  This build installs its action to ${CPUPOWER_POLKIT_ACTIONDIR}, which polkitd will\n"
        "  NOT read, so the tool will fall back to asking for an administrator password.\n"
        "  Either configure with -DCMAKE_INSTALL_PREFIX=/usr, or override the actiondir with\n"
        "  -DCPUPOWER_POLKIT_ACTIONDIR=/usr/share/polkit-1/actions.")
endif()
