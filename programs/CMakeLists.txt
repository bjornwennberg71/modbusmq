#
# modbusmq programs
#
# bjornw: 202410
#

cmake_minimum_required(VERSION 3.12)
project(programs)

include_directories(../lib)

#
# modbusmq
#
# The CMake target name cannot be "modbusmq" -- lib/ already owns that target
# for the shared library -- so the target is modbusmq_prog and OUTPUT_NAME
# gives the installed binary its real name.
#
add_executable(modbusmq_prog modbusmq.c)
set_target_properties(modbusmq_prog PROPERTIES OUTPUT_NAME modbusmq)
target_link_libraries(modbusmq_prog modbusmq mosquitto)


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

install(TARGETS modbusmq_query modbusmq_prog modbusmq_server
        RUNTIME DESTINATION bin)
