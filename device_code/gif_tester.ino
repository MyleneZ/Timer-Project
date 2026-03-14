/*
 * Qualia GIF Tester
 *
 * Plays every .gif found on the device filesystem (prefers "/assets/*.gif" if present)
 * on the Adafruit Qualia ESP32-S3 RGB666 display.
 *
 * This sketch mirrors the display bring-up from device_code/production.ino.
 *
 * Notes:
 * - The GIFs in this repo live at device_code/assets/*.gif (on your computer).
 *   To run this sketch, upload those files to the ESP32 filesystem so they end up as:
 *     /assets/books.gif
 *     /assets/coffee.gif
 *     /assets/dumbell.gif
 *     /assets/mixy.gif
 * - Default filesystem is LittleFS (recommended on ESP32). Flip USE_LITTLEFS if needed.
 */

// Arduino's sketch preprocessor auto-inserts function prototypes near the top of
// the file. Forward-declare gd_GIF so prototypes that use gd_GIF* compile.
struct gd_GIF;
typedef struct gd_GIF gd_GIF;

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>

#include <FS.h>
#include <LittleFS.h>
#include <SPIFFS.h>

#include <algorithm>
#include <vector>
#include <string.h>

// Prefer PSRAM for multi-megabyte frame buffers on ESP32-S3.
#if defined(ESP32)
  #include <esp_heap_caps.h>
  static void *gif_alloc(size_t bytes) {
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
    return malloc(bytes);
  }
#else
  static void *gif_alloc(size_t bytes) { return malloc(bytes); }
#endif

// ======================= CONFIG =======================
// Filesystem selection
#define USE_LITTLEFS 1
#define FS_FORMAT_ON_FAIL 1  // WARNING: true will erase uploaded files if mount fails

static const char *GIF_DIR = "/assets";     // preferred directory on the device FS
static const uint32_t GIF_SHOW_MS = 6000;   // how long to show each GIF before advancing
static const uint32_t BETWEEN_GIFS_MS = 250;
static const uint32_t MIN_FRAME_DELAY_MS = 20;  // clamp for zero/too-fast delays

// ======================= DISPLAY CONFIG (from production.ino) =======================
#define PCLK_HZ         12000000
#define PCLK_ACTIVE_NEG 1
#define H_FRONT   24
#define H_PULSE    4
#define H_BACK    64
#define V_FRONT   12
#define V_PULSE    2
#define V_BACK    20

Arduino_XCA9554SWSPI *expander = new Arduino_XCA9554SWSPI(
  PCA_TFT_RESET, PCA_TFT_CS, PCA_TFT_SCK, PCA_TFT_MOSI, &Wire, 0x3F);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
  TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK,
  TFT_R1, TFT_R2, TFT_R3, TFT_R4, TFT_R5,
  TFT_G0, TFT_G1, TFT_G2, TFT_G3, TFT_G4, TFT_G5,
  TFT_B1, TFT_B2, TFT_B3, TFT_B4, TFT_B5,
  1, H_FRONT, H_PULSE, H_BACK,
  1, V_FRONT, V_PULSE, V_BACK,
  PCLK_ACTIVE_NEG, PCLK_HZ
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
  320, 960, rgbpanel, 0, true,
  expander, GFX_NOT_DEFINED,
  HD458002C40_init_operations, sizeof(HD458002C40_init_operations),
  80
);

// ======================= GIFDEC (from Arduino_GFX example, adapted) =======================
// This is a lightly-modified version of Arduino_GFX's SpriteGif example decoder.
// Key change: we always write decoded indices into the frame buffer (even if equal
// to transparent index), so we can composite in RGB565 correctly even when a GIF
// uses per-frame local color tables.

#include <sys/types.h>

#ifndef MIN
#define MIN(A, B) ((A) < (B) ? (A) : (B))
#endif

#ifndef MAX
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#endif

#define GIF_BUF_SIZE 1024

typedef struct gd_Palette {
  int16_t len;
  uint16_t colors[256];
} gd_Palette;

typedef struct gd_GCE {
  uint16_t delay;
  uint8_t tindex;
  uint8_t disposal;
  uint8_t input;
  uint8_t transparency;
} gd_GCE;

typedef struct gd_Entry {
  int32_t len;
  uint16_t prefix;
  uint8_t suffix;
} gd_Entry;

