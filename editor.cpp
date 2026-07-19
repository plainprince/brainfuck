// editor.cpp — Brainfuck CLI Editor with Notcurses
// See README.md for build instructions

#include <notcurses/notcurses.h>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace fs = std::filesystem;

// Syntax highlight colors (RGB)
static constexpr unsigned HI_PLUS    = 0x00ff00; // green
static constexpr unsigned HI_MINUS   = 0xff0000; // red
static constexpr unsigned HI_RIGHT   = 0x4488ff; // blue
static constexpr unsigned HI_LEFT    = 0x00ffff; // cyan
static constexpr unsigned HI_LOOP    = 0xffaa00; // orange/yellow
static constexpr unsigned HI_OUT     = 0xff00ff; // magenta
static constexpr unsigned HI_IN      = 0xffffff; // white
static constexpr unsigned HI_DEFAULT = 0xcccccc; // grey
static constexpr unsigned HI_COMMENT = 0x666666; // dim grey

// ── Global state ────────────────────────────────────────────────────
static std::string base_dir;       // directory of this executable
static std::string programs_dir;   // base_dir/programs/

// ── Path helpers ───────────────────────────────────────────────────
static std::string dirname_of(const std::string& path) {
    auto pos = path.find_last_of('/');
    return (pos == std::string::npos) ? "." : path.substr(0, pos);
}

static void ensure_dir(const std::string& dir) {
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) {
        mkdir(dir.c_str(), 0755);
    }
}

// ── Get syntax highlight color for a BF command char ───────────────
static unsigned color_for_bf(char c) {
    switch (c) {
        case '+': return HI_PLUS;
        case '-': return HI_MINUS;
        case '>': return HI_RIGHT;
        case '<': return HI_LEFT;
        case '[': case ']': return HI_LOOP;
        case '.': return HI_OUT;
        case ',': return HI_IN;
        default:
            if (c == '\n' || c == '\r' || c == '\t') return HI_DEFAULT;
            if (c >= 32 && c <= 126) return HI_COMMENT;
            return HI_DEFAULT;
    }
}

static bool is_bf_command(char c) {
    return c == '+' || c == '-' || c == '>' || c == '<' ||
           c == '[' || c == ']' || c == '.' || c == ',';
}

// ── File browser ────────────────────────────────────────────────────
static bool file_browser_ui(notcurses* nc, struct ncplane* parent,
                             const std::string& dir,
                             std::string& selected) {
    // Scan directory for .bf files
    std::vector<std::string> files;
    DIR* d = opendir(dir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string name = ent->d_name;
            // Skip hidden files and non-.bf files (unless they have no extension)
            if (name.size() < 4) continue;
            if (name.substr(name.size()-3) == ".bf") {
                files.push_back(name);
            }
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    files.insert(files.begin(), "( new file )");

    unsigned int term_y, term_x;
    notcurses_term_dim_yx(nc, &term_y, &term_x);

    int brow = 14;
    int bcol = 40;
    int by0 = ((int)term_y - brow) / 2;
    int bx0 = ((int)term_x - bcol) / 2;
    if (by0 < 0) by0 = 0;
    if (bx0 < 0) bx0 = 0;
    if (brow > term_y) brow = term_y;
    if (bcol > term_x) bcol = term_x;

    struct ncplane_options nopts = {};
    nopts.y = by0;
    nopts.x = bx0;
    nopts.rows = brow;
    nopts.cols = bcol;

    struct ncplane* browse = ncplane_create(notcurses_stdplane(nc), &nopts);
    if (!browse) return false;

    // Set background
    uint64_t bgch = 0;
    ncchannels_set_fg_rgb(&bgch, 0xe0e0e0);
    ncchannels_set_bg_rgb(&bgch, 0x222222);
    ncplane_set_base(browse, " ", 0, bgch);

    int sel = 0;
    int scroll = 0;
    bool done = false;
    bool result = false;

    while (!done) {
        ncplane_erase(browse);

        // Title
        ncplane_set_fg_rgb(browse, 0xffffff);
        ncplane_set_bg_rgb(browse, 0x335577);
        ncplane_putstr_yx(browse, 0, 1, " Open File ");
        for (int x = 12; x < bcol - 1; x++)
            ncplane_putchar_yx(browse, 0, x, ' ');

        // Files
        int vis_rows = brow - 3;
        if (scroll > sel) scroll = sel;
        if (sel >= scroll + vis_rows) scroll = sel - vis_rows + 1;

        for (int i = 0; i < vis_rows && i + scroll < (int)files.size(); i++) {
            int fi = i + scroll;
            bool is_sel = (fi == sel);
            if (is_sel) {
                ncplane_set_fg_rgb(browse, 0x000000);
                ncplane_set_bg_rgb(browse, 0x88bbff);
            } else {
                ncplane_set_fg_rgb(browse, 0xcccccc);
                ncplane_set_bg_rgb(browse, 0x222222);
            }
            std::string item = " " + files[fi];
            while ((int)item.size() < bcol - 2) item += ' ';
            ncplane_putstr_yx(browse, 2 + i, 1, item.c_str());
        }

        if (files.empty()) {
            ncplane_set_fg_rgb(browse, 0x888888);
            ncplane_set_bg_rgb(browse, 0x222222);
            ncplane_putstr_yx(browse, 2, 2, " (no .bf files) ");
        }

        // Bottom bar
        ncplane_set_fg_rgb(browse, 0xaaaaaa);
        ncplane_set_bg_rgb(browse, 0x333333);
        std::string help = " \x18/\x19 navigate  Enter open  Esc cancel ";
        ncplane_putstr_yx(browse, brow - 1, 1, help.c_str());

        notcurses_render(nc);

        // Get input
        struct ncinput ni;
        uint32_t key = notcurses_get_nblock(nc, &ni);
        if (key == 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 16600000 };
            nanosleep(&ts, nullptr);
            continue;
        }

        if (key == NCKEY_ESC) {
            done = true;
            result = false;
        } else if (key == NCKEY_ENTER || (ni.id == '\n') || (ni.id == '\r')) {
            if (!files.empty() && sel < (int)files.size()) {
                selected = files[sel];
                result = true;
                if (sel == 0) selected.clear(); // signal "new file"
            }
            done = true;
        } else if (key == NCKEY_UP) {
            if (sel > 0) sel--;
        } else if (key == NCKEY_DOWN) {
            if (sel < (int)files.size() - 1) sel++;
        }
    }

    ncplane_destroy(browse);
    notcurses_render(nc);
    return result;
}

// ── ASCII label for tape display (raw unsigned char) ────────────
static const char* ascii_label(unsigned char c) {
    static char lbl[2] = {0};
    lbl[0] = (char)c;
    return lbl;
}

// ── History entry for undo/redo ────────────────────────────────
struct HistoryEntry {
    int ip, ptr, cell;
    unsigned char old_val, new_val;
};

