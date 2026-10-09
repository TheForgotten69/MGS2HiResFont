// MGS2HiResFont.asi - high-resolution dialogue font for METAL GEAR SOLID 2 (Master Collection).
//
// The game rasterises subtitles, codec and menu text on the CPU from a 40 px bitmap font into
// "text surfaces" (textures sized in font pixels), which it then stretches over the screen.
// This plugin leaves the game's font and all of its layout alone and supersamples inside the
// font module instead:
//   - the text-surface allocators create surfaces SCALE times larger;
//   - the lock function and the two texture-size getters used by the menu code report the
//     original (logical) size back to the game;
//   - the glyph, half-size glyph and row-clear routines write at SCALE times the position,
//     taking the glyph bitmaps from MGS2HiResFont.bin (see make_hires_font.py).
// Surfaces that were not created through the hooked allocators go through the game's own code,
// and so does any glyph the .bin does not contain (enlarged from the game's bitmap).
//
// Every hook checks the bytes it replaces first: on another game version nothing is patched.
// Remove the .asi (or the .bin) to disable. Log: MGS2HiResFont.log next to the .asi.

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// ---- game layout (addresses are relative to the image base) ------------------------------

struct Texture {
    uint8_t unknown[0x58];
    int32_t width;
    int32_t height;
};

struct SurfaceSet {  // three textures used in rotation
    int32_t previous;
    int32_t current;
    int32_t frame;
    int32_t pad;
    Texture* textures[3];
};

#pragma pack(push, 1)
struct TextContext {  // "kcb"
    uint8_t unknown0[2];
    uint8_t margin_x;      // +0x02
    uint8_t margin_y;      // +0x03
    uint32_t colour;       // +0x04
    uint8_t flags;         // +0x08
    uint8_t unknown1[3];
    int16_t wrap_width;    // +0x0c
    int16_t pitch;         // +0x0e, bytes
    int16_t height;        // +0x10
    uint8_t unknown2[6];
    uint8_t* pixels;       // +0x18, null while unlocked
    Texture* texture;      // +0x20, single-texture mode
    uint8_t unknown3[8];
    SurfaceSet* set;       // +0x30
};

struct GameGlyph {
    uint8_t width, height;
    int8_t x_offset, y_offset;
    uint8_t advance;
    uint8_t coverage[1];
};

struct GameFont {
    uint32_t magic;
    int32_t cell_width;   // +0x04
    int32_t cell_height;  // +0x08
    int32_t gap;
    uint32_t table;       // +0x10
    int32_t count;        // +0x14
};
#pragma pack(pop)

static_assert(offsetof(TextContext, wrap_width) == 0x0c && offsetof(TextContext, pixels) == 0x18 &&
              offsetof(TextContext, set) == 0x30 && offsetof(SurfaceSet, textures) == 0x10 &&
              offsetof(Texture, width) == 0x58 && offsetof(GameFont, count) == 0x14, "game layout");

const int kDefaultCell = 0x18;
const uint32_t kMissingGlyph = 0xffff0000;
const int kNoWrapMargin = 0x2;

struct Hook {
    const char* name;
    uintptr_t rva;
    int length;            // bytes checked and copied: whole instructions; 14 are overwritten
    uint8_t expected[16];
    void* replacement;
    void** original;       // receives a trampoline to the game's function; null when it is replaced whole
};

const uintptr_t kFontPointer = 0x13d4ef8;
const uintptr_t kCursorWidth = 0x9401d0;
const uintptr_t kUnlock = 0x2b6c0;
const uintptr_t kLock = 0x2ab20;
// Return addresses of the calls to the single-texture allocator that create text surfaces sized from the font.
const uintptr_t kSingleTextSites[] = {0xf099, 0x1cd56d, 0x1df315, 0x1e33f4, 0x1e5c88};

// ---- state ------------------------------------------------------------------------------

uint8_t* g_base;
FILE* g_log;

int g_scale;
int g_count;
const uint8_t* g_entries;  // count * (u32 codepoint, u16 width, u16 height, u32 offset), sorted by codepoint
const uint8_t* g_bitmaps;

struct Registered {
    Texture* texture;
    int32_t width, height;
};
Registered g_registered[8192];  // never evicted: an entry is only replaced when its address is reused
int g_registered_count;
SRWLOCK g_registry_lock = SRWLOCK_INIT;

