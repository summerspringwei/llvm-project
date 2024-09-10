set(LLVM_LINK_COMPONENTS
  Core
  IRReader
  Support
  )

add_llvm_tool(llvm-mdiff
  llvm-mdiff.cpp

  DEPENDS
  intrinsics_gen
  )

add_subdirectory(lib)
target_link_libraries(llvm-mdiff PRIVATE LLVMMDiff)
