// maincompile.cpp — BRAINFUCK → LLVM IR → JIT COMPILER
//
// Compiles BF source → LLVM IR (with our idiom opts) → LLVM pass pipeline → JIT.
//
// Our optimizations (same as mainfast.cpp):
//   • Run-length collapsing: ++++++ → ADD 6,  >>>> → RIGHT 4
//   • [-]  → CLEAR,  [>] → SCAN_RIGHT,  [<] → SCAN_LEFT
//   • [-]+N / [-] -N → SET_VALUE  (clear then set to N)
//   • Generic MOVE_ADD: tape[p+off] += tape[p]*cnt; tape[p]=0
//   • Generic DISTRIBUTE: distribute to two target cells
//
// LLVM handles: mem2reg, instcombine, simplifycfg, loop opts, inlining of putchar etc.
//
// Runtime: LLVM ORC JIT v2.

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

using namespace llvm;
using namespace llvm::orc;

// ── Opcodes ─────────────────────────────────────────────────────────
// +, -, >, <  carry count in a1
// . , are I/O
// [ ] are loops with matching index in a1
// C=clear, S=set_value(a1), R=scan_right, L=scan_left,
// M=move_add(offset=a1, count=a2), D=distribute(off1=a1,c1=a2,off2=a3,c2=a4)
struct Op { char kind; int a1, a2, a3, a4; };

// ── Static idiom detectors ─────────────────────────────────────────

static inline int match_clear(const char* s, int ip, int size) {
    return (ip + 2 < size && s[ip+1] == '-' && s[ip+2] == ']') ? 3 : 0;
}
static inline int match_scan_right(const char* s, int ip, int size) {
    return (ip + 2 < size && s[ip+1] == '>' && s[ip+2] == ']') ? 3 : 0;
}
static inline int match_scan_left(const char* s, int ip, int size) {
    return (ip + 2 < size && s[ip+1] == '<' && s[ip+2] == ']') ? 3 : 0;
}

static inline int match_set_value(const char* s, int ip, int size, int* cnt) {
    if (ip + 3 >= size) return 0;
    if (s[ip+1] != '-' || s[ip+2] != ']') return 0;
    int pos = ip + 3, ct = 0;
    if (s[pos] == '+') ct = 1;
    else if (s[pos] == '-') ct = -1;
    else return 0;
    char dir = s[pos]; pos++;
    while (pos < size && s[pos] == dir) { ct += (dir == '+') ? 1 : -1; pos++; }
    *cnt = ct;
    return pos - ip;
}

static inline int match_move_add_right(const char* s, int ip, int size, int* off, int* cnt) {
    int pos = ip + 2, mv = 0;
    while (pos < size && s[pos] == '>') { mv++; pos++; }
    if (mv == 0) return 0;
    int ct = 0;
    while (pos < size && s[pos] == '+') { ct++; pos++; }
    if (ct == 0) return 0;
    int ret = 0;
    while (pos < size && s[pos] == '<') { ret++; pos++; }
    if (ret != mv) return 0;
    if (pos >= size || s[pos] != ']') return 0;
    *off = mv; *cnt = ct;
    return pos - ip + 1;
}

static inline int match_move_add_left(const char* s, int ip, int size, int* off, int* cnt) {
    int pos = ip + 2, mv = 0;
    while (pos < size && s[pos] == '<') { mv++; pos++; }
    if (mv == 0) return 0;
    int ct = 0;
    while (pos < size && s[pos] == '+') { ct++; pos++; }
    if (ct == 0) return 0;
    int ret = 0;
    while (pos < size && s[pos] == '>') { ret++; pos++; }
    if (ret != mv) return 0;
    if (pos >= size || s[pos] != ']') return 0;
    *off = -mv; *cnt = ct;
    return pos - ip + 1;
}

