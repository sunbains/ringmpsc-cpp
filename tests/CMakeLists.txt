add_executable(test_ringmpsc test_ringmpsc.cpp)
target_link_libraries(test_ringmpsc PRIVATE ringmpsc)

add_test(NAME ringmpsc.unit COMMAND test_ringmpsc)
