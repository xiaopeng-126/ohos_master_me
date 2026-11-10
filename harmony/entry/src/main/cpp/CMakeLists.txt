cmake_minimum_required(VERSION 3.5.0)
project(master_me_ohos LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(MASTER_ME_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.."
  ABSOLUTE
)

add_library(masterme SHARED
  master_me_engine.cpp
  ${MASTER_ME_ROOT}/dpf/distrho/src/DistrhoPlugin.cpp
)

target_include_directories(masterme PRIVATE
  ${MASTER_ME_ROOT}
  ${MASTER_ME_ROOT}/dpf
  ${MASTER_ME_ROOT}/dpf/distrho
  ${MASTER_ME_ROOT}/plugin
  ${MASTER_ME_ROOT}/pregen
)

target_compile_definitions(masterme PRIVATE
  HAVE_CPP11_SUPPORT=1
  __MOD_DEVICES__=1
  DGL_USE_FILE_BROWSER=1
)

target_compile_options(masterme PRIVATE
  -fvisibility=hidden
  -Wno-unused-parameter
  -Wno-overloaded-virtual
  -Wno-ignored-qualifiers
)

target_link_libraries(masterme PUBLIC libace_napi.z.so)