Texture** (*g_alloc_inline)(int, int, int, SurfaceSet*);
SurfaceSet* (*g_alloc_heap)(int, int, int);
Texture* (*g_alloc_single)(int, int, int);
void (*g_lock)(TextContext*, int);
int (*g_glyph)(TextContext*, int, int, int, int);
int (*g_half)(TextContext*, int, int, int);
void (*g_clear)(TextContext*, int, int);

void log_line(const char* format, ...) {
    if (!g_log) return;
    va_list args;
    va_start(args, format);
    vfprintf(g_log, format, args);
    va_end(args);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---- high-resolution font ---------------------------------------------------------------

bool load_font(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(file, nullptr), got = 0;
    uint8_t* data = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    bool ok = data && ReadFile(file, data, size, &got, nullptr) && got == size && size >= 12 && !memcmp(data, "MG2F", 4);
    CloseHandle(file);
    if (!ok) return false;
    memcpy(&g_scale, data + 4, 4);
    memcpy(&g_count, data + 8, 4);
    if (g_scale < 2 || g_scale > 4 || g_count <= 0 || 12 + 12ull * g_count > size) return false;
    g_entries = data + 12;
    g_bitmaps = g_entries + 12ull * g_count;
    return true;
}

// Bitmap of `codepoint`, (width*scale) x (height*scale), or null when the file has none of that size.
const uint8_t* hires_bitmap(uint32_t codepoint, int width, int height) {
    int low = 0, high = g_count - 1;
    while (low <= high) {
        int middle = (low + high) / 2;
        const uint8_t* entry = g_entries + 12 * middle;
        uint32_t found;
        memcpy(&found, entry, 4);
        if (found == codepoint) {
            uint16_t w, h;
            uint32_t offset;
            memcpy(&w, entry + 4, 2);
            memcpy(&h, entry + 6, 2);
            memcpy(&offset, entry + 8, 4);
            return (w == width && h == height) ? g_bitmaps + offset : nullptr;
        }
        if (found < codepoint) low = middle + 1; else high = middle - 1;
    }
    return nullptr;
}

// ---- which surfaces are high-resolution -------------------------------------------------

const int kTexturesPerSet = 3;

bool registry_has_room() {
    return g_registered_count + kTexturesPerSet <= static_cast<int>(sizeof g_registered / sizeof *g_registered);
}

void remember(Texture* texture) {
    if (!texture) return;
    AcquireSRWLockExclusive(&g_registry_lock);
    Registered* slot = nullptr;
    for (int index = 0; index < g_registered_count; ++index)
        if (g_registered[index].texture == texture) slot = &g_registered[index];
    if (!slot) slot = &g_registered[g_registered_count++];
    *slot = {texture, texture->width, texture->height};
    ReleaseSRWLockExclusive(&g_registry_lock);
}

// One log line per distinct code address: shows which screens use which path.
bool first_time(uintptr_t (&seen)[64], void* address) {
    uintptr_t rva = reinterpret_cast<uint8_t*>(address) - g_base;
    for (uintptr_t& entry : seen) {
        if (entry == rva) return false;
        if (!entry) {
            entry = rva;
            return true;
        }
    }
    return false;
}

// The size check drops an address that was freed and reused by an ordinary texture.
bool is_hires(const Texture* texture) {
    if (!texture) return false;
    bool found = false;
    AcquireSRWLockShared(&g_registry_lock);
    for (int index = 0; index < g_registered_count; ++index)
        if (g_registered[index].texture == texture) {
            found = g_registered[index].width == texture->width && g_registered[index].height == texture->height;
            break;
        }
    ReleaseSRWLockShared(&g_registry_lock);
    return found;
}

Texture* target_of(const TextContext* context) {
    if (context->set) return context->set->textures[context->set->current];
    return context->texture;
}

const GameFont* game_font() { return *reinterpret_cast<GameFont**>(g_base + kFontPointer); }

const GameGlyph* game_glyph(const GameFont* font, int codepoint) {
    const uint8_t* start = reinterpret_cast<const uint8_t*>(font);
    const uint32_t* entry = reinterpret_cast<const uint32_t*>(start + font->table);
    for (int index = 0; index < font->count; ++index, entry += 2)
        if (static_cast<int>(entry[0]) == codepoint) return reinterpret_cast<const GameGlyph*>(start + entry[1]);
    return nullptr;
}

// ---- drawing, in the game's (logical) pixels, clipped to the real surface ---------------

struct Canvas {
    uint32_t* pixels;
    int stride;  // pixels
    int width, height;
};

Canvas canvas_of(const TextContext* context, const Texture* texture) {
    return {reinterpret_cast<uint32_t*>(context->pixels), context->pitch / 4, texture->width, texture->height};
}

void fill(const Canvas& canvas, int x, int y, int width, int height, uint32_t value) {
    int left = x * g_scale, top = y * g_scale, right = (x + width) * g_scale, bottom = (y + height) * g_scale;
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > canvas.width) right = canvas.width;
    if (bottom > canvas.height) bottom = canvas.height;
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column) canvas.pixels[row * canvas.stride + column] = value;
}