static inline int match_distribute_right(const char* s, int ip, int size,
                                          int* o1, int* c1, int* o2, int* c2) {
    int pos = ip + 2, total = 0;
    int mv1 = 0; while (pos < size && s[pos] == '>') { mv1++; pos++; }
    if (mv1 == 0) return 0; total += mv1;
    int ct1 = 0; while (pos < size && s[pos] == '+') { ct1++; pos++; }
    if (ct1 == 0) return 0;
    int mv2 = 0; while (pos < size && s[pos] == '>') { mv2++; pos++; }
    if (mv2 == 0) return 0; total += mv2;
    int ct2 = 0; while (pos < size && s[pos] == '+') { ct2++; pos++; }
    if (ct2 == 0) return 0;
    int ret = 0; while (pos < size && s[pos] == '<') { ret++; pos++; }
    if (ret != total) return 0;
    if (pos >= size || s[pos] != ']') return 0;
    *o1 = mv1; *c1 = ct1; *o2 = mv1 + mv2; *c2 = ct2;
    return pos - ip + 1;
}

static inline int match_distribute_left(const char* s, int ip, int size,
                                         int* o1, int* c1, int* o2, int* c2) {
    int pos = ip + 2, total = 0;
    int mv1 = 0; while (pos < size && s[pos] == '<') { mv1++; pos++; }
    if (mv1 == 0) return 0; total += mv1;
    int ct1 = 0; while (pos < size && s[pos] == '+') { ct1++; pos++; }
    if (ct1 == 0) return 0;
    int mv2 = 0; while (pos < size && s[pos] == '<') { mv2++; pos++; }
    if (mv2 == 0) return 0; total += mv2;
    int ct2 = 0; while (pos < size && s[pos] == '+') { ct2++; pos++; }
    if (ct2 == 0) return 0;
    int ret = 0; while (pos < size && s[pos] == '>') { ret++; pos++; }
    if (ret != total) return 0;
    if (pos >= size || s[pos] != ']') return 0;
    *o1 = -mv1; *c1 = ct1; *o2 = -(mv1 + mv2); *c2 = ct2;
    return pos - ip + 1;
}

// ── Parse BF source → flat Op vector ───────────────────────────────
static int parse(const char* src, int size, std::vector<Op>& ops) {
    int ip = 0;
    while (ip < size) {
        unsigned char c = src[ip];

        // Run-length collapse: + - > <
        if (c == '+' || c == '-' || c == '>' || c == '<') {
            int cnt = 1;
            while (ip + cnt < size && src[ip + cnt] == c) cnt++;
            ops.push_back({(char)c, cnt, 0, 0, 0});
            ip += cnt; continue;
        }
        if (c == '.') { ops.push_back({'.', 0, 0, 0, 0}); ip++; continue; }
        if (c == ',') { ops.push_back({',', 0, 0, 0, 0}); ip++; continue; }

        // Bracket patterns
        if (c == '[') {
            int o1, c1, o2, c2, span, off, cnt;

            span = match_distribute_left(src, ip, size, &o1,&c1, &o2,&c2);
            if (span) { ops.push_back({'D', o1, c1, o2, c2}); ip += span; continue; }
            span = match_distribute_right(src, ip, size, &o1,&c1, &o2,&c2);
            if (span) { ops.push_back({'D', o1, c1, o2, c2}); ip += span; continue; }
            span = match_move_add_left(src, ip, size, &off, &cnt);
            if (span) { ops.push_back({'M', off, cnt, 0, 0}); ip += span; continue; }
            span = match_move_add_right(src, ip, size, &off, &cnt);
            if (span) { ops.push_back({'M', off, cnt, 0, 0}); ip += span; continue; }
            span = match_set_value(src, ip, size, &cnt);
            if (span) { ops.push_back({'S', cnt, 0, 0, 0}); ip += span; continue; }
            span = match_scan_right(src, ip, size);
            if (span) { ops.push_back({'R', 0, 0, 0, 0}); ip += span; continue; }
            span = match_scan_left(src, ip, size);
            if (span) { ops.push_back({'L', 0, 0, 0, 0}); ip += span; continue; }
            span = match_clear(src, ip, size);
            if (span) { ops.push_back({'C', 0, 0, 0, 0}); ip += span; continue; }

            ops.push_back({'[', 0, 0, 0, 0}); ip++; continue;
        }
        if (c == ']') { ops.push_back({']', 0, 0, 0, 0}); ip++; continue; }

        ip++; // comment
    }
    return ops.size();
}

