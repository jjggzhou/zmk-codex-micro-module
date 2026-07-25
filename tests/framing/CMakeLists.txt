cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(codex_framing_tests)

file(GLOB CORPUS_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/fuzz_corpus/*"
)
execute_process(
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/embed_corpus.py"
          "${CMAKE_CURRENT_BINARY_DIR}/corpus_data.h" ${CORPUS_FILES}
  COMMAND_ERROR_IS_FATAL ANY
)
target_include_directories(app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

target_sources(app PRIVATE
  src/main.c
  ../../src/protocol/framing.c
  ../../src/protocol/json_value.c
)
