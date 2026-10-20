add_executable(core_tests core_tests.cpp)
target_link_libraries(core_tests PRIVATE imdcore)
imd_warnings(core_tests)
add_test(NAME core_tests COMMAND core_tests)

add_executable(td0_stream_check td0_stream_check.cpp)
target_link_libraries(td0_stream_check PRIVATE imdcore)
imd_warnings(td0_stream_check)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  foreach(seed 1 2)
    add_test(NAME td0_lzhuf_stream_${seed}
             COMMAND ${CMAKE_COMMAND}
               -DPYTHON=${Python3_EXECUTABLE} -DGEN=${CMAKE_CURRENT_SOURCE_DIR}/td0gen.py
               -DCHECK=$<TARGET_FILE:td0_stream_check> -DSEED=${seed}
               -DDIR=${CMAKE_CURRENT_BINARY_DIR}/lzhuf_${seed}
               -P ${CMAKE_CURRENT_SOURCE_DIR}/td0_stream_test.cmake)
  endforeach()
  set(tool_tests ${CMAKE_CURRENT_SOURCE_DIR}/tool_tests.py)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${tool_tests} --list
                  OUTPUT_VARIABLE tool_test_names OUTPUT_STRIP_TRAILING_WHITESPACE)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${tool_tests})
  foreach(name ${tool_test_names})
    add_test(NAME ${name}
             COMMAND ${Python3_EXECUTABLE} ${tool_tests} $<TARGET_FILE_DIR:imdu> ${name})
  endforeach()
else()
  message(WARNING "Python 3 not found: end-to-end tool tests are disabled")
endif()
