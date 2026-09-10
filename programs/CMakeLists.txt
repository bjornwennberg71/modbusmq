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
target_link_libraries(modbusmq_prog modbusmq)

#
# Only link libmosquitto when MQTT is actually compiled in. Linking it
# unconditionally meant a default build -- MQTT is off unless setup.sh is given
# --enable-mqtt -- still needed libmosquitto installed to link at all, so
# anyone without the dev package could not build the program the README tells
# them to build.
#
if(MQTT_ENABLED)
    target_link_libraries(modbusmq_prog mosquitto)
endif()


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