uint32_t shaded(uint32_t colour, uint32_t coverage) {
    return ((coverage * (colour / 255)) & 0xff000000) | (colour & 0x00ffffff);
}

// `bitmap` is source_width x source_height at `step` source pixels per output pixel (1, or 2 for half size).
void blit(const Canvas& canvas, int x, int y, int width, int height, const uint8_t* bitmap, int source_width,
          int source_height, int step, uint32_t colour) {
    for (int row = 0; row < height; ++row) {
        int out_row = y + row;
        if (out_row < 0 || out_row >= canvas.height) continue;
        for (int column = 0; column < width; ++column) {
            int out_column = x + column;
            if (out_column < 0 || out_column >= canvas.width) continue;
            uint32_t sum = 0;
            for (int dy = 0; dy < step; ++dy)
                for (int dx = 0; dx < step; ++dx) {
                    int sy = row * step + dy, sx = column * step + dx;
                    if (sy >= source_height) sy = source_height - 1;
                    if (sx >= source_width) sx = source_width - 1;
                    sum += bitmap[sy * source_width + sx];
                }
            canvas.pixels[out_row * canvas.stride + out_column] = shaded(colour, sum / (step * step));
        }
    }
}

// The game's bitmap enlarged, for glyphs the high-resolution file does not have.
void blit_enlarged(const Canvas& canvas, int x, int y, const GameGlyph* glyph, int factor, uint32_t colour) {
    for (int row = 0; row < glyph->height * factor; ++row)
        for (int column = 0; column < glyph->width * factor; ++column) {
            int out_row = y + row, out_column = x + column;
            if (out_row < 0 || out_row >= canvas.height || out_column < 0 || out_column >= canvas.width) continue;
            uint32_t coverage = glyph->coverage[(row / factor) * glyph->width + column / factor];
            canvas.pixels[out_row * canvas.stride + out_column] = shaded(colour, coverage);
        }
}

int two_thirds_sixteenth(int cell) { return static_cast<int>((static_cast<uint64_t>(2 * cell) * 0xaaaaaaabull) >> 32) >> 4; }

// ---- hooks ------------------------------------------------------------------------------

uintptr_t g_seen_allocations[64], g_seen_fallbacks[64];

// True when the surface asked for at `caller` is to be created SCALE times larger.
bool enlarge(void* caller, int width, int height, bool text_site) {
    bool enlarged = text_site && registry_has_room();
    if (first_time(g_seen_allocations, caller))
        log_line("surface %dx%d created at %#llx: %s", width, height,
                 static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(caller) - g_base),
                 enlarged ? "high resolution" : "left as it is");
    return enlarged;
}

Texture** hook_alloc_inline(int width, int height, int clear, SurfaceSet* set) {
    if (!enlarge(__builtin_return_address(0), width, height, true)) return g_alloc_inline(width, height, clear, set);
    Texture** result = g_alloc_inline(width * g_scale, height * g_scale, clear, set);
    for (Texture* texture : set->textures) remember(texture);
    return result;
}

SurfaceSet* hook_alloc_heap(int width, int height, int clear) {
    if (!enlarge(__builtin_return_address(0), width, height, true)) return g_alloc_heap(width, height, clear);
    SurfaceSet* set = g_alloc_heap(width * g_scale, height * g_scale, clear);
    if (set)
        for (Texture* texture : set->textures) remember(texture);
    return set;
}

// This allocator also serves surfaces that are not text: only the call sites known to be text are enlarged.
Texture* hook_alloc_single(int width, int height, int clear) {
    uintptr_t caller = reinterpret_cast<uint8_t*>(__builtin_return_address(0)) - g_base;
    bool text_site = false;
    for (uintptr_t site : kSingleTextSites) text_site |= site == caller;
    if (!enlarge(__builtin_return_address(0), width, height, text_site)) return g_alloc_single(width, height, clear);
    Texture* texture = g_alloc_single(width * g_scale, height * g_scale, clear);
    remember(texture);
    return texture;
}

