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
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/Casting.h"
#include "llvm/IR/Instructions.h" // For llvm::Instruction
#include "llvm/IR/Type.h"         // For llvm::Type
#include "llvm/Support/raw_ostream.h" // For llvm::outs()
#include <sstream>
#include <vector>
#include <string>
#include <utility>
#include <iostream>
#include <memory>
#include <map>
#include <set>


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
/// in F's body.  If every argument is used, returns a single element {-1}.
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
      const StructType *STy = dyn_cast<StructType>(Arg.getType());
      if (STy) {
        if (STy->getNumElements() == 1) {
          // Recursively check if the only element is a basic type
          const Type *ElemTy = STy->getElementType(0);
          // A "basic type" is not a struct, array, or vector
          if (!ElemTy->isStructTy() && !ElemTy->isArrayTy() && !ElemTy->isVectorTy()) {
            return false; // Only one basic type, not considered a struct type
          }
        }
        return true; // Struct type with more than one element, or single non-basic element
      }
    }
  }
  return false; // No struct type arguments found
}

/// Checks if any instruction in the function accesses a StructType.
static bool hasInstructionAccessingStructType(const Function &F) {
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      // Check the instruction's type
      if (I.getType()->isStructTy()) {
        return true;
      }
      // Check all operands
      for (const auto &Operand : I.operands()) {
        if (Operand->getType()->isStructTy()) {
          return true;
        }
      }
    }
  }
  return false;
}

/// Checks if the module contains any defined struct types.
static bool hasDefinedStructTypes(const Module &M) {
  for (const Type *Ty : M.getIdentifiedStructTypes()) {
    if (!cast<StructType>(Ty)->isOpaque()) {
      return true; // Found a defined struct type
    }
  }
  return false; // No defined struct types found
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
        if (dyn_cast<GlobalVariable>(LI->getPointerOperand())) {
          return true; // Found a global variable being used
        }
      } else if (const StoreInst *SI = dyn_cast<StoreInst>(&I)) {
        if (dyn_cast<GlobalVariable>(SI->getPointerOperand())) {
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

static int getNumLoops(const Function &F) {
  FunctionAnalysisManager FAM;
  PassBuilder PB;
  PB.registerFunctionAnalyses(FAM);
  FAM.registerPass([] { return LoopAnalysis(); });
  auto &LI = FAM.getResult<LoopAnalysis>(const_cast<Function&>(F));
  int numLoops = 0;
  if(LI.empty())
    return 0;
  else {
    for(const Loop *L : LI) {
      (void)L; // Silence unused variable warning
      numLoops++;
    }
  }
  return numLoops;
}

std::vector<size_t> getBBsInstSize(const Function &F){
  std::vector<size_t> BBs;
  for(const BasicBlock &BB : F){
    BBs.push_back(BB.size());
  }
  return BBs;
}


/// Checks if all fields of accessed structs are used in the function.
static bool areAllStructFieldsAccessed(const Function &F) {
  // Map to track which struct types and their fields are accessed
  std::map<const StructType*, std::set<unsigned>> accessedFields;
  auto checkAndAddGEP = [&](const llvm::Value *V, std::string str) {
    // llvm::errs() << str << "In checkAndAddGEP Value: " << *V << "\n";
    if(auto *GV = cast<GlobalVariable>(V)) {
        Type *structTy = GV->getValueType();  // convenience for GV->getType()->getElementType()
        if(structTy && structTy->isStructTy()){
          const StructType *STy = cast<StructType>(structTy);
          accessedFields[STy];
          accessedFields[STy].insert(0);
          // llvm::errs() << str << "FieldIdx " << 0 << "\n";
        }
    }
    if (const llvm::GEPOperator *GEP = llvm::dyn_cast<llvm::GEPOperator>(V)) {
      //  llvm::errs() << str << "GEP: " << *GEP << "\n";
      //  llvm::errs() << "getPointerOperand" << *(GEP->getPointerOperand()) << "\n";
        // Get the source element type of the GEP.
        // This is the type of the pointed-to object *before* indexing.
        llvm::Type *SourceElementType = GEP->getSourceElementType();
        // Check if the source element type exists and is a struct type.
        if (SourceElementType && SourceElementType->isStructTy()) {
            const StructType *STy = cast<StructType>(SourceElementType);
            accessedFields[STy];
            if (GEP->getNumIndices() >= 2) {
              if (const ConstantInt *CI = dyn_cast<ConstantInt>(GEP->getOperand(2))) {
                unsigned FieldIdx = CI->getZExtValue();
                // llvm::errs() << str << "FieldIdx " << FieldIdx << "\n";
                // printf("%s FieldIdx %d\n", str, FieldIdx);
                accessedFields[STy].insert(FieldIdx);
              }
            }
        }
    }
  };

  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      // llvm::errs() << "Raw Inst: " << I << "\n";
      if (const llvm::LoadInst *Load = llvm::dyn_cast<llvm::LoadInst>(&I)){
        checkAndAddGEP(Load->getPointerOperand(), "Load ");
      }else if(const llvm::StoreInst *Store = llvm::dyn_cast<llvm::StoreInst>(&I)){
        char hint[] = "Store";
        checkAndAddGEP(Store->getPointerOperand(), "Store ");
      }else{
        char hint[] = "Inst:";
        checkAndAddGEP(&I, "Inst ");
      }
    }
  }

  // Second pass: verify all fields of each accessed struct are used
  for (const auto &StructFields : accessedFields) {
    const StructType *STy = StructFields.first;
    const std::set<unsigned> &Fields = StructFields.second;
    
    // Check if all fields of this struct type are accessed
    // printf("Struct Name: %s\n", STy->getName().data());
    // printf("Number of Elements: %d\n", STy->getNumElements());
    for (unsigned i = 0; i < STy->getNumElements(); ++i) {
      if (Fields.find(i) == Fields.end()) {
        return false; // Found a field that wasn't accessed
      }
    }
  }
  
  return true; // All fields of all accessed structs were used

}


// Main function to return JSON-like string using stringstream
std::string GetFunctionInfoJson(const Function &F) {
  std::stringstream OS;
  OS << "{";
  OS << "\"name\": \"" << F.getName().str() << "\",";
  // 1. Get unused arguments
  OS << "\"unused_args\": ";
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
  // 5. Check for defined struct types
  OS << ", \"has_defined_structs\": " << (hasDefinedStructTypes(*(F.getParent())) ? "true" : "false");
  // 6. Get number of loops
  OS << ", \"num_loops\": " << getNumLoops(F);
  // 7. Get number of BBs
  auto BBs = getBBsInstSize(F);
  OS << ", \"bbcount\": [";
  for(size_t i = 0; i < BBs.size(); ++i){
    if(i != 0) OS << ", ";
    OS << BBs[i];
  }
  OS << "]";
  OS << ", \"has_inst_structs:\": " << (hasInstructionAccessingStructType(F) ? "true": "false");
  OS << ", \"all_struct_fields_accessed\": " << (areAllStructFieldsAccessed(F) ? "true" : "false");
  OS << "}\n"; // End of function
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

  OS << "]}\n";
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
/*
cmake -G "Ninja" ../llvm \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_CXX_COMPILER=g++ \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="X86" \
*/
