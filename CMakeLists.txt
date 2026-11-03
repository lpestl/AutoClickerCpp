cmake_minimum_required(VERSION 3.15)
project(AutoClickerCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GUI-приложение (без консольного окна)
if(WIN32)
    add_executable(AutoClickerCpp WIN32 main.cpp)
    target_link_libraries(AutoClickerCpp PRIVATE user32 gdi32)
else()
    message(FATAL_ERROR "This project requires Windows (WinAPI).")
endif()