typedef struct gd_Table {
  int16_t bulk;
  int16_t nentries;
  gd_Entry *entries;
} gd_Table;

struct gd_GIF {
  File *fd;
  off_t anim_start;
  uint16_t width, height;
  uint16_t depth;
  uint16_t loop_count;
  gd_GCE gce;
  gd_Palette *palette;
  gd_Palette lct, gct;
  void (*plain_text)(
    struct gd_GIF *gif, uint16_t tx, uint16_t ty,
    uint16_t tw, uint16_t th, uint8_t cw, uint8_t ch,
    uint8_t fg, uint8_t bg);
  void (*comment)(struct gd_GIF *gif);
  void (*application)(struct gd_GIF *gif, char id[8], char auth[3]);
  uint16_t fx, fy, fw, fh;
  uint8_t bgindex;
  gd_Table *table;
  bool read_first_frame;
};

class GifClass {
public:
  gd_GIF *gd_open_gif(File *fd) {
    uint8_t sigver[3];
    uint16_t width, height, depth;
    uint8_t fdsz, bgidx, aspect;
    int16_t gct_sz;
    gd_GIF *gif;

    // init global variables
    gif_buf_last_idx = GIF_BUF_SIZE;
    gif_buf_idx = gif_buf_last_idx; // no buffer yet
    file_pos = 0;

    // Header
    gif_buf_read(fd, sigver, 3);
    if (memcmp(sigver, "GIF", 3) != 0) {
      Serial.println(F("[GIF] Invalid signature"));
      return NULL;
    }
    // Version
    gif_buf_read(fd, sigver, 3);
    if (memcmp(sigver, "89a", 3) != 0) {
      Serial.println(F("[GIF] Invalid version (need 89a)"));
      return NULL;
    }
    // Width x Height
    width = gif_buf_read16(fd);
    height = gif_buf_read16(fd);
    // FDSZ
    gif_buf_read(fd, &fdsz, 1);
    // Presence of GCT
    if (!(fdsz & 0x80)) {
      Serial.println(F("[GIF] No global color table"));
      return NULL;
    }
    // Color Space's Depth
    depth = ((fdsz >> 4) & 7) + 1;
    // GCT Size
    gct_sz = 1 << ((fdsz & 0x07) + 1);
    // Background Color Index
    gif_buf_read(fd, &bgidx, 1);
    // Aspect Ratio
    gif_buf_read(fd, &aspect, 1);

    // Create gd_GIF Structure.
    gif = (gd_GIF *)calloc(1, sizeof(*gif));
    if (!gif) {
      Serial.println(F("[GIF] calloc(gd_GIF) failed"));
      return NULL;
    }
    gif->fd = fd;
    gif->width = width;
    gif->height = height;
    gif->depth = depth;

    // Read GCT
    read_palette(fd, &gif->gct, gct_sz);
    gif->palette = &gif->gct;
    gif->bgindex = bgidx;
    gif->anim_start = file_pos;

    gif->table = new_table();
    gif->read_first_frame = false;
    return gif;
  }

  // Return 1 if got a frame; 0 if got GIF trailer; -1 if error.
  int32_t gd_get_frame(gd_GIF *gif, uint8_t *frame) {
    char sep;

    while (1) {
      gif_buf_read(gif->fd, (uint8_t *)&sep, 1);
      if (sep == 0) {
        gif_buf_read(gif->fd, (uint8_t *)&sep, 1);
      }
      if (sep == ',') {
        break;
      }
      if (sep == ';') {
        return 0;
      }
      if (sep == '!') {
        read_ext(gif);
      } else {
        Serial.print(F("[GIF] Unexpected separator: "));
        Serial.println(sep);
        return -1;
      }
    }

    if (read_image(gif, frame) == -1) return -1;
    return 1;
  }

  void gd_rewind(gd_GIF *gif) {
    gif->fd->seek(gif->anim_start, SeekSet);
    file_pos = gif->anim_start;
    gif_buf_idx = gif_buf_last_idx; // reset buffer
  }

  void gd_close_gif(gd_GIF *gif) {
    gif->fd->close();
    free(gif->table);
    free(gif);
  }

private:
  bool gif_buf_seek(File *fd, int16_t len) {
    if (len > (gif_buf_last_idx - gif_buf_idx)) {
      fd->seek(file_pos + len - (gif_buf_last_idx - gif_buf_idx), SeekSet);
      gif_buf_idx = gif_buf_last_idx;
    } else {
      gif_buf_idx += len;
    }
    file_pos += len;
    return true;
  }