// ── Shared state between UI and interpreter thread ──────────────
struct BFState {
    std::mutex mtx;
    std::condition_variable cv;
    std::string output;
    std::unordered_map<int, unsigned char> tape;
    int ptr = 0;
    bool needs_input = false;
    unsigned char input_val = 0;
    bool input_ready = false;
    bool running = true;
    bool finished = false;
    std::string error;
    int wait_ms = 100;
    // Debug features
    bool paused = false;
    bool recording = false;
    static constexpr int MAX_HISTORY = 100000;
    std::vector<HistoryEntry> history;
    std::string input_buffer;
    bool input_done = false;
};

// ── Interpreter thread (runs BF code, signals for , input) ──────
static void bf_interp_thread(const std::string& code, BFState& state) {
    std::unordered_map<int, int> jumps;
    std::vector<int> stack;
    for (int i = 0; i < (int)code.size(); i++) {
        if (code[i] == '[') stack.push_back(i);
        else if (code[i] == ']') {
            if (stack.empty()) {
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    state.error = "Unmatched ] at " + std::to_string(i);
                    state.finished = true;
                    state.running = false;
                }
                state.cv.notify_one();
                return;
            }
            int j = stack.back(); stack.pop_back();
            jumps[j] = i; jumps[i] = j;
        }
    }
    if (!stack.empty()) {
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.error = "Unmatched [ at " + std::to_string(stack.back());
            state.finished = true;
            state.running = false;
        }
        state.cv.notify_one();
        return;
    }

    int p = 0, ip = 0;
    { std::lock_guard<std::mutex> lk(state.mtx); state.ptr = p; }
    while (ip < (int)code.size()) {
        // ── Check control signals under lock ──
        {
            std::unique_lock<std::mutex> lk(state.mtx);
            // Wait if paused (re-check on cv notify)
            while (state.paused && state.running)
                state.cv.wait(lk);
            if (!state.running) break;
            state.ptr = p;
        }

        char c = code[ip];

        // ── Save history entry (BEFORE execution, only when recording) ──
        bool did_push = false;
        if (state.recording) {
            std::lock_guard<std::mutex> lk(state.mtx);
            unsigned char old_val = c == '+' ? state.tape[p] :
                                  c == '-' ? state.tape[p] :
                                  c == ',' ? state.tape[p] : 0;
            if ((int)state.history.size() < BFState::MAX_HISTORY) {
                state.history.push_back({ip, p,
                    (c == '+' || c == '-' || c == ',') ? p : -1,
                    old_val, 0});
                did_push = true;
            }
        }

        // ── Execute instruction ──
        if (c == '>') { p++; }
        else if (c == '<') { p--; }
        else if (c == '+') {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.tape[p]++;
            if (did_push) state.history.back().new_val = state.tape[p];
        } else if (c == '-') {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.tape[p]--;
            if (did_push) state.history.back().new_val = state.tape[p];
        } else if (c == '.') {
            {
                std::lock_guard<std::mutex> lk(state.mtx);
                state.output += (char)state.tape[p];
            }
            state.cv.notify_one();
        } else if (c == ',') {
            std::unique_lock<std::mutex> lk(state.mtx);
            // Read from input buffer; show dialog if empty
            while (state.input_buffer.empty() && state.running) {
                state.needs_input = true;
                state.input_done = false;
                lk.unlock();
                state.cv.notify_one();
                lk.lock();
                state.cv.wait(lk, [&]{ return state.input_done || !state.running; });
            }
            if (!state.running) return;
            state.tape[p] = (unsigned char)state.input_buffer[0];
            if (did_push) state.history.back().new_val = state.tape[p];
            state.input_buffer.erase(state.input_buffer.begin());
            state.needs_input = false; // only set true when actually waiting
        } else if (c == '[') {
            unsigned char val;
            { std::lock_guard<std::mutex> lk(state.mtx);
              auto it = state.tape.find(p);
              val = (it != state.tape.end()) ? it->second : 0; }
            if (val == 0) ip = jumps[ip];
        } else if (c == ']') {
            unsigned char val;
            { std::lock_guard<std::mutex> lk(state.mtx);
              auto it = state.tape.find(p);
              val = (it != state.tape.end()) ? it->second : 0; }
            if (val != 0) ip = jumps[ip];
        }
        ip++;

        // ── Sleep after instruction ──
        int wm;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            state.ptr = p;
            wm = state.wait_ms;
        }
        if (wm > 0) {
            struct timespec ts = {.tv_sec = wm / 1000, .tv_nsec = (wm % 1000) * 1000000};
            nanosleep(&ts, nullptr);
        }
    }
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.finished = true;
        state.running = false;
    }
    state.cv.notify_one();
}

