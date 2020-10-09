cmake_minimum_required(VERSION 3.0.0)
project(bcDecoder VERSION 0.1.0)

set(CMAKE_VS_PLATFORM_TOOLSET_HOST_ARCHITECTURE x86)
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")

file(GLOB LUA51C ${PROJECT_SOURCE_DIR}/src/lj_*.c ${PROJECT_SOURCE_DIR}/src/lib_*.c )
add_library(Lua51 ${LUA51C})
target_include_directories(Lua51 PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_directories(Lua51 PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(Lua51 PRIVATE "WIN32" "_WINDOWS" "_USRDLL" "LUA51_EXPORTS" "_CRT_SECURE_NO_WARNINGS")
IF( MSVC )
add_custom_command(TARGET Lua51 PRE_BUILD
COMMAND cd /d ${PROJECT_SOURCE_DIR}/src
COMMAND ${PROJECT_SOURCE_DIR}/src/msvcbuild.bat
COMMAND cd /d ${PROJECT_SOURCE_DIR}
)
target_link_libraries(Lua51 ${PROJECT_SOURCE_DIR}/src/lj_vm.obj)
ENDIF ( MSVC )

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
add_executable(bcDec ${PROJECT_SOURCE_DIR}/bcDec/bcDec.cpp)
target_link_libraries(bcDec Lua51)