// ── Bracket matching ────────────────────────────────────────────────
static bool match_brackets(std::vector<Op>& ops) {
    std::vector<int> stack;
    for (int i = 0; i < (int)ops.size(); i++) {
        if (ops[i].kind == '[') {
            stack.push_back(i);
        } else if (ops[i].kind == ']') {
            if (stack.empty()) return false;
            int j = stack.back(); stack.pop_back();
            ops[i].a1 = j;      // ] → matching [
            ops[j].a1 = i;      // [ → matching ]
        }
    }
    return stack.empty();
}

// ── Generate LLVM IR ────────────────────────────────────────────────
static void generate_ir(const std::vector<Op>& ops,
                         LLVMContext& ctx, Module& mod,
                         Function* func) {
    IRBuilder<> b(ctx);
    int n = ops.size();

    // ── Globals / allocas ──────────────────────────────────────────
    // Tape: global [30000 x i8]
    auto* tapeTy = ArrayType::get(Type::getInt8Ty(ctx), 30000);
    auto* tape = new GlobalVariable(mod, tapeTy, false,
        GlobalValue::InternalLinkage,
        ConstantAggregateZero::get(tapeTy), "tape");

    // ptr: alloca i32 in entry block
    auto* entry = BasicBlock::Create(ctx, "entry", func);
    b.SetInsertPoint(entry);
    auto* ptr = b.CreateAlloca(Type::getInt32Ty(ctx), nullptr, "ptr");
    b.CreateStore(ConstantInt::get(Type::getInt32Ty(ctx), 0), ptr);

    // Declare putchar / getchar
    auto* putcharTy = FunctionType::get(Type::getInt32Ty(ctx),
        {Type::getInt32Ty(ctx)}, false);
    auto* putcharFn = Function::Create(putcharTy,
        Function::ExternalLinkage, "putchar", mod);

    auto* getcharTy = FunctionType::get(Type::getInt32Ty(ctx), {}, false);
    auto* getcharFn = Function::Create(getcharTy,
        Function::ExternalLinkage, "getchar", mod);

    // ── Create all basic blocks ────────────────────────────────────
    std::vector<BasicBlock*> bb(n + 1);
    for (int i = 0; i <= n; i++)
        bb[i] = BasicBlock::Create(ctx, "bb" + std::to_string(i), func);

    // Exit block returns void
    b.SetInsertPoint(bb[n]);
    b.CreateRetVoid();

    // Entry block branches to first real block
    b.SetInsertPoint(entry);
    b.CreateBr(bb[0]);

    // Helper lambdas (hoisted outside the loop)
    auto load_cell = [&]() -> Value* {
        Value* p = b.CreateLoad(Type::getInt32Ty(ctx), ptr, "p");
        auto* gep = b.CreateInBoundsGEP(tapeTy, tape,
            {ConstantInt::get(Type::getInt32Ty(ctx), 0), p}, "cellptr");
        return b.CreateLoad(Type::getInt8Ty(ctx), gep, "cell");
    };
    auto store_cell = [&](Value* v) {
        Value* p = b.CreateLoad(Type::getInt32Ty(ctx), ptr, "p");
        auto* gep = b.CreateInBoundsGEP(tapeTy, tape,
            {ConstantInt::get(Type::getInt32Ty(ctx), 0), p}, "cellptr");
        b.CreateStore(v, gep);
    };
    auto adjust_ptr = [&](int delta) {
        Value* p = b.CreateLoad(Type::getInt32Ty(ctx), ptr, "p");
        p = b.CreateAdd(p, ConstantInt::get(Type::getInt32Ty(ctx), delta));
        p = b.CreateSelect(
            b.CreateICmpSGT(p, ConstantInt::get(Type::getInt32Ty(ctx), 29999)),
            ConstantInt::get(Type::getInt32Ty(ctx), 29999), p);
        p = b.CreateSelect(
            b.CreateICmpSLT(p, ConstantInt::get(Type::getInt32Ty(ctx), 0)),
            ConstantInt::get(Type::getInt32Ty(ctx), 0), p);
        b.CreateStore(p, ptr);
    };

    // ── Fill each block ────────────────────────────────────────────
    for (int i = 0; i < n; i++) {
        b.SetInsertPoint(bb[i]);
        const Op& op = ops[i];
        char k = op.kind;

        switch (k) {
        case '+': {
            auto* v = load_cell();
            v = b.CreateAdd(v, ConstantInt::get(Type::getInt8Ty(ctx), op.a1));
            store_cell(v);
            b.CreateBr(bb[i+1]);
            break;
        }
        case '-': {
            auto* v = load_cell();
            v = b.CreateSub(v, ConstantInt::get(Type::getInt8Ty(ctx), op.a1));
            store_cell(v);
            b.CreateBr(bb[i+1]);
            break;
        }
        case '>':
            adjust_ptr(op.a1);
            b.CreateBr(bb[i+1]);
            break;
        case '<':
            adjust_ptr(-op.a1);
            b.CreateBr(bb[i+1]);
            break;
        case '.': {
            auto* v = load_cell();
            v = b.CreateZExt(v, Type::getInt32Ty(ctx));
            b.CreateCall(putcharFn, {v});
            b.CreateBr(bb[i+1]);
            break;
        }
        case ',': {
            Value* v = b.CreateCall(getcharFn, {});
            v = b.CreateTrunc(v, Type::getInt8Ty(ctx));
            store_cell(v);
            b.CreateBr(bb[i+1]);
            break;
        }
        case '[': {
            auto* v = load_cell();
            auto* cmp = b.CreateICmpNE(v,
                ConstantInt::get(Type::getInt8Ty(ctx), 0));
            b.CreateCondBr(cmp, bb[i+1], bb[op.a1 + 1]);
            break;
        }
        case ']':
            b.CreateBr(bb[op.a1]);
            break;
        case 'C':  // CLEAR
            store_cell(ConstantInt::get(Type::getInt8Ty(ctx), 0));
            b.CreateBr(bb[i+1]);
            break;
        case 'S':  // SET_VALUE
            store_cell(ConstantInt::get(Type::getInt8Ty(ctx), op.a1));
            b.CreateBr(bb[i+1]);
            break;
        case 'R': { // SCAN_RIGHT
            auto* loopHdr = BasicBlock::Create(ctx, "scanR_hdr", func);
            auto* loopBody = BasicBlock::Create(ctx, "scanR_body", func);
            auto* loopExit = bb[i+1];
            b.CreateBr(loopHdr);

            b.SetInsertPoint(loopHdr);
            auto* v = load_cell();
            auto* cmp = b.CreateICmpNE(v,
                ConstantInt::get(Type::getInt8Ty(ctx), 0));
            b.CreateCondBr(cmp, loopBody, loopExit);

            b.SetInsertPoint(loopBody);
            adjust_ptr(1);
            b.CreateBr(loopHdr);
            break;
        }
        case 'L': { // SCAN_LEFT
            auto* loopHdr = BasicBlock::Create(ctx, "scanL_hdr", func);
            auto* loopBody = BasicBlock::Create(ctx, "scanL_body", func);
            auto* loopExit = bb[i+1];
            b.CreateBr(loopHdr);

            b.SetInsertPoint(loopHdr);
            auto* v = load_cell();
            auto* cmp = b.CreateICmpNE(v,
                ConstantInt::get(Type::getInt8Ty(ctx), 0));
            b.CreateCondBr(cmp, loopBody, loopExit);

            b.SetInsertPoint(loopBody);
            adjust_ptr(-1);
            b.CreateBr(loopHdr);
            break;
        }
        case 'M': { // MOVE_ADD: tape[p+a1] += tape[p]*a2; tape[p]=0
            auto* val = load_cell();
            // target = p + a1
            Value* p = b.CreateLoad(Type::getInt32Ty(ctx), ptr, "p");
            Value* tp = b.CreateAdd(p, ConstantInt::get(Type::getInt32Ty(ctx), op.a1));
            tp = b.CreateSelect(
                b.CreateICmpSGT(tp, ConstantInt::get(Type::getInt32Ty(ctx), 29999)),
                ConstantInt::get(Type::getInt32Ty(ctx), 29999), tp);
            tp = b.CreateSelect(
                b.CreateICmpSLT(tp, ConstantInt::get(Type::getInt32Ty(ctx), 0)),
                ConstantInt::get(Type::getInt32Ty(ctx), 0), tp);
            auto* tgep = b.CreateInBoundsGEP(tapeTy, tape,
                {ConstantInt::get(Type::getInt32Ty(ctx), 0), tp}, "tcellptr");
            Value* tv = b.CreateLoad(Type::getInt8Ty(ctx), tgep, "tcell");
            tv = b.CreateAdd(tv, b.CreateMul(val,
                ConstantInt::get(Type::getInt8Ty(ctx), op.a2)));
            b.CreateStore(tv, tgep);
            store_cell(ConstantInt::get(Type::getInt8Ty(ctx), 0));
            b.CreateBr(bb[i+1]);
            break;
        }
        case 'D': { // DISTRIBUTE: tape[p+a1]+=val*a2; tape[p+a3]+=val*a4; tape[p]=0
            auto* val = load_cell();
            Value* p = b.CreateLoad(Type::getInt32Ty(ctx), ptr, "p");
            // target 1
            Value* t1 = b.CreateAdd(p, ConstantInt::get(Type::getInt32Ty(ctx), op.a1));
            t1 = b.CreateSelect(b.CreateICmpSGT(t1,
                ConstantInt::get(Type::getInt32Ty(ctx), 29999)),
                ConstantInt::get(Type::getInt32Ty(ctx), 29999), t1);
            t1 = b.CreateSelect(b.CreateICmpSLT(t1,
                ConstantInt::get(Type::getInt32Ty(ctx), 0)),
                ConstantInt::get(Type::getInt32Ty(ctx), 0), t1);
            auto* g1 = b.CreateInBoundsGEP(tapeTy, tape,
                {ConstantInt::get(Type::getInt32Ty(ctx), 0), t1});
            Value* v1 = b.CreateLoad(Type::getInt8Ty(ctx), g1);
            v1 = b.CreateAdd(v1, b.CreateMul(val,
                ConstantInt::get(Type::getInt8Ty(ctx), op.a2)));
            b.CreateStore(v1, g1);
            // target 2
            Value* t2 = b.CreateAdd(p, ConstantInt::get(Type::getInt32Ty(ctx), op.a3));
            t2 = b.CreateSelect(b.CreateICmpSGT(t2,
                ConstantInt::get(Type::getInt32Ty(ctx), 29999)),
                ConstantInt::get(Type::getInt32Ty(ctx), 29999), t2);
            t2 = b.CreateSelect(b.CreateICmpSLT(t2,
                ConstantInt::get(Type::getInt32Ty(ctx), 0)),
                ConstantInt::get(Type::getInt32Ty(ctx), 0), t2);
            auto* g2 = b.CreateInBoundsGEP(tapeTy, tape,
                {ConstantInt::get(Type::getInt32Ty(ctx), 0), t2});
            Value* v2 = b.CreateLoad(Type::getInt8Ty(ctx), g2);
            v2 = b.CreateAdd(v2, b.CreateMul(val,
                ConstantInt::get(Type::getInt8Ty(ctx), op.a4)));
            b.CreateStore(v2, g2);
            store_cell(ConstantInt::get(Type::getInt8Ty(ctx), 0));
            b.CreateBr(bb[i+1]);
            break;
        }
        }
    }
}

