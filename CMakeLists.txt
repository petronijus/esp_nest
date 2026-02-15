# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/components)

# Load secrets (WiFi credentials etc.) if the file exists
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/secrets.defaults")
    list(APPEND SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults"
        "${CMAKE_CURRENT_SOURCE_DIR}/secrets.defaults"
    )
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ZX2D10GE01R)
