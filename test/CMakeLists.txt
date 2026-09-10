#
# modbusmq tests
#
# bjornw: 202509
#

cmake_minimum_required(VERSION 3.12)
project(test)

include_directories(../lib)

#
# Each test program is standalone: it prints its own checks and returns
# non-zero if any failed. run_tests.sh boots a modbusmq_server and aggregates.
#
foreach(T test_scaling test_read test_loop)
    add_executable(${T} ${T}.c)
    target_link_libraries(${T} modbusmq m)
    target_include_directories(${T} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

#
# make check -- builds what it needs, then runs the suite against this tree
#
#
# make check-all -- both MQTT configurations, in throwaway build dirs, so this
# tree's own MQTT setting is left alone
#
add_custom_target(check-all
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_matrix.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)

add_custom_target(check
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/run_tests.sh ${CMAKE_BINARY_DIR}
    DEPENDS test_scaling test_read test_loop modbusmq_prog modbusmq_query modbusmq_server
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    USES_TERMINAL)
