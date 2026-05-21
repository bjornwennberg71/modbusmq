#
# modbusmq library makefile
#
# bjornw: 202410
#
#
set(T modbusmq)
project(${T} VERSION 0.2025.12 DESCRIPTION "modbusmq library")

set(SOURCES modbusmq_tcp.c modbusmq_rtu.c modbusmq.c modbusmq_time.c modbusmq_log.c modbusmq_config.c)

add_library(${T} SHARED ${SOURCES} )

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
	
