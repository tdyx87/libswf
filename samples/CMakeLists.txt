# Sample programs CMakeLists.txt

# SWF Info - displays SWF file information
add_executable(swf_info swf_info.cpp)
target_link_libraries(swf_info PRIVATE swf)
target_include_directories(swf_info PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Simple viewer
if(WIN32)
    add_executable(swf_viewer swf_viewer.cpp)
    target_link_libraries(swf_viewer PRIVATE swf)
    target_include_directories(swf_viewer PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()

# AS3 test runner
add_executable(as3_test as3_test.cpp)
target_link_libraries(as3_test PRIVATE swf)
target_include_directories(as3_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Image buffer test
add_executable(image_buffer_test image_buffer_test.cpp)
target_link_libraries(image_buffer_test PRIVATE swf)
target_include_directories(image_buffer_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Software renderer test
add_executable(software_renderer_test software_renderer_test.cpp)
target_link_libraries(software_renderer_test PRIVATE swf)
target_include_directories(software_renderer_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# SWF creation test
add_executable(swf_create_test swf_create_test.cpp)
target_link_libraries(swf_create_test PRIVATE swf)
target_include_directories(swf_create_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Debug SWF parser
add_executable(debug_swf debug_swf.cpp)
target_link_libraries(debug_swf PRIVATE swf)
target_include_directories(debug_swf PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Performance profiling test
add_executable(profile_test profile_test.cpp)
target_link_libraries(profile_test PRIVATE swf)
target_include_directories(profile_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Filter and blend mode test
add_executable(filter_blend_test filter_blend_test.cpp)
target_link_libraries(filter_blend_test PRIVATE swf)
target_include_directories(filter_blend_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Graphics API integration test
add_executable(graphics_api_test graphics_api_test.cpp)
target_link_libraries(graphics_api_test PRIVATE swf)
target_include_directories(graphics_api_test PRIVATE ${CMAKE_SOURCE_DIR}/include)

# SWF Resource Extractor Tool
add_executable(swf_extract swf_extract.cpp)
target_link_libraries(swf_extract PRIVATE swf)
target_include_directories(swf_extract PRIVATE ${CMAKE_SOURCE_DIR}/include)

# SWF Optimizer Tool
add_executable(swf_optimizer swf_optimizer.cpp)
target_link_libraries(swf_optimizer PRIVATE swf)
target_include_directories(swf_optimizer PRIVATE ${CMAKE_SOURCE_DIR}/include)

# Renderer Abstraction Demo
add_executable(renderer_abstraction_demo renderer_abstraction_demo.cpp)
target_link_libraries(renderer_abstraction_demo PRIVATE swf)
target_include_directories(renderer_abstraction_demo PRIVATE ${CMAKE_SOURCE_DIR}/include)
