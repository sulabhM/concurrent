cmake_minimum_required(VERSION 3.10)
project(c_project C)

set(CMAKE_C_STANDARD 11)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_executable(${PROJECT_NAME}
    src/main.c
    src/list.c
)
target_link_libraries(${PROJECT_NAME} pthread)

add_executable(test_list
    src/test_list.c
    src/list.c
)
target_link_libraries(test_list pthread)

enable_testing()
add_test(NAME test_list COMMAND test_list)