// ── Run brainfuck program (in-process with tape display) ─────────
static void run_program(notcurses* nc, struct ncplane* plane,
                         const std::string& code) {
    unsigned int term_y, term_x;
    notcurses_term_dim_yx(nc, &term_y, &term_x);

    // ── Overlay plane ───────────────────────────────────────────────
    struct ncplane_options nopts = {};
    nopts.y = 0; nopts.x = 0;
    nopts.rows = (int)term_y; nopts.cols = (int)term_x;
    struct ncplane* run_plane = ncplane_create(notcurses_stdplane(nc), &nopts);
    if (!run_plane) return;
    uint64_t bgch = 0;
    ncchannels_set_bg_rgb(&bgch, 0x111111);
    ncplane_set_base(run_plane, " ", 0, bgch);

    const int TAPE_W = 11;
    int tape_col = (int)term_x - TAPE_W;
    if (tape_col < 15) tape_col = 15;

    // ── Spawn interpreter thread ────────────────────────────────────
    BFState state;
    std::thread interp(bf_interp_thread, std::ref(code), std::ref(state));

    // ── Render state ────────────────────────────────────────────────
    int scroll_top = 0, scroll_left = 0;
    bool follow_bottom = true;
    bool done = false;
    std::string cached_out;
    int display_pos = -1; // -1 = live, else index into state.history
    std::unordered_map<int, unsigned char> display_tape;
    int display_ptr = 0;

    while (!done) {
        // ── Snapshot shared state ──
        bool input_needed, finished, has_error;
        int cur_ptr;
        std::unordered_map<int, unsigned char> tape_snap;
        std::string err_msg;
        {
            std::lock_guard<std::mutex> lk(state.mtx);
            cached_out = state.output;
            input_needed = state.needs_input;
            finished = state.finished;
            cur_ptr = state.ptr;
            // Only snapshot cells visible in tape column
            int tape_cells_snap = (int)term_y - 2;
            int mid = tape_cells_snap / 2;
            int first = (cur_ptr < mid) ? 0 : cur_ptr - mid;
            for (int i = 0; i < tape_cells_snap; i++) {
                int idx = first + i;
                auto it = state.tape.find(idx);
                if (it != state.tape.end()) tape_snap[idx] = it->second;
            }
            has_error = !state.error.empty();
            err_msg = state.error;
        }

        // Override with historical state if viewing history
        if (display_pos >= 0) {
            cur_ptr = display_ptr;
            tape_snap = display_tape;
        }

        // ── Render frame ──
        ncplane_erase(run_plane);

        int vis_rows = (int)term_y - 2;
        int out_w = tape_col - 2;

        // Title
        ncplane_set_bg_rgb(run_plane, 0x335577);
        ncplane_set_fg_rgb(run_plane, 0xffffff);
        ncplane_putstr_yx(run_plane, 0, 1, " Program Output ");
        for (int x = 17; x < (int)term_x - 1; x++)
            ncplane_putchar_yx(run_plane, 0, x, ' ');

        // ── Output text ──
        std::vector<int> line_starts = {0};
        for (int i = 0; i < (int)cached_out.size(); i++)
            if (cached_out[i] == '\n') line_starts.push_back(i + 1);
        int total_lines = (int)line_starts.size();

        int max_w = 0;
        for (int i = 0; i < total_lines; i++) {
            int end = (i + 1 < total_lines) ? line_starts[i+1] - 1 : (int)cached_out.size();
            int w = end - line_starts[i];
            if (w > max_w) max_w = w;
        }
        int max_scroll_v = total_lines - vis_rows;
        if (max_scroll_v < 0) max_scroll_v = 0;
        int max_scroll_h = max_w - out_w;
        if (max_scroll_h < 0) max_scroll_h = 0;
        if (scroll_top > max_scroll_v) scroll_top = max_scroll_v;
        if (scroll_top < 0) scroll_top = 0;
        if (scroll_left > max_scroll_h) scroll_left = max_scroll_h;
        if (scroll_left < 0) scroll_left = 0;

        // Auto-scroll to bottom if following
        if (follow_bottom) scroll_top = max_scroll_v;

        ncplane_set_fg_rgb(run_plane, 0xf0f0f0);
        ncplane_set_bg_rgb(run_plane, 0x111111);
        for (int r = 0; r < vis_rows && scroll_top + r < total_lines; r++) {
            int li = scroll_top + r;
            int s = line_starts[li];
            int e = (li + 1 < total_lines) ? line_starts[li+1] - 1 : (int)cached_out.size();
            int c = 0;
            for (int i = s + scroll_left; i < e && c < out_w; i++) {
                char ch = cached_out[i];
                if (ch >= 32) {
                    ncplane_putchar_yx(run_plane, r + 1, c + 1, ch);
                    c++;
                }
            }
        }

        // ── Tape display (right column) ──
        int tape_cells = vis_rows;
        int mid = tape_cells / 2;
        int first_cell = (cur_ptr < mid) ? 0 : cur_ptr - mid;

        // Grey separator bar
        ncplane_set_bg_rgb(run_plane, 0x333333);
        ncplane_set_fg_rgb(run_plane, 0x333333);
        for (int y = 0; y < vis_rows; y++)
            ncplane_putchar_yx(run_plane, y + 1, tape_col - 1, ' ');

        // Tape header label
        ncplane_set_bg_rgb(run_plane, 0x335577);
        ncplane_set_fg_rgb(run_plane, 0xaaaaaa);
        ncplane_putstr_yx(run_plane, 0, tape_col, " Cells");

        // Cell rows
        for (int i = 0; i < tape_cells; i++) {
            int idx = first_cell + i;
            auto it = tape_snap.find(idx);
            unsigned char val = (it != tape_snap.end()) ? it->second : 0;
            bool is_cur = (idx == cur_ptr);
            bool nz = (val != 0);

            if (is_cur) {
                ncplane_set_bg_rgb(run_plane, 0x335577);
                ncplane_set_fg_rgb(run_plane, 0xffffff);
            } else {
                ncplane_set_bg_rgb(run_plane, 0x111111);
                ncplane_set_fg_rgb(run_plane, nz ? 0xcccccc : 0x555555);
            }
            char buf[16];
            snprintf(buf, 16, is_cur ? ">[%3d]:%s" : " [%3d]:%s", (int)val, ascii_label(val));
            ncplane_putstr_yx(run_plane, i + 1, tape_col, buf);
        }

        // ── Status bar ──
        std::string status;
        if (has_error)
            status = " ERROR: " + err_msg + " ";
        else if (finished)
            status = " Finished ";
        else if (input_needed)
            status = " Input ";
        else
            status = " Running ";

        std::string hints;
        if (has_error)            hints = " any key close ";
        else if (finished)        hints = " Q/Esc close  arrows scroll ";
        else if (input_needed)    hints = " Esc=cancel ";
        else {
            int wm; bool paus;
            { std::lock_guard<std::mutex> lk(state.mtx); wm = state.wait_ms; paus = state.paused; }
            if (paus) status += "PAUSED ";
            status += std::to_string(wm) + "ms ";
            hints = " Q=abort  g=pause  j/k=undo/redo  arrows=scroll ";
        }

        ncplane_set_bg_rgb(run_plane, 0x333333);
        ncplane_set_fg_rgb(run_plane, 0xaaaaaa);
        for (int x = 0; x < (int)term_x; x++)
            ncplane_putchar_yx(run_plane, (int)term_y - 1, x, ' ');
        ncplane_putstr_yx(run_plane, (int)term_y - 1, 1, (status + hints).c_str());

        std::string lc = " Ln" + std::to_string(scroll_top + 1) + " Col" + std::to_string(scroll_left + 1);
        ncplane_putstr_yx(run_plane, (int)term_y - 1,
            (int)term_x - (int)lc.size() - 1, lc.c_str());

        notcurses_render(nc);

        // ── Input handling ──
        if (input_needed) {
            // ── Input dialog (multi-key, Enter submits) ──
            unsigned int iy, ix;
            notcurses_term_dim_yx(nc, &iy, &ix);
            int box_h = 6, box_w = 50;
            int by0 = ((int)iy - box_h) / 2;
            int bx0 = ((int)ix - box_w) / 2;
            if (by0 < 0) by0 = 0; if (bx0 < 0) bx0 = 0;

            struct ncplane_options nopts = {};
            nopts.y = by0; nopts.x = bx0;
            nopts.rows = box_h; nopts.cols = box_w;
            struct ncplane* inp = ncplane_create(notcurses_stdplane(nc), &nopts);
            uint64_t bgc = 0;
            ncchannels_set_fg_rgb(&bgc, 0xe0e0e0);
            ncchannels_set_bg_rgb(&bgc, 0x222222);
            ncplane_set_base(inp, " ", 0, bgc);

            std::string inp_text;
            int icursor = 0;
            bool inp_done = false, inp_cancel = false;
            while (!inp_done && !inp_cancel) {
                ncplane_erase(inp);
                ncplane_set_bg_rgb(inp, 0x335577);
                ncplane_set_fg_rgb(inp, 0xffffff);
                ncplane_putstr_yx(inp, 0, 1, " Program Input " );
                for (int x = 16; x < box_w - 1; x++)
                    ncplane_putchar_yx(inp, 0, x, ' ');

                ncplane_set_fg_rgb(inp, 0xcccccc);
                ncplane_set_bg_rgb(inp, 0x111111);
                std::string disp = inp_text;
                int dw = box_w - 4;
                int scroll_off = 0;
                if ((int)disp.size() > dw) {
                    // Scroll to keep cursor visible
                    if (icursor <= dw / 2)
                        scroll_off = 0;
                    else if (icursor >= (int)disp.size() - dw / 2)
                        scroll_off = (int)disp.size() - dw;
                    else
                        scroll_off = icursor - dw / 2;
                    if (scroll_off < 0) scroll_off = 0;
                    if (scroll_off > (int)disp.size() - dw) scroll_off = (int)disp.size() - dw;
                    disp = disp.substr(scroll_off, dw);
                }
                ncplane_putstr_yx(inp, 2, 2, disp.c_str());

                // Draw cursor at correct position
                int cvis = icursor - scroll_off;
                if (cvis >= 0 && cvis < dw) {
                    ncplane_set_fg_rgb(inp, 0x000000);
                    ncplane_set_bg_rgb(inp, 0x44aaff);
                    ncplane_putchar_yx(inp, 2, 2 + cvis,
                        cvis < (int)disp.size() ? disp[cvis] : ' ');
                }

                // Bottom bar — fill full width (same logic as header)
                ncplane_set_fg_rgb(inp, 0xaaaaaa);
                ncplane_set_bg_rgb(inp, 0x333333);
                ncplane_putstr_yx(inp, box_h-1, 1, " Enter=submit  \x18/\x19 cursor  Esc=cancel ");
                for (int x = 39; x < box_w - 1; x++)
                    ncplane_putchar_yx(inp, box_h-1, x, ' ');

                // Check if program finished while dialog was open
                {
                    std::lock_guard<std::mutex> lk(state.mtx);
                    if (!state.running) break;
                }

                notcurses_render(nc);

                struct ncinput ni2;
                uint32_t k2 = notcurses_get_nblock(nc, &ni2);
                if (k2 == 0) {
                    struct timespec ts = { .tv_sec = 0, .tv_nsec = 16600000 };
                    nanosleep(&ts, nullptr);
                    continue;
                }
                if (k2 == NCKEY_ESC) inp_cancel = true;
                else if (k2 == NCKEY_ENTER || ni2.id == '\n' || ni2.id == '\r') {
                    // Insert newline at cursor (like stdio), flush to interpreter, keep dialog open
                    inp_text.insert(inp_text.begin() + icursor, '\n');
                    icursor++;
                    // Flush accumulated input to interpreter
                    {
                        std::lock_guard<std::mutex> lk(state.mtx);
                        state.input_buffer += inp_text;
                        state.input_done = true;
                    }
                    state.cv.notify_one();
                    inp_text.clear();
                    icursor = 0;
                } else if (k2 == NCKEY_BACKSPACE || ni2.id == 0x7f) {
                    if (icursor > 0) {
                        inp_text.erase(icursor - 1, 1);
                        icursor--;
                    }
                } else if (k2 == NCKEY_LEFT) {
                    if (icursor > 0) icursor--;
                } else if (k2 == NCKEY_RIGHT) {
                    if (icursor < (int)inp_text.size()) icursor++;
                } else if (ni2.id >= 32 && ni2.id < 127) {
                    inp_text.insert(inp_text.begin() + icursor, (char)ni2.id);
                    icursor++;
                }
            }
            ncplane_destroy(inp);
            notcurses_render(nc);

            if (inp_cancel) {
                { std::lock_guard<std::mutex> lk(state.mtx); state.running = false; }
                state.cv.notify_one();
                done = true;
            } else {
                { std::lock_guard<std::mutex> lk(state.mtx);
                  state.input_buffer = inp_text;
                  state.input_done = true; }
                state.cv.notify_one();
            }
        } else if (has_error) {
            struct ncinput ni;
            notcurses_get_blocking(nc, &ni);
            done = true;
        } else if (!finished) {
            struct ncinput ni;
            uint32_t k = notcurses_get_nblock(nc, &ni);
            if (k == NCKEY_ESC || ni.id == 'q' || ni.id == 'Q') {
                { std::lock_guard<std::mutex> lk(state.mtx); state.running = false; }
                state.cv.notify_one();
                done = true;
            }
            auto adj_speed = [&](int dir) {
                int wm = state.wait_ms;
                int step = wm < 10 ? 1 : wm < 20 ? 5 : wm < 200 ? 10 : wm < 500 ? 50 : 100;
                int w = state.wait_ms + dir * step;
                if (w < 0) w = 0;
                if (w > 1000) w = 1000;
                state.wait_ms = w;
            };
            // Debug/undo keys
            auto undo_step = [&]() {
                std::lock_guard<std::mutex> lk(state.mtx);
                if (state.history.empty()) return;
                if (display_pos == -1) {
                    display_pos = (int)state.history.size() - 2;
                    display_tape.clear(); display_ptr = 0;
                    for (int i = 0; i <= display_pos; i++) {
                        auto& e = state.history[i];
                        if (e.cell >= 0) display_tape[e.cell] = e.new_val;
                        display_ptr = e.ptr;
                    }
                } else if (display_pos > 0) {
                    auto& e = state.history[display_pos];
                    if (e.cell >= 0) display_tape[e.cell] = e.old_val;
                    display_ptr = e.ptr;
                    display_pos--;
                }
            };
            auto redo_step = [&]() {
                if (display_pos == -1) return;
                display_pos++;
                if (display_pos >= (int)state.history.size() - 1) {
                    display_pos = -1;
                } else {
                    auto& e = state.history[display_pos];
                    if (e.cell >= 0) display_tape[e.cell] = e.new_val;
                    display_ptr = e.ptr;
                }
            };
            if (ni.id == 'g') {
                state.paused = !state.paused;
                if (state.paused) { std::lock_guard<std::mutex> lk(state.mtx); state.recording = true; }
                if (!state.paused) state.cv.notify_one();
            }
            if (state.paused) {
                if (ni.id == 'j') { state.recording = true; undo_step(); }
                else if (ni.id == 'k') redo_step();
            } else {
                if (ni.id == 'k')      adj_speed(1);
                else if (ni.id == 'j')  adj_speed(-1);
            }
            if (k == NCKEY_UP) { scroll_top = std::max(0, scroll_top - 1); follow_bottom = false; }
            else if (k == NCKEY_DOWN) { scroll_top++; if (scroll_top >= max_scroll_v) follow_bottom = true; }
            else if (k == NCKEY_LEFT) scroll_left = std::max(0, scroll_left - 4);
            else if (k == NCKEY_RIGHT) scroll_left += 4;
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 33000000};
            nanosleep(&ts, nullptr);
        } else {
            struct ncinput ni;
            uint32_t k = notcurses_get_nblock(nc, &ni);
            if (k == NCKEY_ESC || ni.id == 'q' || ni.id == 'Q') done = true;
            auto adj_speed = [&](int dir) {
                int wm = state.wait_ms;
                int step = wm < 10 ? 1 : wm < 20 ? 5 : wm < 200 ? 10 : wm < 500 ? 50 : 100;
                int w = state.wait_ms + dir * step;
                if (w < 0) w = 0;
                if (w > 1000) w = 1000;
                state.wait_ms = w;
            };
            auto undo_step = [&]() {
                std::lock_guard<std::mutex> lk(state.mtx);
                if (state.history.empty()) return;
                if (display_pos == -1) {
                    display_pos = (int)state.history.size() - 2;
                    display_tape.clear(); display_ptr = 0;
                    for (int i = 0; i <= display_pos; i++) {
                        auto& e = state.history[i];
                        if (e.cell >= 0) display_tape[e.cell] = e.new_val;
                        display_ptr = e.ptr;
                    }
                } else if (display_pos > 0) {
                    auto& e = state.history[display_pos];
                    if (e.cell >= 0) display_tape[e.cell] = e.old_val;
                    display_ptr = e.ptr;
                    display_pos--;
                }
            };
            auto redo_step = [&]() {
                if (display_pos == -1) return;
                display_pos++;
                if (display_pos >= (int)state.history.size() - 1) {
                    display_pos = -1;
                } else {
                    auto& e = state.history[display_pos];
                    if (e.cell >= 0) display_tape[e.cell] = e.new_val;
                    display_ptr = e.ptr;
                }
            };
            if (ni.id == 'g') {
                state.paused = !state.paused;
                if (state.paused) { std::lock_guard<std::mutex> lk(state.mtx); state.recording = true; }
                if (!state.paused) state.cv.notify_one();
            }
            if (state.paused) {
                if (ni.id == 'j') { state.recording = true; undo_step(); }
                else if (ni.id == 'k') redo_step();
            } else {
                if (ni.id == 'k')      adj_speed(1);
                else if (ni.id == 'j')  adj_speed(-1);
            }
            if (k == NCKEY_UP) { scroll_top = std::max(0, scroll_top - 1); follow_bottom = false; }
            else if (k == NCKEY_DOWN) { scroll_top++; if (scroll_top >= max_scroll_v) follow_bottom = true; }
            else if (k == NCKEY_LEFT) scroll_left = std::max(0, scroll_left - 4);
            else if (k == NCKEY_RIGHT) scroll_left += 4;
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 33000000};
            nanosleep(&ts, nullptr);
        }
    }

    // ── Cleanup ──
    {
        std::lock_guard<std::mutex> lk(state.mtx);
        state.running = false;
    }
    state.cv.notify_one();
    if (interp.joinable()) interp.join();

    ncplane_destroy(run_plane);
    notcurses_render(nc);
}

