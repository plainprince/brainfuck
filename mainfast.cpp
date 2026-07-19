// mainfast.cpp — BRAINFUCK BYTECODE COMPILER + AGGRESSIVE INLINING
//
// Compiles BF source → optimized bytecode IR → executes.
//
// Optimizations (always max):
//   • Run-length collapsing: ++++++ → ADD 6,  >>>> → RIGHT 4
//   • [-]            → CLEAR
//   • [>]            → SCAN_RIGHT (scan right to zero cell)
//   • [<]            → SCAN_LEFT  (scan left to zero cell)
//   • [-]+N / [-] -N → SET_VALUE  (clear then set to constant)
//   • Generic MOVE_ADD: any right/left offset, any count
//     (e.g. [->>++<<])
//   • Generic DISTRIBUTE: two targets, any offsets/counts
//     (e.g. [->+>+<<])
//   • Dynamic superinstructions: static bytecode analysis finds
//     common opcode pairs, recompiles with fused superinstruction,
//     re-executes with optimized bytecode
//   • Computed goto dispatch
//
// Runtime: mmap + madvise + alignas(64) tape + raw C I/O.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#ifdef __clang__
#define RESTRICT __restrict
#else
#define RESTRICT __restrict__
#endif

// ── Bytecode opcodes ────────────────────────────────────────────────
enum Opcode {
    OP_ADD        = 0,  // + (count = a1)
    OP_SUB        = 1,  // - (count = a1)
    OP_RIGHT      = 2,  // > (count = a1)
    OP_LEFT       = 3,  // < (count = a1)
    OP_OUT        = 4,  // .
    OP_IN         = 5,  // ,
    OP_JMP_FWD    = 6,  // [  → target bc index = a1
    OP_JMP_BACK   = 7,  // ]  → target bc index = a1
    OP_CLEAR      = 8,  // [-]  tape[p] = 0
    OP_MOVE_ADD   = 9,  // [->>+<<]  tape[p+a1] += tape[p]*a2; tape[p]=0
    OP_DISTRIBUTE =10,  // [->+>+<<]  tape[p+a1] += tape[p]*a2;
                        //            tape[p+a3] += tape[p]*a4; tape[p]=0
    OP_SCAN_RIGHT =11,  // [>]  scan right until zero cell
    OP_SCAN_LEFT  =12,  // [<]  scan left until zero cell
    OP_SET_VALUE  =13,  // [-]+N or [-]-N  clear cell then set to N
    OP_SUPER      =14,  // dynamic superinstruction fusing two ops
};

// Storage: a1=first fused opcode, a2=second fused opcode,
// a3=first op's arg (a1), a4=second op's arg (a1)
// Only simple ops are fused: ADD, SUB, RIGHT, LEFT, OUT, IN, CLEAR, SET_VALUE
// For these ops, only a1 is meaningful.

typedef struct { int op, a1, a2, a3, a4; } BC;

// True if an opcode can be fused as part of a superinstruction.
// Only simple ops with a single meaningful argument (a1).
static inline bool is_fusable(int op) {
    return op == OP_ADD || op == OP_SUB || op == OP_RIGHT ||
           op == OP_LEFT || op == OP_OUT || op == OP_IN ||
           op == OP_CLEAR || op == OP_SET_VALUE;
}

// ── Static bytecode profiler ───────────────────────────────────────
// Analyze the compiled bytecode to find the most common consecutive
// opcode pair (excluding jump targets that alter flow).
#define MAX_PAIRS 256

static struct {
    int op1, op2;  // the two opcodes
    int hits;      // how many times the pair appears
} profile_pairs[MAX_PAIRS];

static int profile_npairs = 0;

static void profile_reset() {
    profile_npairs = 0;
    for (int i = 0; i < MAX_PAIRS; i++) {
        profile_pairs[i].op1 = -1;
        profile_pairs[i].op2 = -1;
        profile_pairs[i].hits = 0;
    }
}

