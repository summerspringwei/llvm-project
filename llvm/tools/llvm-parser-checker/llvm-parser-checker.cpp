//===-- llvm-diff.cpp - Module comparator command-line driver ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the command-line driver for the difference engine.
//
//===----------------------------------------------------------------------===//

// #include "lib/DiffLog.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/WithColor.h"
#include <sstream>
#include <vector>
#include <string>
#include <utility>
#include <iostream>

// cmake -S llvm -B mybuilddir -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_PARALLEL_JOBS=40
using namespace llvm;

/// Reads a module from a file.  On error, messages are written to stderr
/// and null is returned.
static std::unique_ptr<Module> readModule(LLVMContext &Context,
                                          StringRef Name) {
  SMDiagnostic Diag;
  std::unique_ptr<Module> M = parseIRFile(Name, Diag, Context);
  if (!M)
    Diag.print("llvm-diff", errs());
  return M;
}


/// Returns a vector of 0‐based argument indices that are never used
/// in F’s body.  If every argument is used, returns a single element {-1}.
static std::vector<int> getUnusedArgIndices(const Function &F) {
  std::vector<int> Unused;
  int Idx = 0;
  for (const Argument &Arg : F.args()) {
    if (Arg.use_empty())
      Unused.push_back(Idx);
    ++Idx;
  }
  if (Unused.empty())
    return { -1 };
  return Unused;
}

// Main function to return JSON-like string using stringstream
std::string GetModuleFunctionsUnusedArgs(std::unique_ptr<Module> &LeftM) {
  std::stringstream OS;
  OS << "{";
  bool firstFunc = true;

  for (Function &F : *LeftM) {
    if (F.isDeclaration()) continue;

    auto Unused = getUnusedArgIndices(F);

    if (!firstFunc) {
      OS << ", ";
    }
    firstFunc = false;

    OS << "\"" << F.getName().str() << "\": ";

    if (Unused.size() == 1 && Unused[0] == -1) {
      OS << "[]"; // All arguments used
    } else {
      OS << "[";
      for (size_t i = 0; i < Unused.size(); ++i) {
        if (i != 0) OS << ", ";
        OS << Unused[i];
      }
      OS << "]";
    }
  }

  OS << "}";
  return OS.str();
}


/// Checks if any argument of the given function is of struct type.
static bool hasStructTypeArgument(const Function &F) {
  for (const Argument &Arg : F.args()) {
    if (Arg.getType()->isStructTy()) {
      return true; // Found a struct type argument
    }
  }
  return false; // No struct type arguments found
}

/// Checks if the module contains any global variables.
static bool hasGlobalVariables(const Module &M) {
  for (const GlobalVariable &GV : M.globals()) {
    if (!GV.isDeclaration()) {
      return true; // Found a global variable
    }
  }
  return false; // No global variables found
}


/// Checks if any global variables are used within the body of the given function.
static bool usesGlobalVariables(const Function &F) {
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (const LoadInst *LI = dyn_cast<LoadInst>(&I)) {
        if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(LI->getPointerOperand())) {
          return true; // Found a global variable being used
        }
      } else if (const StoreInst *SI = dyn_cast<StoreInst>(&I)) {
        if (const GlobalVariable *GV = dyn_cast<GlobalVariable>(SI->getPointerOperand())) {
          return true; // Found a global variable being used
        }
      }
    }
  }
  return false; // No global variables used
}


/// Returns a vector of function names that are called within the body of the given function.
static std::vector<std::string> getCalledFunctions(const Function &F) {
  std::vector<std::string> CalledFunctions;

  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (const CallBase *CB = dyn_cast<CallBase>(&I)) {
        if (const Function *Callee = CB->getCalledFunction()) {
          CalledFunctions.push_back(Callee->getName().str());
        }
      }
    }
  }

  return CalledFunctions;
}


// Main function to return JSON-like string using stringstream
std::string GetFunctionInfoJson(const Function &F) {

  std::stringstream OS;
  OS << "{";

  OS << "\"" << F.getName().str() << "\": ";
  // 1. Get unused arguments
  OS << "{\"unused_args\": ";
  auto Unused = getUnusedArgIndices(F);
  if (Unused.size() == 1 && Unused[0] == -1) {
    OS << "[]"; // All arguments used
  } else {
    OS << "[";
    for (size_t i = 0; i < Unused.size(); ++i) {
      if (i != 0) OS << ", ";
      OS << Unused[i];
    }
    OS << "]";
  }
  // 2. Check for struct type arguments
  OS << ", \"struct_args\": " << (hasStructTypeArgument(F) ? "true" : "false");
  // 3. Check for global variables
  OS << ", \"has_globals\": " << (usesGlobalVariables(F) ? "true" : "false");
  // 4. Get called functions
  OS << ", \"called_functions\": [";
  auto CalledFunctions = getCalledFunctions(F);
  for (size_t i = 0; i < CalledFunctions.size(); ++i) {
    if (i != 0) OS << ", ";
    OS << "\"" << CalledFunctions[i] << "\"";
  }
  OS << "]"; // End of called functions

  OS << "}"; // End of function info
  OS << "}"; // End of function
  return OS.str();
}

std::string GetModuleJson(std::unique_ptr<Module> &LeftM) {
  std::stringstream OS;
  OS << "{\"functions\":[";
  bool firstFunc = true;

  for (Function &F : *LeftM) {
    if (F.isDeclaration()) continue;

    if (!firstFunc) {
      OS << ", ";
    }
    firstFunc = false;

    OS << GetFunctionInfoJson(F);
  }

  OS << "]}";
  return OS.str();
}


cl::OptionCategory DiffCategory("Diff Options");

static cl::opt<std::string> LeftFilename(cl::Positional,
                                         cl::desc("<first file>"), cl::Required,
                                         cl::cat(DiffCategory));


int main(int argc, char **argv) {
  cl::HideUnrelatedOptions({&DiffCategory, &getColorCategory()});
  cl::ParseCommandLineOptions(argc, argv);

  LLVMContext Context;
  std::unique_ptr<Module> LeftM = readModule(Context, LeftFilename);
  std::cout << GetModuleJson(LeftM);

  return 0;
}