void hook_lock(TextContext* context, int clear) {
    g_lock(context, clear);
    const Texture* texture = target_of(context);
    if (!is_hires(texture)) return;
    const GameFont* font = game_font();
    int width = texture->width / g_scale;
    if (!(context->flags & kNoWrapMargin)) width -= context->margin_x + (font ? font->cell_width : kDefaultCell);
    context->wrap_width = static_cast<int16_t>(width);
    context->height = static_cast<int16_t>(texture->height / g_scale);
}

int hook_glyph(TextContext* context, int x, int y, int codepoint, int emphasis) {
    const Texture* texture = target_of(context);
    const GameFont* font = game_font();
    if (!context->pixels || !font || !is_hires(texture)) {
        if (texture && first_time(g_seen_fallbacks, __builtin_return_address(0)))
            log_line("text drawn at the game's resolution on a %dx%d surface, from %#llx", texture->width, texture->height,
                     static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(__builtin_return_address(0)) - g_base));
        return g_glyph(context, x, y, codepoint, emphasis);
    }
    Canvas canvas = canvas_of(context, texture);

    if (codepoint <= 0) {
        if (codepoint < 0) return 0;
        int cursor = *reinterpret_cast<int32_t*>(g_base + kCursorWidth);
        int rows = two_thirds_sixteenth(font->cell_height);
        int top = rows / 2 + font->cell_height / 2 + y;
        if (top >= 0) fill(canvas, x + (cursor + 1) / 2 - (cursor - 2) / 2, top, cursor - 2, rows, context->colour);
        return cursor;
    }
    const GameGlyph* glyph = game_glyph(font, codepoint);
    if (!glyph) {
        fill(canvas, x, y, font->cell_height, font->cell_width, kMissingGlyph);
        return font->cell_width;
    }
    if (emphasis) {
        int dot_width = two_thirds_sixteenth(font->cell_width), dot_height = two_thirds_sixteenth(font->cell_height);
        int top = -(dot_height / 2) - (context->margin_y >> 1) + y;
        if (top >= 0)
            fill(canvas, x + ((glyph->advance + 1) >> 1) - dot_width / 2, top, dot_width, dot_height, context->colour);
    }
    int left = x + glyph->x_offset;
    int top = y + glyph->y_offset > 0 ? y + glyph->y_offset : 0;
    int width = glyph->width, height = glyph->height;
    if (codepoint != ' ') {  // the game clears a one-pixel frame around every glyph
        uint32_t clear = context->colour & 0x00ffffff;
        if (left > 0) fill(canvas, left - 1, top, 1, height + 1, clear);
        fill(canvas, left + width, top, 1, height + 1, clear);
        if (top > 0) fill(canvas, left, top - 1, width + 1, 1, clear);
        fill(canvas, left, top + height, width + 1, 1, clear);
        if (left > 0 && top > 0) fill(canvas, left - 1, top - 1, 1, 1, clear);
    }
    if (width && height) {
        const uint8_t* bitmap = hires_bitmap(codepoint, width, height);
        if (bitmap)
            blit(canvas, left * g_scale, top * g_scale, width * g_scale, height * g_scale, bitmap, width * g_scale,
                 height * g_scale, 1, context->colour);
        else
            blit_enlarged(canvas, left * g_scale, top * g_scale, glyph, g_scale, context->colour);
    }
    return glyph->advance;
}

