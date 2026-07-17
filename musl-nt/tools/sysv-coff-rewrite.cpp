/*
 * Keep musl's LP64/System-V calling convention while changing only the
 * object format to Win64 COFF.  Merely overriding llc's triple would make
 * ordinary calls use Win64, while clang had already laid out va_list for
 * System V; printf would then read garbage.  Mark every non-intrinsic call
 * and function explicitly before COFF lowering so the frontend and backend
 * agree, including indirect calls through FILE callbacks and qsort hooks.
 */
#include <system_error>

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        llvm::errs() << "usage: sysv-coff-rewrite INPUT.bc OUTPUT.bc\n";
        return 2;
    }

    llvm::LLVMContext context;
    llvm::SMDiagnostic diagnostic;
    std::unique_ptr<llvm::Module> module =
        llvm::parseIRFile(argv[1], diagnostic, context);
    if (!module) {
        diagnostic.print(argv[0], llvm::errs());
        return 1;
    }

    for (llvm::Function &function : *module)
        if (!function.isIntrinsic())
            function.setCallingConv(llvm::CallingConv::X86_64_SysV);

    for (llvm::Function &function : *module) {
        if (function.isDeclaration()) continue;
        for (llvm::BasicBlock &block : function) {
            for (llvm::Instruction &instruction : block) {
                auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (!call) continue;
                llvm::Value *callee = call->getCalledOperand()->stripPointerCasts();
                if (llvm::isa<llvm::InlineAsm>(callee)) continue;
                if (auto *target = llvm::dyn_cast<llvm::Function>(callee))
                    if (target->isIntrinsic()) continue;
                call->setCallingConv(llvm::CallingConv::X86_64_SysV);
            }
        }
    }

    std::error_code error;
    llvm::raw_fd_ostream output(argv[2], error, llvm::sys::fs::OF_None);
    if (error) {
        llvm::errs() << argv[2] << ": " << error.message() << "\n";
        return 1;
    }
    llvm::WriteBitcodeToFile(*module, output);
    return 0;
}