  int16_t gif_buf_read(File *fd, uint8_t *dest, int16_t len) {
    while (len--) {
      if (gif_buf_idx == gif_buf_last_idx) {
        gif_buf_last_idx = fd->read(gif_buf, GIF_BUF_SIZE);
        gif_buf_idx = 0;
      }
      file_pos++;
      *(dest++) = gif_buf[gif_buf_idx++];
    }
    return len;
  }

  uint8_t gif_buf_read(File *fd) {
    if (gif_buf_idx == gif_buf_last_idx) {
      gif_buf_last_idx = fd->read(gif_buf, GIF_BUF_SIZE);
      gif_buf_idx = 0;
    }
    file_pos++;
    return gif_buf[gif_buf_idx++];
  }

  uint16_t gif_buf_read16(File *fd) {
    return gif_buf_read(fd) + (((uint16_t)gif_buf_read(fd)) << 8);
  }

  void read_palette(File *fd, gd_Palette *dest, int16_t num_colors) {
    uint8_t r, g, b;
    dest->len = num_colors;
    for (int16_t i = 0; i < num_colors; i++) {
      r = gif_buf_read(fd);
      g = gif_buf_read(fd);
      b = gif_buf_read(fd);
      dest->colors[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3);
    }
  }

  void discard_sub_blocks(gd_GIF *gif) {
    uint8_t len;
    do {
      gif_buf_read(gif->fd, &len, 1);
      gif_buf_seek(gif->fd, len);
    } while (len);
  }

  void read_plain_text_ext(gd_GIF *gif) {
    if (gif->plain_text) {
      uint16_t tx, ty, tw, th;
      uint8_t cw, ch, fg, bg;
      gif_buf_seek(gif->fd, 1); /* block size = 12 */
      tx = gif_buf_read16(gif->fd);
      ty = gif_buf_read16(gif->fd);
      tw = gif_buf_read16(gif->fd);
      th = gif_buf_read16(gif->fd);
      cw = gif_buf_read(gif->fd);
      ch = gif_buf_read(gif->fd);
      fg = gif_buf_read(gif->fd);
      bg = gif_buf_read(gif->fd);
      gif->plain_text(gif, tx, ty, tw, th, cw, ch, fg, bg);
    } else {
      gif_buf_seek(gif->fd, 13);
    }
    discard_sub_blocks(gif);
  }

  void read_graphic_control_ext(gd_GIF *gif) {
    uint8_t rdit;
    gif_buf_seek(gif->fd, 1); // block size (always 0x04)
    gif_buf_read(gif->fd, &rdit, 1);
    gif->gce.disposal = (rdit >> 2) & 3;
    gif->gce.input = rdit & 2;
    gif->gce.transparency = rdit & 1;
    gif->gce.delay = gif_buf_read16(gif->fd);
    gif_buf_read(gif->fd, &gif->gce.tindex, 1);
    gif_buf_seek(gif->fd, 1); // terminator
  }

  void read_comment_ext(gd_GIF *gif) {
    if (gif->comment) {
      gif->comment(gif);
    }
    discard_sub_blocks(gif);
  }

  void read_application_ext(gd_GIF *gif) {
    char app_id[8];
    char app_auth_code[3];

    gif_buf_seek(gif->fd, 1); // block size (always 0x0B)
    gif_buf_read(gif->fd, (uint8_t *)app_id, 8);
    gif_buf_read(gif->fd, (uint8_t *)app_auth_code, 3);

    if (!strncmp(app_id, "NETSCAPE", sizeof(app_id))) {
      gif_buf_seek(gif->fd, 2); // block size (0x03) + constant (0x01)
      gif->loop_count = gif_buf_read16(gif->fd);
      gif_buf_seek(gif->fd, 1); // terminator
    } else if (gif->application) {
      gif->application(gif, app_id, app_auth_code);
      discard_sub_blocks(gif);
    } else {
      discard_sub_blocks(gif);
    }
  }

  void read_ext(gd_GIF *gif) {
    uint8_t label;
    gif_buf_read(gif->fd, &label, 1);
    switch (label) {
      case 0x01: read_plain_text_ext(gif); break;
      case 0xF9: read_graphic_control_ext(gif); break;
      case 0xFE: read_comment_ext(gif); break;
      case 0xFF: read_application_ext(gif); break;
      default:
        Serial.print(F("[GIF] Unknown extension: 0x"));
        Serial.println(label, HEX);
        break;
    }
  }

