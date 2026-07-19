# Brainfuck Interpreter & Compiler Suite

Four progressively optimized C++ brainfuck interpreters/compilers and a TUI editor. All accept a `.bf` file path as their first argument; run without arguments or with `--help` for usage.

---

## Table of Contents

- [Architecture](#architecture)
- [Interpreters](#interpreters)
  - [bfs — Simple Interpreter](#bfs--simple-interpreter)
  - [bff — Fast Interpreter](#bff--fast-interpreter)
  - [bfw — Bytecode Compiler](#bfw--bytecode-compiler)
  - [bfc — LLVM JIT Compiler](#bfc--llvm-jit-compiler)
- [Shared Idiom Detection](#shared-idiom-detection)
- [Build & Run](#build--run)
- [Benchmarks](#benchmarks)
- [CLI Editor](#cli-editor)

---

## Architecture

```
main.cpp          → bfs, bff    (direct interpretation)
mainfast.cpp      → bfw         (custom bytecode compiler + VM)
maincompile.cpp   → bfc         (LLVM IR → JIT compiler)
editor.cpp        → editor      (TUI editor via notcurses)
main.js           → node/bun    (JS reference)
```

---

## Interpreters

### bfs — Simple Interpreter

| | |
|---|---|
| **Source** | `main.cpp` |
| **Approach** | Direct interpretation with `std::unordered_map` tape |
| **Usage** | `./bfs <file.bf>` |
| **Optimizations** | None — reads each character and dispatches via `if/else` |

A straightforward, minimal interpreter. Good for constrained environments and as a correctness baseline.

**Build:**
```bash
clang++ -Oz -fno-exceptions -fno-rtti -Wl,-x main.cpp -o bfs
```

### bff — Fast Interpreter

| | |
|---|---|
| **Source** | `main.cpp` (same as bfs) |
| **Approach** | Direct interpretation |
| **Usage** | `./bff <file.bf>` |
| **Optimizations** | Compiler `-O3` level only |

Same code as bfs, compiled with full optimization flags. Good balance of compile time and runtime.

**Build:**
```bash
clang++ -O3 main.cpp -o bff
```

### bfw — Bytecode Compiler

| | |
|---|---|
| **Source** | `mainfast.cpp` |
| **Approach** | BF source → bytecode IR → computed-goto VM |
| **Usage** | `./bfw <file.bf>` |

Compiles brainfuck to an internal bytecode representation, then applies multiple optimization passes before execution via a computed-goto dispatch loop.

**Optimization pipeline (in order):**

1. **Run-length collapsing** — consecutive identical commands are merged:
   - `++++++` → `ADD 6`
   - `>>>>` → `RIGHT 4`

2. **Static idiom detection** — common patterns recognized at parse time (see [Shared Idiom Detection](#shared-idiom-detection))

3. **Bytecode-level loop semantic optimizer** — resolves remaining loop bodies and pattern-matches them against known idioms, replacing generic `[`/`]` + body sequences with fused semantic opcodes

4. **Dynamic superinstructions** — static bytecode profiling finds the most common consecutive opcode pair (e.g., `ADD` + `RIGHT`). If a pair appears above a threshold, the bytecode is recompiled with the pair fused into a single `OP_SUPER`, reducing dispatch overhead

5. **Computed goto dispatch** — uses GCC/Clang's `goto *label` extension for threaded-code dispatch, minimizing branch mispredictions

**Runtime details:**
- `mmap` + `madvise(MADV_SEQUENTIAL | MADV_WILLNEED)` for optimal I/O
- `alignas(64)` tape buffer for cache-line alignment
- `putchar()`/`getchar()` for program I/O
- `write()` syscalls for error messages (no stdio overhead)

**Build:**
```bash
clang++ -O3 -march=native -flto mainfast.cpp -o bfw
```

### bfc — LLVM JIT Compiler

| | |
|---|---|
| **Source** | `maincompile.cpp` |
| **Approach** | BF source → flat Op vector → LLVM IR → O2 pass pipeline → ORC JIT or object file |
| **Requires** | LLVM 22 (`brew install llvm`) |
| **Usage** | `./bfc <file.bf>` — compiles to `<file.bf>.out` (Mach-O object) |
| | `./bfc <file.bf> run` — compiles and JIT-executes immediately |

Compiles brainfuck to LLVM IR and JIT-executes it via LLVM's ORC JIT v2. Applies the same idiom detection as bfw before IR generation, then delegates further optimization to LLVM's `-O2` pass pipeline.

**Compilation phases:**

```
Phase 1: Parse
  BF source → flat Op vector
  (run-length collapse + idiom detection)

Phase 2: Bracket matching
  Link [ ] pairs via index in Op.a1

Phase 3: IR generation
  Each Op → one BasicBlock in an LLVM Function
  • Tape: global [30000 x i8] array
  • Pointer: alloca'd i32
  • Arithmetic: load → add/sub → store
  • I/O: calls to external putchar/getchar
  • Loops: condbr ( [ ) / br ( ] ) between blocks
  • Idioms: inline IR (e.g., CLEAR → store i8 0)

Phase 4: LLVM pass pipeline (O2)
  mem2reg, instcombine, simplifycfg, GVN,
  loop rotation, LICM, inlining, etc.

Phase 5: JIT execution
  ThreadSafeModule → ORC LLJIT → lookup("bf_main") → call
```

**Key design decisions:**
- **One basic block per operation** — generates many blocks for LLVM to merge. LLVM's `simplifycfg` pass merges consecutive straight-line blocks and eliminates unnecessary branches
- **Global tape array** — declared as `[30000 x i8]` global with zeroinitializer. LLVM can optimize accesses to known offsets
- **Pointer bounds clamping** — ptr moves are clamped to `[0, 29999]` via `select` instructions before each store
- **Idiom IR is inline** — CLEAR, SET_VALUE, MOVE_ADD, and DISTRIBUTE generate straight-line IR (no loops), while SCAN_RIGHT/SCAN_LEFT generate self-contained while-loop blocks

**Build:**
```bash
g++ -std=c++17 -O3 maincompile.cpp \
  $(/opt/homebrew/opt/llvm/bin/llvm-config --cxxflags --ldflags --libs core orcjit native) \
  -o bfc
```

**Running:**
```bash
# Compile to object file (default)
DYLD_LIBRARY_PATH=/opt/homebrew/opt/llvm/lib ./bfc programs/helloworld.bf
# → creates programs/helloworld.bf.out

# JIT compile and execute immediately
DYLD_LIBRARY_PATH=/opt/homebrew/opt/llvm/lib ./bfc programs/helloworld.bf run
```

---

## Shared Idiom Detection

Both `mainfast.cpp` (bfw) and `maincompile.cpp` (bfc) use the same static pattern matchers during parsing. Each detector scans for a specific bracket pattern and, if matched, emits a fused operation instead of individual `[`/`]` + body instructions.

| BF Pattern | Emitted Op | Effect |
|---|---|---|
| `[-]` | `CLEAR` | Set current cell to 0 |
| `[>]` | `SCAN_RIGHT` | Move right until zero cell |
| `[<]` | `SCAN_LEFT` | Move left until zero cell |
| `[-]+N` | `SET_VALUE N` | Clear cell, set to N |
| `[-]-N` | `SET_VALUE -N` | Clear cell, set to -N (mod 256) |
| `[->>+<<]` | `MOVE_ADD(off, mult)` | `tape[p+off] += tape[p] * mult; tape[p] = 0` |
| `[-<<+>>]` | `MOVE_ADD(-off, mult)` | Same, left-moving variant |
| `[->+>+<<]` | `DISTRIBUTE(o1,m1,o2,m2)` | Distribute value to two targets, clear source |

**Detector ordering matters.** More specific patterns (DISTRIBUTE) are tried before less specific ones (MOVE_ADD), and the generic `[` / `]` fallback is tried last.

---

## Build & Run

### Prerequisites

- **Clang** or **GCC** with C++17 support
- **Node.js** or **Bun** (optional, for JS reference)
- **LLVM 22** (for bfc only):
  ```bash
  brew install llvm
  ```
- **notcurses** (for editor only):
  ```bash
  brew install notcurses
  ```
- **hyperfine** (for benchmarks):
  ```bash
  brew install hyperfine
  ```

### Building All

```bash
# Simple interpreter (bfs)
clang++ -Oz -fno-exceptions -fno-rtti -Wl,-x main.cpp -o bfs

# Fast interpreter (bff)
clang++ -O3 main.cpp -o bff

# Bytecode compiler (bfw)
clang++ -O3 -march=native -flto mainfast.cpp -o bfw

# LLVM JIT compiler (bfc)
LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config
g++ -std=c++17 -O3 maincompile.cpp \
  $($LLVM_CONFIG --cxxflags --ldflags --libs core orcjit native) \
  -o bfc

# TUI editor
clang++ -std=c++17 -O2 editor.cpp \
  $(pkg-config --cflags --libs notcurses) -o editor
```

### Running

All interpreters accept a `.bf` file as their first argument:
```bash
./bfs programs/helloworld.bf
./bfw programs/helloworld.bf
```

`bfc` has two modes:
```bash
# Compile to object file (default)
DYLD_LIBRARY_PATH=/opt/homebrew/opt/llvm/lib ./bfc programs/helloworld.bf
# → creates programs/helloworld.bf.out

# JIT compile and execute immediately
DYLD_LIBRARY_PATH=/opt/homebrew/opt/llvm/lib ./bfc programs/helloworld.bf run
```

### Benchmarks

```bash
hyperfine --warmup 1 --runs 5 \
  './bfs programs/program_mandelbrot.bf' \
  './bff programs/program_mandelbrot.bf' \
  './bfw programs/program_mandelbrot.bf' \
  'DYLD_LIBRARY_PATH=/opt/homebrew/opt/llvm/lib ./bfc programs/program_mandelbrot.bf run' \
  'node main.js programs/program_mandelbrot.bf' \
  'bun main.js programs/program_mandelbrot.bf'
```

`--warmup 1` runs each command once before measurement (warming disk caches and page tables). `--runs 5` executes each command 5 times, reporting mean, min, and max.

---

## CLI Editor

A terminal-based brainfuck editor built on [notcurses](https://github.com/dankamongmen/notcurses). Source: `editor.cpp`.

### Usage

```
./editor
```

Reads/writes brainfuck files from `programs/`. For execution, saves to a temp file and runs the interpreter directly with the file path.

### Keybindings

| Key | Action |
|---|---|
| **F1** / `Ctrl+R` | Run program |
| **F2** / `Ctrl+S` / `Ctrl+X` | Save as |
| **F3** / `Ctrl+O` | Open file |
| **F4** / `Esc` / `Ctrl+A` | Toggle mode (Code / Input) |
| **F5** / `Ctrl+Q` | Quit |

### Code Mode Mappings

In **Code mode**, these keys insert brainfuck commands (same positional mapping across home row, top row, and number row):

```
Home row: a=,  s=[  d=+  f=<   h=>  j=-  k=]  l=.
Top row:  q=,  w=[  e=+  r=<   y=>  u=-  i=]  o=.
Numbers:  1=,  2=[  3=+  4=<   5=>  6=-  7=]  8=.
```

Direct brainfuck characters `[]+-<>.,` insert themselves. All other keys are ignored in code mode.

In **Input mode**, all keys insert literally — for typing comments or non-BF text.

### Syntax Highlighting

| Color | Characters |
|---|---|
| Green | `+` |
| Red | `-` |
| Blue | `>` |
| Cyan | `<` |
| Orange | `[` `]` |
| Magenta | `.` |
| White | `,` |
| Dim grey | Comments |
