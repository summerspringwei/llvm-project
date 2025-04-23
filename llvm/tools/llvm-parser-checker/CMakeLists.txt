set(LLVM_LINK_COMPONENTS
  Core
  IRReader
  Support
  )

add_llvm_tool(llvm-parser-checker
  llvm-parser-checker.cpp

  DEPENDS
  intrinsics_gen
  )

# add_subdirectory(lib)
# target_link_libraries(llvm-parser-checker PRIVATE LLVMMDiff)