// ── Save file ───────────────────────────────────────────────────────
static std::string save_dialog(notcurses* nc, struct ncplane* parent,
                                const std::string& dir,
                                const std::string& current) {
    unsigned int term_y, term_x;
    notcurses_term_dim_yx(nc, &term_y, &term_x);

    int box_w = 50;
    int box_h = 5;
    int by0 = ((int)term_y - box_h) / 2;
    int bx0 = ((int)term_x - box_w) / 2;

    struct ncplane_options nopts = {};
    nopts.y = by0;
    nopts.x = bx0;
    nopts.rows = box_h;
    nopts.cols = box_w;

    struct ncplane* dbox = ncplane_create(notcurses_stdplane(nc), &nopts);
    if (!dbox) return "";

    uint64_t bgch = 0;
    ncchannels_set_fg_rgb(&bgch, 0xe0e0e0);
    ncchannels_set_bg_rgb(&bgch, 0x222222);
    ncplane_set_base(dbox, " ", 0, bgch);

    ncplane_set_fg_rgb(dbox, 0xffffff);
    ncplane_set_bg_rgb(dbox, 0x335577);
    ncplane_putstr_yx(dbox, 0, 1, " Save As ");
    for (int x = 10; x < box_w - 1; x++)
        ncplane_putchar_yx(dbox, 0, x, ' ');

    // Default filename (strip .bf for display)
    std::string display_name = current.empty() ? "program" : current;
    if (display_name.size() > 3 && display_name.substr(display_name.size()-3) == ".bf")
        display_name = display_name.substr(0, display_name.size()-3);
    std::string result = display_name;
    int cursor = (int)result.size();

    bool done = false;
    while (!done) {
        // Clear
        ncplane_set_fg_rgb(dbox, 0xcccccc);
        ncplane_set_bg_rgb(dbox, 0x111111);
        for (int x = 1; x < box_w - 1; x++)
            ncplane_putchar_yx(dbox, 2, x, ' ');

        // Draw filename with cursor
        std::string disp = result;
        if ((int)disp.size() > box_w - 5)
            disp = disp.substr(disp.size() - (box_w - 5));
        for (int ci = 0; ci < (int)disp.size() && ci + 2 < box_w - 1; ci++) {
            if (ci == cursor) {
                ncplane_set_fg_rgb(dbox, 0x000000);
                ncplane_set_bg_rgb(dbox, 0x44aaff);
            } else {
                ncplane_set_fg_rgb(dbox, 0xcccccc);
                ncplane_set_bg_rgb(dbox, 0x111111);
            }
            ncplane_putchar_yx(dbox, 2, 2 + ci, disp[ci]);
        }
        // Draw cursor at end if past last char
        if (cursor >= (int)disp.size() && 2 + (int)disp.size() < box_w - 1) {
            ncplane_set_fg_rgb(dbox, 0x000000);
            ncplane_set_bg_rgb(dbox, 0x44aaff);
            ncplane_putchar_yx(dbox, 2, 2 + (int)disp.size(), ' ');
        }

        // Bottom bar
        ncplane_set_fg_rgb(dbox, 0x888888);
        ncplane_set_bg_rgb(dbox, 0x333333);
        ncplane_putstr_yx(dbox, box_h-1, 1, " Enter save  Esc cancel ");

        notcurses_render(nc);

        struct ncinput ni;
        uint32_t key = notcurses_get_blocking(nc, &ni);

        if (key == NCKEY_ESC) {
            result.clear();
            done = true;
        } else if (key == NCKEY_ENTER || ni.id == '\n' || ni.id == '\r') {
            if (!result.empty()) {
                result += ".bf";
                done = true;
            }
        } else if (key == NCKEY_BACKSPACE || ni.id == 0x7f) {
            if (cursor > 0) {
                result.erase(cursor - 1, 1);
                cursor--;
            }
        } else if (key == NCKEY_DEL) {
            if (cursor < (int)result.size())
                result.erase(cursor, 1);
        } else if (key == NCKEY_LEFT) {
            if (cursor > 0) cursor--;
        } else if (key == NCKEY_RIGHT) {
            if (cursor < (int)result.size()) cursor++;
        } else if (ni.id >= 32 && ni.id < 127) {
            // Printable character
            if ((int)result.size() < 100) {
                result.insert(result.begin() + cursor, (char)ni.id);
                cursor++;
            }
        }
    }

    ncplane_destroy(dbox);
    notcurses_render(nc);
    return result;
}

