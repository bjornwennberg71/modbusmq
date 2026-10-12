#
# A downstream project as pxos sees it: find_package against an installed
# prefix, nothing from the source tree. Built by test/run_install.sh.
#
cmake_minimum_required(VERSION 3.16)
project(consumer C)

find_package(modbusmq CONFIG REQUIRED)

add_executable(consumer_static consumer.c)
target_link_libraries(consumer_static modbusmq::modbusmq_static)

add_executable(consumer_shared consumer.c)
target_link_libraries(consumer_shared modbusmq::modbusmq)
