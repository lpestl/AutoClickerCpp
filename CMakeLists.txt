cmake_minimum_required(VERSION 3.15)
project(MouseSpaceTracker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GUI-приложение (без консольного окна)
if(WIN32)
    add_executable(MouseSpaceTracker WIN32 main.cpp)
    target_link_libraries(MouseSpaceTracker PRIVATE user32 gdi32)
else()
    message(FATAL_ERROR "This project requires Windows (WinAPI).")
endif()
