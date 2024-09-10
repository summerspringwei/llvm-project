set(LLVM_LINK_COMPONENTS
    Core
    Support
    )

add_llvm_library(LLVMMDiff
  STATIC
  DiffConsumer.cpp
  DiffConsumer.h
  DifferenceEngine.cpp
  DifferenceEngine.h
  DiffLog.cpp
  DiffLog.h
  )
