// icons.cpp - the Build button's hammer icons.
//
// The grid draws icon numbers 1000 and up from res\misc\MenuIcon8_2.URE (frame = number - 1000),
// loaded once at 0x515044 with "push <file name>". At startup we read the player's own copy of
// that file, append our two hammer frames (Human, Dark Legion) and save the result as
// res\misc\kufmod_icons.URE; then the push is pointed at that name. The original file is never
// changed. The hammer frames are our own art, pre-encoded by tools\make_icons_data.py.
//
// URE layout (version 2): 32-byte title, int version, int extra, int frame count, frame headers
// (0x6B bytes each for one-part frames), int size of block 1, block 1 (RGB555) and block 2
// (RGB565), each holding per frame: int word count n, int part offset, n*2 bytes of data.
#include "kufmod.h"
#include <stdio.h>
#include <string.h>
#include <vector>
#include "icons_data.h"

static const char ICON_FILE[] = "kufmod_icons.URE";  // the game looks in res\misc
static const size_t HEADER = 0x6B;

static bool ReadFile(const char* path, std::vector<BYTE>& out) {
    FILE* f = NULL;
    if (fopen_s(&f, path, "rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? size : 0);
    bool ok = size > 0 && fread(out.data(), 1, size, f) == (size_t)size;
    fclose(f);
    return ok;
}

static uint32_t U32(const std::vector<BYTE>& d, size_t o) { return o + 4 <= d.size() ? *(const uint32_t*)&d[o] : 0; }

static void Put32(std::vector<BYTE>& d, uint32_t v) { d.insert(d.end(), (BYTE*)&v, (BYTE*)&v + 4); }

static void PutFrame(std::vector<BYTE>& d, const BYTE* data, size_t bytes) {
    Put32(d, (uint32_t)(bytes / 2));
    Put32(d, 0);
    d.insert(d.end(), data, data + bytes);
}

// Walks the frames of one block starting at o; returns the end offset, or 0 if malformed.
static size_t SkipBlock(const std::vector<BYTE>& d, size_t o, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        if (o + 8 > d.size()) return 0;
        o += 8 + 2 * (size_t)U32(d, o);
    }
    return o <= d.size() ? o : 0;
}

bool PrepareBuildIcons(int32_t* humanIcon, int32_t* devilIcon) {
    char src[MAX_PATH], dst[MAX_PATH];
    sprintf_s(src, "%sres\\misc\\MenuIcon8_2.URE", g_dir);
    sprintf_s(dst, "%sres\\misc\\%s", g_dir, ICON_FILE);
    std::vector<BYTE> d;
    if (!ReadFile(src, d) || d.size() < 0x2C || memcmp(d.data(), "RLE Encode File", 15) || U32(d, 0x20) != 2) {
        Log("icons: res\\misc\\MenuIcon8_2.URE missing or unexpected");
        return false;
    }
    uint32_t count = U32(d, 0x28);
    size_t headers = 0x2C, blocks = headers + count * HEADER;
    if (count == 0 || count > 1000 || blocks + 4 > d.size()) return false;
    for (uint32_t i = 0; i < count; i++)
        if (U32(d, headers + i * HEADER + 4) != 1) return false;  // one part per frame
    size_t b1 = blocks + 4, b1end = SkipBlock(d, b1, count);
    size_t b2end = b1end ? SkipBlock(d, b1end, count) : 0;
    if (!b1end || b1end - b1 != U32(d, blocks) || b2end != d.size()) {
        Log("icons: MenuIcon8_2.URE layout not recognised");
        return false;
    }

    std::vector<BYTE> out(d.begin(), d.begin() + 0x28);
    Put32(out, count + 2);
    out.insert(out.end(), d.begin() + headers, d.begin() + blocks);
    for (int k = 0; k < 2; k++)  // our frames are 52x43 like frame 0: reuse its header
        out.insert(out.end(), d.begin() + headers, d.begin() + headers + HEADER);
    Put32(out, (uint32_t)(b1end - b1 + 16 + sizeof ICON_HUMAN_555 + sizeof ICON_DEVIL_555));
    out.insert(out.end(), d.begin() + b1, d.begin() + b1end);
    PutFrame(out, ICON_HUMAN_555, sizeof ICON_HUMAN_555);
    PutFrame(out, ICON_DEVIL_555, sizeof ICON_DEVIL_555);
    out.insert(out.end(), d.begin() + b1end, d.end());
    PutFrame(out, ICON_HUMAN_565, sizeof ICON_HUMAN_565);
    PutFrame(out, ICON_DEVIL_565, sizeof ICON_DEVIL_565);

    std::vector<BYTE> existing;
    if (!ReadFile(dst, existing) || existing != out) {
        FILE* f = NULL;
        if (fopen_s(&f, dst, "wb") || !f || fwrite(out.data(), 1, out.size(), f) != out.size()) {
            if (f) fclose(f);
            Log("icons: could not write %s", dst);
            return false;
        }
        fclose(f);
    }

    // 0x515044: push 0xA3CB60 ("menuicon8_2.URE") - point it at our file name instead.
    uint32_t expected = 0xA3CB60 + (uint32_t)g_slide, ours = (uint32_t)(uintptr_t)ICON_FILE;
    BYTE before[5] = {0x68}, after[5] = {0x68};
    memcpy(before + 1, &expected, 4);
    memcpy(after + 1, &ours, 4);
    if (!Patch(0x515044, before, after, 5)) {
        Log("icons: unexpected game code at 0x515044");
        return false;
    }
    *humanIcon = 1000 + (int32_t)count;
    *devilIcon = 1000 + (int32_t)count + 1;
    Log("icons: built %s (Build icons %d, %d)", ICON_FILE, *humanIcon, *devilIcon);
    return true;
}