// ── Main editor ─────────────────────────────────────────────────────
static void editor_loop(notcurses* nc) {
    struct ncplane* stdplane = notcurses_stdplane(nc);

    // Editor state
    std::string buffer;
    int cursor = 0;
    int scroll_row = 0;
    int scroll_col = 0;
    bool code_mode = true;
    std::string current_file;
    bool quit = false;

    // Plane pointers (recreated on resize)
    struct ncplane* edplane = nullptr;
    struct ncplane* stplane = nullptr;
    unsigned int prev_term_y = 0, prev_term_x = 0;
    int st_height = 1; // current status bar height (1 or 2)

    // Lambdas to create/destroy planes
    auto destroy_planes = [&]() {
        if (edplane) { ncplane_destroy(edplane); edplane = nullptr; }
        if (stplane) { ncplane_destroy(stplane); stplane = nullptr; }
    };

    auto create_planes = [&]() -> bool {
        unsigned int term_y, term_x;
        notcurses_term_dim_yx(nc, &term_y, &term_x);

        prev_term_y = term_y;
        prev_term_x = term_x;

        // Calculate status bar: try 1 row first
        st_height = 1;
        int ed_rows = (int)term_y - st_height;
        if (ed_rows < 1) ed_rows = 1;

        // Editor plane
        struct ncplane_options nopts = {};
        nopts.y = 0;
        nopts.x = 0;
        nopts.rows = ed_rows;
        nopts.cols = (int)term_x;
        edplane = ncplane_create(stdplane, &nopts);

        // Status plane
        struct ncplane_options snopts = {};
        snopts.y = ed_rows;
        snopts.x = 0;
        snopts.rows = st_height;
        snopts.cols = (int)term_x;
        stplane = ncplane_create(stdplane, &snopts);

        if (!edplane || !stplane) {
            destroy_planes();
            return false;
        }

        uint64_t edbg = 0;
        ncchannels_set_bg_rgb(&edbg, 0x0a0a0a);
        ncplane_set_base(edplane, " ", 0, edbg);

        uint64_t stbg = 0;
        ncchannels_set_fg_rgb(&stbg, 0xcccccc);
        ncchannels_set_bg_rgb(&stbg, 0x222244);
        ncplane_set_base(stplane, " ", 0, stbg);

        return true;
    };

    if (!create_planes()) return;

    while (!quit) {
        // ── Check for resize ──────────────────────────────────────
        {
            unsigned int cur_y, cur_x;
            notcurses_term_dim_yx(nc, &cur_y, &cur_x);
            if (cur_y != prev_term_y || cur_x != prev_term_x) {
                destroy_planes();
                if (!create_planes()) { quit = true; break; }
            }
        }

        // ── Render editor content ────────────────────────────────
        ncplane_erase(edplane);

        unsigned int max_rows_u, max_cols_u;
        ncplane_dim_yx(edplane, &max_rows_u, &max_cols_u);
        int max_rows = (int)max_rows_u;
        int max_cols = (int)max_cols_u;

        // Calculate lines
        std::vector<int> line_starts;
        line_starts.push_back(0);
        for (int i = 0; i < (int)buffer.size(); i++) {
            if (buffer[i] == '\n') line_starts.push_back(i + 1);
        }

        // Find cursor line/col
        int cursor_line = 0, cursor_col = 0;
        for (int i = 0; i < (int)line_starts.size(); i++) {
            int next = (i + 1 < (int)line_starts.size()) ? line_starts[i+1] - 1 : (int)buffer.size();
            if (line_starts[i] <= cursor && cursor <= next) {
                cursor_line = i;
                cursor_col = cursor - line_starts[i];
                break;
            }
        }

        // Adjust vertical scroll
        if (cursor_line < scroll_row) scroll_row = cursor_line;
        if (cursor_line >= scroll_row + max_rows) scroll_row = cursor_line - max_rows + 1;
        if (scroll_row < 0) scroll_row = 0;
        // Adjust horizontal scroll
        if (cursor_col < scroll_col) scroll_col = cursor_col;
        if (cursor_col >= scroll_col + max_cols) scroll_col = cursor_col - max_cols + 1;
        if (scroll_col < 0) scroll_col = 0;

        // Draw visible lines
        for (int row = 0; row < max_rows; row++) {
            int li = scroll_row + row;
            if (li >= (int)line_starts.size()) break;

            int start = line_starts[li];
            int end = (li + 1 < (int)line_starts.size())
                        ? line_starts[li + 1] - 1
                        : (int)buffer.size();

            int col;
            int vis_col = cursor_col - scroll_col;
            for (col = 0; col < max_cols; col++) {
                int pos = start + scroll_col + col;
                if (pos >= end) break;

                char c = buffer[pos];
                unsigned color = color_for_bf(c);

                ncplane_set_fg_rgb(edplane, color);
                ncplane_set_bg_rgb(edplane, 0x0a0a0a);

                if (pos == cursor) {
                    if (code_mode) {
                        ncplane_set_fg_rgb(edplane, 0x000000);
                        ncplane_set_bg_rgb(edplane, 0xffaa00);
                    } else {
                        ncplane_set_fg_rgb(edplane, 0x000000);
                        ncplane_set_bg_rgb(edplane, 0x44aaff);
                    }
                } else if (c == '\t') {
                    c = ' ';
                }

                ncplane_putchar_yx(edplane, row, col, c);
            }

            // Cursor at end of line (only if visible)
            int line_end_pos = start + (end - start);
            if (line_end_pos == cursor && vis_col >= 0 && vis_col < max_cols) {
                if (code_mode) {
                    ncplane_set_fg_rgb(edplane, 0x000000);
                    ncplane_set_bg_rgb(edplane, 0xffaa00);
                } else {
                    ncplane_set_fg_rgb(edplane, 0x000000);
                    ncplane_set_bg_rgb(edplane, 0x44aaff);
                }
                ncplane_putchar_yx(edplane, row, vis_col, ' ');
            }
        }

        // ── Build status content ──────────────────────────────────
        std::string mode_tag = code_mode ? " CODE " : " INPUT ";
        std::string fn_tag = " " + (current_file.empty() ? "(unnamed)" : current_file) + " ";
        std::string keymap_str;
        if (code_mode)
            keymap_str = " ,[<-+>].  a:, s:[ d:< f:-  h:+ j:> k:] l:. ";
        std::string hints = " F1:Save F2:Open F3:Mode F4:Quit F5:Run ";
        std::string lc = " Ln" + std::to_string(cursor_line + 1) +
                         " Col" + std::to_string(cursor_col + 1);

        // Check if everything fits on one row
        std::string line1_content = mode_tag + " " + fn_tag;
        if (!keymap_str.empty()) line1_content += keymap_str;

        int total_needed_1row = (int)line1_content.size() + (int)hints.size() + (int)lc.size() + 3;

        // If one line is enough: use 1-row status bar
        if (total_needed_1row < max_cols && st_height == 1) {
            // Row 0: [mode] [filename] [keymap] ... [hints] [Ln:Col]
            ncplane_erase(stplane);
            ncplane_set_fg_rgb(stplane, 0xcccccc);
            ncplane_set_bg_rgb(stplane, 0x222244);

            // Mode badge
            ncplane_set_bg_rgb(stplane, code_mode ? 0xffaa00 : 0x44aaff);
            ncplane_set_fg_rgb(stplane, 0x000000);
            ncplane_putstr_yx(stplane, 0, 0, mode_tag.c_str());

            // Filename
            ncplane_set_fg_rgb(stplane, 0xcccccc);
            ncplane_set_bg_rgb(stplane, 0x222244);
            ncplane_putstr_yx(stplane, 0, 6, fn_tag.c_str());
            int fn_end = 6 + (int)fn_tag.size();

            // Keymap
            if (!keymap_str.empty() && fn_end + (int)keymap_str.size() < max_cols - (int)hints.size() - (int)lc.size() - 5) {
                ncplane_putstr_yx(stplane, 0, fn_end, keymap_str.c_str());
                fn_end += (int)keymap_str.size();
            }

            // Hints + Ln:Col on right
            int lc_w = (int)lc.size() + 2;
            int hints_w = (int)hints.size() + 1;
            if (fn_end + hints_w + lc_w < max_cols) {
                ncplane_putstr_yx(stplane, 0, max_cols - lc_w - hints_w, hints.c_str());
                ncplane_putstr_yx(stplane, 0, max_cols - lc_w, lc.c_str());
            } else {
                ncplane_putstr_yx(stplane, 0, max_cols - lc_w, lc.c_str());
            }
        } else {
            // Need 2 rows (or re-create with 2 rows)
            if (st_height != 2) {
                // Recreate status plane with 2 rows
                destroy_planes();
                unsigned int term_y, term_x;
                notcurses_term_dim_yx(nc, &term_y, &term_x);
                prev_term_y = term_y;
                prev_term_x = term_x;
                st_height = 2;
                int ed_rows = (int)term_y - st_height;
                if (ed_rows < 1) ed_rows = 1;

                struct ncplane_options nopts = {};
                nopts.y = 0; nopts.x = 0; nopts.rows = ed_rows; nopts.cols = (int)term_x;
                edplane = ncplane_create(stdplane, &nopts);
                struct ncplane_options snopts = {};
                snopts.y = ed_rows; snopts.x = 0; snopts.rows = st_height; snopts.cols = (int)term_x;
                stplane = ncplane_create(stdplane, &snopts);

                uint64_t edbg = 0, stbg = 0;
                ncchannels_set_bg_rgb(&edbg, 0x0a0a0a);
                ncplane_set_base(edplane, " ", 0, edbg);
                ncchannels_set_fg_rgb(&stbg, 0xcccccc);
                ncchannels_set_bg_rgb(&stbg, 0x222244);
                ncplane_set_base(stplane, " ", 0, stbg);

                // Skip this frame — next iteration will redraw from scratch
                notcurses_render(nc);
                continue;
            }

            // 2-row status
            ncplane_erase(stplane);

            // Row 0: mode + filename + keymap
            ncplane_set_bg_rgb(stplane, code_mode ? 0xffaa00 : 0x44aaff);
            ncplane_set_fg_rgb(stplane, 0x000000);
            ncplane_putstr_yx(stplane, 0, 0, mode_tag.c_str());

            ncplane_set_fg_rgb(stplane, 0xcccccc);
            ncplane_set_bg_rgb(stplane, 0x222244);
            ncplane_putstr_yx(stplane, 0, 6, fn_tag.c_str());
            int fn_end = 6 + (int)fn_tag.size();

            if (!keymap_str.empty() && fn_end + (int)keymap_str.size() < max_cols - 2) {
                ncplane_putstr_yx(stplane, 0, fn_end, keymap_str.c_str());
            }

            // Row 1: hints + Ln:Col
            int lc_w = (int)lc.size() + 2;
            int hints_w = (int)hints.size() + 1;
            int needed = hints_w + lc_w;
            if (needed < max_cols) {
                ncplane_putstr_yx(stplane, 1, 0, hints.c_str());
                ncplane_putstr_yx(stplane, 1, max_cols - lc_w, lc.c_str());
            } else {
                ncplane_putstr_yx(stplane, 1, 0, lc.c_str());
            }
        }

        notcurses_render(nc);

        // ── Input handling ───────────────────────────────────────
        struct ncinput ni;
        uint32_t key = notcurses_get_nblock(nc, &ni);

        if (key == 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 16600000 };
            nanosleep(&ts, nullptr);
            continue;
        }

        // ── Global commands ──────────────────────────────────────
        // Ctrl+R or F5 = run
        if ((ncinput_ctrl_p(&ni) && (ni.id == 'r' || ni.id == 'R'))
            || (key == NCKEY_F05)) {
            run_program(nc, edplane, buffer);
            continue;
        }

        // Ctrl+S or Ctrl+X or F1 = save
        if ((ncinput_ctrl_p(&ni) && (ni.id == 's' || ni.id == 'S' || ni.id == 'x' || ni.id == 'X'))
            || (key == NCKEY_F01)) {
            std::string fn = save_dialog(nc, edplane, programs_dir, current_file);
            if (!fn.empty()) {
                std::string path = programs_dir + "/" + fn;
                std::ofstream f(path);
                f << buffer;
                current_file = fn;
            }
            continue;
        }

        // Ctrl+O or F2 = open
        if ((ncinput_ctrl_p(&ni) && (ni.id == 'o' || ni.id == 'O'))
            || (key == NCKEY_F02)) {
            std::string fn;
            if (file_browser_ui(nc, edplane, programs_dir, fn)) {
                if (fn.empty()) {
                    // New file
                    buffer.clear();
                    cursor = 0;
                    scroll_row = 0;
                    current_file.clear();
                } else {
                    std::string path = programs_dir + "/" + fn;
                    std::ifstream f(path);
                    if (f) {
                        buffer = std::string(
                            (std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>()
                        );
                        cursor = 0;
                        scroll_row = 0;
                        current_file = fn;
                    }
                }
            }
            continue;
        }

        // Esc or F3 = toggle mode
        if (key == NCKEY_ESC || key == NCKEY_F03) {
            code_mode = !code_mode;
            continue;
        }

        // Ctrl+Q or F4 = quit
        if ((ncinput_ctrl_p(&ni) && (ni.id == 'q' || ni.id == 'Q'))
            || (key == NCKEY_F04)) {
            quit = true;
            continue;
        }

        // Ctrl+A = toggle mode
        if (ncinput_ctrl_p(&ni) && (ni.id == 'a' || ni.id == 'A')) {
            code_mode = !code_mode;
            continue;
        }

        // ── Arrow keys ───────────────────────────────────────────
        if (key == NCKEY_LEFT) {
            if (cursor > 0) cursor--;
            continue;
        }
        if (key == NCKEY_RIGHT) {
            // Don't advance into newline character
            if (cursor < (int)buffer.size() && buffer[cursor] != '\n') cursor++;
            continue;
        }
        if (key == NCKEY_UP) {
            // Move cursor up one line
            int line_start = cursor;
            while (line_start > 0 && buffer[line_start - 1] != '\n') line_start--;
            int col = cursor - line_start;

            if (line_start > 0) {
                int prev_end = line_start - 1;
                int prev_start = prev_end;
                while (prev_start > 0 && buffer[prev_start - 1] != '\n') prev_start--;
                // If cursor was past prev line end, clamp; if was on \n, go to end-1
                int new_pos = prev_start + col;
                if (new_pos >= prev_end) new_pos = prev_end > prev_start ? prev_end : prev_end;
                if (new_pos > prev_end) new_pos = prev_end;
                cursor = new_pos;
            }
            continue;
        }
        if (key == NCKEY_DOWN) {
            int line_start = cursor;
            while (line_start > 0 && buffer[line_start - 1] != '\n') line_start--;
            int col = cursor - line_start;

            int line_end = cursor;
            while (line_end < (int)buffer.size() && buffer[line_end] != '\n') line_end++;

            if (line_end < (int)buffer.size()) {
                int next_start = line_end + 1;
                int next_end = next_start;
                while (next_end < (int)buffer.size() && buffer[next_end] != '\n') next_end++;
                int new_pos = next_start + col;
                if (new_pos > next_end) new_pos = next_end;
                cursor = new_pos;
            }
            continue;
        }
        if (key == NCKEY_PGUP) {
            int new_scroll = scroll_row - max_rows;
            if (new_scroll < 0) new_scroll = 0;
            scroll_row = new_scroll;
            cursor = line_starts[scroll_row];
            continue;
        }
        if (key == NCKEY_PGDOWN) {
            int target_scroll = scroll_row + max_rows;
            if (target_scroll >= (int)line_starts.size()) target_scroll = (int)line_starts.size() - 1;
            if (target_scroll < 0) target_scroll = 0;
            scroll_row = target_scroll;
            cursor = line_starts[scroll_row];
            continue;
        }
        if (key == NCKEY_HOME) {
            // Move to start of line
            int line_start = 0;
            for (int i = cursor - 1; i >= 0; i--) {
                if (buffer[i] == '\n') {
                    line_start = i + 1;
                    break;
                }
            }
            cursor = line_start;
            continue;
        }
        if (key == NCKEY_END) {
            // Move to end of line
            for (int i = cursor; i < (int)buffer.size(); i++) {
                if (buffer[i] == '\n') {
                    cursor = i;
                    break;
                }
                if (i == (int)buffer.size() - 1) cursor = buffer.size();
            }
            continue;
        }

        // ── Backspace ─────────────────────────────────────────────
        if (key == NCKEY_BACKSPACE || ni.id == 0x7f) {
            if (cursor > 0) {
                buffer.erase(cursor - 1, 1);
                cursor--;
            }
            continue;
        }
        if (key == NCKEY_DEL) {
            if (cursor < (int)buffer.size()) {
                buffer.erase(cursor, 1);
            }
            continue;
        }

        // ── Enter ─────────────────────────────────────────────────
        if (key == NCKEY_ENTER || ni.id == '\n' || ni.id == '\r') {
            buffer.insert(buffer.begin() + cursor, '\n');
            cursor++;
            continue;
        }

        // ── Tab ───────────────────────────────────────────────────
        if (ni.id == '\t') {
            buffer.insert(buffer.begin() + cursor, '\t');
            cursor++;
            continue;
        }

        // ── Character input ──────────────────────────────────────
        char ch = (char)ni.id;
        if (ch < 32 || ch > 126) continue; // skip non-printable

        // Determine what character to insert
        char to_insert = 0;

        if (code_mode) {
            // ── Code mode mappings: , [ < - + > ] . ───────────────
            // Home row: a s d f   h j k l
            if      (ch == 'a' || ch == 'A') to_insert = ',';
            else if (ch == 's' || ch == 'S') to_insert = '[';
            else if (ch == 'd' || ch == 'D') to_insert = '<';
            else if (ch == 'f' || ch == 'F') to_insert = '-';
            else if (ch == 'h' || ch == 'H') to_insert = '+';
            else if (ch == 'j' || ch == 'J') to_insert = '>';
            else if (ch == 'k' || ch == 'K') to_insert = ']';
            else if (ch == 'l' || ch == 'L') to_insert = '.';
            // Top row (above home row): q w e r   y u i o
            else if (ch == 'q' || ch == 'Q') to_insert = ',';
            else if (ch == 'w' || ch == 'W') to_insert = '[';
            else if (ch == 'e' || ch == 'E') to_insert = '<';
            else if (ch == 'r' || ch == 'R') to_insert = '-';
            else if (ch == 'y' || ch == 'Y') to_insert = '+';
            else if (ch == 'u' || ch == 'U') to_insert = '>';
            else if (ch == 'i' || ch == 'I') to_insert = ']';
            else if (ch == 'o' || ch == 'O') to_insert = '.';
            // Number row (same positional mapping): 1 2 3 4   5 6 7 8
            else if (ch == '1') to_insert = ',';
            else if (ch == '2') to_insert = '[';
            else if (ch == '3') to_insert = '<';
            else if (ch == '4') to_insert = '-';
            else if (ch == '5') to_insert = '+';
            else if (ch == '6') to_insert = '>';
            else if (ch == '7') to_insert = ']';
            else if (ch == '8') to_insert = '.';
            // Direct BF commands stay as-is
            else if (is_bf_command(ch)) to_insert = ch;
            // Everything else is ignored in code mode
        } else {
            // ── Input mode: insert everything literally ──────────
            to_insert = ch;
        }

        if (to_insert != 0) {
            buffer.insert(buffer.begin() + cursor, to_insert);
            cursor++;
        }
    }

    ncplane_destroy(stplane);
    ncplane_destroy(edplane);
}

