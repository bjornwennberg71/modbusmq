#
# modbusmq library makefile
#
# bjornw: 202410
#
#
set(T modbusmq)
project(${T} VERSION 2.0.1 DESCRIPTION "modbusmq library")

set(SOURCES modbusmq_tcp.c modbusmq_rtu.c modbusmq.c modbusmq_time.c modbusmq_log.c modbusmq_config.c)

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

# Install library
install(TARGETS ${T}
        LIBRARY DESTINATION lib)

# Install includes
install(FILES modbusmq.h
    DESTINATION include
)
	
