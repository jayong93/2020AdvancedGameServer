cmake_minimum_required(VERSION 3.10)

project("Seamless_Server")
set(CMAKE_VERBOSE_MAKEFILE true)

set(OUTPUT_NAME "${CMAKE_PROJECT_NAME}")
set(SRC_FILES
    main.cpp
    )

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_STANDARD 17)

if(UNIX AND NOT APPLE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY bin)
link_libraries(pthread)
add_compile_options(-g -ggdb)
set(CMAKE_CXX_FLAGS_DEBUG "-DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -Ofast")
endif()

add_executable(${OUTPUT_NAME} ${SRC_FILES})