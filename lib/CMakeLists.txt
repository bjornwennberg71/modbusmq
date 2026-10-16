#
# modbusmq library makefile
#
# bjornw: 202410
#
#
set(T modbusmq)
project(${T} VERSION 2.5.3 DESCRIPTION "modbusmq library")

set(SOURCES modbusmq_tcp.c modbusmq_rtu.c libmodbusmq.c modbusmq_time.c modbusmq_log.c modbusmq_config.c)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

#
# Compile once, position independent, and build both a shared and a static
# library from the same objects. modbusmq::modbusmq is the shared one,
# modbusmq::modbusmq_static the archive for consumers that want a single
# binary. Both are named libmodbusmq.
#
add_library(${T}_objects OBJECT ${SOURCES})
set_target_properties(${T}_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(${T}        SHARED $<TARGET_OBJECTS:${T}_objects>)
add_library(${T}_static STATIC $<TARGET_OBJECTS:${T}_objects>)

# libmodbusmq.so.<full version> with a libmodbusmq.so.<major> SONAME, so
# consumers link against the major version and a minor or build bump does not
# force a relink
set_target_properties(${T} PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)
set_target_properties(${T}_static PROPERTIES OUTPUT_NAME ${T})

#
# Public headers are installed into include/modbusmq/ and the include path
# points into that directory, so consumers write #include "modbusmq.h" both
# in-tree and installed, and nothing lands loose in /usr/include.
#
foreach(L ${T} ${T}_static)
    target_include_directories(${L}
        PUBLIC
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/modbusmq>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    )
    # fabsf in the publish policy
    target_link_libraries(${L} PRIVATE m)
endforeach()

#
# Install library, and record it in an export set so downstream CMake can
# find_package(modbusmq) instead of vendoring the sources.
#
install(TARGETS ${T} ${T}_static
        EXPORT  modbusmq-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

# Install includes
install(FILES modbusmq.h modbusmq_time.h modbusmq_config.h modbusmq_log.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/modbusmq
)

#
# pkg-config, for everyone not using CMake
#
configure_file(modbusmq.pc.in modbusmq.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/modbusmq.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

#
# CMake package: find_package(modbusmq) -> modbusmq::modbusmq and
# modbusmq::modbusmq_static
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
	
