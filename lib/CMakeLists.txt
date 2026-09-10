#
# modbusmq library makefile
#
# bjornw: 202410
#
#
set(T modbusmq)
project(${T} VERSION 2.3.0 DESCRIPTION "modbusmq library")

set(SOURCES modbusmq_tcp.c modbusmq_rtu.c libmodbusmq.c modbusmq_time.c modbusmq_log.c modbusmq_config.c)

add_library(${T} SHARED ${SOURCES} )

# libmodbusmq.so.<full version> with a libmodbusmq.so.<major> SONAME, so
# consumers link against the major version and a minor or build bump does not
# force a relink
set_target_properties(${T} PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

# Public include path when using the library
target_include_directories(${T}
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

#
# Install library, and record it in an export set so downstream CMake can
# find_package(modbusmq) instead of vendoring the sources.
#
install(TARGETS ${T}
        EXPORT  modbusmq-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Install includes
install(FILES modbusmq.h modbusmq_time.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

#
# pkg-config, for everyone not using CMake
#
configure_file(modbusmq.pc.in modbusmq.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/modbusmq.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

#
# CMake package: find_package(modbusmq) -> modbusmq::modbusmq
#
install(EXPORT modbusmq-targets
        FILE        modbusmq-targets.cmake
        NAMESPACE   modbusmq::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/modbusmq)

configure_package_config_file(
    modbusmq-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/modbusmq-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/modbusmq)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/modbusmq-config-version.cmake
    VERSION       ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/modbusmq-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/modbusmq-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/modbusmq)
	
