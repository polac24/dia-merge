cmake_minimum_required(VERSION 3.13)

if(COMMAND project)
  project(dia-merge)
endif()

find_package(Curses REQUIRED)

function(add_dia_executable execname)
  add_executable("${execname}" "${execname}.cpp")
  target_include_directories("${execname}" PRIVATE ${LLVM_DIR}/include)
  target_compile_features("${execname}" PRIVATE cxx_std_14)
  set_target_properties("${execname}" PROPERTIES CXX_EXTENSIONS OFF)
  target_compile_options("${execname}" PRIVATE -Wall -Wextra -fblocks)
  if(NOT LLVM_ENABLE_RTTI)
    target_compile_options("${execname}" PRIVATE -fno-rtti)
  endif()
  target_link_libraries("${execname}" PUBLIC ${LLVM_DIR}/lib/libLLVMBitReader.a ${LLVM_DIR}/lib/libLLVMBitWriter.a ${LLVM_DIR}/lib/libLLVMBitReader.a ${LLVM_DIR}/lib/libLLVMBitWriter.a ${LLVM_DIR}/lib/libclangDirectoryWatcher.a ${LLVM_DIR}/lib/libclangIndex.a ${LLVM_DIR}/lib/libclangFormat.a ${LLVM_DIR}/lib/libclangToolingInclusions.a ${LLVM_DIR}/lib/libclangFrontend.a ${LLVM_DIR}/lib/libclangDriver.a ${LLVM_DIR}/lib/libclangParse.a ${LLVM_DIR}/lib/libLLVMOption.a ${LLVM_DIR}/lib/libclangSerialization.a ${LLVM_DIR}/lib/libclangSema.a ${LLVM_DIR}/lib/libclangAPINotes.a ${LLVM_DIR}/lib/libclangEdit.a ${LLVM_DIR}/lib/libclangAnalysis.a ${LLVM_DIR}/lib/libclangASTMatchers.a ${LLVM_DIR}/lib/libclangAST.a ${LLVM_DIR}/lib/libLLVMFrontendOpenMP.a ${LLVM_DIR}/lib/libLLVMTransformUtils.a ${LLVM_DIR}/lib/libclangToolingCore.a ${LLVM_DIR}/lib/libclangRewrite.a ${LLVM_DIR}/lib/libclangLex.a ${LLVM_DIR}/lib/libclangBasic.a ${LLVM_DIR}/lib/libLLVMAnalysis.a ${LLVM_DIR}/lib/libLLVMProfileData.a ${LLVM_DIR}/lib/libLLVMObject.a ${LLVM_DIR}/lib/libLLVMBitReader.a ${LLVM_DIR}/lib/libLLVMCore.a ${LLVM_DIR}/lib/libLLVMRemarks.a ${LLVM_DIR}/lib/libLLVMBitstreamReader.a ${LLVM_DIR}/lib/libLLVMMCParser.a ${LLVM_DIR}/lib/libLLVMMC.a ${LLVM_DIR}/lib/libLLVMDebugInfoCodeView.a ${LLVM_DIR}/lib/libLLVMDebugInfoMSF.a ${LLVM_DIR}/lib/libLLVMTextAPI.a ${LLVM_DIR}/lib/libLLVMBinaryFormat.a ${LLVM_DIR}/lib/libLLVMSupport.a  ${LLVM_DIR}/lib/libLLVMDemangle.a  ${CURSES_LIBRARIES}
)
  target_link_options("${execname}" PRIVATE -dead_strip)
endfunction()

add_dia_executable(dia-merge)

