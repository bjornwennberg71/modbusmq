#
# modbusmq main makefile
#
# bjornw: 202410
#
cmake_minimum_required(VERSION 3.16)

project(modbusmq)

# modbus_subscribe client program: turn on/off support for mqtt and link with libmosquitto
option(MQTT_ENABLED "Enable MQTT/Mosquitto support" OFF)

# Set preprocessor definition accordingly
if(MQTT_ENABLED)
    add_compile_definitions(MQTT_ENABLED=1)
else()
    add_compile_definitions(MQTT_ENABLED=0)
endif()


add_subdirectory(lib)
add_subdirectory(programs)
add_subdirectory(config)

