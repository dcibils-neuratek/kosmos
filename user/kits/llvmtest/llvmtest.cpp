/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **LLVM on Kosmos** (`docs/gpu3d.md` G1): what `x86-llvm` asks of LLVM's
 * libraries, said as one line.
 *
 * - `jit=`: a module in LLVM's text form, parsed, compiled by the ORC JIT
 *   for the processor it runs on, and its functions called. `squares(n)`
 *   sums the squares below `n`, modulo a prime, in a loop - `n` comes from
 *   the caller at run time, so nothing could have been folded ahead of it -
 *   and `hypot(3, 4)` takes the square root through LLVM's intrinsic, so
 *   the code is floating point as a shader's is. The code is written into
 *   memory that is writable and then made executable, never both at once
 *   (`runtime/posix/mman.c`).
 * - `other=`: the same module written as an object for the other
 *   processor - AArch64 from x86-64, and the reverse - and its size in
 *   bytes, which says the second back end is in the image and works.
 *
 * Every error is said in the line rather than thrown: LLVM is built
 * without exceptions. The process's own symbols are not linked against,
 * because LLJIT reaches them through a dynamic loader Kosmos does not
 * have, and nothing compiled here calls out.
 */

#include <stdio.h>
#include <string.h>

#include "llvm/AsmParser/Parser.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

static const char *MODULE = R"(
define i64 @squares(i64 %n) {
entry:
  %none = icmp eq i64 %n, 0
  br i1 %none, label %out, label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  %s = phi i64 [ 0, %entry ], [ %kept, %loop ]
  %square = mul i64 %i, %i
  %sum = add i64 %s, %square
  %kept = urem i64 %sum, 1000003
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, %n
  br i1 %done, label %out, label %loop
out:
  %r = phi i64 [ 0, %entry ], [ %kept, %loop ]
  ret i64 %r
}

declare double @llvm.sqrt.f64(double)

define double @hypot(double %a, double %b) {
  %aa = fmul double %a, %a
  %bb = fmul double %b, %b
  %sum = fadd double %aa, %bb
  %r = call double @llvm.sqrt.f64(double %sum)
  ret double %r
}
)";

static std::unique_ptr<Module> parse(LLVMContext &context, std::string &error)
{
    SMDiagnostic diagnostic;
    std::unique_ptr<Module> module = parseAssemblyString(MODULE, diagnostic, context);
    if (!module) {
        raw_string_ostream(error) << "parse:" << diagnostic.getMessage();
        return nullptr;
    }
    if (verifyModule(*module, nullptr)) {
        error = "verify";
        return nullptr;
    }
    return module;
}

/* The JIT's half: the module compiled for this processor and called. */
static std::string jit(long n)
{
    /* No symbols from the process itself: LLJIT finds those through the
     * dynamic loader, and Kosmos has none. What compiled code calls is
     * handed to the JIT by name, as lavapipe hands its helpers - and so
     * the inactive platform, since the generic one runs a module's static
     * constructors through those symbols, and a shader has none. */
    auto made = orc::LLJITBuilder()
                    .setLinkProcessSymbolsByDefault(false)
                    .setPlatformSetUp(orc::setUpInactivePlatform)
                    .create();
    if (!made) {
        return "jit=create:" + toString(made.takeError());
    }
    std::unique_ptr<orc::LLJIT> engine = std::move(*made);

    auto context = std::make_unique<LLVMContext>();
    std::string error;
    std::unique_ptr<Module> module = parse(*context, error);
    if (!module) {
        return "jit=" + error;
    }
    if (Error e = engine->addIRModule(orc::ThreadSafeModule(std::move(module), std::move(context)))) {
        return "jit=add:" + toString(std::move(e));
    }

    auto squares = engine->lookup("squares");
    if (!squares) {
        return "jit=lookup:" + toString(squares.takeError());
    }
    auto hypot = engine->lookup("hypot");
    if (!hypot) {
        return "jit=lookup:" + toString(hypot.takeError());
    }

    long s = squares->toPtr<long (*)(long)>()(n);
    double h = hypot->toPtr<double (*)(double, double)>()(3.0, 4.0);
    char said[96];
    snprintf(said, sizeof(said), "jit=%s/squares(%ld)=%ld/hypot(3,4)=%.3f",
             engine->getTargetTriple().getArchName().str().c_str(), n, s, h);
    return said;
}

/* The other back end's half: an object for the processor this is not. */
static std::string other()
{
#if defined(__x86_64__)
    const char *name = "aarch64-unknown-none-elf";
#else
    const char *name = "x86_64-unknown-none-elf";
#endif
    Triple triple(name);
    std::string error;
    const Target *target = TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        return "other=target:" + error;
    }
    TargetOptions options;
    std::unique_ptr<TargetMachine> machine(
        target->createTargetMachine(triple, "generic", "", options, Reloc::Static));
    if (!machine) {
        return "other=machine";
    }

    LLVMContext context;
    std::unique_ptr<Module> module = parse(context, error);
    if (!module) {
        return "other=" + error;
    }
    module->setTargetTriple(triple);
    module->setDataLayout(machine->createDataLayout());

    SmallVector<char, 0> bytes;
    raw_svector_ostream out(bytes);
    legacy::PassManager passes;
    if (machine->addPassesToEmitFile(passes, out, nullptr, CodeGenFileType::ObjectFile)) {
        return "other=emit";
    }
    passes.run(*module);

    /* An ELF object begins with its magic, and says its machine at 18:
     * 183 is AArch64's, 62 is x86-64's. */
    bool elf = bytes.size() > 20 && memcmp(bytes.data(), "\177ELF", 4) == 0;
    unsigned machine_id = elf ? (unsigned char)bytes[18] | ((unsigned char)bytes[19] << 8) : 0;
    char said[96];
    snprintf(said, sizeof(said), "other=%s/elf-machine-%u/%s", triple.getArchName().str().c_str(),
             machine_id, bytes.size() > 200 ? "object" : "too-small");
    return said;
}

extern "C" int kosmos_llvmtest_run(long n, char *out, unsigned long size)
{
    InitializeAllTargetInfos();
    InitializeAllTargets();
    InitializeAllTargetMCs();
    InitializeAllAsmPrinters();

    std::string said = "llvm " + jit(n) + " " + other();
    snprintf(out, size, "%s", said.c_str());
    return 0;
}
