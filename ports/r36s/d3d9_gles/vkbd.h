#pragma once
// On-screen virtual keyboard for text entry on handhelds without a keyboard (profile names etc.).
// Input (main.cpp) moves the cursor and injects DirectInput scan codes into the game with setGuestKey();
// drawing happens in IDirect3DDevice9GLES::Present just before the swap (drawOverlay below).
// Toggle: L3 or R3 (stick click). While open: D-pad/stick move, South=type, West=backspace,
// North=shift, Start=Enter (and close), East/L3/R3=close. Game input is suspended while it is open.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace vkbd {

enum Special : int { kNormal = 0, kShift, kSpace, kDelete, kEnter };

struct Key {
    char lower, upper;   // printed labels
    uint8_t scan;        // DirectInput scan code
    bool needsShift;     // e.g. '_' is Shift+'-'
    int width;           // in units (a normal key is 1)
    Special special;
};

inline const std::vector<std::vector<Key>>& layout() {
    static const std::vector<std::vector<Key>> rows = {
        {{'1','1',0x02,0,1,kNormal},{'2','2',0x03,0,1,kNormal},{'3','3',0x04,0,1,kNormal},{'4','4',0x05,0,1,kNormal},
         {'5','5',0x06,0,1,kNormal},{'6','6',0x07,0,1,kNormal},{'7','7',0x08,0,1,kNormal},{'8','8',0x09,0,1,kNormal},
         {'9','9',0x0A,0,1,kNormal},{'0','0',0x0B,0,1,kNormal}},
        {{'q','Q',0x10,0,1,kNormal},{'w','W',0x11,0,1,kNormal},{'e','E',0x12,0,1,kNormal},{'r','R',0x13,0,1,kNormal},
         {'t','T',0x14,0,1,kNormal},{'y','Y',0x15,0,1,kNormal},{'u','U',0x16,0,1,kNormal},{'i','I',0x17,0,1,kNormal},
         {'o','O',0x18,0,1,kNormal},{'p','P',0x19,0,1,kNormal}},
        {{'a','A',0x1E,0,1,kNormal},{'s','S',0x1F,0,1,kNormal},{'d','D',0x20,0,1,kNormal},{'f','F',0x21,0,1,kNormal},
         {'g','G',0x22,0,1,kNormal},{'h','H',0x23,0,1,kNormal},{'j','J',0x24,0,1,kNormal},{'k','K',0x25,0,1,kNormal},
         {'l','L',0x26,0,1,kNormal},{'-','-',0x0C,0,1,kNormal}},
        {{'z','Z',0x2C,0,1,kNormal},{'x','X',0x2D,0,1,kNormal},{'c','C',0x2E,0,1,kNormal},{'v','V',0x2F,0,1,kNormal},
         {'b','B',0x30,0,1,kNormal},{'n','N',0x31,0,1,kNormal},{'m','M',0x32,0,1,kNormal},{'.','.',0x34,0,1,kNormal},
         {'_','_',0x0C,1,1,kNormal},{'\'','\'',0x28,0,1,kNormal}},
        {{0,0,0x2A,0,2,kShift},{' ',' ',0x39,0,4,kSpace},{0,0,0x0E,0,2,kDelete},{0,0,0x1C,0,2,kEnter}},
    };
    return rows;
}

struct State {
    std::atomic<bool> visible{false};
    std::atomic<int> row{1}, col{0};
    std::atomic<bool> shift{true};  // start in upper case (names)
};
inline State& state() { static State s; return s; }

// Horizontal centre of a key in units, used to keep the column when moving between rows.
inline float keyCenter(int row, int col) {
    const auto& r = layout()[row];
    float x = 0;
    for (int i = 0; i < col; ++i) x += static_cast<float>(r[i].width);
    return x + r[col].width * 0.5f;
}

inline void move(int dx, int dy) {
    State& s = state();
    const auto& rows = layout();
    int row = s.row.load(), col = s.col.load();
    if (dy) {
        const float cx = keyCenter(row, col);
        row = (row + dy + static_cast<int>(rows.size())) % static_cast<int>(rows.size());
        int best = 0; float bestDist = 1e9f;
        for (int i = 0; i < static_cast<int>(rows[row].size()); ++i) {
            const float d = std::abs(keyCenter(row, i) - cx);
            if (d < bestDist) { bestDist = d; best = i; }
        }
        col = best;
    }
    if (dx) {
        const int n = static_cast<int>(rows[row].size());
        col = (col + dx + n) % n;
    }
    s.row = row;
    s.col = col;
}

inline const Key& current() { return layout()[state().row.load()][state().col.load()]; }

}  // namespace vkbd