// Half-size glyphs (reading aids above Japanese text).
int hook_half(TextContext* context, int x, int y, int codepoint) {
    const Texture* texture = target_of(context);
    const GameFont* font = game_font();
    if (!context->pixels || !font || !is_hires(texture)) return g_half(context, x, y, codepoint);
    Canvas canvas = canvas_of(context, texture);

    const GameGlyph* glyph = game_glyph(font, codepoint);
    int width = glyph ? glyph->width : font->cell_width, height = glyph ? glyph->height : font->cell_height;
    int advance = glyph ? glyph->advance : font->cell_width;
    int left = x + (glyph ? (glyph->x_offset + 1) / 2 : 0), top = y + (glyph ? (glyph->y_offset + 1) / 2 : 0);
    int columns = (width + 1) / 2, rows = (height + 1) / 2;
    if (!glyph) {
        fill(canvas, left, top, columns, rows, kMissingGlyph);
    } else if (const uint8_t* bitmap = hires_bitmap(codepoint, width, height)) {
        blit(canvas, left * g_scale, top * g_scale, columns * g_scale, rows * g_scale, bitmap, width * g_scale,
             height * g_scale, 2, context->colour);
    } else if (width && height) {
        blit(canvas, left * g_scale, top * g_scale, columns * g_scale, rows * g_scale, glyph->coverage, width, height, 2,
             context->colour);  // drawn at the game's size inside the larger box: rare, and never out of bounds
    }
    return (advance + 1) / 2;
}

void hook_clear(TextContext* context, int first_row, int end_row) {
    bool locked = context->pixels != nullptr;
    auto lock = reinterpret_cast<void (*)(TextContext*, int)>(g_base + kLock);  // through hook_lock
    auto unlock = reinterpret_cast<void (*)(TextContext*)>(g_base + kUnlock);
    if (!locked) lock(context, 0);
    int factor = is_hires(target_of(context)) ? g_scale : 1;
    memset(context->pixels + context->pitch * first_row * factor, 0,
           static_cast<size_t>(end_row - first_row) * factor * context->pitch);
    if (!locked) unlock(context);
}

// The menu screens (save/load list, codec memory) cut their quads out of the surface with
// texture coordinates computed as pixels / texture size, through these two getters, which only
// text code calls. They must see the logical size, or the quads show a third of the surface.
int hook_texture_height(const Texture* texture) {
    if (!texture) return 0;
    return is_hires(texture) ? texture->height / g_scale : texture->height;
}

int hook_texture_width(const Texture* texture) {
    if (!texture) return 0;
    return is_hires(texture) ? texture->width / g_scale : texture->width;
}

Hook g_hooks[] = {
    {"texture height (text code)", 0xf6c0, 12, {0x48, 0x85, 0xc9, 0x0f, 0x85, 0x77, 0x15, 0x04, 0x00, 0x33, 0xc0, 0xc3},
     reinterpret_cast<void*>(hook_texture_height), nullptr},
    {"texture width (text code)", 0xf6d0, 12, {0x48, 0x85, 0xc9, 0x0f, 0x85, 0x57, 0x15, 0x04, 0x00, 0x33, 0xc0, 0xc3},
     reinterpret_cast<void*>(hook_texture_width), nullptr},
    {"surface allocator (inline)", 0xee80, 14,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x56, 0x57, 0x41, 0x54},
     reinterpret_cast<void*>(hook_alloc_inline), reinterpret_cast<void**>(&g_alloc_inline)},
    {"surface allocator (heap)", 0xef60, 15,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18},
     reinterpret_cast<void*>(hook_alloc_heap), reinterpret_cast<void**>(&g_alloc_heap)},
    {"surface allocator (single)", 0xedf0, 15,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x40, 0x33, 0xc0, 0x41, 0x8b, 0xd8},
     reinterpret_cast<void*>(hook_alloc_single), reinterpret_cast<void**>(&g_alloc_single)},
    {"lock", kLock, 14, {0x40, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x59, 0x30},
     reinterpret_cast<void*>(hook_lock), reinterpret_cast<void**>(&g_lock)},
    {"glyph", 0x2a160, 16,
     {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x10},
     reinterpret_cast<void*>(hook_glyph), reinterpret_cast<void**>(&g_glyph)},
    {"half-size glyph", 0x2a630, 15,
     {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x83, 0xec, 0x10},
     reinterpret_cast<void*>(hook_half), reinterpret_cast<void**>(&g_half)},
    {"row clear", 0x2ad50, 15,
     {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18},
     reinterpret_cast<void*>(hook_clear), reinterpret_cast<void**>(&g_clear)},
};

// ---- patching ---------------------------------------------------------------------------

void write_jump(uint8_t* at, const void* target) {  // jmp [rip+0]; dq target
    const uint8_t jump[6] = {0xff, 0x25, 0, 0, 0, 0};
    memcpy(at, jump, 6);
    memcpy(at + 6, &target, 8);
}

bool code_is_ready() {
    for (const Hook& hook : g_hooks)
        if (memcmp(g_base + hook.rva, hook.expected, hook.length)) return false;
    return true;
}

