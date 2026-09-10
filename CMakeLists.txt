#
# modbusmq main makefile
#
# bjornw: 202410
#
cmake_minimum_required(VERSION 3.16)

project(modbusmq)

# modbusmq client program: turn on/off support for mqtt and link with libmosquitto
option(MQTT_ENABLED "Enable MQTT/Mosquitto support" OFF)

#
# Debug builds compile with the warnings on, so debug/ is where you see them.
# Release stays quiet so a downstream packaging build is not noisy.
#
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-Wall -Wextra)
endif()

# Set preprocessor definition accordingly
if(MQTT_ENABLED)
    add_compile_definitions(MQTT_ENABLED=1)
else()
    add_compile_definitions(MQTT_ENABLED=0)
endif()


add_subdirectory(lib)
add_subdirectory(programs)
add_subdirectory(config)
add_subdirectory(test)

