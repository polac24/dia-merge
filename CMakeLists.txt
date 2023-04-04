cmake_minimum_required(VERSION 3.13)

if(COMMAND project)
  project(dia-merge)
endif()

find_package(Curses REQUIRED)

function(add_dia_executable execname)
  add_executable("${execname}" "${execname}.cpp")
  target_include_directories("${execname}" PRIVATE ${LLVM_DIR}/include)
  target_compile_features("${execname}" PUBLIC cxx_std_17)
  set_target_properties("${execname}" PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options("${execname}" PRIVATE -Wall -Wextra -fblocks -Wno-unused-parameter)
  if(NOT LLVM_ENABLE_RTTI)
    target_compile_options("${execname}" PRIVATE -fno-rtti)
  endif()
  target_link_libraries("${execname}" PUBLIC ${LLVM_DIR}/lib/libclangFrontend.a ${LLVM_DIR}/lib/libclangEdit.a ${LLVM_DIR}/lib/libclangLex.a ${LLVM_DIR}/lib/libclangBasic.a ${LLVM_DIR}/lib/libLLVMBitstreamReader.a ${LLVM_DIR}/lib/libLLVMSupport.a ${LLVM_DIR}/lib/libLLVMTargetParser.a ${CURSES_LIBRARIES}
)
  target_link_options("${execname}" PRIVATE -dead_strip)
endfunction()

add_dia_executable(dia-merge)