// ── Run LLVM optimization passes ────────────────────────────────────
static void optimize_module(Module& mod) {
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;

    PassBuilder PB;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(
        OptimizationLevel::O2);
    MPM.run(mod, MAM);
}

// ── Compile module to object file and link into executable ─────────
static bool compile_to_executable(Module& mod,
                                   const std::string& outPath) {
    auto targetTriple = Triple(sys::getDefaultTargetTriple());
    std::string error;
    auto* target = TargetRegistry::lookupTarget(targetTriple.str(), error);
    if (!target) {
        fprintf(stderr, "error: %s\n", error.c_str());
        return false;
    }

    TargetOptions opt;
    auto RM = std::optional<Reloc::Model>();
    auto TM = std::unique_ptr<TargetMachine>(
        target->createTargetMachine(targetTriple, "generic", "", opt, RM));

    mod.setDataLayout(TM->createDataLayout());
    mod.setTargetTriple(targetTriple);

    // Write object file to temp .o
    std::string tmpObj = outPath + ".o";
    std::error_code EC;
    raw_fd_ostream dest(tmpObj, EC, sys::fs::OF_None);
    if (EC) {
        fprintf(stderr, "error: could not open %s\n", tmpObj.c_str());
        return false;
    }

    legacy::PassManager pass;
    if (TM->addPassesToEmitFile(pass, dest, nullptr, CodeGenFileType::ObjectFile)) {
        dest.close();
        unlink(tmpObj.c_str());
        fprintf(stderr, "error: target does not support object file emission\n");
        return false;
    }
    pass.run(mod);
    dest.flush();
    dest.close();

    // Link with clang to produce executable
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "clang -o \"%s\" \"%s\"",
             outPath.c_str(), tmpObj.c_str());
    int rc = system(cmd);
    unlink(tmpObj.c_str());
    if (rc != 0) {
        fprintf(stderr, "error: linking failed\n");
        return false;
    }
    return true;
}