static void profile_record(int op1, int op2) {
    for (int i = 0; i < profile_npairs; i++) {
        if (profile_pairs[i].op1 == op1 && profile_pairs[i].op2 == op2) {
            profile_pairs[i].hits++;
            return;
        }
    }
    if (profile_npairs < MAX_PAIRS) {
        profile_pairs[profile_npairs].op1 = op1;
        profile_pairs[profile_npairs].op2 = op2;
        profile_pairs[profile_npairs].hits = 1;
        profile_npairs++;
    }
}

// Analyze bytecode for hot consecutive opcode pairs.
// Only profiles pairs where both ops are fusable into a superinstruction.
static void profile_bytecode(BC* bc, int n) {
    profile_reset();
    for (int i = 0; i < n - 1; i++) {
        int op1 = bc[i].op;
        int op2 = bc[i+1].op;
        // Only profile pairs where both ops can be fused into a superinstruction.
        // This keeps the profiler in sync with is_fusable() — non-fusable ops
        // like jumps, MOVE_ADD, DISTRIBUTE, SCAN, and OP_SUPER itself are
        // naturally excluded.
        if (!is_fusable(op1) || !is_fusable(op2)) continue;
        profile_record(op1, op2);
    }
}

// Find the most common pair.
static int profile_best(int min_hits) {
    int best_idx = -1;
    int best_hits = 0;
    for (int i = 0; i < profile_npairs; i++) {
        if (profile_pairs[i].hits > best_hits) {
            best_hits = profile_pairs[i].hits;
            best_idx = i;
        }
    }
    if (best_idx < 0 || profile_pairs[best_idx].hits < min_hits)
        return -1;
    return best_idx;
}

// ── Static idiom detectors ─────────────────────────────────────────
// Each returns the character-span length, or 0 if no match,
// and fills in the bytecode arguments.

//  [-]  →  CLEAR
static inline int match_clear(const char* s, int ip, int size) {
    return (ip + 2 < size && s[ip+1] == '-' && s[ip+2] == ']') ? 3 : 0;
}

//  [>]  →  SCAN_RIGHT  (scan right until zero)
static inline int match_scan_right(const char* s, int ip, int size) {
    return (ip + 2 < size && s[ip+1] == '>' && s[ip+2] == ']') ? 3 : 0;
}

//  [<]  →  SCAN_LEFT  (scan left until zero)
static inline int match_scan_left(const char* s, int ip, int size) {
    return (ip + 2 < size && s[ip+1] == '<' && s[ip+2] == ']') ? 3 : 0;
}

//  [-]+N  or  [-] -N  →  SET_VALUE  (clear then set value)
static inline int match_set_value(const char* s, int ip, int size,
                                   int* count) {
    if (ip + 3 >= size) return 0;
    if (s[ip+1] != '-' || s[ip+2] != ']') return 0;
    int pos = ip + 3;
    if (pos >= size) return 0;
    int ct = 0;
    if (s[pos] == '+') ct = 1;
    else if (s[pos] == '-') ct = -1;
    else return 0;
    char dir = s[pos];
    pos++;
    while (pos < size && s[pos] == dir) {
        ct += (dir == '+') ? 1 : -1;
        pos++;
    }
    *count = ct;
    return pos - ip;
}

//  [ - (>) (+) (<)  ]   right-moving MOVE_ADD
static inline int match_move_add_right(const char* s, int ip, int size,
                                        int* offset, int* count) {
    int pos = ip + 2;
    int mv = 0;
    while (pos < size && s[pos] == '>') { mv++; pos++; }
    if (mv == 0) return 0;
    int ct = 0;
    while (pos < size && s[pos] == '+') { ct++; pos++; }
    if (ct == 0) return 0;
    int ret = 0;
    while (pos < size && s[pos] == '<') { ret++; pos++; }
    if (ret != mv) return 0;
    if (pos >= size || s[pos] != ']') return 0;
    *offset = mv; *count = ct;
    return pos - ip + 1;
}

