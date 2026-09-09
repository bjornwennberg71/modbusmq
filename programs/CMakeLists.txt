#
# modbusmq programs
#
# bjornw: 202410
#

cmake_minimum_required(VERSION 3.12)
project(programs)

include_directories(../lib)

#
# modbusmq_bridge
#
add_executable(modbusmq_bridge modbusmq_bridge.c)
target_link_libraries(modbusmq_bridge modbusmq mosquitto)


#
# modbusmq_query
#
add_executable(modbusmq_query modbusmq_query.c)
target_link_libraries(modbusmq_query modbusmq)

#
# modbusmq_server
#
add_executable(modbusmq_server modbusmq_server.c)
target_link_libraries(modbusmq_server modbusmq)

install(TARGETS modbusmq_query modbusmq_bridge modbusmq_server
        RUNTIME DESTINATION bin)
	

