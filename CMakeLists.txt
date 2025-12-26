cmake_minimum_required(VERSION 3.16)
project(ipc_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Werror)

include_directories(${CMAKE_SOURCE_DIR})

file(GLOB CONN_SOURCES "${CMAKE_SOURCE_DIR}/conn_*.cpp")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

foreach(CONN_FILE ${CONN_SOURCES})
    get_filename_component(CONN_NAME ${CONN_FILE} NAME_WE) # conn_xxx
    string(REGEX REPLACE "^conn_" "" TYPE_CODE ${CONN_NAME})

    add_executable(host_${TYPE_CODE} host.cpp ${CONN_FILE})
    target_link_libraries(host_${TYPE_CODE} PRIVATE pthread rt)

    add_executable(client_${TYPE_CODE} client.cpp ${CONN_FILE})
    target_link_libraries(client_${TYPE_CODE} PRIVATE pthread rt)
endforeach()