  gd_Table *new_table() {
    int32_t s = (int32_t)sizeof(gd_Table) + (int32_t)(sizeof(gd_Entry) * 4096);
    gd_Table *table = (gd_Table *)malloc((size_t)s);
    if (!table) {
      Serial.print(F("[GIF] new_table malloc failed: "));
      Serial.println(s);
      return nullptr;
    }
    table->entries = (gd_Entry *)&table[1];
    return table;
  }

  void reset_table(gd_Table *table, uint16_t key_size) {
    table->nentries = (1 << key_size) + 2;
    for (uint16_t key = 0; key < (1 << key_size); key++) {
      table->entries[key] = (gd_Entry){1, 0xFFF, (uint8_t)key};
    }
  }

  int32_t add_entry(gd_Table *table, int32_t len, uint16_t prefix, uint8_t suffix) {
    table->entries[table->nentries] = (gd_Entry){len, prefix, suffix};
    table->nentries++;
    if ((table->nentries & (table->nentries - 1)) == 0) return 1;
    return 0;
  }

  uint16_t get_key(gd_GIF *gif, uint16_t key_size, uint8_t *sub_len, uint8_t *shift, uint8_t *byte) {
    int16_t bits_read;
    int16_t rpad;
    int16_t frag_size;
    uint16_t key;

    key = 0;
    for (bits_read = 0; bits_read < (int16_t)key_size; bits_read += frag_size) {
      rpad = (*shift + bits_read) % 8;
      if (rpad == 0) {
        if (*sub_len == 0) gif_buf_read(gif->fd, sub_len, 1);
        gif_buf_read(gif->fd, byte, 1);
        (*sub_len)--;
      }
      frag_size = MIN((int16_t)key_size - bits_read, (int16_t)8 - rpad);
      key |= ((uint16_t)((*byte) >> rpad)) << bits_read;
    }
    key &= (1 << key_size) - 1;
    *shift = (*shift + key_size) % 8;
    return key;
  }

  int16_t interlaced_line_index(int16_t h, int16_t y) {
    int16_t p;
    p = (h - 1) / 8 + 1;
    if (y < p) return y * 8;
    y -= p;
    p = (h - 5) / 8 + 1;
    if (y < p) return y * 8 + 4;
    y -= p;
    p = (h - 3) / 4 + 1;
    if (y < p) return y * 4 + 2;
    y -= p;
    return y * 2 + 1;
  }

  int8_t read_image_data(gd_GIF *gif, int16_t interlace, uint8_t *frame) {
    uint8_t sub_len, shift, byte, table_is_full = 0;
    uint16_t init_key_size, key_size;
    int32_t frm_off, str_len = 0, p, x, y;
    uint16_t key, clear, stop;
    int32_t ret;
    gd_Entry entry = {0, 0, 0};

    gif_buf_read(gif->fd, &byte, 1);
    key_size = (uint16_t)byte;
    clear = 1 << key_size;
    stop = clear + 1;

    if (!gif->table) return -1;
    reset_table(gif->table, key_size);

    key_size++;
    init_key_size = key_size;
    sub_len = shift = 0;
    key = get_key(gif, key_size, &sub_len, &shift, &byte); // clear code
    frm_off = 0;
    ret = 0;

    while (1) {
      if (key == clear) {
        key_size = init_key_size;
        gif->table->nentries = (1 << (key_size - 1)) + 2;
        table_is_full = 0;
      } else if (!table_is_full) {
        ret = add_entry(gif->table, str_len + 1, key, entry.suffix);
        if (gif->table->nentries == 0x1000) {
          ret = 0;
          table_is_full = 1;
        }
      }

      key = get_key(gif, key_size, &sub_len, &shift, &byte);
      if (key == clear) continue;
      if (key == stop) break;
      if (ret == 1) key_size++;

      entry = gif->table->entries[key];
      str_len = entry.len;

      while (1) {
        p = frm_off + entry.len - 1;
        x = p % gif->fw;
        y = p / gif->fw;
        if (interlace) {
          y = interlaced_line_index((int16_t)gif->fh, (int16_t)y);
        }

        // Always write decoded indices, including the transparent index.
        // We composite in RGB565 at a higher level, which avoids palette-change artifacts.
        frame[(gif->fy + y) * gif->width + gif->fx + x] = entry.suffix;

        if (entry.prefix == 0xFFF) break;
        entry = gif->table->entries[entry.prefix];
      }

      frm_off += str_len;
      if (key < (uint16_t)gif->table->nentries - 1 && !table_is_full) {
        gif->table->entries[gif->table->nentries - 1].suffix = entry.suffix;
      }
    }

    gif_buf_read(gif->fd, &sub_len, 1); // Must be zero
    gif->read_first_frame = true;
    return 0;
  }