bool install() {
    uint8_t* trampolines = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampolines) return false;
    for (Hook& hook : g_hooks) {  // all trampolines first, so no hook runs without its original
        if (!hook.original) continue;
        memcpy(trampolines, g_base + hook.rva, hook.length);
        write_jump(trampolines + hook.length, g_base + hook.rva + hook.length);
        *hook.original = trampolines;
        trampolines += 32;
    }
    for (const Hook& hook : g_hooks) {
        uint8_t* at = g_base + hook.rva;
        DWORD old;
        if (!VirtualProtect(at, 14, PAGE_EXECUTE_READWRITE, &old)) return false;
        write_jump(at, hook.replacement);
        VirtualProtect(at, 14, old, &old);
        FlushInstructionCache(GetCurrentProcess(), at, 14);
        log_line("hooked %s", hook.name);
    }
    return true;
}

// The executable's code is still encrypted (Steam) when plugins load: wait for it, then patch.
DWORD WINAPI wait_and_install(void*) {
    for (int waited = 0; waited < 120000; ++waited) {
        if (code_is_ready()) {
            if (install())
                log_line("installed after %d ms, scale %d, %d glyphs", waited, g_scale, g_count);
            else
                log_line("could not install");
            return 0;
        }
        Sleep(1);
    }
    log_line("the game's code does not match the version this plugin was written for: nothing was changed");
    return 0;
}

void start(HMODULE module) {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length < 4 || length >= MAX_PATH) return;
    wcscpy(path + length - 3, L"log");
    g_log = _wfopen(path, L"w");
    wcscpy(path + length - 3, L"bin");
    g_base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    if (!load_font(path)) {
        log_line("MGS2HiResFont.bin is missing or not valid: nothing was changed");
        return;
    }
    CloseHandle(CreateThread(nullptr, 0, wait_and_install, nullptr, 0, nullptr));
}

#ifdef MGS2HIRES_TEST
// Offline check of the drawing path: test.exe <game font blob> <hires font> <text> <out.rgba>
void fake_lock(TextContext* context, int) {
    const Texture* texture = target_of(context);
    context->wrap_width = static_cast<int16_t>(texture->width - context->margin_x - game_font()->cell_width);
    context->height = static_cast<int16_t>(texture->height);
}

int test_main(int argc, wchar_t** argv) {
    if (argc < 5 || !load_font(argv[2])) return 2;
    g_log = stdout;
    g_base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kFontPointer + 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FILE* blob = _wfopen(argv[1], L"rb");
    static uint8_t font[8 << 20];
    fread(font, 1, sizeof font, blob);
    *reinterpret_cast<uint8_t**>(g_base + kFontPointer) = font;
    *reinterpret_cast<int32_t*>(g_base + kCursorWidth) = 20;
    g_lock = fake_lock;

    const int width = 21 * 40, height = 124;
    static Texture texture;
    texture.width = width * g_scale;
    texture.height = height * g_scale;
    remember(&texture);
    static uint32_t pixels[21 * 40 * 4 * 124 * 4];
    TextContext context = {};
    context.colour = 0x80ffffff;
    context.texture = &texture;
    context.pixels = reinterpret_cast<uint8_t*>(pixels);
    context.pitch = static_cast<int16_t>(texture.width * 4);
    hook_lock(&context, 0);
    printf("scale %d wrap %d height %d\n", g_scale, context.wrap_width, context.height);
    int x = 4;
    for (const wchar_t* c = argv[3]; *c; ++c) x += hook_glyph(&context, x, 0, *c, *c == L'S');
    x = 4;
    for (const wchar_t* c = argv[3]; *c; ++c) x += hook_half(&context, x, 48, *c);
    hook_glyph(&context, 4, 80, 0, 0);       // cursor
    hook_glyph(&context, 60, 80, 0x1234, 0); // missing glyph
    hook_glyph(&context, 830, 100, 'W', 0);  // crosses the right and bottom edges: must clip
    printf("pen %d\n", x);
    FILE* out = _wfopen(argv[4], L"wb");
    fwrite(pixels, 4, static_cast<size_t>(texture.width) * texture.height, out);
    fclose(out);
    return 0;
}
#endif

}  // namespace

#ifdef MGS2HIRES_TEST
int wmain(int argc, wchar_t** argv) { return test_main(argc, argv); }
#endif

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        start(module);
    }
    return TRUE;
}
