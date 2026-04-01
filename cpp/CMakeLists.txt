cmake_minimum_required(VERSION 3.20)
project(nsgd_cpp)

set(CMAKE_CXX_STANDARD 20)

add_executable(nsgd_cpp main.cpp
        SimProcess.h
        FunctionInstance.h
        FunctionInstance.cpp
        ServerlessSimulatorFixed.h
        ServerlessSimulatorFixed.cpp
        AutoScalingAlgorithm.h
        AutoScalingAlgorithm.cpp
        input_test_tiny.json
)

target_include_directories(nsgd_cpp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
