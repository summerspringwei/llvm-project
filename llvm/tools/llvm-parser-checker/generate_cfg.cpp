#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Analysis/CFGPrinter.h"

#include <memory>
#include <string>
#include <filesystem>

using namespace llvm;

int main(int argc, char **argv) {
    if (argc < 2) {
        errs() << "Usage: " << argv[0] << " <input.ll>\n";
        return 1;
    }

    LLVMContext Context;
    SMDiagnostic Err;
    std::unique_ptr<Module> M = parseIRFile(argv[1], Err, Context);

    if (!M) {
        Err.print(argv[0], errs());
        return 1;
    }

    for (Function &F : *M) {
        if (F.isDeclaration())
            continue;

        std::string Filename = F.getName().str() + ".dot";
        std::error_code EC;
        raw_fd_ostream File(Filename, EC);

        if (EC) {
            errs() << "Could not open file: " << EC.message() << "\n";
            return 1;
        }

        WriteGraph(File, (const Function*)&F, false);
        outs() << "Wrote CFG to " << Filename << "\n";
    }

    return 0;
}