  int8_t read_image(gd_GIF *gif, uint8_t *frame) {
    uint8_t fisrz;
    int16_t interlace;

    gif->fx = gif_buf_read16(gif->fd);
    gif->fy = gif_buf_read16(gif->fd);
    gif->fw = gif_buf_read16(gif->fd);
    gif->fh = gif_buf_read16(gif->fd);
    gif_buf_read(gif->fd, &fisrz, 1);

    interlace = fisrz & 0x40;
    if (fisrz & 0x80) {
      read_palette(gif->fd, &gif->lct, 1 << ((fisrz & 0x07) + 1));
      gif->palette = &gif->lct;
    } else {
      gif->palette = &gif->gct;
    }

    return read_image_data(gif, interlace, frame);
  }

  int16_t gif_buf_last_idx, gif_buf_idx;
  int32_t file_pos;
  uint8_t gif_buf[GIF_BUF_SIZE];
};

static GifClass gifClass;

// ======================= APP STATE =======================
static std::vector<String> g_gif_paths;
static size_t g_gif_index = 0;

static FS &gifFS() {
#if USE_LITTLEFS
  return LittleFS;
#else
  return SPIFFS;
#endif
}

static const char *fsName() {
#if USE_LITTLEFS
  return "LittleFS";
#else
  return "SPIFFS";
#endif
}

static bool isGifPath(const String &path) {
  String p = path;
  p.toLowerCase();
  return p.endsWith(".gif");
}

static void collectGifs() {
  g_gif_paths.clear();
  FS &fs = gifFS();

  auto scan_dir = [&](const char *dir_path) {
    File dir = fs.open(dir_path);
    if (!dir) return;
    if (!dir.isDirectory()) {
      dir.close();
      return;
    }

    File file = dir.openNextFile();
    while (file) {
      if (!file.isDirectory()) {
        String path = file.name();
        if (!path.startsWith("/")) {
          if (strcmp(dir_path, "/") == 0) path = String("/") + path;
          else path = String(dir_path) + "/" + path;
        }
        if (isGifPath(path)) g_gif_paths.push_back(path);
      }
      file.close();
      file = dir.openNextFile();
    }
    dir.close();
  };

  // For LittleFS, we can open "/assets" as a directory. For SPIFFS, this often
  // fails (SPIFFS doesn't have real directories), so we fall back to scanning "/".
  scan_dir(GIF_DIR);
  if (g_gif_paths.empty()) scan_dir("/");

  std::sort(g_gif_paths.begin(), g_gif_paths.end());

  Serial.printf("[FS] Found %u GIF(s)\n", (unsigned)g_gif_paths.size());
  for (const auto &p : g_gif_paths) {
    Serial.printf("  - %s\n", p.c_str());
  }
}

static void drawHeader(const char *title, const char *subtitle = nullptr) {
  gfx->fillScreen(BLACK);
  gfx->setTextWrap(false);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 12);
  gfx->print(title);

  if (subtitle) {
    gfx->setTextSize(1);
    gfx->setCursor(12, 44);
    gfx->print(subtitle);
  }
}

static void clearCanvasRect(uint16_t *canvas, uint16_t canvasW, uint16_t canvasH,
                            uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            uint16_t color) {
  if (!canvas) return;
  if (x >= canvasW || y >= canvasH) return;
  if (x + w > canvasW) w = canvasW - x;
  if (y + h > canvasH) h = canvasH - y;
  for (uint16_t row = 0; row < h; row++) {
    uint16_t *dst = canvas + (size_t)(y + row) * canvasW + x;
    for (uint16_t col = 0; col < w; col++) dst[col] = color;
  }
}

