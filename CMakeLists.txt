cmake_minimum_required(VERSION 3.0)
project(nvds_template)

set(CMAKE_C_STANDARD_REQUIRED 11)

find_package(PkgConfig REQUIRED)

find_package(CUDA 11.1 REQUIRED)

pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)

include_directories(
    ${GLIB_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${CUDA_INCLUDE_DIRS}
    /opt/nvidia/deepstream/deepstream-5.1/sources/includes
)


file(GLOB SOURCES src/*.c)
link_directories(/opt/nvidia/deepstream/deepstream-5.1/lib)
add_executable(${PROJECT_NAME} ${SOURCES}) 
target_link_libraries(${PROJECT_NAME} ${GSTREAMER_LIBRARIES} ${CUDA_LIBRARIES} -L/opt/nvidia/deepstream/deepstream-5.1/lib -lnvdsgst_helper -lnvdsgst_meta -lnvds_meta -lm)