// ── JIT compile and execute ────────────────────────────────────────
static ExitOnError ExitOnErr;

static void jit_execute(std::unique_ptr<Module> mod, std::unique_ptr<LLVMContext> ctx) {
    auto J = ExitOnErr(LLJITBuilder().create());
    ExitOnErr(J->addIRModule(ThreadSafeModule(std::move(mod), std::move(ctx))));
    auto addr = ExitOnErr(J->lookup("main"));
    auto* bf_main = addr.toPtr<void()>();
    bf_main();
    putchar('\n');
}

// ── Print help ──────────────────────────────────────────────────────
static void print_help(const char* prog) {
    fprintf(stderr, "Usage: %s <file.bf> [run]\n", prog);
    fprintf(stderr, "  LLVM JIT compiler for brainfuck.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  Without 'run': compiles to <file>.out (linked executable)\n");
    fprintf(stderr, "  With 'run':    compiles and JIT-executes immediately\n");
}

// ── Main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        print_help(argc > 0 ? argv[0] : "bfc");
        return 1;
    }

    bool do_run = false;
    const char* filepath = argv[1];
    if (argc >= 3 && strcmp(argv[2], "run") == 0)
        do_run = true;

    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();

    // Read source file
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "error: could not open %s\n", filepath);
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    const int size = st.st_size;

    char* code = (char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (code == MAP_FAILED) { fprintf(stderr, "error: mmap failed\n"); return 1; }

    // Phase 1: Parse BF → flat Op vector
    std::vector<Op> ops;
    parse(code, size, ops);
    munmap(code, size);

    // Phase 2: Bracket matching
    if (!match_brackets(ops)) {
        fprintf(stderr, "error: unmatched brackets\n"); return 1;
    }

    // Phase 3: Generate LLVM IR
    auto ctx = std::make_unique<LLVMContext>();
    auto mod = std::make_unique<Module>("bf_module", *ctx);
    auto* funcTy = FunctionType::get(Type::getVoidTy(*ctx), false);
    auto* func = Function::Create(funcTy, Function::ExternalLinkage, "main", *mod);

    generate_ir(ops, *ctx, *mod, func);

    // Verify IR
    if (verifyFunction(*func, &errs())) {
        fprintf(stderr, "error: IR verification failed\n"); return 1;
    }

    // Phase 4: Optimize
    optimize_module(*mod);

    if (do_run) {
        // Phase 5: JIT execute
        jit_execute(std::move(mod), std::move(ctx));
    } else {
        // Phase 5: Compile to executable
        std::string out_path = std::string(filepath) + ".out";
        if (!compile_to_executable(*mod, out_path)) return 1;
        fprintf(stderr, "wrote %s\n", out_path.c_str());
    }

    return 0;
}