static void compositeFrameRectToCanvas(gd_GIF *gif, const uint8_t *idxFrame, uint16_t *canvas565) {
  const bool hasTrans = gif->gce.transparency != 0;
  const uint8_t tindex = gif->gce.tindex;
  const uint16_t *colors = gif->palette->colors; // current (GCT or LCT for this frame)

  for (uint16_t j = 0; j < gif->fh; j++) {
    size_t rowBase = (size_t)(gif->fy + j) * gif->width + gif->fx;
    for (uint16_t k = 0; k < gif->fw; k++) {
      uint8_t idx = idxFrame[rowBase + k];
      if (!hasTrans || idx != tindex) {
        canvas565[rowBase + k] = colors[idx];
      }
    }
  }
}

static void scaleCanvasToOutbuf(const uint16_t *canvas, uint16_t inW, uint16_t inH,
                                uint16_t *outbuf, int outW, int outH,
                                const uint16_t *xmap, const uint16_t *ymap) {
  for (int y = 0; y < outH; y++) {
    uint16_t sy = ymap[y];
    const uint16_t *srcRow = canvas + (size_t)sy * inW;
    uint16_t *dstRow = outbuf + (size_t)y * outW;
    for (int x = 0; x < outW; x++) {
      dstRow[x] = srcRow[xmap[x]];
    }
  }
}

static bool playGifPath(const char *path) {
  FS &fs = gifFS();
  File f = fs.open(path, "r");
  if (!f || f.isDirectory()) {
    Serial.printf("[GIF] Failed to open: %s\n", path);
    return false;
  }

  gd_GIF *gif = gifClass.gd_open_gif(&f);
  if (!gif) {
    Serial.printf("[GIF] gd_open_gif failed: %s\n", path);
    f.close();
    return false;
  }

  const uint16_t inW = gif->width;
  const uint16_t inH = gif->height;
  const size_t pixels = (size_t)inW * inH;

  uint8_t *idxFrame = (uint8_t *)gif_alloc(pixels);
  uint16_t *canvas565 = (uint16_t *)gif_alloc(pixels * sizeof(uint16_t));
  if (!idxFrame || !canvas565) {
    Serial.printf("[GIF] OOM allocating buffers for %ux%u\n", (unsigned)inW, (unsigned)inH);
    if (idxFrame) free(idxFrame);
    if (canvas565) free(canvas565);
    gifClass.gd_close_gif(gif);
    return false;
  }

  const uint16_t bgColor = gif->gct.colors[gif->bgindex];
  for (size_t i = 0; i < pixels; i++) canvas565[i] = bgColor;

  // Determine output size (scale down to fit display; don't upscale).
  const int dispW = gfx->width();
  const int dispH = gfx->height();
  int outW = (int)inW;
  int outH = (int)inH;
  if (outW > dispW || outH > dispH) {
    // scale = min(dispW/inW, dispH/inH)
    if ((int64_t)dispW * (int64_t)inH < (int64_t)dispH * (int64_t)inW) {
      outW = dispW;
      outH = (int)((int64_t)inH * (int64_t)dispW / (int64_t)inW);
    } else {
      outH = dispH;
      outW = (int)((int64_t)inW * (int64_t)dispH / (int64_t)inH);
    }
  }
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;

  const int x0 = (dispW - outW) / 2;
  const int y0 = (dispH - outH) / 2;

  uint16_t *outbuf = (uint16_t *)gif_alloc((size_t)outW * (size_t)outH * sizeof(uint16_t));
  uint16_t *xmap = (uint16_t *)gif_alloc((size_t)outW * sizeof(uint16_t));
  uint16_t *ymap = (uint16_t *)gif_alloc((size_t)outH * sizeof(uint16_t));
  if (!outbuf || !xmap || !ymap) {
    Serial.println(F("[GIF] OOM allocating scale buffers"));
    if (outbuf) free(outbuf);
    if (xmap) free(xmap);
    if (ymap) free(ymap);
    free(idxFrame);
    free(canvas565);
    gifClass.gd_close_gif(gif);
    return false;
  }

  for (int x = 0; x < outW; x++) xmap[x] = (uint16_t)(((uint32_t)x * (uint32_t)inW) / (uint32_t)outW);
  for (int y = 0; y < outH; y++) ymap[y] = (uint16_t)(((uint32_t)y * (uint32_t)inH) / (uint32_t)outH);

  // Static header (drawn outside the image region).
  gfx->fillScreen(BLACK);
  gfx->setTextWrap(false);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(12, 12);
  gfx->print("GIF Tester");
  gfx->setTextSize(1);
  gfx->setCursor(12, 44);
  gfx->print(path);

  char info[64];
  snprintf(info, sizeof(info), "%ux%u -> %dx%d  (%s)", (unsigned)inW, (unsigned)inH, outW, outH, fsName());
  gfx->setCursor(12, 62);
  gfx->print(info);

  const uint32_t show_start = millis();
  uint32_t frames = 0;

  while (millis() - show_start < GIF_SHOW_MS) {
    int32_t res = gifClass.gd_get_frame(gif, idxFrame);
    if (res == 0) {
      gifClass.gd_rewind(gif);
      continue;
    }
    if (res < 0) {
      Serial.printf("[GIF] Decode error: %s\n", path);
      break;
    }

    compositeFrameRectToCanvas(gif, idxFrame, canvas565);
    scaleCanvasToOutbuf(canvas565, inW, inH, outbuf, outW, outH, xmap, ymap);

    gfx->startWrite();
    gfx->draw16bitRGBBitmap(x0, y0, outbuf, outW, outH);
    gfx->endWrite();

    frames++;

    uint32_t delay_ms = (uint32_t)gif->gce.delay * 10;
    if (delay_ms < MIN_FRAME_DELAY_MS) delay_ms = MIN_FRAME_DELAY_MS;

    // Wait for frame delay, but don't oversleep past GIF_SHOW_MS.
    uint32_t waited = 0;
    while (waited < delay_ms && (millis() - show_start) < GIF_SHOW_MS) {
      delay(1);
      waited++;
    }

    // Disposal handling (restore to background) is common in these assets.
    if (gif->gce.disposal == 2) {
      clearCanvasRect(canvas565, inW, inH, gif->fx, gif->fy, gif->fw, gif->fh, bgColor);
    }
  }

  Serial.printf("[GIF] %s: drew %lu frame(s)\n", path, (unsigned long)frames);

  free(outbuf);
  free(xmap);
  free(ymap);
  free(idxFrame);
  free(canvas565);
  gifClass.gd_close_gif(gif);
  return true;
}

