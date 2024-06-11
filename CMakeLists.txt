cmake_minimum_required(VERSION 3.1)

project(Sizer LANGUAGES CXX)

set(CMAKE_C_COMPILER "g++")
set(CMAKE_CXX_STANDARD 17)

# Home Directory
set(PROJECT_HOME ${PROJECT_SOURCE_DIR})

add_subdirectory(${PROJECT_HOME}/module/OpenSTA)

# Package Settings
find_package(SWIG REQUIRED)
find_package(TCL REQUIRED)

add_compile_options(-fPIC)

# Source Code
set(PROJECT_SRC 
	src/main.cpp
)

# Executable
add_executable(${PROJECT_NAME} ${PROJECT_SRC})

# Include Directory
target_include_directories(${PROJECT_NAME}
  PUBLIC
	module/OpenSTA/include
)

# Link Library
target_link_libraries(${PROJECT_NAME} 
  PUBLIC
	OpenSTA
	sta_swig
)
