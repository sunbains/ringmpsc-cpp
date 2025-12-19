add_executable(test_ringmpsc test_ringmpsc.cpp)
target_link_libraries(test_ringmpsc PRIVATE ringmpsc)

add_test(NAME ringmpsc.unit COMMAND test_ringmpsc)

add_executable(bench_final bench_final.cpp)
target_link_libraries(bench_final PRIVATE ringmpsc)

add_executable(bench_final_parity bench_final_parity.cpp)
target_link_libraries(bench_final_parity PRIVATE ringmpsc)

add_executable(bench_final_coroutine bench_final_coroutine.cpp)
target_link_libraries(bench_final_coroutine PRIVATE ringmpsc)