// ======================= Arduino Setup/Loop =======================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] Qualia GIF Tester");

  // Display bring-up (mirrors production.ino)
  Wire.setClock(1000000);
  gfx->begin();
  gfx->setRotation(1);
  expander->pinMode(PCA_TFT_BACKLIGHT, OUTPUT);
  expander->digitalWrite(PCA_TFT_BACKLIGHT, HIGH);

  drawHeader("GIF Tester", "Mounting filesystem...");

  bool ok = false;
#if USE_LITTLEFS
  ok = LittleFS.begin(false);
  if (!ok && FS_FORMAT_ON_FAIL) {
    Serial.println("[FS] LittleFS mount failed; formatting...");
    ok = LittleFS.begin(true);
  }
#else
  ok = SPIFFS.begin(false);
  if (!ok && FS_FORMAT_ON_FAIL) {
    Serial.println("[FS] SPIFFS mount failed; formatting...");
    ok = SPIFFS.begin(true);
  }
#endif

  if (!ok) {
    Serial.printf("[FS] %s mount failed\n", fsName());
    drawHeader("FS mount failed", "Check Partition Scheme (with SPIFFS)");
    while (true) delay(1000);
  }

  Serial.printf("[FS] Mounted %s\n", fsName());
  collectGifs();

  if (g_gif_paths.empty()) {
    drawHeader("No GIFs found", "Upload to /assets/*.gif");
    Serial.println("[FS] No GIFs found. Expected /assets/*.gif on the device FS.");
  } else {
    drawHeader("GIF Tester", "Starting playback...");
    delay(500);
  }
}

void loop() {
  if (g_gif_paths.empty()) {
    delay(1000);
    return;
  }

  if (g_gif_index >= g_gif_paths.size()) g_gif_index = 0;
  const String &path = g_gif_paths[g_gif_index];

  Serial.printf("[PLAY] %u/%u: %s\n",
                (unsigned)(g_gif_index + 1),
                (unsigned)g_gif_paths.size(),
                path.c_str());

  playGifPath(path.c_str());

  g_gif_index = (g_gif_index + 1) % g_gif_paths.size();
  delay(BETWEEN_GIFS_MS);
}
