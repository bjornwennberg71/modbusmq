#
# modbusmq programs
#
# bjornw: 202410
#

cmake_minimum_required(VERSION 3.12)
project(programs)

include_directories(../lib)

#
# modbusmq_subscribe
#
add_executable(modbusmq_subscribe modbusmq_subscribe.c)
target_link_libraries(modbusmq_subscribe modbusmq mosquitto)


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

install(TARGETS modbusmq_query modbusmq_subscribe modbusmq_server
        RUNTIME DESTINATION bin)
	

