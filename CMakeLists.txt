cmake_minimum_required(VERSION 3.5)
project(brokenithm LANGUAGES CXX)
SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/")
INCLUDE_DIRECTORIES("${CMAKE_SOURCE_DIR}/include/")
IF(NOT CMAKE_BUILD_TYPE)
    SET(CMAKE_BUILD_TYPE Release)
ENDIF()
SET(CMAKE_CXX_STANDARD 17)

IF (NOT WIN32)
    MESSAGE(FATAL_ERROR "This is not supposed to be run on non-Windows platforms!")
ENDIF()


IF (NOT MSVC)
    ADD_COMPILE_OPTIONS(-Wall -Wextra -Wno-unused-parameter -Wno-unused-result)
ELSE()
    ADD_COMPILE_OPTIONS(/W4 /utf-8)
    ADD_COMPILE_DEFINITIONS(NOMINMAX)
ENDIF()

ADD_EXECUTABLE(brokenithm 
    src/main.cpp
    src/shared_memory.cpp
    src/signal_processor.cpp
    src/resources.rc)
INCLUDE_DIRECTORIES(src)
LINK_DIRECTORIES(${CMAKE_SOURCE_DIR})

TARGET_LINK_LIBRARIES(brokenithm ws2_32)
