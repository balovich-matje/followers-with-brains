cmake_minimum_required(VERSION 3.24)
project(FollowersWithBrains VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CommonLibSSE CONFIG REQUIRED)

# The brain: everything the mod does (FollowersWithBrains/FWBBrain.dll, can be reloaded in game).
file(GLOB_RECURSE BRAIN_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_library(FWBBrain SHARED ${BRAIN_SOURCES})
target_include_directories(FWBBrain PRIVATE src)
target_link_libraries(FWBBrain PRIVATE CommonLibSSE::CommonLibSSE)
target_precompile_headers(FWBBrain PRIVATE src/PCH.h)

# The loader: the SKSE plugin (FollowersWithBrains.dll), which loads the brain.
file(GLOB_RECURSE LOADER_SOURCES CONFIGURE_DEPENDS loader/*.cpp)
add_library(${PROJECT_NAME} SHARED ${LOADER_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)
target_compile_definitions(${PROJECT_NAME} PRIVATE PLUGIN_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	PLUGIN_VERSION_MINOR=${PROJECT_VERSION_MINOR} PLUGIN_VERSION_PATCH=${PROJECT_VERSION_PATCH})
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
