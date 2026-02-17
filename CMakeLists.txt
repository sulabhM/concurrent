cmake_minimum_required(VERSION 3.10)
project(c_project C)

set(CMAKE_C_STANDARD 11)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(${PROJECT_NAME}
    src/main.c
    src/list.c
)
target_link_libraries(${PROJECT_NAME} pthread)
