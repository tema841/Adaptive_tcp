cmake_minimum_required(VERSION 3.20)
project(adaptive_tcp_transfer VERSION 5.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "Adaptive TCP v4 is supported on Windows 10/11 only.")
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # MinGW-w64 (including MSYS2 UCRT64) is supported on Windows too.
    add_compile_options(-Wall -Wextra -Wpedantic)
else()
    message(FATAL_ERROR "Use MSVC or MinGW-w64/GCC to build Adaptive TCP v4 on Windows.")
endif()

find_package(Threads REQUIRED)

add_library(core STATIC socket.cpp transfer.cpp sha256.cpp metrics.cpp)
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(core PUBLIC ws2_32)
endif()

add_executable(server server.cpp)
target_link_libraries(server PRIVATE core)
add_executable(client client.cpp)
target_link_libraries(client PRIVATE core)

include(CTest)
if(BUILD_TESTING)
    add_executable(tests tests.cpp)
    target_link_libraries(tests PRIVATE core)
    add_test(NAME core_and_transfer_tests COMMAND tests)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME tcp_end_to_end
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py
                    $<TARGET_FILE:server> $<TARGET_FILE:client>
                    ${CMAKE_CURRENT_SOURCE_DIR}/web)
        set_tests_properties(tcp_end_to_end PROPERTIES TIMEOUT 40)
        add_test(NAME dashboard_http_api
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/dashboard_http.py
                    $<TARGET_FILE:server>
                    ${CMAKE_CURRENT_SOURCE_DIR}/web)
        set_tests_properties(dashboard_http_api PROPERTIES TIMEOUT 60)
        add_test(NAME adaptive_browser_upload
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/adaptive_upload.py
                    $<TARGET_FILE:server>
                    ${CMAKE_CURRENT_SOURCE_DIR}/web)
        set_tests_properties(adaptive_browser_upload PROPERTIES TIMEOUT 90)
        add_test(NAME graceful_server_shutdown
            COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/server_shutdown.py
                    $<TARGET_FILE:server>
                    ${CMAKE_CURRENT_SOURCE_DIR}/web)
        set_tests_properties(graceful_server_shutdown PROPERTIES TIMEOUT 30)
    endif()
endif()

add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_CURRENT_SOURCE_DIR}/web $<TARGET_FILE_DIR:server>/web)
