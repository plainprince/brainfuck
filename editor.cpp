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

// ── Run brainfuck program ──────────────────────────────────────────
// Saves code to temp file, forks interpreter with file path arg
static void run_program(notcurses* nc, struct ncplane* plane,
                         const std::string& code) {
    // Save to temp file in programs_dir
    std::string tmp_path = programs_dir + "/_tmp_editor_run.bf";
    {
        std::ofstream f(tmp_path);
        f << code;
    }

    // List of interpreters to try (all accept <file.bf> arg now)
    struct Interp {
        std::string cmd;
        std::string arg;
    };

    std::vector<Interp> interps;
    interps.push_back({"brainfuck", tmp_path});

    struct stat st;
    std::string bfw_path = base_dir + "/bfw";
    if (stat(bfw_path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
        interps.push_back({bfw_path, tmp_path});
    std::string bfs_path = base_dir + "/bfs";
    if (stat(bfs_path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
        interps.push_back({bfs_path, tmp_path});
    std::string bff_path = base_dir + "/bff";
    if (stat(bff_path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
        interps.push_back({bff_path, tmp_path});

    if (interps.empty()) {
        ncplane_set_fg_rgb(plane, 0xff4444);
        ncplane_putstr_yx(plane, 0, 0, "No brainfuck interpreter found!");
        notcurses_render(nc);
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, nullptr);
        return;
    }

    std::string output_path = base_dir + "/output.txt";

    // ── Create output plane ────────────────────────────────────────
    unsigned int term_y_u, term_x_u;
    notcurses_term_dim_yx(nc, &term_y_u, &term_x_u);
    struct ncplane_options nopts = {};
    nopts.y = 0;
    nopts.x = 0;
    nopts.rows = (int)term_y_u;
    nopts.cols = (int)term_x_u;
    struct ncplane* run_plane = ncplane_create(notcurses_stdplane(nc), &nopts);
    if (!run_plane) return;
    uint64_t bgch = 0;
    ncchannels_set_bg_rgb(&bgch, 0x111111);
    ncplane_set_base(run_plane, " ", 0, bgch);

    int output_row = 2;
    int output_col = 1;
    int scroll_top = 0;
    int scroll_left = 0;
    std::string captured_output;
    bool ran = false;
    bool aborted = false;

    // Line-scan helpers
    auto count_lines = [&]() -> int {
        if (captured_output.empty()) return 0;
        int n = 1;
        for (auto c : captured_output) if (c == '\n') n++;
        return n;
    };
    auto get_line = [&](int line_num, int& start, int& len) -> bool {
        int cur = 0; start = 0;
        for (size_t i = 0; i < captured_output.size(); i++) {
            if (captured_output[i] == '\n') {
                if (cur == line_num) { len = (int)i - start; return true; }
                cur++; start = (int)i + 1;
            }
        }
        if (cur == line_num) { len = (int)captured_output.size() - start; return true; }
        return false;
    };

    // Render current output onto run_plane
    auto render_output = [&](const std::string& bottom_text) {
        ncplane_erase(run_plane);
        // Title
        ncplane_set_fg_rgb(run_plane, 0xffffff);
        ncplane_set_bg_rgb(run_plane, 0x335577);
        ncplane_putstr_yx(run_plane, 0, 1, " Program Output ");
        for (int x = 17; x < (int)term_x_u - 1; x++)
            ncplane_putchar_yx(run_plane, 0, x, ' ');

        int vis_rows = (int)term_y_u - output_row - 1;
        int vis_cols = (int)term_x_u - output_col - 1;
        int total = count_lines();
        if (scroll_top > total - vis_rows) scroll_top = total - vis_rows;
        if (scroll_top < 0) scroll_top = 0;

        // Find longest line in entire output to clamp scroll_left
        int max_width = 0;
        for (int i = 0; i < total; i++) {
            int s = 0, l = 0;
            if (get_line(i, s, l) && l > max_width) max_width = l;
        }
        int max_scroll = max_width - vis_cols;
        if (max_scroll < 0) max_scroll = 0;
        if (scroll_left > max_scroll) scroll_left = max_scroll;
        if (scroll_left < 0) scroll_left = 0;

        ncplane_set_fg_rgb(run_plane, 0xf0f0f0);
        ncplane_set_bg_rgb(run_plane, 0x111111);
        int r = 0;
        for (int line = scroll_top; line < total && r < vis_rows; line++) {
            int start = 0, len = 0;
            if (!get_line(line, start, len)) break;
            int c = 0;
            for (int i = scroll_left; i < len && c < vis_cols; i++) {
                char ch = captured_output[start + i];
                if (ch >= 32) {
                    ncplane_putchar_yx(run_plane, r + output_row, c + output_col, ch);
                    c++;
                }
            }
            r++;
        }

        // Bottom bar
        std::string scroll_info;
        if (total > vis_rows)
            scroll_info += " Ln " + std::to_string(scroll_top + 1) + "/" + std::to_string(total);
        if (scroll_left > 0)
            scroll_info += " Col " + std::to_string(scroll_left + 1);
        if (!scroll_info.empty()) scroll_info += " ";
        std::string bar = scroll_info + bottom_text;
        ncplane_set_fg_rgb(run_plane, 0xaaaaaa);
        ncplane_set_bg_rgb(run_plane, 0x333333);
        for (int x = 1; x < (int)term_x_u - 1; x++)
            ncplane_putchar_yx(run_plane, (int)term_y_u - 1, x, ' ');
        ncplane_putstr_yx(run_plane, (int)term_y_u - 1, 1, bar.c_str());
        notcurses_render(nc);
    };

    // Check if stdbuf is available for line-buffered streaming
    static bool stdbuf_ok = false;
    static bool stdbuf_checked = false;
    if (!stdbuf_checked) {
        stdbuf_checked = true;
        struct stat st;
        stdbuf_ok = (stat("/opt/homebrew/bin/stdbuf", &st) == 0) ||
                    (stat("/usr/local/bin/stdbuf", &st) == 0) ||
                    (stat("/usr/bin/stdbuf", &st) == 0);
    }

    for (auto& interp : interps) {
        if (ran || aborted) break;

        const char* argv_list[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
        int ai = 0;
        if (stdbuf_ok) {
            argv_list[ai++] = "stdbuf";
            argv_list[ai++] = "-oL";
        }
        argv_list[ai++] = interp.cmd.c_str();
        if (!interp.arg.empty()) argv_list[ai++] = interp.arg.c_str();
        argv_list[ai] = nullptr;

        pid_t pid = fork();
        if (pid == 0) {
            // Child: redirect stdout+stderr to output.txt
            int fd = open(output_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
            execvp(argv_list[0], const_cast<char**>(argv_list));
            _exit(127);
        } else if (pid > 0) {
            // Parent: poll output.txt + keyboard until child exits
            int status = 0;
            captured_output.clear();

            while (true) {
                // Read output.txt and display
                {
                    std::ifstream f(output_path);
                    if (f) {
                        captured_output = std::string(
                            (std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
                    }
                }
                render_output(" Running... Q=abort \x18/\x19/jk \xe2\x86\x90/\xe2\x86\x92/hl ");

                // Check keyboard (non-blocking)
                struct ncinput ni;
                uint32_t k = notcurses_get_nblock(nc, &ni);
                if (k != 0) {
                    if (ni.id == 'q' || ni.id == 'Q') {
                        kill(pid, SIGKILL);
                        waitpid(pid, &status, 0);
                        captured_output += "\n[Aborted by user (Q)]\n";
                        aborted = true;
                        break;
                    }
                    int total = count_lines();
                    int vis = (int)term_y_u - output_row - 1;
                    if (k == NCKEY_UP || ni.id == 'k') { if (scroll_top > 0) scroll_top--; }
                    else if (k == NCKEY_DOWN || ni.id == 'j') { if (scroll_top < total - vis) scroll_top++; }
                    else if (k == NCKEY_LEFT || ni.id == 'h') { if (scroll_left > 0) scroll_left -= 4; if (scroll_left < 0) scroll_left = 0; }
                    else if (k == NCKEY_RIGHT || ni.id == 'l') { scroll_left += 4; }
                }

                // Check child exit
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w == pid) {
                    // One last read of output.txt
                    std::ifstream f(output_path);
                    if (f) {
                        captured_output = std::string(
                            (std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
                    }
                    break;
                }

                struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; // 50ms
                nanosleep(&ts, nullptr);
            }

            if (aborted) { ran = true; break; }

            if (WIFEXITED(status)) {
                int ec = WEXITSTATUS(status);
                if (ec == 127) {
                    captured_output.clear();
                    continue; // try next interpreter
                }
                ran = true;
                if (ec != 0)
                    captured_output += "\n[Exit code: " + std::to_string(ec) + "]\n";
            } else if (WIFSIGNALED(status)) {
                captured_output += "\n[Killed by signal " + std::to_string(WTERMSIG(status)) + "]\n";
                ran = true;
            }
        }
    }

    // ── Final display ──────────────────────────────────────────────
    if (!ran && !aborted && captured_output.empty()) {
        ncplane_set_fg_rgb(run_plane, 0xff4444);
        ncplane_putstr_yx(run_plane, output_row, output_col, "All interpreters failed!");
        notcurses_render(nc);
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, nullptr);
    } else {
        bool done = false;
        while (!done) {
            render_output(" Esc|Q=close  \x18/\x19/jk \xe2\x86\x90/\xe2\x86\x92/hl ");
            struct ncinput ni;
            uint32_t k = notcurses_get_blocking(nc, &ni);
            if (k == NCKEY_ESC || ni.id == 'q' || ni.id == 'Q') {
                done = true;
            } else            if (k == NCKEY_UP || ni.id == 'k') {
                if (scroll_top > 0) scroll_top--;
            } else if (k == NCKEY_DOWN || ni.id == 'j') {
                int vis = (int)term_y_u - output_row - 1;
                int total = count_lines();
                if (scroll_top < total - vis) scroll_top++;
            } else if (k == NCKEY_LEFT || ni.id == 'h') {
                if (scroll_left > 0) scroll_left -= 4;
                if (scroll_left < 0) scroll_left = 0;
            } else if (k == NCKEY_RIGHT || ni.id == 'l') {
                scroll_left += 4;
            }
        }
    }

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

        // Adjust scroll
        if (cursor_line < scroll_row) scroll_row = cursor_line;
        if (cursor_line >= scroll_row + max_rows) scroll_row = cursor_line - max_rows + 1;
        if (scroll_row < 0) scroll_row = 0;

        // Draw visible lines
        for (int row = 0; row < max_rows; row++) {
            int li = scroll_row + row;
            if (li >= (int)line_starts.size()) break;

            int start = line_starts[li];
            int end = (li + 1 < (int)line_starts.size())
                        ? line_starts[li + 1] - 1
                        : (int)buffer.size();

            int col;
            for (col = 0; col < max_cols; col++) {
                int pos = start + col;
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

            // Cursor at end of line
            int line_cursor = start + (end - start);
            if (line_cursor == cursor && col < max_cols) {
                if (code_mode) {
                    ncplane_set_fg_rgb(edplane, 0x000000);
                    ncplane_set_bg_rgb(edplane, 0xffaa00);
                } else {
                    ncplane_set_fg_rgb(edplane, 0x000000);
                    ncplane_set_bg_rgb(edplane, 0x44aaff);
                }
                ncplane_putchar_yx(edplane, row, col, ' ');
            }
        }

        // ── Build status content ──────────────────────────────────
        std::string mode_tag = code_mode ? " CODE " : " INPUT ";
        std::string fn_tag = " " + (current_file.empty() ? "(unnamed)" : current_file) + " ";
        std::string keymap_str;
        if (code_mode)
            keymap_str = " a:, s:[ d:+ f:<  h:> j:- k:] l:. ";
        std::string hints = " F1:Save F2:Open F3:Mode F4:Quit F5:Run ";
        std::string lc = " Ln " + std::to_string(cursor_line + 1) +
                         " Col " + std::to_string(cursor_col + 1);

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
            if (cursor < (int)buffer.size()) cursor++;
            continue;
        }
        if (key == NCKEY_UP) {
            // Move cursor up one line
            // Find start of current line
            int line_start = cursor;
            while (line_start > 0 && buffer[line_start - 1] != '\n') line_start--;
            int col = cursor - line_start;

            // Find start of previous line (if any)
            if (line_start > 0) {
                int prev_end = line_start - 1; // position of the \n
                int prev_start = prev_end;
                while (prev_start > 0 && buffer[prev_start - 1] != '\n') prev_start--;
                // col stays same, but clamped to prev line length
                int new_pos = prev_start + col;
                if (new_pos > prev_end) new_pos = prev_end;
                cursor = new_pos;
            }
            continue;
        }
        if (key == NCKEY_DOWN) {
            // Move cursor down one line
            // Find start and end of current line
            int line_start = cursor;
            while (line_start > 0 && buffer[line_start - 1] != '\n') line_start--;
            int col = cursor - line_start;

            // Find end of current line (position of \n or end of buffer)
            int line_end = cursor;
            while (line_end < (int)buffer.size() && buffer[line_end] != '\n') line_end++;

            // If there's a next line
            if (line_end < (int)buffer.size()) {
                int next_start = line_end + 1;
                // Find end of next line
                int next_end = next_start;
                while (next_end < (int)buffer.size() && buffer[next_end] != '\n') next_end++;
                // Clamp column to next line length
                int new_pos = next_start + col;
                if (new_pos > next_end) new_pos = next_end;
                cursor = new_pos;
            }
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
            // ── Code mode mappings ────────────────────────────────
            // Home row: a s d f   h j k l
            if      (ch == 'a' || ch == 'A') to_insert = ',';
            else if (ch == 's' || ch == 'S') to_insert = '[';
            else if (ch == 'd' || ch == 'D') to_insert = '+';
            else if (ch == 'f' || ch == 'F') to_insert = '<';
            else if (ch == 'h' || ch == 'H') to_insert = '>';
            else if (ch == 'j' || ch == 'J') to_insert = '-';
            else if (ch == 'k' || ch == 'K') to_insert = ']';
            else if (ch == 'l' || ch == 'L') to_insert = '.';
            // Top row (above home row): q w e r   y u i o
            else if (ch == 'q' || ch == 'Q') to_insert = ',';
            else if (ch == 'w' || ch == 'W') to_insert = '[';
            else if (ch == 'e' || ch == 'E') to_insert = '+';
            else if (ch == 'r' || ch == 'R') to_insert = '<';
            else if (ch == 'y' || ch == 'Y') to_insert = '>';
            else if (ch == 'u' || ch == 'U') to_insert = '-';
            else if (ch == 'i' || ch == 'I') to_insert = ']';
            else if (ch == 'o' || ch == 'O') to_insert = '.';
            // Number row (same positional mapping): 1 2 3 4   5 6 7 8
            else if (ch == '1') to_insert = ',';
            else if (ch == '2') to_insert = '[';
            else if (ch == '3') to_insert = '+';
            else if (ch == '4') to_insert = '<';
            else if (ch == '5') to_insert = '>';
            else if (ch == '6') to_insert = '-';
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
