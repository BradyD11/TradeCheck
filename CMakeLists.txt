cmake_minimum_required(VERSION 3.20)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wpedantic)

option(LOB_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(LOB_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(lob src/order_book.cpp)
target_include_directories(lob PUBLIC include)

add_executable(itch_replay tools/itch_replay.cpp)
add_executable(gen_itch tools/gen_itch.cpp)
add_executable(bench_matching bench/bench_matching.cpp)
foreach(t itch_replay gen_itch bench_matching)
  target_link_libraries(${t} lob)
endforeach()

enable_testing()
foreach(t unit property itch)
  add_executable(test_${t} tests/test_${t}.cpp)
  target_link_libraries(test_${t} lob)
  add_test(NAME ${t} COMMAND test_${t})
endforeach()

# Each mutant is the engine with one deliberate bug. The property test must catch every one.
#   1: LIFO instead of FIFO within a price level
#   2: a share disappears on partial fill
#   3: no match at exactly the limit price (book can lock)
foreach(m 1 2 3)
  add_library(lob_mutant${m} STATIC src/order_book.cpp)
  target_include_directories(lob_mutant${m} PUBLIC include)
  target_compile_definitions(lob_mutant${m} PRIVATE LOB_MUTANT=${m})
  add_executable(test_property_mutant${m} tests/test_property.cpp)
  target_link_libraries(test_property_mutant${m} lob_mutant${m})
  add_test(NAME mutant${m}_is_caught COMMAND test_property_mutant${m})
  set_tests_properties(mutant${m}_is_caught PROPERTIES WILL_FAIL TRUE)
endforeach()
