# Test subdirectory CMakeLists.txt

# Find GTest (optional)
find_package(GTest QUIET)

# Test sources
set(TEST_SOURCES
    test_parser.cpp
    test_renderer.cpp
    test_avm2.cpp
    test_runner.cpp
)

# Individual test executables (if GTest not available, build simpler versions)
if(GTest_FOUND)
    # Create test executable with GTest
    add_executable(swf_tests ${TEST_SOURCES})
    
    target_link_libraries(swf_tests PRIVATE
        swf
        GTest::GTest
        GTest::Main
    )
    
    target_include_directories(swf_tests PRIVATE
        ${CMAKE_SOURCE_DIR}/include
    )
    
    # Add tests
    add_test(NAME ParserTests COMMAND swf_tests --gtest_filter=Parser*)
    add_test(NAME RendererTests COMMAND swf_tests --gtest_filter=Renderer*)
    add_test(NAME AVM2Tests COMMAND swf_tests --gtest_filter=AVM2*)
else()
    message(STATUS "GTest not found, building simple tests instead")
    
    # Simple test runner
    add_executable(test_simple test_runner.cpp)
    target_link_libraries(test_simple PRIVATE swf)
    target_include_directories(test_simple PRIVATE ${CMAKE_SOURCE_DIR}/include)
endif()