//  [ - (<) (+) (>)  ]   left-moving MOVE_ADD
static inline int match_move_add_left(const char* s, int ip, int size,
                                       int* offset, int* count) {
    int pos = ip + 2;
    int mv = 0;
    while (pos < size && s[pos] == '<') { mv++; pos++; }
    if (mv == 0) return 0;
    int ct = 0;
    while (pos < size && s[pos] == '+') { ct++; pos++; }
    if (ct == 0) return 0;
    int ret = 0;
    while (pos < size && s[pos] == '>') { ret++; pos++; }
    if (ret != mv) return 0;
    if (pos >= size || s[pos] != ']') return 0;
    *offset = -mv; *count = ct;
    return pos - ip + 1;
}

//  [ - (>)(+) (>)(+) (<)  ]   right-moving 2-target DISTRIBUTE
static inline int match_distribute_right(const char* s, int ip, int size,
                                          int* o1, int* c1,
                                          int* o2, int* c2) {
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

//  [ - (<)(+) (<)(+) (>)  ]   left-moving 2-target DISTRIBUTE
static inline int match_distribute_left(const char* s, int ip, int size,
                                         int* o1, int* c1,
                                         int* o2, int* c2) {
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

// ── Bytecode compiler ───────────────────────────────────────────────
static int compile(const char* RESTRICT src, int size,
                   BC* RESTRICT bc, int cap) {
    int n = 0;

    auto emit = [&](int op, int a1, int a2, int a3, int a4) -> bool {
        if (n >= cap) return false;
        bc[n].op = op; bc[n].a1 = a1; bc[n].a2 = a2;
        bc[n].a3 = a3; bc[n].a4 = a4;
        n++; return true;
    };

    int ip = 0;
    while (ip < size) {
        unsigned char c = src[ip];

        // ── Run-length collapse: + - > < ─────────────────────────
        if (c == '+') {
            int cnt = 1;
            while (ip + cnt < size && src[ip + cnt] == '+') cnt++;
            if (!emit(OP_ADD, cnt, 0,0,0)) return -1;
            ip += cnt; continue;
        }
        if (c == '-') {
            int cnt = 1;
            while (ip + cnt < size && src[ip + cnt] == '-') cnt++;
            if (!emit(OP_SUB, cnt, 0,0,0)) return -1;
            ip += cnt; continue;
        }
        if (c == '>') {
            int cnt = 1;
            while (ip + cnt < size && src[ip + cnt] == '>') cnt++;
            if (!emit(OP_RIGHT, cnt, 0,0,0)) return -1;
            ip += cnt; continue;
        }
        if (c == '<') {
            int cnt = 1;
            while (ip + cnt < size && src[ip + cnt] == '<') cnt++;
            if (!emit(OP_LEFT, cnt, 0,0,0)) return -1;
            ip += cnt; continue;
        }

        // ── I/O ──────────────────────────────────────────────────
        if (c == '.') { if (!emit(OP_OUT, 0,0,0,0)) return -1; ip++; continue; }
        if (c == ',') { if (!emit(OP_IN,  0,0,0,0)) return -1; ip++; continue; }

        // ── Bracket patterns (aggressive generic matching) ────────
        if (c == '[') {
            int o1, c1, o2, c2, span, off, cnt;

            // Try left-moving DISTRIBUTE first (more specific)
            span = match_distribute_left(src, ip, size, &o1,&c1, &o2,&c2);
            if (span) { if (!emit(OP_DISTRIBUTE, o1,c1, o2,c2)) return -1; ip += span; continue; }

            // Right-moving DISTRIBUTE
            span = match_distribute_right(src, ip, size, &o1,&c1, &o2,&c2);
            if (span) { if (!emit(OP_DISTRIBUTE, o1,c1, o2,c2)) return -1; ip += span; continue; }

            // Left-moving MOVE_ADD
            span = match_move_add_left(src, ip, size, &off, &cnt);
            if (span) { if (!emit(OP_MOVE_ADD, off,cnt, 0,0)) return -1; ip += span; continue; }

            // Right-moving MOVE_ADD
            span = match_move_add_right(src, ip, size, &off, &cnt);
            if (span) { if (!emit(OP_MOVE_ADD, off,cnt, 0,0)) return -1; ip += span; continue; }

            // SET_VALUE  ([-]+N or [-] -N)
            span = match_set_value(src, ip, size, &cnt);
            if (span) { if (!emit(OP_SET_VALUE, cnt,0, 0,0)) return -1; ip += span; continue; }

            // SCAN_RIGHT  [>]
            span = match_scan_right(src, ip, size);
            if (span) { if (!emit(OP_SCAN_RIGHT, 0,0, 0,0)) return -1; ip += span; continue; }

            // SCAN_LEFT  [<]
            span = match_scan_left(src, ip, size);
            if (span) { if (!emit(OP_SCAN_LEFT, 0,0, 0,0)) return -1; ip += span; continue; }

            // CLEAR (simplest, check last)
            span = match_clear(src, ip, size);
            if (span) { if (!emit(OP_CLEAR, 0,0,0,0)) return -1; ip += span; continue; }

            // Plain forward jump (no match)
            if (!emit(OP_JMP_FWD, 0,0,0,0)) return -1;
            ip++; continue;
        }

        if (c == ']') {
            if (!emit(OP_JMP_BACK, 0,0,0,0)) return -1;
            ip++; continue;
        }

        // Any other char → comment
        ip++;
    }
    return n;
}

// ── Jump resolution ─────────────────────────────────────────────────
static int resolve_jumps(BC* bc, int n) {
    int stack[5000], sp = 0;
    for (int i = 0; i < n; i++) {
        if      (bc[i].op == OP_JMP_FWD) stack[sp++] = i;
        else if (bc[i].op == OP_JMP_BACK) {
            if (sp == 0) return -1;
            int j = stack[--sp];
            bc[j].a1 = i; bc[i].a1 = j;
        }
    }
    return (sp == 0) ? 0 : -1;
}

// ── Bytecode-level loop semantic optimizer ─────────────────────────
// Analyzes resolved bytecode and replaces loops with their semantic
// equivalent opcodes. Catches patterns the static compiler missed due
// to whitespace, comments, or non-standard formatting in the source.
//
// Matched patterns:
//   CLEAR:       JMP_FWD → [SUB]* → JMP_BACK         (any decrement count to zero)
//   MOVE_ADD:    JMP_FWD → SUB 1 → RIGHT R → ADD C → LEFT R → JMP_BACK
//   DISTRIBUTE:  JMP_FWD → SUB 1 → RIGHT R1 → ADD C1 → RIGHT R2 → ADD C2 → LEFT(R1+R2) → JMP_BACK
//   (left variants also handled)

// Try to match a loop body against known bytecode patterns.
// Returns the optimized opcode (OP_CLEAR, OP_MOVE_ADD, OP_DISTRIBUTE)
// or -1 if no match. Fills in the opcode arguments.
static int match_loop_pattern(BC* body, int len,
                               int& a1, int& a2, int& a3, int& a4) {
    // Pattern 1: CLEAR — only safe when total decrement per iteration is exactly 1.
    // SUB N with N > 1 can wrap unsigned char and never reach 0.
    // (e.g. `[---]` with cell=2: 2→255→252→..., never hits 0)
    if (len == 1 && body[0].op == OP_SUB && body[0].a1 == 1) {
        a1 = a2 = a3 = a4 = 0;
        return OP_CLEAR;
    }

    // Pattern 2: Right-moving MOVE_ADD
    // body = [SUB 1, RIGHT R, ADD C, LEFT R]
    if (len == 4 &&
        body[0].op == OP_SUB && body[0].a1 == 1 &&
        body[1].op == OP_RIGHT &&
        body[2].op == OP_ADD &&
        body[3].op == OP_LEFT &&
        body[1].a1 == body[3].a1)  // RIGHT and LEFT distances match
    {
        a1 = body[1].a1;  // offset (positive = right)
        a2 = body[2].a1;  // count per unit
        a3 = a4 = 0;
        return OP_MOVE_ADD;
    }

    // Pattern 3: Left-moving MOVE_ADD
    // body = [SUB 1, LEFT R, ADD C, RIGHT R]
    if (len == 4 &&
        body[0].op == OP_SUB && body[0].a1 == 1 &&
        body[1].op == OP_LEFT &&
        body[2].op == OP_ADD &&
        body[3].op == OP_RIGHT &&
        body[1].a1 == body[3].a1)  // LEFT and RIGHT distances match
    {
        a1 = -body[1].a1;  // offset (negative = left)
        a2 = body[2].a1;   // count per unit
        a3 = a4 = 0;
        return OP_MOVE_ADD;
    }

    // Pattern 4: Right-moving DISTRIBUTE (two targets)
    // body = [SUB 1, RIGHT R1, ADD C1, RIGHT R2, ADD C2, LEFT(R1+R2)]
    if (len >= 6 &&
        body[0].op == OP_SUB && body[0].a1 == 1)
    {
        int pos = 1;
        if (body[pos].op != OP_RIGHT) goto try_left_distribute;
        int r1 = body[pos].a1; pos++;
        if (body[pos].op != OP_ADD) goto try_left_distribute;
        int c1 = body[pos].a1; pos++;
        if (body[pos].op != OP_RIGHT) goto try_left_distribute;
        int r2 = body[pos].a1; pos++;
        if (body[pos].op != OP_ADD) goto try_left_distribute;
        int c2 = body[pos].a1; pos++;
        if (pos >= len || body[pos].op != OP_LEFT) goto try_left_distribute;
        if (body[pos].a1 != r1 + r2) goto try_left_distribute;
        pos++;
        if (pos == len) {
            a1 = r1; a2 = c1;
            a3 = r1 + r2; a4 = c2;
            return OP_DISTRIBUTE;
        }
    }

    try_left_distribute:
    // Pattern 5: Left-moving DISTRIBUTE (two targets)
    // body = [SUB 1, LEFT L1, ADD C1, LEFT L2, ADD C2, RIGHT(L1+L2)]
    if (len >= 6 &&
        body[0].op == OP_SUB && body[0].a1 == 1)
    {
        int pos = 1;
        if (body[pos].op != OP_LEFT) return -1;
        int l1 = body[pos].a1; pos++;
        if (body[pos].op != OP_ADD) return -1;
        int c1 = body[pos].a1; pos++;
        if (body[pos].op != OP_LEFT) return -1;
        int l2 = body[pos].a1; pos++;
        if (body[pos].op != OP_ADD) return -1;
        int c2 = body[pos].a1; pos++;
        if (pos >= len || body[pos].op != OP_RIGHT) return -1;
        if (body[pos].a1 != l1 + l2) return -1;
        pos++;
        if (pos == len) {
            a1 = -l1; a2 = c1;
            a3 = -(l1 + l2); a4 = c2;
            return OP_DISTRIBUTE;
        }
    }

    return -1;  // no match
}

// Optimize bytecode by detecting loop patterns and replacing them with
// semantic equivalents. Uses old_to_new mapping to remap jump targets.
// Returns new bytecode count, or 0 if no optimization possible, or -1 on error.
static int optimize_bytecode(BC* old, int n_old, BC* new_bc, int cap) {
    // Phase 1: identify optimizable loops, build old_to_new mapping
    int* old_to_new = new int[n_old];
    int n_new = 0;

    for (int i = 0; i < n_old; ) {
        if (old[i].op == OP_JMP_FWD && old[i].a1 > i + 1) {
            int j = old[i].a1;  // matching JMP_BACK
            int body_len = j - i - 1;
            int a1, a2, a3, a4;
            if (match_loop_pattern(&old[i + 1], body_len, a1, a2, a3, a4) >= 0) {
                // This entire loop (i..j) maps to one new opcode
                for (int k = i; k <= j; k++) old_to_new[k] = n_new;
                i = j + 1;
                n_new++;
                continue;
            }
        }
        old_to_new[i] = n_new;
        i++;
        n_new++;
    }

    if (n_new == n_old) {
        // No optimization possible
        delete[] old_to_new;
        return 0;
    }

    if (n_new > cap) {
        delete[] old_to_new;
        return -1;
    }

    // Phase 2: fill new bytecode with remapped jump targets
    int ni = 0;
    for (int i = 0; i < n_old; ) {
        if (old[i].op == OP_JMP_FWD && old[i].a1 > i + 1) {
            int j = old[i].a1;
            int body_len = j - i - 1;
            int a1, a2, a3, a4;
            int opt_op = match_loop_pattern(&old[i + 1], body_len, a1, a2, a3, a4);
            if (opt_op >= 0) {
                new_bc[ni].op = opt_op;
                new_bc[ni].a1 = a1;
                new_bc[ni].a2 = a2;
                new_bc[ni].a3 = a3;
                new_bc[ni].a4 = a4;
                ni++;
                i = j + 1;
                continue;
            }
        }

        // Copy op as-is, remapping jump targets via old_to_new
        new_bc[ni] = old[i];
        if (old[i].op == OP_JMP_FWD || old[i].op == OP_JMP_BACK) {
            new_bc[ni].a1 = old_to_new[old[i].a1];
        }
        ni++;
        i++;
    }

    delete[] old_to_new;
    return n_new;
}

// ── Recompile with dynamic superinstruction ─────────────────────────
// Scan bytecode, fuse (op1, op2) into OP_SUPER, remap jumps.
// Returns new count, or -1 on error.
static int recompile_with_super(BC* old, int n_old,
                                 BC* new_bc, int cap,
                                 int fuse_op1, int fuse_op2) {
    // Phase 1: build old-index → new-index mapping
    int* old_to_new = new int[n_old];
    if (!old_to_new) return -1;

    int n_new = 0;
    int i = 0;
    while (i < n_old) {
        old_to_new[i] = n_new;
        // If this and next form a fusible pair
        if (i + 1 < n_old && old[i].op == fuse_op1 &&
            old[i+1].op == fuse_op2 &&
            is_fusable(fuse_op1) && is_fusable(fuse_op2)) {
            old_to_new[i+1] = n_new;
            n_new++;
            i += 2;
        } else {
            n_new++;
            i++;
        }
    }

    if (n_new > cap) { delete[] old_to_new; return -1; }

    // Phase 2: fill new bytecode
    int ni = 0;
    i = 0;
    while (i < n_old && ni < n_new) {
        if (i + 1 < n_old && old[i].op == fuse_op1 &&
            old[i+1].op == fuse_op2 &&
            is_fusable(fuse_op1) && is_fusable(fuse_op2)) {
            // Fuse: op1's arg = old[i].a1, op2's arg = old[i+1].a1
            new_bc[ni].op = OP_SUPER;
            new_bc[ni].a1 = fuse_op1;
            new_bc[ni].a2 = fuse_op2;
            new_bc[ni].a3 = old[i].a1;      // op1's argument
            new_bc[ni].a4 = old[i+1].a1;    // op2's argument
            ni++;
            i += 2;
        } else {
            // Copy as-is (including jumps, MOVE_ADD, DISTRIBUTE, etc.)
            new_bc[ni] = old[i];
            ni++;
            i++;
        }
    }

    // Phase 3: remap jump targets
    for (int j = 0; j < n_new; j++) {
        if (new_bc[j].op == OP_JMP_FWD || new_bc[j].op == OP_JMP_BACK) {
            int old_target = new_bc[j].a1;
            if (old_target >= 0 && old_target < n_old) {
                new_bc[j].a1 = old_to_new[old_target];
            }
        }
    }

    delete[] old_to_new;
    return n_new;
}

// ── Bytecode execution via computed goto ───────────────────────────
// Handles all opcodes including OP_SUPER.
static void exec(BC* RESTRICT bc, int n) {
    if (n == 0) return;

    alignas(64) unsigned char tape[30000] = {0};
    int p = 0, ip = 0;

    static void* dispatch[16];
    dispatch[OP_ADD]        = &&op_add;
    dispatch[OP_SUB]        = &&op_sub;
    dispatch[OP_RIGHT]      = &&op_right;
    dispatch[OP_LEFT]       = &&op_left;
    dispatch[OP_OUT]        = &&op_out;
    dispatch[OP_IN]         = &&op_in;
    dispatch[OP_JMP_FWD]    = &&op_jmp_fwd;
    dispatch[OP_JMP_BACK]   = &&op_jmp_back;
    dispatch[OP_CLEAR]      = &&op_clear;
    dispatch[OP_MOVE_ADD]   = &&op_move_add;
    dispatch[OP_DISTRIBUTE] = &&op_distribute;
    dispatch[OP_SCAN_RIGHT] = &&op_scan_right;
    dispatch[OP_SCAN_LEFT]  = &&op_scan_left;
    dispatch[OP_SET_VALUE]  = &&op_set_value;
    dispatch[OP_SUPER]      = &&op_super;

    goto *dispatch[bc[ip].op];

op_add:
    tape[p] += bc[ip].a1; ip++; goto dispatch_next;
op_sub:
    tape[p] -= bc[ip].a1; ip++; goto dispatch_next;
op_right:
    p += bc[ip].a1;
    if (p >= 29997) p = 29997;
    ip++; goto dispatch_next;
op_left: {
    int d = bc[ip].a1;
    p -= d;
    if (p < 0) p = 0;
    ip++; goto dispatch_next;
}
op_out:
    putchar(tape[p]); ip++; goto dispatch_next;
op_in:
    tape[p] = (unsigned char)getchar(); ip++; goto dispatch_next;
op_jmp_fwd:
    if (!tape[p]) ip = bc[ip].a1; else ip++;
    goto dispatch_next;
op_jmp_back:
    if (tape[p]) ip = bc[ip].a1; else ip++;
    goto dispatch_next;
op_clear:
    tape[p] = 0; ip++; goto dispatch_next;
op_move_add: {
    unsigned char val = tape[p];
    if (val) {
        int t = p + bc[ip].a1;
        if (t >= 0 && t < 30000) tape[t] += val * bc[ip].a2;
    }
    tape[p] = 0; ip++; goto dispatch_next;
}
op_distribute: {
    unsigned char val = tape[p];
    if (val) {
        int t1 = p + bc[ip].a1;
        if (t1 >= 0 && t1 < 30000) tape[t1] += val * bc[ip].a2;
        int t2 = p + bc[ip].a3;
        if (t2 >= 0 && t2 < 30000) tape[t2] += val * bc[ip].a4;
    }
    tape[p] = 0; ip++; goto dispatch_next;
}
op_scan_right:
    while (p < 29999 && tape[p] != 0) p++;
    ip++; goto dispatch_next;
op_scan_left:
    while (p > 0 && tape[p] != 0) p--;
    ip++; goto dispatch_next;
op_set_value:
    tape[p] = (unsigned char)(bc[ip].a1);
    ip++; goto dispatch_next;
op_super: {
    // Fused superinstruction: execute both ops in sequence.
    // a1 = first opcode, a2 = second opcode,
    // a3 = first op's argument, a4 = second op's argument.
    int op1 = bc[ip].a1;
    int op2 = bc[ip].a2;
    int a1v = bc[ip].a3;
    int a2v = bc[ip].a4;

    // Execute first op
    switch (op1) {
        case OP_ADD:      tape[p] += a1v; break;
        case OP_SUB:      tape[p] -= a1v; break;
        case OP_RIGHT:    p += a1v; if (p >= 29997) p = 29997; break;
        case OP_LEFT:     p -= a1v; if (p < 0) p = 0; break;
        case OP_OUT:      putchar(tape[p]); break;
        case OP_IN:       tape[p] = (unsigned char)getchar(); break;
        case OP_CLEAR:    tape[p] = 0; break;
        case OP_SET_VALUE: tape[p] = (unsigned char)a1v; break;
        default: break;
    }

    // Execute second op
    switch (op2) {
        case OP_ADD:      tape[p] += a2v; break;
        case OP_SUB:      tape[p] -= a2v; break;
        case OP_RIGHT:    p += a2v; if (p >= 29997) p = 29997; break;
        case OP_LEFT:     p -= a2v; if (p < 0) p = 0; break;
        case OP_OUT:      putchar(tape[p]); break;
        case OP_IN:       tape[p] = (unsigned char)getchar(); break;
        case OP_CLEAR:    tape[p] = 0; break;
        case OP_SET_VALUE: tape[p] = (unsigned char)a2v; break;
        default: break;
    }

    ip++; goto dispatch_next;
}
dispatch_next:
    if (ip >= n) goto cleanup;
    goto *dispatch[bc[ip].op];
cleanup:
    return;
}

// ── Main ────────────────────────────────────────────────────────────
static void print_help(const char* prog) {
    write(2, "Usage: ", 7);
    write(2, prog, strlen(prog));
    write(2, " <file.bf>\n", 11);
    write(2, "  Bytecode-compiling brainfuck interpreter with aggressive opts.\n", 67);
}

int main(int argc, char* argv[]) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        print_help(argc > 0 ? argv[0] : "bfw");
        return 1;
    }

    setvbuf(stdout, NULL, _IONBF, 0);

    const char* filepath = argv[1];
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        write(2, "error: could not open ", 22);
        write(2, filepath, strlen(filepath));
        write(2, "\n", 1);
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    const int size = st.st_size;

    const char* RESTRICT code = (const char*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (code == MAP_FAILED) { write(2, "error: mmap failed\n", 19); return 1; }
    madvise((void*)code, size, MADV_SEQUENTIAL | MADV_WILLNEED);

    int cap = size > 0 ? size : 1;
    BC* bc1 = new BC[cap];

    int n = compile(code, size, bc1, cap);
    if (n < 0) { write(2, "error: bytecode buffer overflow\n", 32); return 1; }

    if (resolve_jumps(bc1, n) < 0) {
        write(2, "error: unmatched brackets\n", 26);
        delete[] bc1; munmap((void*)code, size); return 1;
    }

    // ── Bytecode-level semantic optimization ───────────────────────
    BC* bc2 = new BC[cap];
    int n2 = optimize_bytecode(bc1, n, bc2, cap);
    if (n2 > 0) {
        delete[] bc1;
        bc1 = bc2;
        n = n2;
    } else {
        delete[] bc2;
    }

    // ── Static bytecode profiling + dynamic superinstructions ──────
    profile_bytecode(bc1, n);
    int best_idx = profile_best(2);
    if (best_idx >= 0) {
        BC* bc3 = new BC[cap];
        int n3 = recompile_with_super(bc1, n, bc3, cap,
                                       profile_pairs[best_idx].op1,
                                       profile_pairs[best_idx].op2);
        if (n3 > 0) {
            delete[] bc1;
            bc1 = bc3;
            n = n3;
        } else {
            delete[] bc3;
        }
    }

    exec(bc1, n);
    putchar('\n');

    delete[] bc1;
    munmap((void*)code, size);
    return 0;
}
