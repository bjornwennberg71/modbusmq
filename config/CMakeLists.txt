#
# make for config/
#
# bjornwennberg71@gmail.com
#
#
include(GNUInstallDirs)

# Only *.config files in THIS directory (where this CMakeLists.txt lives)
file(GLOB CONFIG_FILES
     CONFIGURE_DEPENDS
     LIST_DIRECTORIES false
     "${CMAKE_CURRENT_LIST_DIR}/*.config")

if(CONFIG_FILES)
  install(FILES ${CONFIG_FILES}
          DESTINATION "config")   # -> <prefix>/config
endif()