// ── Main ────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Determine base directory (where the executable lives)
    if (argc > 0 && argv[0]) {
        std::string exec_path(argv[0]);
        if (exec_path.find('/') != std::string::npos) {
            // Has a path component
            base_dir = dirname_of(exec_path);
            // Resolve relative paths
            if (base_dir == ".") {
                char cwd[1024];
                if (getcwd(cwd, sizeof(cwd))) {
                    base_dir = cwd;
                }
            }
        } else {
            // Just a bare filename, search PATH or use cwd
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd))) {
                base_dir = cwd;
            }
        }
    } else {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            base_dir = cwd;
        }
    }

    // Resolve symlinks (use realpath)
    char resolved[1024];
    if (realpath(base_dir.c_str(), resolved)) {
        base_dir = resolved;
    }

    programs_dir = base_dir + "/programs";
    ensure_dir(programs_dir);

    // Also ensure program.bf can be written
    // Check if program.bf exists in base_dir - if not, it'll be created

    // Initialize notcurses
    notcurses_options nc_opts = {};
    nc_opts.flags = NCOPTION_NO_ALTERNATE_SCREEN
                  | NCOPTION_SUPPRESS_BANNERS
                  | NCOPTION_NO_CLEAR_BITMAPS;
    struct notcurses* nc = notcurses_init(&nc_opts, stdout);
    if (!nc) {
        std::cerr << "Failed to initialize notcurses" << std::endl;
        return 1;
    }

    // Cursor is rendered visually via inverted colors

    // Run editor
    editor_loop(nc);

    // Cleanup
    notcurses_stop(nc);
    return 0;
}
