#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <map>
#include <unordered_map>
#include <iterator>
#include <array>
#include <climits>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <memory>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <dirent.h>

#include "griddb.h"
#include "forwarder.h"
#include "launcher_update.h"
#include "localization.h"
#include "SwitchStorage.h"
#include "ui_audio.h"

// SDL uses Xbox button names.
#define BTN_CONFIRM  SDL_CONTROLLER_BUTTON_B
#define BTN_CANCEL   SDL_CONTROLLER_BUTTON_A
#define BTN_SETTINGS SDL_CONTROLLER_BUTTON_Y

static const char *DATA_DIR    = "sdmc:/switch/drastic";
static const char *LAUNCHER_INI= "sdmc:/switch/drastic/launcher.ini";
static const char *EMU_INI     = "sdmc:/switch/drastic/drastic.ini";
static const char *COVERS_DIR = "sdmc:/switch/drastic/covers";
static const char *CORES_DIR  = "sdmc:/switch/drastic/cores";
static const char *GAMECFG_DIR= "sdmc:/switch/drastic/gamecfg";
static const char *DEF_GAMEDIR= "sdmc:/switch/drastic/games";
static const char *SYSTEM_DIR = "sdmc:/switch/drastic/system";
static const char *USER_DIR   = "sdmc:/switch/drastic/user";
static const char *CACHE_DIR  = "sdmc:/switch/drastic/cache";
static const char *METADATA_INI = "sdmc:/switch/drastic/cache/library_metadata.ini";
static const char *SHADERS_DIR= "sdmc:/switch/drastic/shaders";
static const char *BUNDLED_SHADERS_DIR=
    "sdmc:/switch/drastic/shaders/Bundled";
static const char *LSFG_DIR   = "sdmc:/switch/drastic/lsfg";
static const char *EMU_HOST_DIR = "sdmc:/switch/drastic/.emu";
static const char *LSFG_DLL_FILE =
    "sdmc:/switch/drastic/lsfg/Lossless.dll";
static const char *CORE_SO_PATH = "/switch/drastic/cores/libdrastic_arm64.so";
struct KV { std::string k, v; };
struct Store {
  std::vector<KV> kv;
  // Launcher/library files contain thousands of stable-identity keys.  Keep
  // the ordered vector for deterministic serialization, but index lookups so
  // loading and updating the library remains linear instead of quadratic.
  mutable std::unordered_map<std::string,size_t> index;
  mutable size_t indexedSize=SIZE_MAX;
};

static Store g_global;
static Store g_game;
static Store g_titles;
static Store g_metadata;
static Store *g_active = &g_global;
static const char *TITLES_INI = "sdmc:/switch/drastic/titles.ini";

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}
static void ensureStoreIndex(const Store &s) {
  if(s.indexedSize==s.kv.size()) return;
  s.index.clear();
  s.index.reserve(s.kv.size());
  for(size_t item=0;item<s.kv.size();item++) s.index[s.kv[item].k]=item;
  s.indexedSize=s.kv.size();
}
static void invalidateStoreIndex(Store &s) {
  s.index.clear();
  s.indexedSize=SIZE_MAX;
}
static const char *storeGet(Store &s, const char *key, const char *def) {
  ensureStoreIndex(s);
  const auto found=s.index.find(key);
  return found==s.index.end()?def:s.kv[found->second].v.c_str();
}
static void storeSet(Store &s, const char *key, const char *val) {
  ensureStoreIndex(s);
  const auto found=s.index.find(key);
  if(found!=s.index.end()){s.kv[found->second].v=val;return;}
  s.kv.push_back({ key, val });
  s.index[s.kv.back().k]=s.kv.size()-1;
  s.indexedSize=s.kv.size();
}
static void storeRemove(Store &s, const char *key) {
  ensureStoreIndex(s);
  const auto found=s.index.find(key);
  if(found==s.index.end())return;
  s.kv.erase(s.kv.begin()+found->second);
  invalidateStoreIndex(s);
}
static bool storeHas(const Store &s, const char *key) {
  ensureStoreIndex(s);
  return s.index.find(key)!=s.index.end();
}
static bool migrateHotkeyDefaults(Store &store) {
  const int version = atoi(storeGet(
      store, "Wrapper/HotkeyDefaultsVersion", "0"));
  if (version >= 3)
    return false;

  if (version < 2 &&
      !strcasecmp(storeGet(store, "Wrapper/HotkeyMenu", ""), "L+R+Minus") &&
      !strcasecmp(storeGet(store, "Wrapper/HotkeyQuit", ""), "L+R+Plus")) {
    storeSet(store, "Wrapper/HotkeyMenu", "L+R+Plus");
    storeSet(store, "Wrapper/HotkeyQuit", "None");
  }
  struct HotkeyDefault {
    const char *key;
    const char *oldValue;
    const char *newValue;
  };
  static const HotkeyDefault saferDefaults[] = {
    {"Wrapper/HotkeySaveState", "L+R+Y",    "L+R+Minus+Y"},
    {"Wrapper/HotkeyLoadState", "L+R+X",    "L+R+Minus+X"},
    {"Wrapper/HotkeyNextSlot",  "L+R+Up",   "L+R+Minus+Up"},
    {"Wrapper/HotkeyPreviousSlot", "L+R+Down", "L+R+Minus+Down"},
    {"Wrapper/HotkeyReset",     "L+R+A",    "L+R+Minus+A"},
  };
  for (const HotkeyDefault &binding : saferDefaults)
    if (!strcasecmp(storeGet(store, binding.key, ""), binding.oldValue))
      storeSet(store, binding.key, binding.newValue);
  storeSet(store, "Wrapper/HotkeyDefaultsVersion", "3");
  return true;
}
static bool normalizeCpuThreads(Store &store) {
  if (!storeHas(store, "Drastic/CpuThreads")) return false;
  const int threads = atoi(storeGet(store, "Drastic/CpuThreads", "3"));
  if (threads >= 1 && threads <= 3) return false;
  storeSet(store, "Drastic/CpuThreads", "3");
  return true;
}
static bool migrateStylusMode(Store &store, bool addDefault) {
  bool changed=false;
  if(!storeHas(store,"Wrapper/StylusMode")){
    if(storeHas(store,"Wrapper/AnalogStylus")){
      const char *legacy=storeGet(store,"Wrapper/AnalogStylus","true");
      const bool enabled=strcasecmp(legacy,"false")&&strcmp(legacy,"0")&&
                         strcasecmp(legacy,"off");
      storeSet(store,"Wrapper/StylusMode",enabled?"stick":"off");
      changed=true;
    }else if(addDefault){
      storeSet(store,"Wrapper/StylusMode","stick");
      changed=true;
    }
  }
  const char *mode=storeGet(store,"Wrapper/StylusMode","stick");
  if(strcmp(mode,"off")&&strcmp(mode,"stick")&&strcmp(mode,"motion")){
    storeSet(store,"Wrapper/StylusMode","stick");
    changed=true;
  }
  if(storeHas(store,"Wrapper/AnalogStylus")){
    storeRemove(store,"Wrapper/AnalogStylus");
    changed=true;
  }
  return changed;
}
static bool migrateLauncherSettings(Store &store) {
  bool changed = migrateHotkeyDefaults(store);
  changed = normalizeCpuThreads(store) || changed;
  changed = migrateStylusMode(store,true) || changed;

  /* The original launcher option was a boolean whose enabled state rendered
     the UI at 90 degrees. Preserve that exact orientation while replacing it
     with the complete four-way rotation setting. */
  if (!storeHas(store, "Wrapper/LauncherRotation")) {
    const bool portrait = storeHas(store, "Wrapper/LauncherPortrait") &&
        !strcmp(storeGet(store, "Wrapper/LauncherPortrait", "false"), "true");
    storeSet(store, "Wrapper/LauncherRotation", portrait ? "1" : "0");
    changed = true;
  }
  if (storeHas(store, "Wrapper/LauncherPortrait")) {
    storeRemove(store, "Wrapper/LauncherPortrait");
    changed = true;
  }
  const int launcherRotation = atoi(storeGet(
      store, "Wrapper/LauncherRotation", "0"));
  if (launcherRotation < 0 || launcherRotation > 3) {
    storeSet(store, "Wrapper/LauncherRotation", "0");
    changed = true;
  }

  /* Startup boost is now an unconditional host policy, and cheat enablement
     is controlled per title from the in-game overlay. Do not keep obsolete
     global settings that would silently override those policies. */
  if (storeHas(store, "Wrapper/CpuBoost")) {
    storeRemove(store, "Wrapper/CpuBoost");
    changed = true;
  }
  if (storeHas(store, "Drastic/CheatsEnabled")) {
    storeRemove(store, "Drastic/CheatsEnabled");
    changed = true;
  }

  const int version = atoi(storeGet(
      store, "Wrapper/LauncherSettingsVersion", "0"));
  if (version < 1) {
    /* English was the old implicit default. Move that default to Auto while
       preserving every other explicitly selected firmware language. */
    if (!strcmp(storeGet(store, "Drastic/FirmwareLanguage", "1"), "1"))
      storeSet(store, "Drastic/FirmwareLanguage", "-1");
    changed = true;
  }
  if (version < 2) {
    /* Vertical was written into every old profile as the implicit default,
       so migrate that value once while preserving every other layout. */
    if (!storeHas(store, "Wrapper/Layout") ||
        !strcmp(storeGet(store, "Wrapper/Layout", "vertical"), "vertical"))
      storeSet(store, "Wrapper/Layout", "horizontal");
    storeSet(store, "Wrapper/LauncherSettingsVersion", "2");
    changed = true;
  }
  if (version < 3) {
    /* Versions through 1.0.4 exposed native value 0 as "Unlimited", but
       DraStic's real table is 0=50% through 5=Unlimited. Preserve the user's
       old selection before exposing the complete native speed list. */
    if (!strcmp(storeGet(store, "Drastic/FastForwardSpeed", "2"), "0"))
      storeSet(store, "Drastic/FastForwardSpeed", "5");
    storeSet(store, "Wrapper/LauncherSettingsVersion", "3");
    changed = true;
  }
  if (version < 4) {
    storeSet(store, "Wrapper/LauncherSettingsVersion", "4");
    changed = true;
  }
  return changed;
}
static void storeRemovePrefix(Store &s, const char *prefix) {
  const size_t length = strlen(prefix);
  s.kv.erase(std::remove_if(s.kv.begin(), s.kv.end(), [&](const KV &entry) {
    return entry.k.compare(0, length, prefix) == 0;
  }), s.kv.end());
  invalidateStoreIndex(s);
}
static bool recoverAtomicFile(const std::string &path);
static void storeLoad(Store &s, const char *path) {
  s.kv.clear();
  invalidateStoreIndex(s);
  if (!recoverAtomicFile(path)) return;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[2048];
  while (fgets(line, sizeof(line), f)) {
    std::string t = trim(line);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(t.substr(0, eq)), v = trim(t.substr(eq + 1));
    if (!k.empty()) s.kv.push_back({ k, v });
  }
  fclose(f);
}

static bool queryRegularFile(const std::string &path, bool &exists) {
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    exists = true;
    return S_ISREG(st.st_mode);
  }
  exists = false;
  return errno == ENOENT;
}

static bool regularFileExists(const std::string &path) {
  bool exists = false;
  return queryRegularFile(path, exists) && exists;
}

static bool lsfgDllInstalled() {
  return regularFileExists(LSFG_DLL_FILE);
}

static bool normalizeLsfgStore(Store &store) {
  bool changed = false;
  if (storeHas(store, "Wrapper/LSFGDllPath")) {
    storeRemove(store, "Wrapper/LSFGDllPath");
    changed = true;
  }
  if (storeHas(store, "Wrapper/LSFGFlowScale")) {
    const double flow = std::strtod(
        storeGet(store, "Wrapper/LSFGFlowScale", "0.25"), nullptr);
    const char *normalized = flow > 0.375 ? "0.5" : "0.25";
    if (strcmp(storeGet(store, "Wrapper/LSFGFlowScale", "0.25"),
               normalized)) {
      storeSet(store, "Wrapper/LSFGFlowScale", normalized);
      changed = true;
    }
  }
  if (!lsfgDllInstalled() &&
      storeHas(store, "Wrapper/LSFGEnabled") &&
      !strcmp(storeGet(store, "Wrapper/LSFGEnabled", "false"), "true")) {
    storeSet(store, "Wrapper/LSFGEnabled", "false");
    changed = true;
  }
  return changed;
}

static bool recoverAtomicFile(const std::string &path) {
  const std::string tmp = path + ".tmp";
  const std::string old = path + ".old";
  bool currentExists = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, currentExists) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists)) return false;
  if (!currentExists && oldExists) {
    if (rename(old.c_str(), path.c_str()) != 0) return false;
    fsdevCommitDevice("sdmc");
    currentExists = true;
    oldExists = false;
  }
  if (tmpExists && remove(tmp.c_str()) != 0) return false;
  if (currentExists && oldExists && remove(old.c_str()) != 0) return false;
  if (tmpExists || oldExists) fsdevCommitDevice("sdmc");
  return true;
}

static bool replaceAtomic(const std::string &path, const std::string &tmp) {
  const std::string old = path + ".old";
  bool hadCurrent = false, oldExists = false, tmpExists = false;
  if (!queryRegularFile(path, hadCurrent) || !queryRegularFile(old, oldExists) ||
      !queryRegularFile(tmp, tmpExists) || !tmpExists) return false;
  if (oldExists && remove(old.c_str()) != 0) return false;
  if (hadCurrent && rename(path.c_str(), old.c_str()) != 0) return false;
  if (rename(tmp.c_str(), path.c_str()) != 0) {
    if (hadCurrent) {
      rename(old.c_str(), path.c_str());
      fsdevCommitDevice("sdmc");
    }
    return false;
  }
  fsdevCommitDevice("sdmc");
  if (hadCurrent && remove(old.c_str()) == 0) fsdevCommitDevice("sdmc");
  return true;
}

static bool writeAtomicText(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  if (!recoverAtomicFile(path)) return false;
  FILE *file = fopen(tmp.c_str(), "wb");
  if (!file) return false;
  bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
  if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
  if (fclose(file) != 0) ok = false;
  if (!ok) { remove(tmp.c_str()); return false; }
  if (!replaceAtomic(path, tmp)) { remove(tmp.c_str()); return false; }
  return true;
}

static bool storeSave(Store &s, const char *path) {
  mkdir(DATA_DIR, 0777);
  std::string text = "# DrasticDS_nx launcher\n";
  for (auto &e : s.kv) text += e.k + " = " + e.v + "\n";
  return writeAtomicText(path, text);
}

static bool migrateFastForwardProfiles() {
  DIR *directory = opendir(GAMECFG_DIR);
  if (!directory) return errno == ENOENT;

  bool success = true;
  while (dirent *entry = readdir(directory)) {
    const char *extension = strrchr(entry->d_name, '.');
    if (!extension || strcasecmp(extension, ".ini")) continue;

    const std::string path = std::string(GAMECFG_DIR) + "/" + entry->d_name;
    Store profile;
    storeLoad(profile, path.c_str());
    if (strcmp(storeGet(profile, "Drastic/FastForwardSpeed", "2"), "0"))
      continue;
    storeSet(profile, "Drastic/FastForwardSpeed", "5");
    if (!storeSave(profile, path.c_str())) success = false;
  }
  closedir(directory);
  return success;
}

static const char *iniGet(const char *key, const char *def) {
  if (g_active == &g_game) {
    for (auto &e : g_game.kv)   if (e.k == key) return e.v.c_str();
    for (auto &e : g_global.kv) if (e.k == key) return e.v.c_str();
    return def;
  }
  return storeGet(*g_active, key, def);
}
static void iniSet(const char *key, const char *val) { storeSet(*g_active, key, val); }

enum OType { OT_CHOICE, OT_RANGE, OT_SCALED_RANGE, OT_SUBMENU, OT_TEXT,
             OT_HOTKEY, OT_STATUS, OT_SHADER, OT_DATETIME };
struct Choice { const char *label, *val; };
struct Opt {
  const char *label;
  const char *key;
  OType type;
  const Choice *ch; int nch;
  int lo, hi, step;
  const char *def;
  int sub;
  const char *gateKey;
  const char *gateOff;
  int multiplier;
  const char *suffix;
};
#define O_CHOICE(l,k,c,d)      { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_RANGE(l,k,lo,hi,s,d) { l, k, OT_RANGE,  nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, 1, nullptr }
#define O_SCALED_RANGE(l,k,lo,hi,s,d,m,u) { l, k, OT_SCALED_RANGE, nullptr,0, lo,hi,s, d, 0, nullptr, nullptr, m, u }
#define O_SCALED_RANGEG(l,k,lo,hi,s,d,m,u,gk,go) { l, k, OT_SCALED_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go, m, u }
#define O_SUB(l,scr)           { l, nullptr, OT_SUBMENU, nullptr,0, 0,0,0, nullptr, scr, nullptr, nullptr, 1, nullptr }
#define O_CHOICEG(l,k,c,d,gk,go) { l, k, OT_CHOICE, c, (int)(sizeof(c)/sizeof(*c)), 0,0,0, d, 0, gk, go, 1, nullptr }
#define O_RANGEG(l,k,lo,hi,s,d,gk,go) { l, k, OT_RANGE, nullptr,0, lo,hi,s, d, 0, gk, go, 1, nullptr }
#define O_TEXT(l,k,d)          { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_TEXTG(l,k,d,gk,go)   { l, k, OT_TEXT, nullptr,0, 0,0,0, d, 0, gk, go, 1, nullptr }
#define O_HOTKEY(l,k,d)        { l, k, OT_HOTKEY, nullptr,0, 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_HOTKEYG(l,k,d,gk,go) { l, k, OT_HOTKEY, nullptr,0, 0,0,0, d, 0, gk, go, 1, nullptr }
#define O_STATUS(l)            { l, nullptr, OT_STATUS, nullptr,0, 0,0,0, nullptr, 0, nullptr, nullptr, 1, nullptr }
#define O_SHADER(l,k,d)        { l, k, OT_SHADER, nullptr,0, 0,0,0, d, 0, nullptr, nullptr, 1, nullptr }
#define O_DATETIMEG(l,k,d,gk,go) { l, k, OT_DATETIME, nullptr,0, 0,0,0, d, 0, gk, go, 1, nullptr }

static char g_autoFirmwareLanguage[32] = "Auto (English)";

static void updateAutoFirmwareLanguageLabel() {
  const char *resolved = "English";
  if (R_SUCCEEDED(setInitialize())) {
    u64 languageCode = 0;
    SetLanguage language = SetLanguage_ENUS;
    const Result getResult = setGetSystemLanguage(&languageCode);
    const Result convertResult = R_SUCCEEDED(getResult)
        ? setMakeLanguage(languageCode, &language) : getResult;
    if (R_SUCCEEDED(getResult) && R_SUCCEEDED(convertResult)) {
      switch (language) {
        case SetLanguage_JA: resolved = "Japanese"; break;
        case SetLanguage_ENUS:
        case SetLanguage_ENGB: resolved = "English"; break;
        case SetLanguage_FR:
        case SetLanguage_FRCA: resolved = "French"; break;
        case SetLanguage_DE: resolved = "German"; break;
        case SetLanguage_IT: resolved = "Italian"; break;
        case SetLanguage_ES:
        case SetLanguage_ES419: resolved = "Spanish"; break;
        case SetLanguage_KO: resolved = "Korean"; break;
        default: break; /* Unsupported DS language: English fallback. */
      }
    }
    setExit();
  }
  snprintf(g_autoFirmwareLanguage, sizeof(g_autoFirmwareLanguage),
           "Auto (%s)", resolved);
}

static const Choice C_backend[]  = { {"Vulkan (NVK)","vk"}, {"OpenGL (NVC0)","gl"},
                                     {"Zink (OpenGL on NVK)","zink"} };
static const Choice C_bool[]     = { {"Off","false"}, {"On","true"} };
static const Choice C_lsfgFlow[] = { {"Quarter (recommended)","0.25"},
                                     {"Half","0.5"} };
static const Choice C_layout[]   = { {"Vertical","vertical"}, {"Horizontal","horizontal"},
                                     {"Top screen only","top"}, {"Touch screen only","bottom"},
                                     {"Hybrid (top large)","hybrid_top"}, {"Hybrid (touch large)","hybrid_bottom"},
                                     {"Custom (in-game editor)","custom"} };
static const Choice C_rotation[] = { {"0 degrees","0"}, {"90 degrees","1"}, {"180 degrees","2"}, {"270 degrees","3"} };
  static const Choice C_filter[]   = { {"Nearest","nearest"}, {"Linear","linear"},
                                     {"Quilez smooth","quilez"}, {"Scanline","scanline"},
                                     {"Scale2x","scale2x"}, {"HQ2x","hq2x"}, {"FXAA","fxaa"},
                                     {"FXAA high quality","fxaa_hq"}, {"SMAA","smaa"},
                                     {"Custom shader","custom"} };
static const Choice C_latency[]  = { {"Low","0"}, {"Balanced","1"}, {"High compatibility","2"}, {"Maximum","3"} };
static const Choice C_micSource[] = { {"Simulated noise","noise"},
                                      {"External microphone","external"} };
static const Choice C_micLevel[] = { {"Low","0"}, {"Normal","1"}, {"High","2"}, {"Maximum","3"} };
static const Choice C_threads[]  = { {"1","1"}, {"2","2"}, {"3 (recommended)","3"} };
static const Choice C_frameskipType[] = { {"Automatic","0"}, {"Fixed","1"}, {"Aggressive","2"}, {"Maximum","3"} };
static const Choice C_ffSpeed[]  = { {"50%","0"}, {"150%","1"},
                                     {"200%","2"}, {"300%","3"},
                                     {"400%","4"}, {"Unlimited","5"} };
static const Choice C_autofire[] = { {"Slow","0"}, {"Normal","2"}, {"Fast","4"}, {"Very fast","7"} };
/* Native DraStic Slot-2 enum. Keep these values aligned with build 109:
 * 0=None, 1=GBA, 2=SRAM, 3=rumble, 4/5=motion accessories. */
static const Choice C_slot2[]    = { {"None","0"}, {"GBA Cart","1"},
                                     {"SRAM Cart","2"}, {"Rumble Pack","3"},
                                     {"Motion Pack (Official)","4"},
                                     {"Motion Pack (Homebrew)","5"} };
static const Choice C_autosave[] = { {"Off","0"}, {"1 minute","60"}, {"5 minutes","300"}, {"10 minutes","600"}, {"30 minutes","1800"} };
static const Choice C_firmwareLanguage[] = { {g_autoFirmwareLanguage,"-1"}, {"Japanese","0"}, {"English","1"}, {"French","2"}, {"German","3"},
                                             {"Italian","4"}, {"Spanish","5"}, {"Korean","6"} };
/* Nintendo DS IPL user-color order and RGB15 palette. The stored values stay
   identical to DraStic's native 0..15 firmware field. */
static const Choice C_firmwareColor[] = {
  {"Gray","0"}, {"Brown","1"}, {"Red","2"}, {"Pink","3"},
  {"Orange","4"}, {"Yellow","5"}, {"Lime green","6"}, {"Green","7"},
  {"Dark green","8"}, {"Sea green","9"}, {"Turquoise","10"}, {"Blue","11"},
  {"Dark blue","12"}, {"Dark purple","13"}, {"Violet","14"}, {"Magenta","15"},
};
static const SDL_Color C_firmwareColorRgb[] = {
  { 99,132,156,255}, {189, 74,  0,255}, {255,  0, 24,255}, {255,140,255,255},
  {255,148,  0,255}, {247,231,  0,255}, {173,255,  0,255}, {  0,255,  0,255},
  {  0,165, 57,255}, { 74,222,140,255}, { 49,189,247,255}, {  0, 90,247,255},
  {  0,  0,148,255}, {140,  0,214,255}, {214,  0,239,255}, {255,  0,148,255},
};
static const Choice C_btn[]      = { {"A","A"},{"B","B"},{"X","X"},{"Y","Y"},{"L","L"},{"R","R"},{"ZL","ZL"},{"ZR","ZR"},
                                     {"Plus","Plus"},{"Minus","Minus"},{"L-Stick","StickL"},{"R-Stick","StickR"},
                                     {"D-Up","Up"},{"D-Down","Down"},{"D-Left","Left"},{"D-Right","Right"},{"None","None"} };
static const Choice C_hotkeyMode[] = { {"Hold","hold"}, {"Toggle","toggle"} };
static const Choice C_stylusMode[] = { {"Off","off"},
                                       {"Right stick","stick"},
                                       {"Motion controls","motion"} };
static const Choice C_launcherTheme[] = { {"XMB (PS3)","xmb"}, {"Glow","animated"}, {"Bubbles","homebrew"},
                                          {"Classic","classic"}, {"OLED black","oled"} };
static const Choice C_gridColumns[] = { {"3","3"}, {"4","4"}, {"5","5"}, {"6","6"}, {"7","7"}, {"8","8"} };
static const Choice C_gridRows[] = { {"1","1"}, {"2","2"}, {"3","3"} };
static const Choice C_uiLanguage[] = { {"System","system"}, {"English","en"}, {"Français","fr"},
                                       {"Deutsch","de"}, {"Español","es"}, {"Italiano","it"},
                                       {"Português","pt"} };

enum { SCR_GRAPHICS, SCR_ENHANCE, SCR_FRAMEGEN, SCR_AUDIO, SCR_EMU,
       SCR_FRAMERATE, SCR_NETWORK, SCR_CONTROLLER, SCR_FIRMWARE, SCR_COUNT };

static const Opt S_graphics[] = {
  O_CHOICE("Renderer",          "Wrapper/Renderer", C_backend, "vk"),
  O_CHOICEG("Low-latency Vulkan", "Wrapper/VulkanLowLatency", C_bool,
            "false", "Wrapper/Renderer", "=vk"),
  O_CHOICE("Screen layout",     "Wrapper/Layout", C_layout, "horizontal"),
  O_CHOICE("Swap DS screens",   "Wrapper/SwapScreens", C_bool, "false"),
  O_CHOICE("Rotation",          "Wrapper/Rotation", C_rotation, "0"),
  O_RANGE ("Screen gap",        "Wrapper/ScreenGap", 0, 128, 2, "8"),
  O_CHOICE("Integer scaling",   "Wrapper/IntegerScale", C_bool, "false"),
  O_CHOICE("Drastic filter",       "Wrapper/VideoFilter", C_filter, "nearest"),
  O_SHADER("Custom shader...",     "Wrapper/CustomShader", ""),
  O_CHOICE("Show FPS",          "Drastic/ShowFPS", C_bool, "false"),
  O_SUB   ("3D and display options...", SCR_ENHANCE),
};
static const Opt S_enhance[] = {
  O_CHOICE("High-resolution 3D", "Drastic/Hires3D", C_bool, "false"),
  O_CHOICE("Threaded 3D",        "Drastic/Threaded3D", C_bool, "true"),
  O_CHOICE("Disable edge marking","Drastic/DisableEdgeMarking", C_bool, "false"),
  O_CHOICE("16-bit color",       "Drastic/Use16BitColor", C_bool, "false"),
  O_CHOICE("Frame blending",     "Drastic/Blend", C_bool, "false"),
  O_CHOICE("Fix main-engine screen", "Drastic/FixMainEngineScreen", C_bool, "false"),
};
static const Opt S_framegen[] = {
  O_CHOICE("LSFG 2x (Vulkan only)", "Wrapper/LSFGEnabled", C_bool,
           "false"),
  O_CHOICEG("Flow resolution", "Wrapper/LSFGFlowScale", C_lsfgFlow,
            "0.25", "Wrapper/LSFGEnabled", "false"),
  O_CHOICEG("Performance mode", "Wrapper/LSFGPerformance", C_bool,
            "true", "Wrapper/LSFGEnabled", "false"),
  O_STATUS("Lossless.dll"),
};
static const Opt S_audio[] = {
  O_CHOICE("Sound",             "Drastic/SoundEnabled", C_bool, "true"),
  O_RANGE ("Volume",            "Wrapper/Volume", 0, 100, 5, "100"),
  O_CHOICE("Audio latency",     "Drastic/AudioLatency", C_latency, "2"),
  O_CHOICE("Microphone",           "Drastic/MicEnabled", C_bool, "true"),
  O_CHOICEG("Microphone source", "Wrapper/MicrophoneSource", C_micSource,
            "noise", "Drastic/MicEnabled", "false"),
  O_CHOICEG("Microphone level", "Drastic/MicLevel", C_micLevel, "1", "Drastic/MicEnabled", "false"),
};
static const Opt S_emu[] = {
  O_CHOICE("CPU worker threads", "Drastic/CpuThreads", C_threads, "3"),
  O_CHOICE("Preload ROM",        "Drastic/PreloadRoms", C_bool, "true"),
  O_CHOICE("Auto-trim ROM",      "Drastic/AutoTrim", C_bool, "false"),
  O_CHOICE("Ignore card size limit", "Drastic/IgnoreGamecardLimit", C_bool, "false"),
  O_CHOICE("Always sync RTC", "Drastic/RtcSystemTime", C_bool, "true"),
  O_CHOICE("Custom clock", "Drastic/CustomClockEnable", C_bool, "false"),
  O_DATETIMEG("Custom date and time", "Drastic/CustomClock", "0",
              "Drastic/CustomClockEnable", "false"),
  O_CHOICE("Autosave interval",  "Drastic/AutosaveInterval", C_autosave, "300"),
  O_RANGE ("Default state slot", "Wrapper/StateSlot", 0, 9, 1, "0"),
  O_SUB   ("Nintendo DS firmware profile...", SCR_FIRMWARE),
  O_SUB   ("Frame skip and fast-forward...", SCR_FRAMERATE),
};
static const Opt S_gameplay[] = {
  O_CHOICE("Lua scripts",           "Drastic/LuaEnabled", C_bool, "true"),
  O_CHOICE("Slot-2 accessory",      "Drastic/Slot2Type", C_slot2, "1"),
  O_CHOICE("Savestate backup data", "Drastic/BackupInSavestates", C_bool, "true"),
  O_CHOICE("Raw save-file format",  "Drastic/RawSaveFormat", C_bool, "false"),
};
static const Opt S_controller[] = {
  O_CHOICE("Rumble Pak vibration","Wrapper/Vibration", C_bool, "true"),
  O_CHOICE("Gyro & accelerometer","Wrapper/Motion", C_bool, "true"),
  O_CHOICE("DS A",               "Wrapper/Pad/A", C_btn, "A"),
  O_CHOICE("DS B",               "Wrapper/Pad/B", C_btn, "B"),
  O_CHOICE("DS X",               "Wrapper/Pad/X", C_btn, "X"),
  O_CHOICE("DS Y",               "Wrapper/Pad/Y", C_btn, "Y"),
  O_CHOICE("DS L",               "Wrapper/Pad/L", C_btn, "L"),
  O_CHOICE("DS R",               "Wrapper/Pad/R", C_btn, "R"),
  O_CHOICE("DS Start",           "Wrapper/Pad/Start", C_btn, "Plus"),
  O_CHOICE("DS Select",          "Wrapper/Pad/Select", C_btn, "Minus"),
  O_CHOICE("D-Pad Up",           "Wrapper/Pad/Up", C_btn, "Up"),
  O_CHOICE("D-Pad Down",         "Wrapper/Pad/Down", C_btn, "Down"),
  O_CHOICE("D-Pad Left",         "Wrapper/Pad/Left", C_btn, "Left"),
  O_CHOICE("D-Pad Right",        "Wrapper/Pad/Right", C_btn, "Right"),
  O_CHOICE("Analog stick as D-Pad", "Wrapper/AnalogDpad", C_bool, "true"),
  O_RANGEG("Analog deadzone %",   "Wrapper/AnalogDeadzone", 5, 80, 5, "35", "Wrapper/AnalogDpad", "false"),
  O_CHOICE("Virtual stylus",      "Wrapper/StylusMode", C_stylusMode, "stick"),
  O_CHOICEG("Stylus touch button","Wrapper/AnalogTouchButton", C_btn, "StickR", "Wrapper/StylusMode", "off"),
  O_RANGEG("Stick cursor speed",  "Wrapper/AnalogStylusSpeed", 1, 20, 1, "8", "Wrapper/StylusMode", "=stick"),
  O_RANGEG("Motion sensitivity",  "Wrapper/MotionStylusSensitivity", 1, 20, 1, "10", "Wrapper/StylusMode", "=motion"),
  O_HOTKEYG("Recenter motion stylus", "Wrapper/HotkeyMotionStylusRecenter", "L+R+StickR", "Wrapper/StylusMode", "=motion"),
  O_CHOICE("USB mouse stylus",    "Wrapper/MouseStylus", C_bool, "true"),
  O_HOTKEY("In-game menu",        "Wrapper/HotkeyMenu", "L+R+Plus"),
  O_HOTKEY("Fast-forward hotkey", "Wrapper/HotkeyFastForward", "ZR"),
  O_CHOICE("Fast-forward mode",   "Wrapper/FastForwardMode", C_hotkeyMode, "hold"),
  O_HOTKEY("Swap screens hotkey", "Wrapper/HotkeySwapScreens", "ZL"),
  O_HOTKEY("Microphone hotkey",   "Wrapper/HotkeyMicrophone", "StickL"),
  O_HOTKEY("Auto-fire modifier",  "Wrapper/HotkeyAutoFire", "None"),
  O_HOTKEY("Close/open lid",      "Wrapper/HotkeyLid", "None"),
  O_HOTKEY("Save state",          "Wrapper/HotkeySaveState", "L+R+Minus+Y"),
  O_HOTKEY("Load state",          "Wrapper/HotkeyLoadState", "L+R+Minus+X"),
  O_HOTKEY("Next state slot",     "Wrapper/HotkeyNextSlot", "L+R+Minus+Up"),
  O_HOTKEY("Previous state slot", "Wrapper/HotkeyPreviousSlot", "L+R+Minus+Down"),
  O_HOTKEY("Reset game",          "Wrapper/HotkeyReset", "L+R+Minus+A"),
  O_HOTKEY("Quit to launcher",    "Wrapper/HotkeyQuit", "None"),
};
static const Opt S_framerate[] = {
  O_RANGE ("Frames to skip",     "Drastic/FrameskipValue", 0, 9, 1, "0"),
  O_CHOICE("Frame-skip method",  "Drastic/FrameskipType", C_frameskipType, "0"),
  O_CHOICE("Safe frame skipping","Drastic/FrameskipSafe", C_bool, "false"),
  O_CHOICE("Fast-forward speed", "Drastic/FastForwardSpeed", C_ffSpeed, "2"),
  O_CHOICE("Auto-fire speed",    "Drastic/AutoFireSpeed", C_autofire, "2"),
};
static const Opt S_firmware[] = {
  O_TEXT  ("Nickname",           "Drastic/FirmwareNickname", "Switch"),
  O_CHOICE("Language",           "Drastic/FirmwareLanguage", C_firmwareLanguage, "-1"),
  O_CHOICE("Favorite color",     "Drastic/FirmwareColor", C_firmwareColor, "0"),
  O_RANGE ("Birthday month",     "Drastic/FirmwareBirthdayMonth", 1, 12, 1, "6"),
  O_RANGE ("Birthday day",       "Drastic/FirmwareBirthdayDay", 1, 31, 1, "6"),
};
static const Opt S_launcher[] = {
  O_CHOICE("Language",          "Wrapper/Language",       C_uiLanguage,    "system"),
  O_CHOICE("Theme",             "Wrapper/Theme",          C_launcherTheme, "animated"),
  O_CHOICE("Launcher rotation", "Wrapper/LauncherRotation", C_rotation,       "0"),
  O_CHOICE("Games per row",     "Wrapper/GridColumns",    C_gridColumns,   "6"),
  O_CHOICE("Rows per page",     "Wrapper/GridRows",       C_gridRows,      "2"),
  O_CHOICE("Show game titles",  "Wrapper/ShowGameTitles", C_bool,          "true"),
  O_CHOICE("Show region flags", "Wrapper/ShowRegionFlags", C_bool,         "true"),
  O_CHOICE("Show custom settings badges", "Wrapper/ShowCustomSettingsBadges", C_bool, "true"),
  O_CHOICE("UI animations",     "Wrapper/UiAnimations",   C_bool,          "true"),
  O_CHOICE("Sound effects",     "Wrapper/UiSounds",       C_bool,          "true"),
  O_CHOICE("Check updates at boot", "Wrapper/CheckUpdatesAtBoot", C_bool,   "true"),
};
struct Screen { const char *title; const Opt *opts; int n; bool binds; };
static const Screen g_screens[SCR_COUNT] = {
  { "Graphics",            S_graphics,   (int)(sizeof(S_graphics)/sizeof(Opt)),   false },
  { "3D / Compatibility",  S_enhance,    (int)(sizeof(S_enhance)/sizeof(Opt)),    false },
  { "Frame Generation",    S_framegen,   (int)(sizeof(S_framegen)/sizeof(Opt)),   false },
  { "Audio",               S_audio,      (int)(sizeof(S_audio)/sizeof(Opt)),      false },
  { "Emulation / System",  S_emu,        (int)(sizeof(S_emu)/sizeof(Opt)),        false },
  { "Frame Rate Control",  S_framerate,  (int)(sizeof(S_framerate)/sizeof(Opt)),  false },
  { "Gameplay / Features", S_gameplay,   (int)(sizeof(S_gameplay)/sizeof(Opt)),   false },
  { "Controller",          S_controller, (int)(sizeof(S_controller)/sizeof(Opt)), true  },
  { "Nintendo DS Firmware",S_firmware,   (int)(sizeof(S_firmware)/sizeof(Opt)),   false },
};

struct SettingHelpEntry {
  const char *key;
  const char *kind;
  const char *text;
};

/* DraStic-owned descriptions are condensed from the manual bundled with the
 * Android application. Wrapper-only settings are documented here alongside
 * them so every launcher option has one contextual source of truth. */
static const SettingHelpEntry SETTING_HELP[] = {
  {"Wrapper/Renderer", "Display backend",
   "Chooses the Switch presentation backend. Vulkan (NVK) is recommended for performance and is required by LSFG. OpenGL uses native NVC0, while Zink runs OpenGL on NVK for an additional compatibility path."},
  {"Wrapper/VulkanLowLatency", "Latency / performance",
   "Reduces queued Vulkan presentation work so new controller input reaches the display sooner. It can reduce performance headroom, so it is disabled by default."},
  {"Wrapper/Layout", "Screen layout",
   "Arranges the two Nintendo DS screens. Hybrid modes enlarge one screen, single-screen modes hide the other, and Custom uses positions saved by the in-game layout editor."},
  {"Wrapper/SwapScreens", "Screen layout",
   "Exchanges the displayed positions of the top and touch screens. Touch input continues to follow the emulated touch screen."},
  {"Wrapper/Rotation", "Screen layout",
   "Rotates gameplay and the in-game interface for book or tate play. Menu navigation directions remain tied to the physical controller."},
  {"Wrapper/ScreenGap", "Screen layout",
   "Adds space between the two emulated screens before the layout is fitted to the display. Larger gaps leave less room for the game image."},
  {"Wrapper/IntegerScale", "Image scaling",
   "Uses whole-number scaling for sharper, evenly sized pixels. Depending on the layout, this can leave unused borders and prevent the image from filling the display."},
  {"Wrapper/VideoFilter", "Post-processing",
   "Applies a GPU scaling filter after DraStic renders the screens. Filters may sharpen, smooth, or stylize the image; Custom uses the shader selected below."},
  {"Wrapper/CustomShader", "Post-processing",
   "Selects a DraStic .dfx custom shader. Choosing one automatically changes DraStic filter to Custom. Vulkan shaders also need their compiled .dfx.nxvk pack."},
  {"Drastic/ShowFPS", "Performance display",
   "Shows DraStic's on-screen speed and frame-rate indicator while a game is running."},

  {"Drastic/Hires3D", "Visual enhancement",
   "Renders Nintendo DS 3D graphics at twice their normal resolution. This improves 3D clarity but substantially increases emulation work and does not upscale 2D layers."},
  {"Drastic/Threaded3D", "Performance / compatibility",
   "Offloads 3D screen-update work to another CPU thread. It can improve performance, but DraStic warns that some games may show graphical glitches, swapped screens, or instability; disable it when troubleshooting."},
  {"Drastic/DisableEdgeMarking", "Compatibility / visual trade-off",
   "Disables the Nintendo DS edge-marking effect used to draw outlines around some 3D polygons. This can avoid edge-related glitches, but removes an intended visual effect."},
  {"Drastic/Use16BitColor", "Performance / visual trade-off",
   "Uses a lower-precision 16-bit texture format. It may improve performance or reduce memory use, but lowers image quality and can introduce color banding."},
  {"Drastic/Blend", "Compatibility / visual trade-off",
   "Blends consecutive emulated frames. This can reproduce effects that rely on rapid flicker, but may soften motion or create visible ghosting."},
  {"Drastic/FixMainEngineScreen", "Game-specific workaround",
   "Forces the main 2D+3D display engine onto the emulated top screen instead of honoring a game's engine swaps. It helps some games such as the Sonic titles, but can put content on the wrong screen in others."},

  {"Wrapper/LSFGEnabled", "Frame generation",
   "Generates one intermediate frame for each real frame to target a smoother 2x presentation. It does not increase emulation speed and may add artifacts or latency. Vulkan only."},
  {"Wrapper/LSFGFlowScale", "Frame generation quality",
   "Sets the resolution used for optical-flow analysis. Half can improve motion detail but costs more GPU time and memory; Quarter is recommended on Switch."},
  {"Wrapper/LSFGPerformance", "Frame generation performance",
   "Uses LSFG's lighter performance-oriented processing path. Disable it only when you prefer image quality and have enough GPU headroom."},

  {"Drastic/SoundEnabled", "Audio",
   "Enables or mutes emulated Nintendo DS sound."},
  {"Wrapper/Volume", "Audio",
   "Sets the final Switch output volume without changing the emulated game's own mixer settings."},
  {"Drastic/AudioLatency", "Audio latency / stability",
   "Controls DraStic's audio buffering. Lower settings react sooner but are more likely to crackle or stutter; higher settings are safer when a game cannot maintain steady speed."},
  {"Drastic/MicEnabled", "Nintendo DS microphone",
   "Enables the emulated DS microphone input path. Its source can be simulated noise or a compatible microphone connected to the Switch."},
  {"Wrapper/MicrophoneSource", "Nintendo DS microphone",
   "Simulated noise uses the configured microphone hotkey for games that expect blowing. External microphone captures real audio from a CTIA headset microphone or compatible USB input; Bluetooth microphone input is not supported by Switch."},
  {"Drastic/MicLevel", "Nintendo DS microphone",
   "Sets the microphone strength supplied to the game for both simulated and external input."},
  {"Wrapper/HotkeyMicrophone", "Nintendo DS microphone",
   "Feeds simulated white noise while held. This binding is intentionally inactive when External microphone is selected, because DraStic then receives the captured waveform directly."},

  {"Drastic/CpuThreads", "CPU performance",
   "Sets the number of host worker threads DraStic may use. Switch applications have three usable CPU cores, so 3 is recommended; reduce it only for troubleshooting."},
  {"Drastic/PreloadRoms", "Loading / memory",
   "Loads an uncompressed ROM into memory before play for fast, consistent access. Disabling it lowers memory use but can increase storage access during emulation."},
  {"Drastic/AutoTrim", "Compatibility / memory",
   "Uses only the ROM length declared in its Nintendo DS header. This can reduce memory use for oversized dumps, but should stay off for patched or malformed ROMs unless needed."},
  {"Drastic/IgnoreGamecardLimit", "Game-specific workaround",
   "Ignores the normal Nintendo DS cartridge-size limit. Enable it only when an oversized patched ROM refuses to load."},
  {"Drastic/RtcSystemTime", "Nintendo DS clock",
   "Continuously synchronizes the Nintendo DS clock to the Switch clock. Enabling a custom clock turns this off so time follows emulation speed and savestates instead."},
  {"Drastic/CustomClockEnable", "Nintendo DS clock",
   "Starts the game from the custom date and time below. From then on the clock follows emulation speed and savestates. It takes effect after restarting the game."},
  {"Drastic/CustomClock", "Nintendo DS clock",
   "Sets the local date and time supplied to DraStic when the game starts. This is useful for time-based events in games such as Pokemon and Animal Crossing."},
  {"Drastic/AutosaveInterval", "Save protection",
   "Controls how often DraStic commits automatic in-game save data while running. Shorter intervals reduce the amount of progress at risk after a crash, but write to storage more often."},
  {"Wrapper/StateSlot", "Savestates",
   "Selects the savestate slot used by the quick save and quick load hotkeys. Slots range from 0 to 9."},

  {"Drastic/LuaEnabled", "Gameplay feature",
   "Allows DraStic Lua scripts to run for supported games. Disable it if you do not use scripts or when troubleshooting script-related behavior."},
  {"Drastic/Slot2Type", "Nintendo DS accessory",
   "Chooses the accessory emulated in the Nintendo DS Slot-2 port. GBA Cart loads matching .gba/.sav files for supported game bonuses; the other modes emulate SRAM, rumble, or motion accessories."},
  {"Drastic/BackupInSavestates", "Save compatibility",
   "Stores a copy of the in-game save inside each savestate so both remain synchronized. DraStic recommends keeping this enabled because mismatched saves can cause corruption, especially in Pokemon games."},
  {"Drastic/RawSaveFormat", "Save-file compatibility",
   "Writes in-game backup memory as a raw save file for easier interchange with other emulators and tools. Leave it off when you need DraStic's normal save format."},

  {"Wrapper/Vibration", "Slot-2 Rumble Pak",
   "Forwards an emulated Slot-2 Rumble Pak effect to Switch controller vibration. It only has an effect when a game and the selected Slot-2 accessory use rumble."},
  {"Wrapper/Motion", "Motion input",
   "Forwards Joy-Con gyro and accelerometer data to DraStic's motion input. It is mainly useful with a compatible Slot-2 motion accessory or motion-aware homebrew."},
  {"Wrapper/AnalogDpad", "Controller input",
   "Lets the left analog stick act as the Nintendo DS D-Pad in addition to the physical D-Pad."},
  {"Wrapper/AnalogDeadzone", "Controller input",
   "Sets how far the left stick must move before it presses a DS direction. Raise it to prevent drift; lower it for faster response."},
  {"Wrapper/StylusMode", "Touch input",
   "Off disables the controller cursor. Right stick moves a relative cursor, while Motion controls map the calibrated controller angle to an absolute point on the DS touch screen."},
  {"Wrapper/AnalogTouchButton", "Touch input",
   "Chooses the controller button that presses the virtual stylus at its current stick or motion-controlled cursor position."},
  {"Wrapper/AnalogStylusSpeed", "Touch input",
   "Controls how quickly the right-stick stylus cursor moves across the DS touch screen."},
  {"Wrapper/MotionStylusSensitivity", "Touch input",
   "Controls how far the motion stylus travels for a given controller tilt. Recenter after changing how you hold the Switch, Joy-Con, or Pro Controller."},
  {"Wrapper/HotkeyMotionStylusRecenter", "Touch input",
   "Calibrates the current controller angle as the center of the DS touch screen. It is consumed by the wrapper while motion stylus mode is active."},
  {"Wrapper/MouseStylus", "Touch input",
   "Lets a connected USB mouse point at the displayed DS touch screen. The left mouse button presses the stylus, and screen rotation is applied automatically."},
  {"Wrapper/FastForwardMode", "Hotkey behavior",
   "Hold runs fast-forward only while its hotkey is held. Toggle keeps fast-forward active until the hotkey is pressed again."},

  {"Drastic/FrameskipValue", "Performance / visual trade-off",
   "Sets how many frames the selected frame-skip method may omit. Zero disables skipping. Higher values can improve speed but make motion choppy and may visually break some games."},
  {"Drastic/FrameskipType", "Performance / visual trade-off",
   "Chooses when frames are omitted. Automatic skips when emulation falls behind; Fixed uses the configured ratio, while Aggressive and Maximum favor speed more strongly over smoothness and compatibility."},
  {"Drastic/FrameskipSafe", "Compatibility / performance",
   "Uses DraStic's more conservative frame-skipping behavior to reduce visual problems in sensitive games. It may provide less speedup than normal skipping."},
  {"Drastic/FastForwardSpeed", "Speed control",
   "Caps the speed used while fast-forward is active. 50% acts as slow motion; Unlimited runs only as fast as the Switch and current game allow."},
  {"Drastic/AutoFireSpeed", "Controller input",
   "Sets how rapidly supported DS buttons repeat while the auto-fire modifier hotkey is active."},

  {"Drastic/FirmwareNickname", "Firmware profile",
   "Sets the nickname stored in the emulated Nintendo DS firmware profile. Games may display this name."},
  {"Drastic/FirmwareLanguage", "Firmware profile",
   "Sets the language reported by the emulated DS firmware. Auto follows the Switch system language and falls back to English when that language is not supported by Nintendo DS."},
  {"Drastic/FirmwareColor", "Firmware profile",
   "Sets the favorite color stored in the emulated Nintendo DS user profile using the original 16-color Nintendo DS palette. Some games use it for personalization."},
  {"Drastic/FirmwareBirthdayMonth", "Firmware profile",
   "Sets the birthday month stored in the emulated Nintendo DS user profile."},
  {"Drastic/FirmwareBirthdayDay", "Firmware profile",
   "Sets the birthday day stored in the emulated Nintendo DS user profile."},

  {"Wrapper/Theme", "Launcher appearance",
   "Changes the launcher background and visual theme. It does not affect gameplay rendering."},
  {"Wrapper/LauncherRotation", "Launcher orientation",
   "Rotates the complete SDL launcher by 0, 90, 180, or 270 degrees. The 90 and 270 degree modes use the portrait layout. Touch follows the displayed interface, while D-Pad and stick navigation keep their normal physical directions."},
  {"Wrapper/GridColumns", "Library layout",
   "Sets how many game covers are displayed across each library row. More columns make each cover smaller."},
  {"Wrapper/GridRows", "Library layout",
   "Sets how many rows of game covers are shown on each library page. More rows make each cover smaller."},
  {"Wrapper/ShowGameTitles", "Library layout",
   "Shows or hides game names below their covers in the library."},
  {"Wrapper/ShowRegionFlags", "Library layout",
   "Shows or hides the region flag in the top-left corner of each game cover."},
  {"Wrapper/ShowCustomSettingsBadges", "Library layout",
   "Shows or hides the square badge on games that have per-game settings. The settings themselves are not changed."},
  {"Wrapper/UiAnimations", "Launcher appearance",
   "Enables launcher transitions, moving highlights, and animated theme effects."},
  {"Wrapper/UiSounds", "Launcher audio",
   "Enables navigation, confirmation, and back sound effects in the SDL launcher."},
  {"Wrapper/CheckUpdatesAtBoot", "Launcher updates",
   "Checks GitHub for a newer DrasticDS_nx release when the launcher starts. The check is skipped for HOME Menu game shortcuts."},
  {"Wrapper/Pad/A", "Controller mapping", "Maps the Nintendo DS A button to a Switch controller button."},
  {"Wrapper/Pad/B", "Controller mapping", "Maps the Nintendo DS B button to a Switch controller button."},
  {"Wrapper/Pad/X", "Controller mapping", "Maps the Nintendo DS X button to a Switch controller button."},
  {"Wrapper/Pad/Y", "Controller mapping", "Maps the Nintendo DS Y button to a Switch controller button."},
  {"Wrapper/Pad/L", "Controller mapping", "Maps the Nintendo DS L shoulder button to a Switch controller button."},
  {"Wrapper/Pad/R", "Controller mapping", "Maps the Nintendo DS R shoulder button to a Switch controller button."},
  {"Wrapper/Pad/Start", "Controller mapping", "Maps the Nintendo DS Start button to a Switch controller button."},
  {"Wrapper/Pad/Select", "Controller mapping", "Maps the Nintendo DS Select button to a Switch controller button."},
  {"Wrapper/Pad/Up", "Controller mapping", "Maps Nintendo DS D-Pad Up to a Switch controller button."},
  {"Wrapper/Pad/Down", "Controller mapping", "Maps Nintendo DS D-Pad Down to a Switch controller button."},
  {"Wrapper/Pad/Left", "Controller mapping", "Maps Nintendo DS D-Pad Left to a Switch controller button."},
  {"Wrapper/Pad/Right", "Controller mapping", "Maps Nintendo DS D-Pad Right to a Switch controller button."},
  {"Wrapper/HotkeyMenu", "Hotkey binding", "Opens DraStic's in-game menu with the selected Switch button combination."},
  {"Wrapper/HotkeyFastForward", "Hotkey binding", "Holds or toggles fast-forward using the behavior selected by Fast-forward mode."},
  {"Wrapper/HotkeySwapScreens", "Hotkey binding", "Swaps the Nintendo DS top and bottom screens during gameplay."},
  {"Wrapper/HotkeyAutoFire", "Hotkey binding", "Uses the selected button combination as the auto-fire modifier."},
  {"Wrapper/HotkeyLid", "Hotkey binding", "Closes or opens the emulated Nintendo DS lid."},
  {"Wrapper/HotkeySaveState", "Hotkey binding", "Saves a state to the currently selected savestate slot."},
  {"Wrapper/HotkeyLoadState", "Hotkey binding", "Loads a state from the currently selected savestate slot."},
  {"Wrapper/HotkeyNextSlot", "Hotkey binding", "Selects the next savestate slot."},
  {"Wrapper/HotkeyPreviousSlot", "Hotkey binding", "Selects the previous savestate slot."},
  {"Wrapper/HotkeyReset", "Hotkey binding", "Resets the running Nintendo DS game."},
  {"Wrapper/HotkeyQuit", "Hotkey binding", "Stops emulation and returns to the DraStic launcher."},
  {"Wrapper/Language", "Launcher language", "Chooses the language used by the SDL launcher. System follows the Switch console language."},
};

struct SettingHelpInfo {
  const char *kind;
  std::string text;
};

static SettingHelpInfo settingHelpFor(const Opt &option) {
  if(option.key){
    for(const SettingHelpEntry &entry:SETTING_HELP)
      if(!strcmp(entry.key,option.key)) return {entry.kind,entry.text};
  }
  if(option.type==OT_SUBMENU){
    if(option.sub==SCR_ENHANCE)
      return {"Settings group","Contains DraStic 3D enhancements, performance trade-offs, and game-specific visual compatibility workarounds."};
    if(option.sub==SCR_FIRMWARE)
      return {"Settings group","Edits the nickname, language, favorite color, and birthday reported by the emulated Nintendo DS firmware."};
    if(option.sub==SCR_FRAMERATE)
      return {"Settings group","Contains frame skipping, fast-forward speed, and auto-fire timing. Frame skipping trades visual accuracy and smoothness for performance."};
  }
  if(option.type==OT_STATUS)
    return {"Required component","Shows whether the Lossless Scaling frame-generation library is installed. LSFG settings remain unavailable when this component is missing."};
  return {"Setting help unavailable","This setting is missing its required exact help entry."};
}

static void commitAll() {
  for (int s = 0; s < SCR_COUNT; s++)
    for (int i = 0; i < g_screens[s].n; i++) {
      const Opt &o = g_screens[s].opts[i];
      if (o.key && (o.type == OT_CHOICE || o.type == OT_RANGE || o.type == OT_SCALED_RANGE ||
                    o.type == OT_TEXT || o.type == OT_HOTKEY ||
                    o.type == OT_SHADER || o.type == OT_DATETIME)) {
        std::string v = iniGet(o.key, o.def);
        iniSet(o.key, v.c_str());
      }
    }
}

static SDL_Window   *g_win = nullptr;
static SDL_Renderer *g_ren = nullptr;
static TTF_Font     *g_font = nullptr, *g_font_sm = nullptr, *g_font_big = nullptr;
static SDL_Texture  *g_logo = nullptr;
static int SW = 1280, SH = 720;
static int g_outputW = 1280, g_outputH = 720;
static int g_launcherRotation = 0;
static bool g_launcherPortrait = false;
static SDL_Texture *g_uiTarget = nullptr;
static bool g_romfsReady = false;
static bool g_sdlReady = false;
static bool g_ttfReady = false;
static bool g_imgReady = false;
static bool g_plReady = false;
static bool g_griddbReady = false;
static bool g_storageSocketReady = false;
static std::string g_launcherNroPath;
static std::string g_updateNoticeTag;
static std::string g_updateNotifiedTag;
static Uint32 g_updateNoticeUntil = 0;
static Uint32 g_fxT = 0;
static std::string g_toastMessage;
static Uint32 g_toastUntil = 0;
static void drawPendingToast();

static bool configureLauncherOrientation(int rotation) {
  if(!g_ren || g_outputW<1 || g_outputH<1) return false;
  if(rotation<0||rotation>3) rotation=0;
  if(rotation==0){
    SDL_SetRenderTarget(g_ren,nullptr);
    if(g_uiTarget) SDL_DestroyTexture(g_uiTarget);
    g_uiTarget=nullptr;
    g_launcherRotation=0;
    g_launcherPortrait=false;
    SW=g_outputW;
    SH=g_outputH;
    SDL_RenderSetViewport(g_ren,nullptr);
    SDL_RenderSetScale(g_ren,1.0f,1.0f);
    return true;
  }
  const bool portrait=(rotation&1)!=0;
  const int logicalWidth=portrait?g_outputH:g_outputW;
  const int logicalHeight=portrait?g_outputW:g_outputH;
  if(g_uiTarget && SW==logicalWidth && SH==logicalHeight) {
    g_launcherRotation=rotation;
    g_launcherPortrait=portrait;
    SDL_SetRenderTarget(g_ren,g_uiTarget);
    return true;
  }

  SDL_Texture *previous=g_uiTarget;
  SDL_SetRenderTarget(g_ren,nullptr);
  SDL_Texture *target=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET,
                                        logicalWidth,logicalHeight);
  if(!target){
    if(previous) SDL_SetRenderTarget(g_ren,previous);
    return false;
  }
  SDL_SetTextureBlendMode(target,SDL_BLENDMODE_NONE);
  if(SDL_SetRenderTarget(g_ren,target)!=0){
    SDL_DestroyTexture(target);
    if(previous) SDL_SetRenderTarget(g_ren,previous);
    return false;
  }
  g_uiTarget=target;
  g_launcherRotation=rotation;
  g_launcherPortrait=portrait;
  SW=logicalWidth;
  SH=logicalHeight;
  if(previous) SDL_DestroyTexture(previous);
  SDL_RenderSetViewport(g_ren,nullptr);
  SDL_RenderSetScale(g_ren,1.0f,1.0f);
  return true;
}

static void presentUi() {
  drawPendingToast();
  if(!g_uiTarget){
    SDL_RenderPresent(g_ren);
    return;
  }
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderTarget(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,0,0,0,255);
  SDL_RenderClear(g_ren);
  /* Odd quarter-turns use a portrait render target. RenderCopyEx rotates its
     destination around the centre, so its pre-rotation rectangle swaps the
     physical dimensions and is centred beyond the output bounds. */
  SDL_Rect destination = (g_launcherRotation&1)
      ? SDL_Rect{(g_outputW-g_outputH)/2,(g_outputH-g_outputW)/2,
                 g_outputH,g_outputW}
      : SDL_Rect{0,0,g_outputW,g_outputH};
  SDL_RenderCopyEx(g_ren,g_uiTarget,nullptr,&destination,
                   g_launcherRotation*90.0,nullptr,
                   SDL_FLIP_NONE);
  SDL_RenderPresent(g_ren);
  SDL_SetRenderTarget(g_ren,g_uiTarget);
}

enum class LauncherTheme { Xmb, Glow, Bubbles, Classic, Oled };
static LauncherTheme g_launcherTheme = LauncherTheme::Glow;
static bool g_uiAnimations = true;
static bool g_showGameTitles = true;
static bool g_showRegionFlags = true;
static bool g_showCustomSettingsBadges = true;
static int g_gridColumns = 6;
static int g_gridRows = 2;
static SDL_Texture *g_glowTexture = nullptr;

static SDL_Color COL_BG    = { 8, 12, 24, 255 };
static SDL_Color COL_TXT   = { 235, 239, 247, 255 };
static SDL_Color COL_DIM   = { 151, 163, 184, 255 };
static SDL_Color COL_HI    = { 100, 211, 255, 255 };
static SDL_Color COL_VAL   = { 255, 215, 120, 255 };
static SDL_Color COL_SEL   = { 116, 200, 255, 255 };
static SDL_Color COL_PANEL = { 16, 23, 39, 184 };
static SDL_Color COL_CARD  = { 22, 30, 49, 214 };
static SDL_Color COL_FOCUS = { 28, 69, 92, 210 };

static void fillRect(int x,int y,int w,int h, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); SDL_Rect r={x,y,w,h}; SDL_RenderFillRect(g_ren,&r); }
static void border(int x,int y,int w,int h,int t, SDL_Color c){ SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a); for(int i=0;i<t;i++){ SDL_Rect r={x-i,y-i,w+2*i,h+2*i}; SDL_RenderDrawRect(g_ren,&r); } }

struct TextKey {
  TTF_Font *font;
  Uint32 color;
  std::string text;
  bool operator==(const TextKey &other) const {
    return font == other.font && color == other.color && text == other.text;
  }
};

struct TextKeyHash {
  size_t operator()(const TextKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<Uint32>{}(key.color) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct TextEntry {
  SDL_Texture *texture;
  int width;
  int height;
  size_t bytes;
  Uint64 use;
};

struct MetricKey {
  TTF_Font *font;
  std::string text;
  bool operator==(const MetricKey &other) const { return font == other.font && text == other.text; }
};

struct MetricKeyHash {
  size_t operator()(const MetricKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    return hash ^ (std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2));
  }
};

struct MetricEntry { int width; Uint64 use; };

struct EllipsisKey {
  TTF_Font *font;
  int maxWidth;
  std::string text;
  bool operator==(const EllipsisKey &other) const {
    return font == other.font && maxWidth == other.maxWidth && text == other.text;
  }
};

struct EllipsisKeyHash {
  size_t operator()(const EllipsisKey &key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    hash ^= std::hash<TTF_Font *>{}(key.font) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= std::hash<int>{}(key.maxWidth) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct EllipsisEntry { std::string text; Uint64 use; };

static std::unordered_map<TextKey, TextEntry, TextKeyHash> g_textCache;
static std::unordered_map<MetricKey, MetricEntry, MetricKeyHash> g_metricCache;
static std::unordered_map<EllipsisKey, EllipsisEntry, EllipsisKeyHash> g_ellipsisCache;
static size_t g_textCacheBytes = 0;
static Uint64 g_textUseSerial = 0;
static constexpr size_t TEXT_CACHE_LIMIT = 512;
static constexpr size_t TEXT_CACHE_BYTES = 12 * 1024 * 1024;
static constexpr size_t METRIC_CACHE_LIMIT = 2048;
static constexpr size_t ELLIPSIS_CACHE_LIMIT = 512;

static Uint32 packColor(SDL_Color color) {
  return (Uint32)color.r | ((Uint32)color.g << 8) | ((Uint32)color.b << 16) | ((Uint32)color.a << 24);
}

static void rememberTextMetric(TTF_Font *font, const std::string &text, int width) {
  MetricKey key{font, text};
  auto found = g_metricCache.find(key);
  if (found != g_metricCache.end()) {
    found->second.width = width;
    found->second.use = ++g_textUseSerial;
    return;
  }
  if (g_metricCache.size() >= METRIC_CACHE_LIMIT) {
    auto victim = g_metricCache.begin();
    for (auto it = std::next(g_metricCache.begin()); it != g_metricCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    g_metricCache.erase(victim);
  }
  g_metricCache.emplace(std::move(key), MetricEntry{width, ++g_textUseSerial});
}

static void evictTextEntries(size_t incomingBytes) {
  while (!g_textCache.empty() &&
         (g_textCache.size() >= TEXT_CACHE_LIMIT || g_textCacheBytes > TEXT_CACHE_BYTES - incomingBytes)) {
    auto victim = g_textCache.begin();
    for (auto it = std::next(g_textCache.begin()); it != g_textCache.end(); ++it)
      if (it->second.use < victim->second.use) victim = it;
    SDL_DestroyTexture(victim->second.texture);
    g_textCacheBytes -= victim->second.bytes;
    g_textCache.erase(victim);
  }
}

static void clearTextCaches() {
  for (auto &entry : g_textCache) SDL_DestroyTexture(entry.second.texture);
  g_textCache.clear();
  g_metricCache.clear();
  g_ellipsisCache.clear();
  g_textCacheBytes = 0;
  g_textUseSerial = 0;
}

static void applyLauncherAppearance() {
  LauncherTheme previous = g_launcherTheme;
  const char *theme = storeGet(g_global, "Wrapper/Theme", "animated");
  g_launcherTheme = !strcmp(theme, "classic") ? LauncherTheme::Classic :
                    !strcmp(theme, "oled") ? LauncherTheme::Oled :
                    !strcmp(theme, "homebrew") ? LauncherTheme::Bubbles :
                    !strcmp(theme, "xmb") ? LauncherTheme::Xmb : LauncherTheme::Glow;
  g_uiAnimations = strcmp(storeGet(g_global, "Wrapper/UiAnimations", "true"), "false") != 0;
  g_showGameTitles = strcmp(storeGet(g_global, "Wrapper/ShowGameTitles", "true"), "false") != 0;
  g_showRegionFlags = strcmp(storeGet(g_global, "Wrapper/ShowRegionFlags", "true"), "false") != 0;
  g_showCustomSettingsBadges =
      strcmp(storeGet(g_global, "Wrapper/ShowCustomSettingsBadges", "true"), "false") != 0;
  g_gridColumns = std::max(3, std::min(8, atoi(storeGet(g_global, "Wrapper/GridColumns", "6"))));
  g_gridRows = std::max(1, std::min(3, atoi(storeGet(g_global, "Wrapper/GridRows", "2"))));

  if (g_launcherTheme == LauncherTheme::Xmb) {
    COL_BG={2,35,92,255}; COL_TXT={246,250,255,255}; COL_DIM={176,207,233,255};
    COL_HI={151,229,255,255}; COL_VAL={255,255,255,255}; COL_SEL={116,218,255,255};
    COL_PANEL={4,28,73,164}; COL_CARD={5,36,86,196}; COL_FOCUS={20,91,148,214};
  } else if (g_launcherTheme == LauncherTheme::Classic) {
    COL_BG={22,24,30,255}; COL_TXT={228,230,235,255}; COL_DIM={150,155,165,255};
    COL_HI={96,200,255,255}; COL_VAL={255,210,100,255}; COL_SEL={255,170,0,255};
    COL_PANEL={28,31,40,255}; COL_CARD={24,26,34,255}; COL_FOCUS={66,56,30,235};
  } else if (g_launcherTheme == LauncherTheme::Oled) {
    COL_BG={0,0,0,255}; COL_TXT={245,247,249,255}; COL_DIM={145,151,158,255};
    COL_HI={105,220,255,255}; COL_VAL={255,255,255,255}; COL_SEL={0,210,190,255};
    COL_PANEL={4,4,5,248}; COL_CARD={8,8,10,250}; COL_FOCUS={0,58,53,245};
  } else if (g_launcherTheme == LauncherTheme::Bubbles) {
    COL_BG={0,8,16,255}; COL_TXT={235,248,255,255}; COL_DIM={143,192,216,255};
    COL_HI={118,222,255,255}; COL_VAL={194,239,255,255}; COL_SEL={61,183,235,255};
    COL_PANEL={4,31,50,190}; COL_CARD={5,35,56,218}; COL_FOCUS={12,76,108,220};
  } else {
    COL_BG={8,12,24,255}; COL_TXT={235,239,247,255}; COL_DIM={151,163,184,255};
    COL_HI={100,211,255,255}; COL_VAL={255,215,120,255}; COL_SEL={116,200,255,255};
    COL_PANEL={16,23,39,184}; COL_CARD={22,30,49,214}; COL_FOCUS={28,69,92,208};
  }
  if (previous != g_launcherTheme && g_ren)
    clearTextCaches();
}

static void ensureGlowTexture() {
  if (g_glowTexture || !g_ren) return;
  constexpr int size=256;
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,size,size,32,SDL_PIXELFORMAT_RGBA32);
  if(!surface) return;
  if(SDL_LockSurface(surface)==0){
    for(int y=0;y<size;y++){
      auto *row=(Uint32*)((Uint8*)surface->pixels+y*surface->pitch);
      for(int x=0;x<size;x++){
        float dx=(x-(size-1)*0.5f)/(size*0.5f),dy=(y-(size-1)*0.5f)/(size*0.5f);
        float distance=sqrtf(dx*dx+dy*dy);
        float strength=distance>=1.f?0.f:1.f-distance;
        Uint8 alpha=(Uint8)(255.f*strength*strength);
        row[x]=SDL_MapRGBA(surface->format,255,255,255,alpha);
      }
    }
    SDL_UnlockSurface(surface);
    g_glowTexture=SDL_CreateTextureFromSurface(g_ren,surface);
    if(g_glowTexture) SDL_SetTextureBlendMode(g_glowTexture,SDL_BLENDMODE_BLEND);
  }
  SDL_FreeSurface(surface);
}

static bool hasAnimatedBackground() {
  return g_launcherTheme==LauncherTheme::Xmb||g_launcherTheme==LauncherTheme::Bubbles||g_launcherTheme==LauncherTheme::Glow;
}

static void drawGlow(float x,float y,float radius,Uint8 red,Uint8 green,Uint8 blue,Uint8 alpha) {
  int diameter=(int)(SH*radius);
  SDL_Rect destination={(int)(SW*x)-diameter/2,(int)(SH*y)-diameter/2,diameter,diameter};
  SDL_SetTextureColorMod(g_glowTexture,red,green,blue);
  SDL_SetTextureAlphaMod(g_glowTexture,alpha);
  SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&destination);
}

static void drawBackgroundParticles(float time,SDL_Color color,int count,float speed) {
  for(int i=0;i<count;i++){
    float travel=fmodf(i*0.371f+time*speed*(0.65f+(i%5)*0.11f),1.12f)-0.06f;
    float y=fmodf(i*0.217f+0.11f*sinf(time*0.29f+i*1.73f),1.f);
    float pulse=0.45f+0.55f*sinf(time*(0.9f+(i%4)*0.17f)+i);
    Uint8 alpha=(Uint8)(color.a*(0.55f+0.45f*pulse));
    int size=(i%9==0)?3:2;
    fillRect((int)(travel*SW),(int)(y*SH),size,size,(SDL_Color){color.r,color.g,color.b,alpha});
  }
}

static Uint8 blendChannel(Uint8 first,Uint8 second,float amount) {
  return (Uint8)(first+(second-first)*std::clamp(amount,0.f,1.f));
}

static float xmbWaveY(float x,float time,float center,float amplitude,float frequency,float slope,float phase) {
  const float primary=sinf(x*6.2831853f*frequency+phase+time*0.115f);
  const float detail=sinf(x*6.2831853f*(frequency*2.07f)+phase*0.61f-time*0.072f);
  return center+slope*(x-0.5f)+amplitude*(primary+detail*0.24f);
}

static void drawXmbRibbon(float time,float center,float amplitude,float frequency,float slope,float phase,
                          int halfWidth,SDL_Color color) {
  constexpr int pointCount=121;
  std::array<SDL_Point,pointCount> points{};
  for(int offset=-halfWidth;offset<=halfWidth;offset++){
    float distance=halfWidth?fabsf((float)offset/halfWidth):0.f;
    Uint8 alpha=(Uint8)(color.a*powf(std::max(0.f,1.f-distance),1.45f));
    if(alpha<2) continue;
    for(int point=0;point<pointCount;point++){
      float x=(float)point/(pointCount-1);
      points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)+offset};
    }
    SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,alpha);
    SDL_RenderDrawLines(g_ren,points.data(),pointCount);
  }
}

static void drawXmbFilament(float time,float center,float amplitude,float frequency,float slope,float phase,
                            SDL_Color color) {
  constexpr int pointCount=161;
  std::array<SDL_Point,pointCount> points{};
  for(int point=0;point<pointCount;point++){
    float x=(float)point/(pointCount-1);
    points[point]={(int)(x*SW),(int)(xmbWaveY(x,time,center,amplitude,frequency,slope,phase)*SH)};
  }
  SDL_SetRenderDrawColor(g_ren,color.r,color.g,color.b,color.a);
  SDL_RenderDrawLines(g_ren,points.data(),pointCount);
}

static void drawXmbSparkles(float time) {
  for(int index=0;index<42;index++){
    float x=fmodf(index*0.618034f+time*(0.0022f+(index%5)*0.00045f),1.08f)-0.04f;
    float y=xmbWaveY(x,time,0.585f,0.095f,0.91f,0.075f,0.4f)+
            (fmodf(index*0.413f,1.f)-0.5f)*0.31f;
    float pulse=0.5f+0.5f*sinf(time*(0.55f+(index%7)*0.08f)+index*1.731f);
    Uint8 alpha=(Uint8)(28.f+pulse*(index%9==0?142.f:82.f));
    int px=(int)(x*SW),py=(int)(y*SH);
    fillRect(px,py,index%9==0?3:2,index%9==0?3:2,(SDL_Color){220,246,255,alpha});
    if(index%9==0&&pulse>0.55f){
      SDL_SetRenderDrawColor(g_ren,235,251,255,(Uint8)(alpha*0.62f));
      SDL_RenderDrawLine(g_ren,px-5,py+1,px+7,py+1);
      SDL_RenderDrawLine(g_ren,px+1,py-5,px+1,py+7);
    }
  }
}

static void drawXmbBackground(float time) {
  const SDL_Color top={3,37,102,255},middle={8,93,184,255},bottom={0,20,68,255};
  constexpr int bands=72;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.52f){
      float amount=y/0.52f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.52f)/0.48f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }
  if(g_glowTexture){
    drawGlow(0.10f,0.43f,1.18f,55,157,255,54);
    drawGlow(0.84f,0.38f,0.92f,41,112,228,42);
  }
  drawXmbRibbon(time,0.655f,0.082f,0.78f,-0.105f,2.15f,std::max(12,SH/18),(SDL_Color){63,166,255,31});
  drawXmbRibbon(time,0.575f,0.074f,0.96f,0.080f,0.35f,std::max(10,SH/25),(SDL_Color){189,235,255,48});
  drawXmbRibbon(time,0.605f,0.049f,1.28f,-0.025f,3.82f,std::max(5,SH/54),(SDL_Color){230,250,255,72});
  for(int trace=0;trace<9;trace++){
    float offset=(trace-4)*0.009f;
    drawXmbFilament(time,0.588f+offset,0.083f+trace*0.0017f,0.91f,0.052f,
                    0.62f+trace*0.19f,(SDL_Color){202,241,255,(Uint8)(18+trace%3*8)});
  }
  drawXmbFilament(time,0.578f,0.073f,0.96f,0.080f,0.35f,(SDL_Color){243,253,255,136});
  drawXmbSparkles(time);
}

static void drawBubble(int centerX,int centerY,int radius,Uint8 alpha) {
  if(radius<3||alpha==0) return;
  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,90,205,255);
    SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(alpha/5));
    SDL_Rect glow={centerX-radius*2,centerY-radius*2,radius*4,radius*4};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&glow);
  }
  const int segments=24;
  SDL_SetRenderDrawColor(g_ren,124,220,255,alpha);
  std::array<SDL_Point,segments+1> outer{},inner{};
  for(int segment=0;segment<=segments;segment++){
    float angle=segment*6.2831853f/segments;
    float x=cosf(angle),y=sinf(angle);
    outer[segment]={centerX+(int)(x*radius),centerY+(int)(y*radius)};
    inner[segment]={centerX+(int)(x*(radius-1)),centerY+(int)(y*(radius-1))};
  }
  SDL_RenderDrawLines(g_ren,outer.data(),(int)outer.size());
  SDL_RenderDrawLines(g_ren,inner.data(),(int)inner.size());
  SDL_SetRenderDrawColor(g_ren,235,252,255,(Uint8)std::min(255,(int)alpha+55));
  std::array<SDL_Point,6> highlight{};
  for(int segment=0;segment<(int)highlight.size();segment++){
    float angle=3.55f+segment*0.13f;
    highlight[segment]={centerX+(int)(cosf(angle)*radius),centerY+(int)(sinf(angle)*radius)};
  }
  SDL_RenderDrawLines(g_ren,highlight.data(),(int)highlight.size());
}

static void drawBubblesBackground(float time) {
  const SDL_Color top={20,126,169,255},middle={4,54,82,255},bottom={0,5,11,255};
  constexpr int bands=56;
  for(int band=0;band<bands;band++){
    float y=(band+0.5f)/bands;
    SDL_Color color{};
    if(y<0.58f){
      float amount=y/0.58f;
      color={blendChannel(top.r,middle.r,amount),blendChannel(top.g,middle.g,amount),blendChannel(top.b,middle.b,amount),255};
    } else {
      float amount=(y-0.58f)/0.42f;
      color={blendChannel(middle.r,bottom.r,amount),blendChannel(middle.g,bottom.g,amount),blendChannel(middle.b,bottom.b,amount),255};
    }
    int y0=band*SH/bands,y1=(band+1)*SH/bands;
    fillRect(0,y0,SW,y1-y0,color);
  }

  if(g_glowTexture){
    SDL_SetTextureColorMod(g_glowTexture,118,225,255);
    SDL_SetTextureAlphaMod(g_glowTexture,105);
    SDL_Rect surface={-SW/6,-SH/3,SW*4/3,SH*2/3};
    SDL_RenderCopy(g_ren,g_glowTexture,nullptr,&surface);
    for(int ray=0;ray<7;ray++){
      float sway=sinf(time*(0.10f+ray*0.013f)+ray*1.31f);
      int width=SW*(11+(ray%3)*3)/100;
      int x=SW*(8+ray*14)/100+(int)(sway*SW*0.025f)-width/2;
      SDL_Rect shaft={x,-SH/3,width,SH*4/3};
      SDL_SetTextureAlphaMod(g_glowTexture,(Uint8)(23+(ray%3)*7));
      SDL_RenderCopyEx(g_ren,g_glowTexture,nullptr,&shaft,-9.0+ray*2.7+sway*2.0,nullptr,SDL_FLIP_NONE);
    }
  }

  for(int index=0;index<18;index++){
    float progress=fmodf(index*0.173f+time*(0.038f+(index%5)*0.007f),1.18f);
    float y=1.08f-progress;
    float x=0.05f+fmodf(index*0.283f,0.90f)+0.032f*sinf(time*(0.31f+(index%4)*0.04f)+index);
    float fade=std::min(std::clamp((1.10f-y)*5.f,0.f,1.f),std::clamp((y+0.12f)*6.f,0.f,1.f));
    int radius=(int)(SH*(0.009f+(index%6)*0.0042f));
    if(index%11==0) radius=radius*3/2;
    drawBubble((int)(x*SW),(int)(y*SH),radius,(Uint8)(fade*(85+(index%4)*24)));
  }
  drawBackgroundParticles(time,(SDL_Color){164,228,255,62},24,0.008f);
}

static void clearUiBackground() {
  SDL_RenderSetClipRect(g_ren,nullptr);
  SDL_SetRenderDrawColor(g_ren,COL_BG.r,COL_BG.g,COL_BG.b,255);
  SDL_RenderClear(g_ren);
  if(!hasAnimatedBackground()) return;
  ensureGlowTexture();
  float time=g_uiAnimations?SDL_GetTicks()/1000.f:0.f;
  if(g_launcherTheme==LauncherTheme::Xmb){
    drawXmbBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(g_launcherTheme==LauncherTheme::Bubbles){
    drawBubblesBackground(time);
    if(g_glowTexture){ SDL_SetTextureColorMod(g_glowTexture,255,255,255); SDL_SetTextureAlphaMod(g_glowTexture,255); }
    return;
  }
  if(!g_glowTexture) return;
  drawGlow(0.10f+0.13f*sinf(time*0.43f),0.20f+0.11f*cosf(time*0.37f),0.90f,45,140,255,128);
  drawGlow(0.84f+0.12f*cosf(time*0.34f),0.34f+0.10f*sinf(time*0.41f),0.78f,154,75,255,112);
  drawGlow(0.54f+0.10f*sinf(time*0.29f),0.91f+0.06f*cosf(time*0.33f),0.94f,0,210,190,94);
  drawGlow(0.42f+0.08f*cosf(time*0.25f),0.48f+0.09f*sinf(time*0.31f),0.58f,64,125,255,67);
  drawBackgroundParticles(time,(SDL_Color){182,224,255,88},28,0.011f);
  SDL_SetTextureColorMod(g_glowTexture,255,255,255);
  SDL_SetTextureAlphaMod(g_glowTexture,255);
}

static void glassPanel(int x,int y,int width,int height) {
  fillRect(x,y,width,height,COL_PANEL);
  border(x,y,width,height,1,(SDL_Color){255,255,255,(Uint8)(hasAnimatedBackground()?28:16)});
}

static void drawText(TTF_Font*f,int x,int y,const char*s,SDL_Color c){
  if(!f||!s||!*s) return;
  TextKey key{f,packColor(c),s};
  auto found=g_textCache.find(key);
  if(found!=g_textCache.end()){
    found->second.use=++g_textUseSerial;
    SDL_Rect d={x,y,found->second.width,found->second.height};
    SDL_RenderCopy(g_ren,found->second.texture,nullptr,&d);
    return;
  }
  SDL_Surface*sf=TTF_RenderUTF8_Blended(f,s,c); if(!sf) return;
  SDL_Texture*t=SDL_CreateTextureFromSurface(g_ren,sf);
  int w=sf->w,h=sf->h; SDL_FreeSurface(sf);
  if(!t) return;
  rememberTextMetric(f,s,w);
  const size_t bytes=(size_t)w*(size_t)h*4;
  if(bytes<=TEXT_CACHE_BYTES){
    evictTextEntries(bytes);
    TextEntry entry{t,w,h,bytes,++g_textUseSerial};
    auto inserted=g_textCache.emplace(std::move(key),entry);
    g_textCacheBytes+=bytes;
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,inserted.first->second.texture,nullptr,&d);
  } else {
    SDL_Rect d={x,y,w,h}; SDL_RenderCopy(g_ren,t,nullptr,&d); SDL_DestroyTexture(t);
  }
}
static int textW(TTF_Font*f,const char*s){
  if(!f||!s||!*s) return 0;
  MetricKey key{f,s}; auto found=g_metricCache.find(key);
  if(found!=g_metricCache.end()){ found->second.use=++g_textUseSerial; return found->second.width; }
  int w=0,h=0; if(TTF_SizeUTF8(f,s,&w,&h)!=0) return 0;
  rememberTextMetric(f,s,w); return w;
}

static const std::string &ellipsizedText(TTF_Font *font, const std::string &text, int maxWidth) {
  EllipsisKey key{font,maxWidth,text};
  auto found=g_ellipsisCache.find(key);
  if(found!=g_ellipsisCache.end()){ found->second.use=++g_textUseSerial; return found->second.text; }

  std::vector<size_t> boundaries{0};
  for(size_t i=0;i<text.size();){
    const unsigned char lead=(unsigned char)text[i];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(i+length>text.size()) length=1;
    for(size_t j=1;j<length;j++) if(((unsigned char)text[i+j]&0xc0)!=0x80){ length=1; break; }
    i+=length; boundaries.push_back(i);
  }
  size_t low=0,high=boundaries.size()-1;
  while(low<high){
    size_t middle=(low+high+1)/2;
    std::string candidate=text.substr(0,boundaries[middle])+"...";
    if(textW(font,candidate.c_str())<=maxWidth) low=middle; else high=middle-1;
  }
  std::string shortened=text.substr(0,boundaries[low])+"...";
  if(g_ellipsisCache.size()>=ELLIPSIS_CACHE_LIMIT){
    auto victim=g_ellipsisCache.begin();
    for(auto it=std::next(g_ellipsisCache.begin());it!=g_ellipsisCache.end();++it)
      if(it->second.use<victim->second.use) victim=it;
    g_ellipsisCache.erase(victim);
  }
  auto inserted=g_ellipsisCache.emplace(std::move(key),EllipsisEntry{std::move(shortened),++g_textUseSerial});
  return inserted.first->second.text;
}
static std::string fittedText(TTF_Font *font,const std::string &text,int maxWidth){
  return textW(font,text.c_str())<=maxWidth?text:ellipsizedText(font,text,maxWidth);
}
static void drawTextR(TTF_Font*f,int xr,int y,const char*s,SDL_Color c){ drawText(f,xr-textW(f,s),y,s,c); }
static void drawTextC(TTF_Font*f,int cx,int y,const char*s,SDL_Color c){ drawText(f,cx-textW(f,s)/2,y,s,c); }

static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col);
static void downloadAllCovers();
static void runUpdateScreen();
static std::string installedReleaseTag();
static void pollUpdateNotification();
static void drawUpdateNotification();
static void toast(const char *msg, Uint32 durationMs=1800);
static void modalMessage(const char *title, const std::vector<std::string> &lines);
static bool confirmBox(const char *title, const std::vector<std::string> &lines);
static int dropdown(const char *title, const char *const *labels, int n, int cur,
                    const SDL_Color *swatches=nullptr);
static void beginScreenFx();
static void drawFadeIn();
static int topBarH();
static bool highResolutionUi();
static int settingsRowH();
static int settingsListY();
static int settingsFooterReserve();
static void drawHeader(const char *title,const char *ctx);
static void drawSettingsRowText(const char *label,const char *value,
                                int slotY,int colW,int labelX,int valX,
                                bool current,SDL_Color labelColor,
                                SDL_Color valueColor,bool scrollValue=false,
                                int rowHeight=0);
static void drawFooterText(const char *text,int centerY=-1);
static void drawScrollTextR(TTF_Font *font,int xRight,int y,int maxWidth,const char *text,SDL_Color color);
static void drawScrollTextL(TTF_Font *font,int x,int y,int maxWidth,const char *text,SDL_Color color);
static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color);
static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height);
static void pumpCoverDecodeResults();
static void cancelQueuedCoverDecodes();
static bool g_rescanAfterSettings = false;

static SDL_Texture *g_flag[4] = { nullptr, nullptr, nullptr, nullptr };
static void fillCircle(int cx,int cy,int r,SDL_Color c){
  SDL_SetRenderDrawColor(g_ren,c.r,c.g,c.b,c.a);
  for(int dy=-r;dy<=r;dy++){ int dx=(int)(sqrt((double)(r*r-dy*dy))+0.5); SDL_RenderDrawLine(g_ren,cx-dx,cy+dy,cx+dx,cy+dy); }
}
static SDL_Texture *makeFlagTex(int region,int W,int H){
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_Texture *previous=SDL_GetRenderTarget(g_ren);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  if(region==3){
    fillRect(0,0,W,H,(SDL_Color){245,245,245,255});
    fillCircle(W/2,H/2,H*30/100,(SDL_Color){188,0,45,255});
  } else if(region==1){
    for(int i=0;i<7;i++) fillRect(0,i*H/7,W,H/7+1,(i%2)?(SDL_Color){235,235,235,255}:(SDL_Color){178,34,52,255});
    fillRect(0,0,W*2/5,(H*4)/7,(SDL_Color){45,50,110,255});
    for(int ry=0;ry<2;ry++)for(int cc=0;cc<3;cc++) fillRect(5+cc*(W*2/5-8)/3,4+ry*8,2,2,(SDL_Color){255,255,255,255});
  } else if(region==2){
    fillRect(0,0,W,H,(SDL_Color){0,51,153,255});
    for(int i=0;i<12;i++){ double a=i*6.28318/12.0; int sx=W/2+(int)(cos(a)*W*0.30), sy=H/2+(int)(sin(a)*H*0.32);
      fillRect(sx-1,sy-1,2,2,(SDL_Color){255,204,0,255}); }
  }
  SDL_SetRenderTarget(g_ren,previous);
  return t;
}
static void makeFlags(){ g_flag[1]=makeFlagTex(1,36,24); g_flag[2]=makeFlagTex(2,36,24); g_flag[3]=makeFlagTex(3,36,24); }

static SDL_Texture *g_gA=nullptr,*g_gB=nullptr,*g_gX=nullptr,*g_gY=nullptr,
                   *g_gPlus=nullptr,*g_gMinus=nullptr,*g_gLeftRight=nullptr,
                   *g_gUpDown=nullptr,*g_gL=nullptr,*g_gR=nullptr;
// Supersampling keeps the downscaled glyphs crisp.
static const int GLYPH_SS = 3;
static SDL_Texture *makeGlyph(const char *label, bool pill){
  if(!g_font_sm || !g_font_big) return nullptr;
  const int S=GLYPH_SS, base=TTF_FontHeight(g_font_sm)+6;
  int H=base*S, W=(pill? base*8/5 : base)*S;
  SDL_Texture *t=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,W,H);
  if(!t) return nullptr;
  SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND);
  SDL_Texture *previous=SDL_GetRenderTarget(g_ren);
  SDL_SetRenderTarget(g_ren,t);
  SDL_SetRenderDrawColor(g_ren,0,0,0,0); SDL_RenderClear(g_ren);
  SDL_Color edge={14,16,22,255}, hi={92,99,114,255}, face={52,57,68,255}, ink={246,248,252,255};
  if(pill){
    int r=H/2;
    fillCircle(r,r,r,edge);     fillCircle(W-r,r,r,edge);     fillRect(r,0,W-2*r,H,edge);
    fillCircle(r,r,r-S,hi);     fillCircle(W-r,r,r-S,hi);     fillRect(r,S,W-2*r,H-2*S,hi);
    fillCircle(r,r,r-S*2,face); fillCircle(W-r,r,r-S*2,face); fillRect(r,S*2,W-2*r,H-S*4,face);
  } else {
    int R=H/2;
    fillCircle(W/2,H/2,R,edge);
    fillCircle(W/2,H/2,R-S,hi);
    fillCircle(W/2,H/2,R-S*2,face);
  }
  SDL_Surface *sf=TTF_RenderUTF8_Blended(g_font_big,label,ink);
  if(sf){ SDL_Texture *lt=SDL_CreateTextureFromSurface(g_ren,sf);
    if(lt) SDL_SetTextureBlendMode(lt,SDL_BLENDMODE_BLEND);
    int inner=H*56/100, lw=sf->w, lh=sf->h;
    if(lh>0){ lw=lw*inner/lh; lh=inner; }
    SDL_Rect d={(W-lw)/2,(H-lh)/2,lw,lh}; SDL_FreeSurface(sf);
    if(lt){ SDL_RenderCopy(g_ren,lt,nullptr,&d); SDL_DestroyTexture(lt); } }
  SDL_SetRenderTarget(g_ren,previous);
  return t;
}
static void makeGlyphs(){
  g_gA=makeGlyph("A",false); g_gB=makeGlyph("B",false);
  g_gX=makeGlyph("X",false); g_gY=makeGlyph("Y",false);
  g_gPlus=makeGlyph("+",false); g_gMinus=makeGlyph("−",false);
  g_gLeftRight=makeGlyph("‹ ›",true);g_gUpDown=makeGlyph("↕",true);
  g_gL=makeGlyph("L",true); g_gR=makeGlyph("R",true);
}

enum FootAct { FA_NONE, FA_LAUNCH, FA_SORT, FA_OPTIONS, FA_SETTINGS, FA_FILTER, FA_PAGEL, FA_PAGER, FA_QUIT };
struct FootItem { const char *button; const char *label; int act; };
static SDL_Rect g_footHit[10]; static int g_footAct[10]; static int g_footN=0;
static SDL_Texture *footerGlyph(const char *button){
  if(!button) return nullptr;
  if(!strcmp(button,"A")) return g_gA;
  if(!strcmp(button,"B")) return g_gB;
  if(!strcmp(button,"X")) return g_gX;
  if(!strcmp(button,"Y")) return g_gY;
  if(!strcmp(button,"+")) return g_gPlus;
  if(!strcmp(button,"-")) return g_gMinus;
  if(!strcmp(button,"Left / Right")) return g_gLeftRight;
  if(!strcmp(button,"Up / Down")) return g_gUpDown;
  if(!strcmp(button,"L")) return g_gL;
  if(!strcmp(button,"R")) return g_gR;
  return nullptr;
}
static void footerButtonSize(const char *button,int &width,int &height){
  SDL_Texture *glyph=footerGlyph(button);
  width=height=0;
  if(glyph){
    SDL_QueryTexture(glyph,nullptr,nullptr,&width,&height);
    width/=GLYPH_SS; height/=GLYPH_SS;
  } else {
    width=textW(g_font_sm,button?button:"")+14;
    height=TTF_FontHeight(g_font_sm)+6;
  }
}
static int footerHintWidth(const char *button,const char *label){
  int width=0,height=0; footerButtonSize(button,width,height);
  if(label&&*label) width+=8+textW(g_font_sm,label);
  return width;
}
static void drawButtonHint(int x,int centerY,const char *button,const char *label){
  int width=0,height=0; footerButtonSize(button,width,height);
  SDL_Texture *glyph=footerGlyph(button);
  if(glyph){
    SDL_Rect destination={x,centerY-height/2,width,height};
    SDL_RenderCopy(g_ren,glyph,nullptr,&destination);
  } else {
    border(x,centerY-height/2,width,height,1,COL_DIM);
    drawTextC(g_font_sm,x+width/2,centerY-TTF_FontHeight(g_font_sm)/2,
              button?button:"",COL_TXT);
  }
  if(label&&*label)
    drawText(g_font_sm,x+width+8,centerY-TTF_FontHeight(g_font_sm)/2,label,COL_DIM);
}
static void drawFooterHints(const FootItem *it,int n,int cy){
  const int gap=8;
  const int pairGap=g_launcherPortrait?12:26;
  const int glyphGap=g_launcherPortrait?8:16;
  const int fh=TTF_FontHeight(g_font_sm),maxWidth=SW-24;
  int itemWidth[10]={},gapAfter[10]={};
  for(int i=0;i<n&&i<10;i++){
    bool hasLabel=it[i].label&&it[i].label[0];
    itemWidth[i]=footerHintWidth(it[i].button,it[i].label);
    gapAfter[i]=hasLabel?pairGap:glyphGap;
  }
  int rowStart[10]={0},rowEnd[10]={0},rowWidth[10]={0},rowCount=0;
  for(int first=0;first<n&&first<10;){
    int last=first,width=0;
    while(last<n&&last<10){
      int added=itemWidth[last]+(last>first?gapAfter[last-1]:0);
      if(last>first&&width+added>maxWidth) break;
      width+=added;
      last++;
    }
    rowStart[rowCount]=first;
    rowEnd[rowCount]=last;
    rowWidth[rowCount]=width;
    rowCount++;
    first=last;
  }
  const int rowSpacing=fh+14;
  g_footN=0;
  for(int row=0;row<rowCount;row++){
    const int rowY=cy-(rowCount-1-row)*rowSpacing;
    int x=(SW-rowWidth[row])/2;
    for(int i=rowStart[row];i<rowEnd[row];i++){
      int bw=0,bh=0; footerButtonSize(it[i].button,bw,bh);
      int x0=x;
      drawButtonHint(x,rowY,it[i].button,it[i].label);
      x+=bw;
      bool hasLabel=it[i].label&&it[i].label[0];
      if(hasLabel) x+=gap+textW(g_font_sm,it[i].label);
      if(g_footN<10){
        g_footHit[g_footN]={x0-6,rowY-bh/2-8,(x-x0)+12,std::max(bh,fh)+16};
        g_footAct[g_footN]=it[i].act;
        g_footN++;
      }
      if(i+1<rowEnd[row]) x+=gapAfter[i];
    }
  }
}
static int footTapAct(int px,int py){
  for(int i=0;i<g_footN;i++){ SDL_Rect &r=g_footHit[i];
    if(px>=r.x && px<r.x+r.w && py>=r.y && py<r.y+r.h) return g_footAct[i]; }
  return FA_NONE;
}

enum TouchKind { TOUCH_NONE, TOUCH_TAP, TOUCH_SWIPE_L, TOUCH_SWIPE_R, TOUCH_SCROLL_UP, TOUCH_SCROLL_DOWN };
struct TouchG {
  bool active=false, vertical=false;
  SDL_FingerID fid=0;
  float x0=0,y0=0,lastY=0;
  Uint32 t0=0;
};
static TouchG g_touch;
static int g_touchScrollSteps=1;
static void touchUiPoint(float normalizedX,float normalizedY,float &x,float &y){
  switch(g_launcherRotation){
    case 1:
      x=normalizedY*SW;
      y=(1.0f-normalizedX)*SH;
      break;
    case 2:
      x=(1.0f-normalizedX)*SW;
      y=(1.0f-normalizedY)*SH;
      break;
    case 3:
      x=(1.0f-normalizedY)*SW;
      y=normalizedX*SH;
      break;
    default:
      x=normalizedX*SW;
      y=normalizedY*SH;
      break;
  }
}
static TouchKind touchFeed(const SDL_Event &e,int *ox,int *oy){
  const int TAP_MOVE=26, SWIPE_DX=90, SCROLL_STEP=30; const Uint32 TAP_MS=400;
  if(e.type==SDL_FINGERDOWN){
    if(g_touch.active && SDL_GetTicks()-g_touch.t0 < 2000) return TOUCH_NONE;
    g_touch.active=true; g_touch.vertical=false; g_touch.fid=e.tfinger.fingerId;
    touchUiPoint(e.tfinger.x,e.tfinger.y,g_touch.x0,g_touch.y0);
    g_touch.lastY=g_touch.y0; g_touch.t0=SDL_GetTicks();
  } else if(e.type==SDL_FINGERMOTION && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    float ux=0,uy=0; touchUiPoint(e.tfinger.x,e.tfinger.y,ux,uy);
    float dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    if(!g_touch.vertical && fabsf(dy)>TAP_MOVE && fabsf(dy)>fabsf(dx)*1.15f) g_touch.vertical=true;
    if(g_touch.vertical){
      float step=uy-g_touch.lastY;
      if(fabsf(step)>=SCROLL_STEP){
        g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(step)/SCROLL_STEP)));
        g_touch.lastY=uy;
        if(ox) *ox=(int)ux;
        if(oy) *oy=(int)uy;
        return step<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
      }
    }
  } else if(e.type==SDL_FINGERUP && g_touch.active && e.tfinger.fingerId==g_touch.fid){
    g_touch.active=false;
    float ux=0,uy=0; touchUiPoint(e.tfinger.x,e.tfinger.y,ux,uy);
    float dx=ux-g_touch.x0, dy=uy-g_touch.y0;
    Uint32 dt=SDL_GetTicks()-g_touch.t0;
    if(ox) *ox=(int)ux;
    if(oy) *oy=(int)uy;
    if(g_touch.vertical || (fabsf(dy)>=55 && fabsf(dy)>fabsf(dx)*1.15f)){
      float remaining=uy-g_touch.lastY;
      if(fabsf(remaining)<18 && g_touch.vertical) return TOUCH_NONE;
      g_touchScrollSteps=std::min(6,std::max(1,(int)(fabsf(g_touch.vertical?remaining:dy)/SCROLL_STEP)));
      return (g_touch.vertical?remaining:dy)<0?TOUCH_SCROLL_UP:TOUCH_SCROLL_DOWN;
    }
    if(fabsf(dx)>=SWIPE_DX && fabsf(dx)>fabsf(dy)*1.5f) return dx<0?TOUCH_SWIPE_L:TOUCH_SWIPE_R;
    if(fabsf(dx)<=TAP_MOVE && fabsf(dy)<=TAP_MOVE && dt<=TAP_MS) return TOUCH_TAP;
  }
  return TOUCH_NONE;
}

static bool touchScrollList(TouchKind kind,int &sel,int &top,int count,int visible){
  if((kind!=TOUCH_SCROLL_UP && kind!=TOUCH_SCROLL_DOWN) || count<=0) return false;
  const int previous=sel;
  int delta=(kind==TOUCH_SCROLL_UP?1:-1)*g_touchScrollSteps;
  sel=std::max(0,std::min(count-1,sel+delta));
  if(sel<top) top=sel;
  if(sel>=top+visible) top=sel-visible+1;
  if(top<0) top=0;
  if(sel!=previous) uiAudioPlay(UiSound::Navigate);
  return true;
}

static bool g_stickXLatched=false, g_stickYLatched=false;
static char stickNav(const SDL_Event &e){
  const int TH=18000, DZ=8000;
  if(e.type!=SDL_CONTROLLERAXISMOTION) return 0;
  if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX){
    if(!g_stickXLatched && e.caxis.value<-TH){ g_stickXLatched=true; return 'L'; }
    if(!g_stickXLatched && e.caxis.value> TH){ g_stickXLatched=true; return 'R'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickXLatched=false;
  } else if(e.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY){
    if(!g_stickYLatched && e.caxis.value<-TH){ g_stickYLatched=true; return 'U'; }
    if(!g_stickYLatched && e.caxis.value> TH){ g_stickYLatched=true; return 'D'; }
    if(e.caxis.value>-DZ && e.caxis.value<DZ) g_stickYLatched=false;
  }
  return 0;
}
static void pumpStick(const SDL_Event &e){
  char n=stickNav(e); if(!n) return;
  SDL_Event s; memset(&s,0,sizeof(s));
  s.type=SDL_CONTROLLERBUTTONDOWN;
  s.cbutton.button = n=='U'?SDL_CONTROLLER_BUTTON_DPAD_UP : n=='D'?SDL_CONTROLLER_BUTTON_DPAD_DOWN
                   : n=='L'?SDL_CONTROLLER_BUTTON_DPAD_LEFT : SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  SDL_PushEvent(&s);
}

static SDL_GameController *g_pad=nullptr;
static bool g_exitRequested=false;
static int g_navHeld=0;
static Uint32 g_navSince=0,g_navLast=0;

static void openController(int index) {
  if (!g_pad && index >= 0 && SDL_IsGameController(index))
    g_pad = SDL_GameControllerOpen(index);
}

static void closeController() {
  if (!g_pad) return;
  SDL_GameControllerClose(g_pad);
  g_pad = nullptr;
  g_stickXLatched = g_stickYLatched = false;
  g_navHeld = 0;
  g_navSince = g_navLast = 0;
}

static bool beginUiFrame() {
  if (g_exitRequested) return false;
  if (!appletMainLoop()) {
    g_exitRequested = true;
    return false;
  }
  if(g_pad&&!SDL_GameControllerGetAttached(g_pad)) closeController();
  // Texture creation belongs to the SDL thread.  Image decoding/scaling is
  // performed by the cover worker and only completed pixel buffers arrive
  // here, capped to a small upload budget per frame.
  pumpCoverDecodeResults();
  return true;
}

// Block when the UI is idle, but keep a 60 Hz cadence for active transitions.
// Worker wake events make scans, USB hotplug and downloads immediately visible.
static void waitForNextUiFrame(bool animated=true, Uint32 requestedDeadline=0) {
  for(;;){
    const Uint32 now=SDL_GetTicks();
    const bool transitionActive=animated&&g_uiAnimations&&now-g_fxT<180;
    Uint32 deadline=requestedDeadline;
    auto includeDeadline=[&](Uint32 candidate){
      if(candidate&&!SDL_TICKS_PASSED(now,candidate)&&
         (!deadline||SDL_TICKS_PASSED(deadline,candidate))) deadline=candidate;
    };
    includeDeadline(g_toastUntil);
    includeDeadline(g_updateNoticeUntil);
    if(g_navHeld) includeDeadline(g_navLast+85);
    int timeout=transitionActive?16:250;
    if(deadline){
      const Uint32 remaining=deadline-now;
      timeout=std::min(timeout,(int)std::min<Uint32>(remaining,250));
    }
    SDL_Event event{};
    if(SDL_WaitEventTimeout(&event,std::max(1,timeout))){SDL_PushEvent(&event);return;}
    const Uint32 after=SDL_GetTicks();
    if(transitionActive||
       (requestedDeadline&&SDL_TICKS_PASSED(after,requestedDeadline))||
       (g_toastUntil&&SDL_TICKS_PASSED(after,g_toastUntil))||
       (g_updateNoticeUntil&&SDL_TICKS_PASSED(after,g_updateNoticeUntil))||
       (g_navHeld&&SDL_TICKS_PASSED(after,g_navLast+85))) return;
    // Static screens stay asleep, but periodically service the HOME/lifecycle
    // state so app shutdown never waits on an unbounded SDL event wait.
    if(!appletMainLoop()){
      g_exitRequested=true;
      SDL_Event quit{};quit.type=SDL_QUIT;SDL_PushEvent(&quit);
      return;
    }
  }
}

static int keyboardNavigationButton(SDL_Keycode key) {
  switch(key){
    case SDLK_RETURN: case SDLK_KP_ENTER: return BTN_CONFIRM;
    case SDLK_ESCAPE: return BTN_CANCEL;
    case SDLK_UP: return SDL_CONTROLLER_BUTTON_DPAD_UP;
    case SDLK_DOWN: return SDL_CONTROLLER_BUTTON_DPAD_DOWN;
    case SDLK_LEFT: return SDL_CONTROLLER_BUTTON_DPAD_LEFT;
    case SDLK_RIGHT: return SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
    case SDLK_PAGEUP: return SDL_CONTROLLER_BUTTON_LEFTSHOULDER;
    case SDLK_PAGEDOWN: return SDL_CONTROLLER_BUTTON_RIGHTSHOULDER;
    case SDLK_s: return SDL_CONTROLLER_BUTTON_X;
    case SDLK_F1: return BTN_SETTINGS;
    case SDLK_SPACE: return SDL_CONTROLLER_BUTTON_START;
    default: return -1;
  }
}

static bool pollUiEvent(SDL_Event &event) {
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) {
      g_exitRequested = true;
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
      openController(event.cdevice.which);
      continue;
    }
    if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
      if (g_pad) {
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(g_pad);
        if (joystick && SDL_JoystickInstanceID(joystick) == event.cdevice.which)
          closeController();
      }
      continue;
    }
    if(event.type==SDL_KEYDOWN){
      int button=keyboardNavigationButton(event.key.keysym.sym);
      if(button>=0){
        SDL_Event press{};
        press.type=SDL_CONTROLLERBUTTONDOWN;
        press.cbutton.button=(Uint8)button;
        SDL_PushEvent(&press);
      }
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
        case BTN_CONFIRM: uiAudioPlay(UiSound::Confirm); break;
        case BTN_CANCEL: uiAudioPlay(UiSound::Back); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
          uiAudioPlay(UiSound::Navigate); break;
        default: break;
      }
    }
    return true;
  }
  return false;
}

static void navRepeat(){
  if(!g_pad||!SDL_GameControllerGetAttached(g_pad)) return;
  const int TH=18000;
  int dir=0;
  if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)   || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_UP;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTY)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_DOWN;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)<-TH) dir=SDL_CONTROLLER_BUTTON_DPAD_LEFT;
  else if(SDL_GameControllerGetButton(g_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || SDL_GameControllerGetAxis(g_pad,SDL_CONTROLLER_AXIS_LEFTX)> TH) dir=SDL_CONTROLLER_BUTTON_DPAD_RIGHT;
  Uint32 now=SDL_GetTicks();
  if(dir!=g_navHeld){ g_navHeld=dir; g_navSince=now; g_navLast=now; return; }
  if(!dir) return;
  const Uint32 DELAY=360, RATE=85;
  if(now-g_navSince<DELAY || now-g_navLast<RATE) return;
  g_navLast=now;
  SDL_Event s; memset(&s,0,sizeof(s)); s.type=SDL_CONTROLLERBUTTONDOWN; s.cbutton.button=(Uint8)dir;
  SDL_PushEvent(&s);
}

struct Game {
  std::string path;
  std::string file;
  std::string title;
  std::string headerTitle;
  std::string gameCode;
  std::string key;
  std::string baseIdentity;
  std::string canonicalPath;
  // Stable physical volume identity captured when the file was scanned.  Do
  // not infer ownership from a mutable umsN: alias after a hotplug event.
  std::string sourceStableId;
  // The previous path-derived key and oldest filename-derived key are retained
  // only for one-way migration and old forwarder compatibility.
  std::string pathKey;
  std::string legacyKey;
  uint64_t fingerprint = 0;
  long long fileSize = 0;
  long long modified = 0;
  SDL_Texture *cover = nullptr;
  bool coverIsRomIcon = false;
  Uint32 coverAt = 0;
  Uint64 coverUse = 0;
  Uint64 coverRequest = 0;
  bool triedCover = false;
  bool coverQueued = false;
  bool hasCfg = false;
  bool legacyUnique = false;
  bool allowLegacyMigration = true;
  int region = 0;
  long long added = 0;
  long long played = 0;
};
static std::vector<Game> g_games;
struct LibraryIdentityRecord{
  std::string id,fingerprint,baseIdentity,canonicalPath,currentPath;
  std::vector<std::string> previousPaths;
  bool retired=false;
};
static std::vector<LibraryIdentityRecord> g_libraryIdentities;
static std::unordered_set<std::string> g_claimedLibraryIds,g_reservedLibraryIds;
static bool g_libraryIdentitiesDirty=false;
static constexpr size_t MAX_PREVIOUS_LIBRARY_PATHS=4;
static std::string foldedKey(std::string key);
struct Collection { std::string name; std::unordered_set<std::string> games; };
static std::unordered_set<std::string> g_favorites;
static std::vector<Collection> g_collections;
static std::string g_activeCollection;
static std::string g_searchQuery;
static std::vector<Game*> g_libraryView;
static Uint64 g_coverUseSerial = 0;
// Two maximum-size 8x3 pages fit without evicting the page being viewed.
// Each decoded cover is capped at 360x540 RGBA (under 0.75 MiB).
static constexpr size_t COVER_CACHE_LIMIT = 64;

enum { SORT_ALPHA, SORT_RECENT, SORT_ADDED, SORT_COUNT };
static const char *SORT_NAME[SORT_COUNT] = { "A-Z", "Recently played", "Recently added" };
static int g_sort = SORT_ALPHA;
static Store g_recent;
static const char *RECENT_INI = "sdmc:/switch/drastic/recent.ini";

static int detectRegion(const std::string &code, const std::string &file) {
  if (code.size() == 4) {
    switch ((char)toupper((unsigned char)code[3])) {
      case 'J': return 3;
      case 'E': case 'N': return 1;
      case 'P': case 'D': case 'F': case 'H': case 'I': case 'S':
      case 'U': case 'V': case 'X': case 'Y': case 'Z': return 2;
      default: break;
    }
  }
  std::string tags="|"; int depth = 0;
  for (char c : file) {
    if (c=='('||c=='[') depth++;
    else if (c==')'||c==']') { if (depth) depth--; if (depth==0) tags += '|'; }
    else if (depth) tags += (char)tolower((unsigned char)c);
  }
  auto has = [&](const char *s){ return tags.find(s) != std::string::npos; };
  if (has("japan")||has("ntsc-j")||has("jpn")||has("|j|")) return 3;
  if (has("usa")||has("ntsc-u")||has("america")||has("|u|")) return 1;
  if (has("europe")||has("pal")||has("australia")||has("|uk|")||has("france")||
      has("germany")||has("spain")||has("ital")||has("|e|")) return 2;
  std::string l; for (char c : file) l += (char)tolower((unsigned char)c);
  if (l.find("ntsc-j")!=std::string::npos) return 3;
  if (l.find("ntsc-u")!=std::string::npos) return 1;
  return 0;
}
static void rebuildLibraryView() {
  g_libraryView.clear();
  const Collection *collection=nullptr;
  if(!g_activeCollection.empty()&&g_activeCollection!="favorites")
    for(const Collection &candidate:g_collections) if(candidate.name==g_activeCollection){collection=&candidate;break;}
  const std::string query=foldedKey(g_searchQuery);
  for(Game &game:g_games){
    if(g_activeCollection=="favorites"&&!g_favorites.count(game.key)) continue;
    if(collection&&!collection->games.count(game.key)) continue;
    if(!query.empty()&&foldedKey(game.title+" "+game.file+" "+game.path+" "+game.key).find(query)==std::string::npos) continue;
    g_libraryView.push_back(&game);
  }
}
static void loadLibraryOrganization() {
  g_favorites.clear();g_collections.clear();
  const int favorites=std::max(0,std::min(16384,atoi(storeGet(g_global,"Library/FavoriteCount","0"))));
  for(int i=0;i<favorites;i++){const char *id=storeGet(g_global,("Library/Favorite"+std::to_string(i)).c_str(),"");if(id[0])g_favorites.insert(id);}
  const int count=std::max(0,std::min(128,atoi(storeGet(g_global,"Library/CollectionCount","0"))));
  for(int i=0;i<count;i++){
    const std::string prefix="Library/Collection"+std::to_string(i);
    Collection collection;collection.name=storeGet(g_global,(prefix+"Name").c_str(),"");
    const int members=std::max(0,std::min(16384,atoi(storeGet(g_global,(prefix+"Count").c_str(),"0"))));
    for(int m=0;m<members;m++){const char *id=storeGet(g_global,(prefix+"Game"+std::to_string(m)).c_str(),"");if(id[0])collection.games.insert(id);}
    if(!collection.name.empty())g_collections.emplace_back(std::move(collection));
  }
  // Views are transient by design: every launcher boot starts at All games.
  g_activeCollection.clear();g_searchQuery.clear();
  storeRemove(g_global,"Library/ActiveCollection");storeRemove(g_global,"Library/Search");
}
static void saveLibraryOrganization() {
  storeRemovePrefix(g_global,"Library/Favorite");
  storeSet(g_global,"Library/FavoriteCount",std::to_string(g_favorites.size()).c_str());
  size_t index=0;for(const std::string &id:g_favorites)storeSet(g_global,("Library/Favorite"+std::to_string(index++)).c_str(),id.c_str());
  storeRemovePrefix(g_global,"Library/Collection");
  storeSet(g_global,"Library/CollectionCount",std::to_string(g_collections.size()).c_str());
  for(size_t i=0;i<g_collections.size();i++){
    const std::string prefix="Library/Collection"+std::to_string(i);
    storeSet(g_global,(prefix+"Name").c_str(),g_collections[i].name.c_str());
    storeSet(g_global,(prefix+"Count").c_str(),std::to_string(g_collections[i].games.size()).c_str());
    size_t member=0;for(const std::string &id:g_collections[i].games)storeSet(g_global,(prefix+"Game"+std::to_string(member++)).c_str(),id.c_str());
  }
  storeSave(g_global,LAUNCHER_INI);
}
static void applySort() {
  auto cmpTitle = [](const Game &a, const Game &b){ return strcasecmp(a.title.c_str(), b.title.c_str()) < 0; };
  std::sort(g_games.begin(), g_games.end(), [&](const Game &a, const Game &b){
    if (g_sort == SORT_RECENT && a.played != b.played) return a.played > b.played;
    if (g_sort == SORT_ADDED  && a.added  != b.added)  return a.added  > b.added;
    return cmpTitle(a, b);
  });
  rebuildLibraryView();
}
static void recordPlayed(const Game &game){
  long long seq = atoll(storeGet(g_global,"Wrapper/PlaySeq","0")) + 1;
  char b[24]; snprintf(b,sizeof(b),"%lld",seq);
  storeSet(g_global,"Wrapper/PlaySeq",b);
  storeSet(g_recent,game.key.c_str(),b);
  if (!game.pathKey.empty()) storeRemove(g_recent,game.pathKey.c_str());
  if (game.legacyUnique && !game.legacyKey.empty())
    storeRemove(g_recent, game.legacyKey.c_str());
}

static bool hasGameExtension(const char *n) {
  const char *e = strrchr(n, '.');
  if (!e) return false;
  static const char *x[] = { ".nds", ".zip", ".rar" };
  for (auto s : x) if (!strcasecmp(e, s)) return true;
  return false;
}
static std::string toEmu(const std::string &path) {
  return path.rfind("sdmc:", 0) == 0 ? path.substr(5) : path;
}
static bool launcherOnlyWrapperKey(const std::string &key) {
  if(key.rfind("Wrapper/GamePath",0)==0) return true;
  static const char *keys[]={
    "Wrapper/GameDir","Wrapper/SteamGridDBKey","Wrapper/UiSounds",
    "Wrapper/Theme","Wrapper/Language","Wrapper/LauncherRotation",
    "Wrapper/GridColumns","Wrapper/GridRows","Wrapper/ShowGameTitles",
    "Wrapper/ShowRegionFlags","Wrapper/ShowCustomSettingsBadges",
    "Wrapper/UiAnimations","Wrapper/CheckUpdatesAtBoot",
    "Wrapper/InstalledReleaseTag","Wrapper/SortMode","Wrapper/PlaySeq"
  };
  return std::any_of(std::begin(keys),std::end(keys),[&](const char *candidate){return key==candidate;});
}
static bool runtimeOwnedKey(const std::string &key) {
  return key=="Wrapper/CoreSo"||key=="Wrapper/LauncherPath"||
         key=="Wrapper/GameKey"||key=="Wrapper/GameConfigPath"||
         key=="Wrapper/CpuBoost"||key=="Drastic/RomPath";
}
static bool runtimeConfigKey(const std::string &key) {
  if(key.rfind("Drastic/",0)==0) return true;
  if(key.rfind("Storage/Smb",0)==0) return true;
  return key.rfind("Wrapper/",0)==0&&!launcherOnlyWrapperKey(key);
}
static Store makeRuntimeConfig(const Store &global,const std::string &romPath) {
  Store runtime;
  // Required launch paths stay first even if a future runtime file grows.
  storeSet(runtime,"Wrapper/CoreSo",CORE_SO_PATH);
  storeSet(runtime,"Drastic/RomPath",romPath.c_str());
  for(const auto &entry:global.kv)
    if(runtimeConfigKey(entry.k)&&!runtimeOwnedKey(entry.k))
      storeSet(runtime,entry.k.c_str(),entry.v.c_str());
  return runtime;
}
static std::string join(const std::string &b, const std::string &n) { std::string r=b; if(!r.empty()&&r.back()=='/') r.pop_back(); return r+"/"+n; }
static std::string foldedKey(std::string key);
static bool pathAtOrBelow(const std::string &path,const std::string &root);

static std::string normalizeLocationPath(const std::string &input) {
  std::string path=trim(input);
  if(path.empty()) return {};
  std::string output;
  output.reserve(path.size()+1);
  bool slash=false;
  for(char c:path){
    if(c=='\\') c='/';
    if(c=='/'){
      if(slash) continue;
      slash=true;
    } else slash=false;
    output+=c;
  }
  size_t colon=output.find(':');
  if(colon!=std::string::npos && colon+1==output.size()) output+='/';
  size_t minimum=colon==std::string::npos?1:colon+2;
  while(output.size()>minimum && output.back()=='/') output.pop_back();
  return output;
}

static std::string pathIdentity(const std::string &input) {
  return foldedKey(normalizeLocationPath(input));
}

static constexpr const char *UNAVAILABLE_USB_PREFIX="unavailable_usb:/";

static bool decodeUnavailableUsbSource(const std::string &source,std::string *stableId,
                                       std::string *relative) {
  if(source.rfind(UNAVAILABLE_USB_PREFIX,0)!=0)return false;
  const size_t start=strlen(UNAVAILABLE_USB_PREFIX),slash=source.find('/',start);
  const std::string id=source.substr(start,slash==std::string::npos?std::string::npos:slash-start);
  if(id.empty()||id.find(':')!=std::string::npos)return false;
  if(stableId)*stableId=id;
  if(relative)*relative=slash==std::string::npos?std::string{}:source.substr(slash);
  return true;
}

static std::string unavailableUsbSource(const std::string &stableId,const std::string &relative) {
  std::string result=std::string(UNAVAILABLE_USB_PREFIX)+stableId;
  if(!relative.empty()&&relative.front()!='/')result+='/';
  result+=relative;
  return normalizeLocationPath(result);
}

static std::vector<std::string> loadGameSources() {
  std::vector<std::string> paths;
  if(storeHas(g_global,"Wrapper/GamePathCount")){
    int count=std::max(0,std::min(16,atoi(storeGet(g_global,"Wrapper/GamePathCount","0"))));
    for(int i=0;i<count;i++){
      std::string key="Wrapper/GamePath"+std::to_string(i);
      std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
      const std::string stableId=storeGet(g_global,("Wrapper/GamePathStable"+std::to_string(i)).c_str(),"");
      if(!stableId.empty()){
        const std::string root=SwitchStorage::ResolveUsbPath(stableId);
        if(!root.empty()) path=normalizeLocationPath(root+storeGet(g_global,("Wrapper/GamePathRelative"+std::to_string(i)).c_str(),""));
        else path=unavailableUsbSource(stableId,storeGet(g_global,("Wrapper/GamePathRelative"+std::to_string(i)).c_str(),""));
      }
      if(!path.empty()) paths.push_back(std::move(path));
    }
  } else {
    std::string legacy=normalizeLocationPath(storeGet(g_global,"Wrapper/GameDir",DEF_GAMEDIR));
    if(!legacy.empty()) paths.push_back(std::move(legacy));
  }
  std::unordered_set<std::string> seen;
  paths.erase(std::remove_if(paths.begin(),paths.end(),[&](const std::string &path){
    return !seen.insert(pathIdentity(path)).second;
  }),paths.end());
  return paths;
}

static void saveGameSources(const std::vector<std::string> &input) {
  struct PersistedBinding{std::string id,relative,path;};
  std::vector<PersistedBinding> persisted;
  const int oldCount=std::max(0,std::min(16,atoi(storeGet(g_global,"Wrapper/GamePathCount","0"))));
  persisted.reserve(oldCount);
  for(int i=0;i<oldCount;i++){
    PersistedBinding binding;
    binding.path=normalizeLocationPath(storeGet(g_global,("Wrapper/GamePath"+std::to_string(i)).c_str(),""));
    binding.id=storeGet(g_global,("Wrapper/GamePathStable"+std::to_string(i)).c_str(),"");
    binding.relative=storeGet(g_global,("Wrapper/GamePathRelative"+std::to_string(i)).c_str(),"");
    if(!binding.id.empty())persisted.emplace_back(std::move(binding));
  }
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty() && seen.insert(pathIdentity(path)).second && paths.size()<16) paths.push_back(std::move(path));
  }
  struct Binding{std::string id,relative;};
  std::vector<Binding> bindings(paths.size());
  const auto usbLocations=SwitchStorage::ListUsbLocations();
  for(size_t i=0;i<paths.size();i++){
    if(decodeUnavailableUsbSource(paths[i],&bindings[i].id,&bindings[i].relative))continue;
    for(const auto &location:usbLocations){
      const std::string root=normalizeLocationPath(location.path);
      if(pathAtOrBelow(paths[i],root)){
        bindings[i].id=location.id;
        bindings[i].relative=paths[i].substr(root.size());
        break;
      }
    }
    if(!bindings[i].id.empty())continue;
    // Saving while USB is still initializing must not erase a known stable
    // binding. Match the exact previously resolved path only; never transfer
    // a binding between arbitrary paths merely because an alias is similar.
    for(const auto &old:persisted)if(pathIdentity(old.path)==pathIdentity(paths[i])){
      bindings[i].id=old.id;bindings[i].relative=old.relative;break;
    }
  }
  storeRemovePrefix(g_global,"Wrapper/GamePath");
  storeSet(g_global,"Wrapper/GamePathCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Wrapper/GamePath"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
    if(!bindings[i].id.empty()){
      storeSet(g_global,("Wrapper/GamePathStable"+std::to_string(i)).c_str(),bindings[i].id.c_str());
      storeSet(g_global,("Wrapper/GamePathRelative"+std::to_string(i)).c_str(),bindings[i].relative.c_str());
    }
  }
  storeRemove(g_global,"Wrapper/GameDir");
}

static std::vector<std::string> loadFavoriteFolders() {
  std::vector<std::string> paths;
  int count=std::max(0,std::min(24,atoi(storeGet(g_global,"Browser/FavoriteCount","0"))));
  std::unordered_set<std::string> seen;
  for(int i=0;i<count;i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    std::string path=normalizeLocationPath(storeGet(g_global,key.c_str(),""));
    if(!path.empty() && seen.insert(pathIdentity(path)).second) paths.push_back(std::move(path));
  }
  return paths;
}

static void saveFavoriteFolders(const std::vector<std::string> &input) {
  std::vector<std::string> paths;
  std::unordered_set<std::string> seen;
  for(const auto &entry:input){
    std::string path=normalizeLocationPath(entry);
    if(!path.empty()&&seen.insert(pathIdentity(path)).second&&paths.size()<24) paths.push_back(std::move(path));
  }
  storeRemovePrefix(g_global,"Browser/Favorite");
  storeSet(g_global,"Browser/FavoriteCount",std::to_string(paths.size()).c_str());
  for(size_t i=0;i<paths.size();i++){
    std::string key="Browser/Favorite"+std::to_string(i);
    storeSet(g_global,key.c_str(),paths[i].c_str());
  }
  storeSave(g_global,LAUNCHER_INI);
}

static std::vector<SwitchStorage::SmbShare> loadSmbSharesFromStore() {
  std::vector<SwitchStorage::SmbShare> shares;
  std::unordered_set<std::string> ids;
  int count=std::max(0,std::min(8,atoi(storeGet(g_global,"Storage/SmbCount","0"))));
  for(int i=0;i<count;i++){
    std::string prefix="Storage/Smb"+std::to_string(i);
    SwitchStorage::SmbShare share;
    share.id=storeGet(g_global,(prefix+"Id").c_str(),"");
    share.name=storeGet(g_global,(prefix+"Name").c_str(),"");
    share.server=storeGet(g_global,(prefix+"Server").c_str(),"");
    share.share=storeGet(g_global,(prefix+"Share").c_str(),"");
    share.path=storeGet(g_global,(prefix+"Path").c_str(),"");
    share.user=storeGet(g_global,(prefix+"User").c_str(),"");
    share.password=storeGet(g_global,(prefix+"Password").c_str(),"");
    share.domain=storeGet(g_global,(prefix+"Domain").c_str(),"");
    const char *automatic=storeGet(g_global,(prefix+"AutoMount").c_str(),"true");
    share.autoMount=!strcmp(automatic,"true")||!strcmp(automatic,"1");
    if(!SwitchStorage::SmbRootPath(share.id).empty()&&!share.server.empty()&&!share.share.empty()&&ids.insert(share.id).second)
      shares.push_back(std::move(share));
  }
  return shares;
}

static void saveSmbShares(const std::vector<SwitchStorage::SmbShare> &shares) {
  storeRemovePrefix(g_global,"Storage/Smb");
  storeSet(g_global,"Storage/SmbCount",std::to_string(shares.size()).c_str());
  for(size_t i=0;i<shares.size();i++){
    const auto &share=shares[i]; std::string prefix="Storage/Smb"+std::to_string(i);
    storeSet(g_global,(prefix+"Id").c_str(),share.id.c_str());
    storeSet(g_global,(prefix+"Name").c_str(),share.name.c_str());
    storeSet(g_global,(prefix+"Server").c_str(),share.server.c_str());
    storeSet(g_global,(prefix+"Share").c_str(),share.share.c_str());
    storeSet(g_global,(prefix+"Path").c_str(),share.path.c_str());
    storeSet(g_global,(prefix+"User").c_str(),share.user.c_str());
    storeSet(g_global,(prefix+"Password").c_str(),share.password.c_str());
    storeSet(g_global,(prefix+"Domain").c_str(),share.domain.c_str());
    storeSet(g_global,(prefix+"AutoMount").c_str(),share.autoMount?"true":"false");
  }
  storeSave(g_global,LAUNCHER_INI);
}

static bool isJunkToken(const std::string &tok) {
  std::string l;
  for (char c : tok) l += (char)tolower((unsigned char)c);
  static const char *junk[] = {
    "pal","ntsc","ntsc-u","ntsc-j","ntscu","ntscj","usa","us","europe","eu","japan","jp","jpn",
    "world","korea","asia","multi","multi3","multi5","proper","unl","demo","beta","prototype","enfrespt",
  };
  for (auto j : junk) if (l == j) return true;
  if (l.size() >= 2 && l[0] == 'v' && isdigit((unsigned char)l[1])) return true;
  return false;
}
static std::string cleanTitle(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o; int depth = 0;
  for (char c : s) {
    if (c == '(' || c == '[' || c == '{') depth++;
    else if (c == ')' || c == ']' || c == '}') { if (depth) depth--; }
    else if (!depth) o += (c == '_') ? ' ' : c;
  }
  std::string w; bool sp = true;
  for (char c : o) { if (isspace((unsigned char)c)) { if (!sp) w += ' '; sp = true; } else { w += c; sp = false; } }
  o = trim(w);
  std::string filtered;
  for(size_t start=0;start<o.size();){
    size_t end=o.find(' ',start);
    std::string token=o.substr(start,end==std::string::npos?std::string::npos:end-start);
    if(foldedKey(token)!="enfrespt"){
      if(!filtered.empty()) filtered+=' ';
      filtered+=token;
    }
    if(end==std::string::npos) break;
    start=end+1;
  }
  o=std::move(filtered);
  for (;;) {
    size_t p = o.find_last_of(" -");
    std::string last = (p == std::string::npos) ? o : o.substr(p + 1);
    if (!last.empty() && isJunkToken(last) && p != std::string::npos) {
      o = trim(o.substr(0, p));
      while (!o.empty() && (o.back() == '-' || o.back() == ' ' || o.back() == '.')) o.pop_back();
    } else break;
  }
  return trim(o);
}
static std::string sanitize(const std::string &file) {
  std::string s = file;
  size_t dot = s.find_last_of('.');
  if (dot != std::string::npos) s = s.substr(0, dot);
  std::string o;
  for (char c : s) o += (isalnum((unsigned char)c) || c=='-'||c=='_') ? c : '_';
  return o;
}

static std::string foldedKey(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return key;
}

static bool readNdsMetadata(const std::string &path, std::string &title,
                            std::string &code) {
  const char *extension = strrchr(path.c_str(), '.');
  if (!extension || strcasecmp(extension, ".nds")) return false;
  unsigned char header[16] = {};
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) return false;
  const bool ok = fread(header, 1, sizeof(header), file) == sizeof(header);
  fclose(file);
  if (!ok) return false;

  title.clear();
  for (int index = 0; index < 12 && header[index]; index++) {
    const unsigned char character = header[index];
    title += (character >= 32 && character < 127) ? (char)character : ' ';
  }
  title = trim(title);
  while (!title.empty() && title.back() == ' ') title.pop_back();

  code.clear();
  for (int index = 12; index < 16; index++) {
    const unsigned char character = header[index];
    if (!isalnum(character)) { code.clear(); break; }
    code += (char)toupper(character);
  }
  return !title.empty() || code.size() == 4;
}

static std::string makeGameKey(const std::string &file, const std::string &path,
                               const std::string &code) {
  std::string base = code.size() == 4 ? sanitize(code) : sanitize(file);
  if (base.empty()) base = "game";
  if (base.size() > 80) base.resize(80);

  uint64_t hash = 1469598103934665603ULL;
  for (unsigned char c : pathIdentity(path)) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long)hash);
  return base + suffix;
}

static uint64_t fingerprintGameFile(const std::string &path, const struct stat &st) {
  uint64_t hash=1469598103934665603ULL;
  auto feed=[&](const void *data,size_t size){
    const auto *bytes=static_cast<const unsigned char*>(data);
    for(size_t i=0;i<size;i++){ hash^=bytes[i]; hash*=1099511628211ULL; }
  };
  const uint64_t size=static_cast<uint64_t>(st.st_size);
  feed(&size,sizeof(size));
  FILE *file=fopen(path.c_str(),"rb");
  if(!file) return hash;
  std::array<unsigned char,65536> buffer{};
  size_t read=fread(buffer.data(),1,buffer.size(),file);
  feed(buffer.data(),read);
  if(st.st_size>static_cast<off_t>(buffer.size())&&
     fseeko(file,st.st_size-static_cast<off_t>(buffer.size()),SEEK_SET)==0){
    read=fread(buffer.data(),1,buffer.size(),file);
    feed(buffer.data(),read);
  }
  fclose(file);
  return hash;
}

static std::string hexFingerprint(uint64_t fingerprint){char value[24]{};snprintf(value,sizeof(value),"%016llx",(unsigned long long)fingerprint);return value;}
static std::string stableGameKey(const std::string &code,uint64_t fingerprint);

static std::string canonicalGamePath(const std::string &input){
  const std::string path=normalizeLocationPath(input);
  for(const auto &location:SwitchStorage::ListUsbLocations()){
    const std::string root=normalizeLocationPath(location.path);
    if(!pathAtOrBelow(path,root))continue;
    std::string relative=path.substr(std::min(path.size(),root.size()));while(!relative.empty()&&relative.front()=='/')relative.erase(relative.begin());
    return "usb:"+foldedKey(location.id)+"/"+foldedKey(relative);
  }
  for(const auto &share:SwitchStorage::LoadSmbShares(LAUNCHER_INI)){
    const std::string root=normalizeLocationPath(SwitchStorage::SmbRootPath(share.id));
    if(root.empty()||!pathAtOrBelow(path,root))continue;
    std::string relative=path.substr(std::min(path.size(),root.size()));while(!relative.empty()&&relative.front()=='/')relative.erase(relative.begin());
    return "smb:"+foldedKey(share.id)+"/"+foldedKey(relative);
  }
  return pathIdentity(path);
}

static std::string identityScope(const std::string &canonical){
  if(canonical.rfind("usb:",0)==0||canonical.rfind("smb:",0)==0){const size_t slash=canonical.find('/',4);return canonical.substr(0,slash);}
  const size_t colon=canonical.find(':');return colon==std::string::npos?std::string{}:canonical.substr(0,colon+1);
}

static void rememberPreviousPath(LibraryIdentityRecord &record,const std::string &path){
  const std::string normalized=normalizeLocationPath(path);if(normalized.empty())return;
  record.previousPaths.erase(std::remove_if(record.previousPaths.begin(),record.previousPaths.end(),[&](const std::string &entry){return pathIdentity(entry)==pathIdentity(normalized);}),record.previousPaths.end());
  record.previousPaths.push_back(normalized);if(record.previousPaths.size()>MAX_PREVIOUS_LIBRARY_PATHS)record.previousPaths.erase(record.previousPaths.begin(),record.previousPaths.end()-MAX_PREVIOUS_LIBRARY_PATHS);
}

static bool loadLibraryIdentities(){
  g_libraryIdentities.clear();std::unordered_set<std::string> ids;
  const bool migrateFromGlobal=!storeHas(g_metadata,"Library/IdentityCount")&&
                               storeHas(g_global,"Library/IdentityCount");
  Store &identityStore=migrateFromGlobal?g_global:g_metadata;
  const int count=std::clamp(atoi(storeGet(identityStore,"Library/IdentityCount","0")),0,16384);
  for(int index=0;index<count;index++){
    const std::string prefix="Library/Identity"+std::to_string(index);LibraryIdentityRecord record;
    record.id=storeGet(identityStore,(prefix+"Id").c_str(),"");record.fingerprint=storeGet(identityStore,(prefix+"Fingerprint").c_str(),"");record.baseIdentity=storeGet(identityStore,(prefix+"BaseIdentity").c_str(),"");record.canonicalPath=storeGet(identityStore,(prefix+"Path").c_str(),"");record.currentPath=storeGet(identityStore,(prefix+"CurrentPath").c_str(),"");record.retired=!strcmp(storeGet(identityStore,(prefix+"Retired").c_str(),"false"),"true");
    const int previous=std::clamp(atoi(storeGet(identityStore,(prefix+"PreviousPathCount").c_str(),"0")),0,(int)MAX_PREVIOUS_LIBRARY_PATHS);
    for(int item=0;item<previous;item++){std::string path=normalizeLocationPath(storeGet(identityStore,(prefix+"PreviousPath"+std::to_string(item)).c_str(),""));if(!path.empty())record.previousPaths.push_back(std::move(path));}
    const bool valid=!record.id.empty()&&record.id.size()<=96&&std::all_of(record.id.begin(),record.id.end(),[](unsigned char c){return std::isalnum(c)||c=='-'||c=='_';});
    if(valid&&!record.fingerprint.empty()&&ids.insert(record.id).second)g_libraryIdentities.push_back(std::move(record));
  }
  return migrateFromGlobal;
}

static void saveLibraryIdentities(){
  storeRemovePrefix(g_global,"Library/Identity");
  storeRemovePrefix(g_metadata,"Library/Identity");storeSet(g_metadata,"Library/IdentityCount",std::to_string(g_libraryIdentities.size()).c_str());
  for(size_t index=0;index<g_libraryIdentities.size();index++){
    const auto &record=g_libraryIdentities[index];const std::string prefix="Library/Identity"+std::to_string(index);
    storeSet(g_metadata,(prefix+"Id").c_str(),record.id.c_str());storeSet(g_metadata,(prefix+"Fingerprint").c_str(),record.fingerprint.c_str());storeSet(g_metadata,(prefix+"BaseIdentity").c_str(),record.baseIdentity.c_str());storeSet(g_metadata,(prefix+"Path").c_str(),record.canonicalPath.c_str());storeSet(g_metadata,(prefix+"CurrentPath").c_str(),record.currentPath.c_str());storeSet(g_metadata,(prefix+"Retired").c_str(),record.retired?"true":"false");storeSet(g_metadata,(prefix+"PreviousPathCount").c_str(),std::to_string(record.previousPaths.size()).c_str());
    for(size_t item=0;item<record.previousPaths.size();item++)storeSet(g_metadata,(prefix+"PreviousPath"+std::to_string(item)).c_str(),record.previousPaths[item].c_str());
  }
  g_libraryIdentitiesDirty=false;
}

static bool identityPathExists(const LibraryIdentityRecord &record){struct stat info{};return !record.retired&&!record.currentPath.empty()&&stat(record.currentPath.c_str(),&info)==0&&S_ISREG(info.st_mode)&&canonicalGamePath(record.currentPath)==record.canonicalPath;}

static void assignStableIdentity(Game &game){
  game.canonicalPath=canonicalGamePath(game.path);game.baseIdentity=game.gameCode.size()==4?"nds:"+foldedKey(game.gameCode):"anonymous:"+hexFingerprint(game.fingerprint);
  const std::string fingerprint=hexFingerprint(game.fingerprint);auto compatible=[&](const LibraryIdentityRecord &record){return !record.baseIdentity.empty()?record.baseIdentity==game.baseIdentity:record.fingerprint==fingerprint;};
  LibraryIdentityRecord *match=nullptr;bool changed=false;
  for(auto &record:g_libraryIdentities){
    if(record.retired||g_claimedLibraryIds.count(record.id)||record.canonicalPath!=game.canonicalPath)continue;
    if(compatible(record)){match=&record;break;}
    game.allowLegacyMigration=false;rememberPreviousPath(record,record.currentPath);record.currentPath.clear();record.retired=true;g_reservedLibraryIds.erase(record.id);changed=true;
  }
  if(!match){const std::string scope=identityScope(game.canonicalPath);for(auto &record:g_libraryIdentities){if(g_claimedLibraryIds.count(record.id)||scope.empty()||identityScope(record.canonicalPath)!=scope||record.fingerprint!=fingerprint||!compatible(record))continue;if(g_reservedLibraryIds.count(record.id)){if(identityPathExists(record))continue;g_reservedLibraryIds.erase(record.id);}match=&record;break;}}
  if(!match){
    std::string stem=stableGameKey(game.gameCode,game.fingerprint),id=stem;unsigned collision=1;
    auto exists=[&](const std::string &candidate){return std::any_of(g_libraryIdentities.begin(),g_libraryIdentities.end(),[&](const auto &record){return record.id==candidate;});};while(exists(id))id=stem+"-"+std::to_string(++collision);
    g_libraryIdentities.push_back({id,fingerprint,game.baseIdentity,game.canonicalPath,normalizeLocationPath(game.path),{},false});match=&g_libraryIdentities.back();changed=true;
  }else{
    const std::string current=normalizeLocationPath(game.path);if(!match->currentPath.empty()&&pathIdentity(match->currentPath)!=pathIdentity(current))rememberPreviousPath(*match,match->currentPath);
    changed|=match->fingerprint!=fingerprint||match->baseIdentity!=game.baseIdentity||match->canonicalPath!=game.canonicalPath||match->currentPath!=current||match->retired;
    match->fingerprint=fingerprint;match->baseIdentity=game.baseIdentity;match->canonicalPath=game.canonicalPath;match->currentPath=current;match->retired=false;
  }
  g_libraryIdentitiesDirty|=changed;game.key=match->id;g_reservedLibraryIds.erase(game.key);g_claimedLibraryIds.insert(game.key);
}

static std::string stableGameKey(const std::string &code,uint64_t fingerprint) {
  char key[80]{};
  if(code.size()==4)
    snprintf(key,sizeof(key),"nds-%s-%016llx",sanitize(code).c_str(),
             (unsigned long long)fingerprint);
  else
    snprintf(key,sizeof(key),"content-%016llx",(unsigned long long)fingerprint);
  return key;
}

static void migrateIdentityFile(const char *directory,const std::string &oldKey,
                                const std::string &newKey,const char *extension) {
  if(oldKey.empty()||oldKey==newKey) return;
  const std::string oldPath=std::string(directory)+"/"+oldKey+extension;
  const std::string newPath=std::string(directory)+"/"+newKey+extension;
  struct stat oldInfo{},newInfo{};
  if(stat(oldPath.c_str(),&oldInfo)!=0||stat(newPath.c_str(),&newInfo)==0) return;
  (void)rename(oldPath.c_str(),newPath.c_str());
}

static void migrateGameIdentity(Game &game) {
  if(!game.allowLegacyMigration)return;
  const std::array<std::string,2> oldKeys{game.pathKey,game.legacyUnique?game.legacyKey:std::string{}};
  for(const std::string &oldKey:oldKeys){
    if(oldKey.empty()||oldKey==game.key) continue;
    if(!storeGet(g_titles,game.key.c_str(),"")[0]){
      const char *value=storeGet(g_titles,oldKey.c_str(),"");
      if(value[0]) storeSet(g_titles,game.key.c_str(),value);
    }
    if(!storeGet(g_recent,game.key.c_str(),"")[0]){
      const char *value=storeGet(g_recent,oldKey.c_str(),"");
      if(value[0]) storeSet(g_recent,game.key.c_str(),value);
    }
    migrateIdentityFile(COVERS_DIR,oldKey,game.key,".png");
    migrateIdentityFile(GAMECFG_DIR,oldKey,game.key,".ini");
  }
}

static const char *gameStoreGet(Store &store, const Game &game, const char *def) {
  const char *value = storeGet(store, game.key.c_str(), "");
  if(*value) return value;
  value=storeGet(store,game.pathKey.c_str(),"");
  if(*value) return value;
  if(game.legacyUnique&&!game.legacyKey.empty()){
    value=storeGet(store,game.legacyKey.c_str(),"");
    if(*value) return value;
  }
  return def;
}

static bool gameFileExists(const char *dir, const Game &game, const char *extension) {
  if (regularFileExists(std::string(dir) + "/" + game.key + extension))
    return true;
  if (!game.pathKey.empty() && regularFileExists(std::string(dir) + "/" + game.pathKey + extension))
    return true;
  return game.legacyUnique && !game.legacyKey.empty() &&
         regularFileExists(std::string(dir) + "/" + game.legacyKey + extension);
}

[[maybe_unused]] static void scanGames(const std::vector<std::string> &sourcePaths) {
  for (auto &g : g_games) if (g.cover) SDL_DestroyTexture(g.cover);
  g_games.clear();
  g_coverUseSerial = 0;
  Store refreshedMetadata;
  std::unordered_set<std::string> seenPaths;
  for (const auto &source : sourcePaths) {
    DIR *d = opendir(source.c_str());
    if(!d) continue;
    struct dirent *e;
    while ((e = readdir(d))) {
      if(e->d_name[0]=='.') continue;
      std::string full = join(source, e->d_name);
      struct stat sst{};
      if (stat(full.c_str(), &sst) != 0 || !S_ISREG(sst.st_mode) || !hasGameExtension(e->d_name)) continue;
      if(!seenPaths.insert(pathIdentity(full)).second) continue;
      Game g;
      g.file = e->d_name;
      g.path = full;
      g.legacyKey = sanitize(g.file);
      readNdsMetadata(full,g.headerTitle,g.gameCode);
      g.pathKey = makeGameKey(g.file, full, g.gameCode);
      g.key = g.pathKey;
      g.added = (long long)sst.st_mtime;
      g.modified = g.added;
      g.fileSize = (long long)sst.st_size;
      const char *cached=storeGet(g_metadata,g.pathKey.c_str(),"");
      long long cachedSize=0,cachedTime=0;
      unsigned long long cachedFingerprint=0;
      int consumed=0;
      if(sscanf(cached,"%lld,%lld,%llx%n",&cachedSize,&cachedTime,
                &cachedFingerprint,&consumed)==3&&cached[consumed]==0&&
         cachedSize==g.fileSize&&cachedTime==g.modified)
        g.fingerprint=(uint64_t)cachedFingerprint;
      if(!g.fingerprint) g.fingerprint=fingerprintGameFile(full,sst);
      g.key=stableGameKey(g.gameCode,g.fingerprint);
      char metadata[96];
      snprintf(metadata,sizeof(metadata),"%lld,%lld,%016llx",g.fileSize,g.modified,
               (unsigned long long)g.fingerprint);
      storeSet(refreshedMetadata,g.pathKey.c_str(),metadata);
      g_games.push_back(std::move(g));
    }
    closedir(d);
  }

  std::map<std::string, size_t> legacyCounts;
  for (const auto &game : g_games)
    legacyCounts[foldedKey(game.legacyKey)]++;
  for (auto &game : g_games) {
    game.legacyUnique = legacyCounts[foldedKey(game.legacyKey)] == 1;
    migrateGameIdentity(game);
    const char *customTitle = gameStoreGet(g_titles, game, "");
    const std::string filenameTitle = cleanTitle(game.file);
    game.title = *customTitle ? customTitle :
                 (!filenameTitle.empty() ? filenameTitle : game.headerTitle);
    game.region = detectRegion(game.gameCode, game.file);
    game.played = atoll(gameStoreGet(g_recent, game, "0"));
    game.hasCfg = gameFileExists(GAMECFG_DIR, game, ".ini");
  }
  g_metadata=std::move(refreshedMetadata);
  saveLibraryIdentities();
  storeSave(g_metadata,METADATA_INI);
  storeSave(g_titles,TITLES_INI);
  storeSave(g_recent,RECENT_INI);
  applySort();
}

struct LibraryScanState {
  std::mutex mutex;
  std::deque<Game> ready;
  Store refreshedMetadata;
  std::atomic<bool> cancel{false};
  std::atomic<bool> done{false};
  bool replace=true;
  bool cleared=false;
  size_t unsortedPublished=0;
  std::vector<std::string> sources;
  std::unordered_map<std::string,std::string> sourceStableIds;
  std::vector<std::string> completedSources;
  std::unordered_set<std::string> foundPaths;
  std::thread worker;
};
static std::shared_ptr<LibraryScanState> g_libraryScan;

static void wakeUiFromWorker(int code){
  if(!g_sdlReady)return;
  SDL_Event event{};event.type=SDL_USEREVENT;event.user.code=code;SDL_PushEvent(&event);
}
static void usbStatusWake(void*){wakeUiFromWorker(0x55534248);}

static void libraryScanWorker(const std::shared_ptr<LibraryScanState> &state,
                              std::vector<std::string> sources,Store titles,
                              Store recent,Store metadata){
  std::unordered_set<std::string> seenPaths;
  auto publish=[&](Game game){
    if(state->cancel.load())return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->ready.emplace_back(std::move(game));
    if(state->ready.size()==1||state->ready.size()%8==0)wakeUiFromWorker(0x5343414e);
  };
  for(const std::string &source:sources){
    if(state->cancel.load())break;
    DIR *directory=opendir(source.c_str());if(!directory)continue;
    state->completedSources.push_back(normalizeLocationPath(source));
    while(!state->cancel.load()){
      dirent *entry=readdir(directory);if(!entry)break;
      if(entry->d_name[0]=='.'||!hasGameExtension(entry->d_name))continue;
      const std::string full=join(source,entry->d_name);
      struct stat info{};
      if(stat(full.c_str(),&info)!=0||!S_ISREG(info.st_mode)||!seenPaths.insert(pathIdentity(full)).second)continue;
      Game game;game.file=entry->d_name;game.path=full;game.legacyKey=sanitize(game.file);
      const auto sourceId=state->sourceStableIds.find(pathIdentity(source));
      if(sourceId!=state->sourceStableIds.end())game.sourceStableId=sourceId->second;
      readNdsMetadata(full,game.headerTitle,game.gameCode);
      game.pathKey=makeGameKey(game.file,full,game.gameCode);
      game.added=(long long)info.st_mtime;game.modified=game.added;game.fileSize=(long long)info.st_size;
      const char *cached=storeGet(metadata,game.pathKey.c_str(),"");
      long long size=0,mtime=0;unsigned long long fingerprint=0;int consumed=0;
      if(sscanf(cached,"%lld,%lld,%llx%n",&size,&mtime,&fingerprint,&consumed)==3&&cached[consumed]==0&&
         size==game.fileSize&&mtime==game.modified)game.fingerprint=(uint64_t)fingerprint;
      if(!game.fingerprint)game.fingerprint=fingerprintGameFile(full,info);
      game.key=stableGameKey(game.gameCode,game.fingerprint);
      char cache[96];snprintf(cache,sizeof(cache),"%lld,%lld,%016llx",game.fileSize,game.modified,
                              (unsigned long long)game.fingerprint);
      storeSet(state->refreshedMetadata,game.pathKey.c_str(),cache);
      const char *custom=storeGet(titles,game.key.c_str(),"");
      if(!custom[0])custom=storeGet(titles,game.pathKey.c_str(),"");
      if(!custom[0])custom=storeGet(titles,game.legacyKey.c_str(),"");
      const std::string filenameTitle=cleanTitle(game.file);
      game.title=custom[0]?custom:(!filenameTitle.empty()?filenameTitle:game.headerTitle);
      game.region=detectRegion(game.gameCode,game.file);
      const char *played=storeGet(recent,game.key.c_str(),"");
      if(!played[0])played=storeGet(recent,game.pathKey.c_str(),"");
      if(!played[0])played=storeGet(recent,game.legacyKey.c_str(),"0");
      game.played=atoll(played);
      game.hasCfg=regularFileExists(std::string(GAMECFG_DIR)+"/"+game.key+".ini")||
                  regularFileExists(std::string(GAMECFG_DIR)+"/"+game.pathKey+".ini")||
                  regularFileExists(std::string(GAMECFG_DIR)+"/"+game.legacyKey+".ini");
      {std::lock_guard<std::mutex> lock(state->mutex);state->foundPaths.insert(pathIdentity(full));}
      publish(std::move(game));
    }
    closedir(directory);
  }
  state->done=true;wakeUiFromWorker(0x5343414e);
}

static void stopGameScan(){
  auto state=g_libraryScan;if(!state)return;state->cancel=true;
  if(state->worker.joinable())state->worker.join();
  g_libraryScan.reset();
  if(g_libraryIdentitiesDirty){saveLibraryIdentities();storeSave(g_metadata,METADATA_INI);storeSave(g_global,LAUNCHER_INI);}
}
static void startGameScan(std::vector<std::string> sources,bool replace=true){
  stopGameScan();if(replace)cancelQueuedCoverDecodes();g_reservedLibraryIds.clear();for(const auto &record:g_libraryIdentities)if(!record.retired)g_reservedLibraryIds.insert(record.id);
  g_claimedLibraryIds.clear();if(!replace)for(const Game &game:g_games){const bool targeted=std::any_of(sources.begin(),sources.end(),[&](const std::string &source){return pathAtOrBelow(game.path,source);});if(!targeted)g_claimedLibraryIds.insert(game.key);}
  auto state=std::make_shared<LibraryScanState>();state->replace=replace;state->sources=sources;
  for(const auto &location:SwitchStorage::ListUsbLocations()){
    const std::string root=normalizeLocationPath(location.path);
    for(const std::string &source:sources)if(pathAtOrBelow(source,root))
      state->sourceStableIds[pathIdentity(source)]=location.id;
  }
  sources.erase(std::remove_if(sources.begin(),sources.end(),[](const std::string &source){return source.rfind(UNAVAILABLE_USB_PREFIX,0)==0;}),sources.end());
  state->worker=std::thread(libraryScanWorker,state,std::move(sources),g_titles,g_recent,g_metadata);
  g_libraryScan=std::move(state);
}
static bool pumpGameScan(){
  auto state=g_libraryScan;if(!state)return false;
  std::vector<Game> batch;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    // Identity migration and per-game file checks run on the SDL thread.
    // Publish only two records per frame so a large SD/SMB library cannot
    // monopolize input/rendering while still filling the first page quickly.
    const size_t count=std::min(size_t{2},state->ready.size());
    for(size_t i=0;i<count;i++){
      batch.emplace_back(std::move(state->ready.front()));
      state->ready.pop_front();
    }
  }
  if(!batch.empty()&&state->replace&&!state->cleared){
    for(Game &game:g_games)if(game.cover)SDL_DestroyTexture(game.cover);
    g_games.clear();state->cleared=true;
  }
  for(Game &game:batch){
    assignStableIdentity(game);
    migrateGameIdentity(game);
    const char *customTitle=gameStoreGet(g_titles,game,"");const std::string filenameTitle=cleanTitle(game.file);game.title=*customTitle?customTitle:(!filenameTitle.empty()?filenameTitle:game.headerTitle);game.played=atoll(gameStoreGet(g_recent,game,"0"));game.hasCfg=gameFileExists(GAMECFG_DIR,game,".ini");
    auto existing=std::find_if(g_games.begin(),g_games.end(),[&](const Game &candidate){return candidate.key==game.key;});
    if(existing==g_games.end())g_games.emplace_back(std::move(game));
    else{const bool metadataUnchanged=existing->fingerprint==game.fingerprint;if(metadataUnchanged){game.cover=existing->cover;game.coverIsRomIcon=existing->coverIsRomIcon;game.triedCover=existing->triedCover;game.coverQueued=existing->coverQueued;game.coverRequest=existing->coverRequest;game.coverAt=existing->coverAt;game.coverUse=existing->coverUse;}else if(existing->cover)SDL_DestroyTexture(existing->cover);*existing=std::move(game);}
  }
  if(!batch.empty()){
    state->unsortedPublished+=batch.size();
    // Keep the first (largest possible) page correctly ordered, then sort in
    // larger chunks. Sorting the whole growing vector after every two games
    // made a large library effectively quadratic during startup.
    if(g_games.size()<=24||state->unsortedPublished>=16){
      applySort();state->unsortedPublished=0;
    }else rebuildLibraryView();
  }
  bool empty=false;{std::lock_guard<std::mutex> lock(state->mutex);empty=state->ready.empty();}
  if(state->done.load()&&empty){
    if(state->worker.joinable())state->worker.join();
    if(state->replace&&!state->cleared){for(Game &game:g_games)if(game.cover)SDL_DestroyTexture(game.cover);g_games.clear();state->cleared=true;}
    if(!state->replace&&!state->completedSources.empty()){
      std::unordered_set<std::string> present;{std::lock_guard<std::mutex> lock(state->mutex);present=state->foundPaths;}
      // Entries from a successfully scanned target root which were not republished are stale.
      g_games.erase(std::remove_if(g_games.begin(),g_games.end(),[&](Game &game){const bool targeted=std::any_of(state->completedSources.begin(),state->completedSources.end(),[&](const std::string &source){return pathAtOrBelow(game.path,source);});if(!targeted||present.count(pathIdentity(game.path)))return false;if(game.cover)SDL_DestroyTexture(game.cover);return true;}),g_games.end());
    }
    std::map<std::string,size_t> counts;for(const Game &game:g_games)counts[foldedKey(game.legacyKey)]++;
    // Stable path-key migration and configuration checks already ran as each
    // record was published. Finalization only needs the now-known basename
    // uniqueness; the legacy lookup fallbacks remain available on demand.
    // Repeating
    // three or more stat() calls per game here caused a visible end-of-scan
    // stall on SD and an even larger one over network-backed paths.
    for(Game &game:g_games)game.legacyUnique=counts[foldedKey(game.legacyKey)]==1;
    if(state->replace)g_metadata=std::move(state->refreshedMetadata);
    else for(const KV &entry:state->refreshedMetadata.kv)storeSet(g_metadata,entry.k.c_str(),entry.v.c_str());
    saveLibraryIdentities();storeSave(g_metadata,METADATA_INI);storeSave(g_titles,TITLES_INI);storeSave(g_recent,RECENT_INI);storeSave(g_global,LAUNCHER_INI);
    applySort();g_libraryScan.reset();
  }
  return !batch.empty();
}
static std::string coverPath(const Game &g) { return std::string(COVERS_DIR) + "/" + g.key + ".png"; }
static std::vector<std::string> coverCandidatePaths(const Game &g) {
  std::vector<std::string> paths;
  paths.emplace_back(coverPath(g));
  if(!g.pathKey.empty()){
    const std::string pathKey=std::string(COVERS_DIR)+"/"+g.pathKey+".png";
    if(pathKey!=paths.front())paths.emplace_back(pathKey);
  }
  if(g.legacyUnique&&!g.legacyKey.empty()){
    const std::string legacy=std::string(COVERS_DIR)+"/"+g.legacyKey+".png";
    if(std::find(paths.begin(),paths.end(),legacy)==paths.end())paths.emplace_back(legacy);
  }
  return paths;
}
static std::string existingCoverPath(const Game &g) {
  const std::vector<std::string> paths=coverCandidatePaths(g);
  for(const std::string &path:paths)if(regularFileExists(path))return path;
  return paths.front();
}

static Game *findGameByKey(const std::string &key) {
  for (auto &game : g_games)
    if (game.key == key || game.pathKey == key) return &game;
  Game *match = nullptr;
  for (auto &game : g_games) {
    if (!game.legacyUnique || game.legacyKey != key)
      continue;
    if (match) return nullptr;
    match = &game;
  }
  return match;
}

static constexpr int COVER_REQUEST_BUDGET = 48;
static constexpr int COVER_UPLOAD_BUDGET = 2;
static constexpr size_t COVER_JOB_LIMIT = 96;
static constexpr size_t COVER_READY_LIMIT = 4;
static int g_cover_budget = 1 << 30;

struct CoverDecodeJob {
  std::string key;
  std::vector<std::string> paths;
  std::string romPath;
  Uint64 request=0;
  Uint64 epoch=0;
};
struct CoverDecodeResult {
  std::string key;
  Uint64 request=0;
  Uint64 epoch=0;
  int width=0,height=0;
  bool isRomIcon=false;
  std::vector<Uint8> pixels;
};
static std::mutex g_coverDecodeMutex;
static std::condition_variable g_coverDecodeCondition;
static std::deque<CoverDecodeJob> g_coverDecodeJobs;
static std::deque<CoverDecodeResult> g_coverDecodeReady;
static std::thread g_coverDecodeWorker;
static bool g_coverDecodeStarted=false,g_coverDecodeStop=false;
static Uint64 g_coverDecodeEpoch=1,g_coverRequestSerial=0;

static bool decodeNdsBannerIcon(const std::string &path,
                                std::vector<Uint8> &pixels) {
  const char *extension=strrchr(path.c_str(),'.');
  if(!extension||strcasecmp(extension,".nds"))return false;
  std::unique_ptr<FILE,decltype(&fclose)> file(fopen(path.c_str(),"rb"),fclose);
  if(!file||fseek(file.get(),0x68,SEEK_SET)!=0)return false;
  Uint8 offsetBytes[4]={};
  if(fread(offsetBytes,1,sizeof(offsetBytes),file.get())!=sizeof(offsetBytes))return false;
  const Uint32 bannerOffset=(Uint32)offsetBytes[0]|((Uint32)offsetBytes[1]<<8)|
      ((Uint32)offsetBytes[2]<<16)|((Uint32)offsetBytes[3]<<24);
  if(bannerOffset<0x200||fseek(file.get(),(long)bannerOffset,SEEK_SET)!=0)return false;
  std::array<Uint8,0x240> banner{};
  if(fread(banner.data(),1,banner.size(),file.get())!=banner.size())return false;
  const Uint16 version=(Uint16)banner[0]|((Uint16)banner[1]<<8);
  if(version!=1&&version!=2&&version!=3&&version!=0x103)return false;

  constexpr int width=32,height=32;
  pixels.assign((size_t)width*height*4,0);
  bool visible=false;
  for(int tileY=0;tileY<4;tileY++)for(int tileX=0;tileX<4;tileX++){
    const size_t tileOffset=0x20+(size_t)(tileY*4+tileX)*32;
    for(int row=0;row<8;row++)for(int column=0;column<8;column++){
      const Uint8 packed=banner[tileOffset+(size_t)row*4+(size_t)column/2];
      const Uint8 paletteIndex=(column&1)?packed>>4:packed&0x0f;
      const int x=tileX*8+column,y=tileY*8+row;
      Uint8 *destination=pixels.data()+((size_t)y*width+x)*4;
      if(!paletteIndex)continue;
      const size_t paletteOffset=0x220+(size_t)paletteIndex*2;
      const Uint16 color=(Uint16)banner[paletteOffset]|
          ((Uint16)banner[paletteOffset+1]<<8);
      destination[0]=(Uint8)(((color&0x1f)*255+15)/31);
      destination[1]=(Uint8)((((color>>5)&0x1f)*255+15)/31);
      destination[2]=(Uint8)((((color>>10)&0x1f)*255+15)/31);
      destination[3]=255;visible=true;
    }
  }
  if(!visible)pixels.clear();
  return visible;
}

static CoverDecodeResult decodeCover(const CoverDecodeJob &job) {
  CoverDecodeResult result;result.key=job.key;result.request=job.request;result.epoch=job.epoch;
  // All cover filesystem probing stays on this worker.  A page change only
  // queues candidate names and never blocks rendering on SD stat/open calls.
  SDL_Surface *source=nullptr;
  for(const std::string &path:job.paths){
    source=IMG_Load(path.c_str());
    if(source)break;
  }
  if(!source&&decodeNdsBannerIcon(job.romPath,result.pixels)){
    result.width=32;result.height=32;result.isRomIcon=true;
    return result;
  }
  if(!source||source->w<1||source->h<1||source->w>8192||source->h>8192||
     (Uint64)source->w*(Uint64)source->h>16ull*1024*1024){
    if(source)SDL_FreeSurface(source);
    return result;
  }
  constexpr int maxWidth=360,maxHeight=540;
  int width=source->w,height=source->h;
  if(width>maxWidth){height=(int)((long long)height*maxWidth/width);width=maxWidth;}
  if(height>maxHeight){width=(int)((long long)width*maxHeight/height);height=maxHeight;}
  width=std::max(1,width);height=std::max(1,height);
  SDL_Surface *rgba=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
  if(!rgba){SDL_FreeSurface(source);return result;}
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  const bool converted=SDL_BlitScaled(source,nullptr,rgba,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);SDL_FreeSurface(source);
  if(!converted){SDL_FreeSurface(rgba);return result;}
  const bool mustLock=SDL_MUSTLOCK(rgba);
  if(mustLock&&SDL_LockSurface(rgba)!=0){SDL_FreeSurface(rgba);return result;}
  result.pixels.resize((size_t)width*(size_t)height*4);
  for(int row=0;row<height;row++)
    memcpy(result.pixels.data()+(size_t)row*(size_t)width*4,
           (const Uint8*)rgba->pixels+(size_t)row*(size_t)rgba->pitch,
           (size_t)width*4);
  if(mustLock)SDL_UnlockSurface(rgba);
  SDL_FreeSurface(rgba);result.width=width;result.height=height;
  return result;
}

static void coverDecodeThread() {
  for(;;){
    CoverDecodeJob job;
    {
      std::unique_lock<std::mutex> lock(g_coverDecodeMutex);
      g_coverDecodeCondition.wait(lock,[]{return g_coverDecodeStop||
          (!g_coverDecodeJobs.empty()&&g_coverDecodeReady.size()<COVER_READY_LIMIT);});
      if(g_coverDecodeStop)return;
      job=std::move(g_coverDecodeJobs.front());g_coverDecodeJobs.pop_front();
    }
    CoverDecodeResult result=decodeCover(job);bool publish=false;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(!g_coverDecodeStop&&job.epoch==g_coverDecodeEpoch){
        g_coverDecodeReady.emplace_back(std::move(result));publish=true;
      }
    }
    if(publish)wakeUiFromWorker(0x434f5652);
  }
}

static void startCoverDecodeWorker() {
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
  if(g_coverDecodeStarted)return;
  g_coverDecodeStop=false;g_coverDecodeStarted=true;
  g_coverDecodeWorker=std::thread(coverDecodeThread);
}

static void stopCoverDecodeWorker() {
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    if(!g_coverDecodeStarted)return;
    g_coverDecodeStop=true;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  g_coverDecodeCondition.notify_all();
  if(g_coverDecodeWorker.joinable())g_coverDecodeWorker.join();
  std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
  g_coverDecodeStarted=false;
}

static void cancelQueuedCoverDecodes() {
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    ++g_coverDecodeEpoch;g_coverDecodeJobs.clear();g_coverDecodeReady.clear();
  }
  for(Game &game:g_games){game.coverQueued=false;game.coverRequest=0;}
  g_coverDecodeCondition.notify_all();
}

static void queueCoverDecode(Game &game,bool priority) {
  if(game.cover||game.triedCover)return;
  if(game.coverQueued){
    if(priority){
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      const auto found=std::find_if(g_coverDecodeJobs.begin(),g_coverDecodeJobs.end(),
          [&](const CoverDecodeJob &job){return job.request==game.coverRequest;});
      if(found!=g_coverDecodeJobs.end()&&found!=g_coverDecodeJobs.begin()){
        CoverDecodeJob job=std::move(*found);g_coverDecodeJobs.erase(found);
        g_coverDecodeJobs.emplace_front(std::move(job));g_coverDecodeCondition.notify_one();
      }
    }
    return;
  }
  if(g_cover_budget<=0)return;
  --g_cover_budget;
  CoverDecodeJob job;job.key=game.key;job.paths=coverCandidatePaths(game);job.romPath=game.path;
  job.request=++g_coverRequestSerial;game.coverRequest=job.request;game.coverQueued=true;
  CoverDecodeJob dropped;bool didDrop=false;
  {
    std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
    job.epoch=g_coverDecodeEpoch;
    if(g_coverDecodeJobs.size()>=COVER_JOB_LIMIT){
      dropped=std::move(g_coverDecodeJobs.back());g_coverDecodeJobs.pop_back();didDrop=true;
    }
    if(priority)g_coverDecodeJobs.emplace_front(std::move(job));
    else g_coverDecodeJobs.emplace_back(std::move(job));
  }
  if(didDrop)if(Game *old=findGameByKey(dropped.key))if(old->coverRequest==dropped.request){old->coverQueued=false;old->coverRequest=0;}
  g_coverDecodeCondition.notify_one();
}

static void touchCover(Game &g) {
  if (g.cover) g.coverUse = ++g_coverUseSerial;
}

static void evictLeastRecentlyUsedCover() {
  Game *victim = nullptr;
  for (auto &candidate : g_games)
    if (candidate.cover && (!victim || candidate.coverUse < victim->coverUse)) victim = &candidate;
  if (!victim) return;
  SDL_DestroyTexture(victim->cover);
  victim->cover = nullptr;
  victim->coverIsRomIcon = false;
  victim->coverUse = 0;
  victim->triedCover = false;
}

static void installCover(Game &g, SDL_Texture *cover, bool isRomIcon) {
  if (!cover) return;
  size_t resident = 0;
  for (const auto &candidate : g_games) if (candidate.cover) resident++;
  if (resident >= COVER_CACHE_LIMIT) evictLeastRecentlyUsedCover();
  g.cover = cover;
  g.coverIsRomIcon = isRomIcon;
  g.coverAt = SDL_GetTicks();
  touchCover(g);
}

static SDL_Texture *uploadCoverTexture(const CoverDecodeResult &result) {
  if(result.width<1||result.height<1||result.pixels.empty()||!g_ren)return nullptr;
  SDL_Texture *texture=SDL_CreateTexture(g_ren,SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STATIC,
                                        result.width,result.height);
  if(texture&&SDL_UpdateTexture(texture,nullptr,result.pixels.data(),result.width*4)!=0){
    SDL_DestroyTexture(texture);texture=nullptr;
  }
  if(!texture){
    SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<Uint8*>(result.pixels.data()),result.width,result.height,
        32,result.width*4,SDL_PIXELFORMAT_RGBA32);
    if(surface){texture=SDL_CreateTextureFromSurface(g_ren,surface);SDL_FreeSurface(surface);}
  }
  if(texture)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  if(texture&&result.isRomIcon)SDL_SetTextureScaleMode(texture,SDL_ScaleModeNearest);
  return texture;
}

static void pumpCoverDecodeResults() {
  int uploads=0,processed=0;
  while(processed<12){
    CoverDecodeResult result;
    {
      std::lock_guard<std::mutex> lock(g_coverDecodeMutex);
      if(g_coverDecodeReady.empty())break;
      if(!g_coverDecodeReady.front().pixels.empty()&&uploads>=COVER_UPLOAD_BUDGET)break;
      result=std::move(g_coverDecodeReady.front());g_coverDecodeReady.pop_front();
    }
    g_coverDecodeCondition.notify_one();++processed;
    Game *game=findGameByKey(result.key);
    if(!game||game->coverRequest!=result.request)continue;
    game->coverQueued=false;game->triedCover=true;
    if(!result.pixels.empty()){
      SDL_Texture *texture=uploadCoverTexture(result);++uploads;
      if(texture)installCover(*game,texture,result.isRomIcon);
    }
  }
}

static void ensureCover(Game &g,bool priority=false) {
  if (g.cover) { touchCover(g); return; }
  queueCoverDecode(g,priority);
}
static void reloadCover(Game &g) {
  if (g.cover) { SDL_DestroyTexture(g.cover); g.cover = nullptr; }
  g.coverIsRomIcon=false;g.coverUse=0;g.triedCover=false;g.coverQueued=false;g.coverRequest=0;
  g_cover_budget=std::max(g_cover_budget,1);queueCoverDecode(g,true);
}

static void drawGameArtwork(const Game &game,int x,int y,int width,int height,
                            Uint8 alpha,Uint8 shade) {
  if(!game.cover)return;
  SDL_SetTextureAlphaMod(game.cover,alpha);
  SDL_SetTextureColorMod(game.cover,shade,shade,shade);
  SDL_Rect destination={x,y,width,height};
  if(game.coverIsRomIcon){
    fillRect(x,y,width,height,COL_CARD);
    const int padding=std::clamp(width/10,6,18);
    const int size=std::max(1,std::min(width-padding*2,height-padding*2));
    destination={x+(width-size)/2,y+(height-size)/2,size,size};
  }
  SDL_RenderCopy(g_ren,game.cover,nullptr,&destination);
}

static bool promptTextMode(const char *header, const char *initial, char *out, size_t outSize,
                           bool password, bool allowEmpty,
                           const char *subText=nullptr, const char *guideText=nullptr) {
  SwkbdConfig kbd;
  out[0] = 0;
  if (R_FAILED(swkbdCreate(&kbd, 0))) return false;
  if(password) swkbdConfigMakePresetPassword(&kbd); else swkbdConfigMakePresetDefault(&kbd);
  if (header) swkbdConfigSetHeaderText(&kbd, header);
  if (subText) swkbdConfigSetSubText(&kbd, subText);
  if (guideText) swkbdConfigSetGuideText(&kbd, guideText);
  if (initial && *initial) swkbdConfigSetInitialText(&kbd, initial);
  swkbdConfigSetStringLenMax(&kbd, (u32)(outSize - 1));
  Result rc = swkbdShow(&kbd, out, outSize);
  swkbdClose(&kbd);
  return R_SUCCEEDED(rc) && (allowEmpty || out[0]);
}
static bool promptText(const char *header, const char *initial, char *out, size_t outSize) {
  return promptTextMode(header,initial,out,outSize,false,false);
}

struct FileClipboard {
  std::string path;
  bool move=false;
};
static FileClipboard g_fileClipboard;

static bool filesystemRoot(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t colon=normalized.find(':');
  if(colon==std::string::npos) return normalized=="/";
  for(size_t i=colon+1;i<normalized.size();i++) if(normalized[i]!='/') return false;
  return true;
}

static std::string parentFolder(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  if(filesystemRoot(normalized)) return {};
  size_t slash=normalized.find_last_of('/');
  if(slash==std::string::npos) return {};
  size_t colon=normalized.find(':');
  if(colon!=std::string::npos && slash<=colon+1) return normalized.substr(0,colon+2);
  return normalized.substr(0,slash);
}

static std::string fileNameOf(const std::string &path) {
  std::string normalized=normalizeLocationPath(path);
  size_t slash=normalized.find_last_of('/');
  return slash==std::string::npos?normalized:normalized.substr(slash+1);
}

static std::string deviceOf(const std::string &path) {
  size_t colon=path.find(':');
  return foldedKey(colon==std::string::npos?std::string{}:path.substr(0,colon));
}

static bool pathAtOrBelow(const std::string &path,const std::string &root) {
  std::string candidate=pathIdentity(path), base=pathIdentity(root);
  if(base.empty()||candidate.size()<base.size()||candidate.compare(0,base.size(),base)!=0) return false;
  if(candidate.size()==base.size()) return true;
  return base.back()=='/'||candidate[base.size()]=='/';
}

static std::string gameLocationLabel(const Game &game) {
  const std::string path=normalizeLocationPath(game.path);
  if(path.empty()) return "Unknown location";
  for(const auto &share:loadSmbSharesFromStore()){
    const std::string root=normalizeLocationPath(SwitchStorage::SmbRootPath(share.id));
    if(!pathAtOrBelow(path,root)) continue;
    std::string relative=path.substr(std::min(path.size(),root.size()));
    while(!relative.empty()&&relative.front()=='/') relative.erase(relative.begin());
    std::string address="SMB: smb://"+share.server+"/"+share.share;
    if(!relative.empty()) address+="/"+relative;
    return address;
  }
  if(path.rfind("sdmc:",0)==0) return "SD: "+path;
  if(path.rfind("ums",0)==0) return "USB: "+path;
  return path;
}

static bool replacePathPrefix(std::string &path,const std::string &oldPath,const std::string &newPath) {
  const std::string normalizedOld=normalizeLocationPath(oldPath);
  const std::string normalizedNew=normalizeLocationPath(newPath);
  const std::string normalizedPath=normalizeLocationPath(path);
  const std::string oldIdentity=pathIdentity(normalizedOld);
  const std::string identity=pathIdentity(normalizedPath);
  if(oldIdentity.empty()||identity.size()<oldIdentity.size()||
     identity.compare(0,oldIdentity.size(),oldIdentity)!=0) return false;
  if(identity.size()!=oldIdentity.size()&&oldIdentity.back()!='/'&&
     identity[oldIdentity.size()]!='/') return false;
  const std::string replaced=identity.size()==oldIdentity.size()?normalizedNew:
      normalizeLocationPath(normalizedNew+normalizedPath.substr(normalizedOld.size()));
  if(pathIdentity(replaced)==identity) return false;
  path=replaced;return true;
}

static void replaceSavedPathPrefix(const std::string &oldPath,const std::string &newPath) {
  auto replace=[&](std::vector<std::string> &paths){
    for(auto &path:paths) replacePathPrefix(path,oldPath,newPath);
  };
  auto sources=loadGameSources(); replace(sources); saveGameSources(sources);
  auto favorites=loadFavoriteFolders(); replace(favorites); saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty()) replacePathPrefix(g_fileClipboard.path,oldPath,newPath);
  g_rescanAfterSettings=true;
}

static bool migrateV109SmbPaths() {
  bool changed=false,identitiesChanged=false;
  for(const auto &share:loadSmbSharesFromStore()){
    const std::string oldRoot="cemusmb_"+share.id+":/";
    const std::string newRoot=SwitchStorage::SmbRootPath(share.id);
    if(newRoot.empty()||pathIdentity(oldRoot)==pathIdentity(newRoot))continue;
    for(auto &entry:g_global.kv){
      const bool gamePath=entry.k.rfind("Wrapper/GamePath",0)==0;
      const bool favoritePath=entry.k.rfind("Browser/Favorite",0)==0;
      if((gamePath||favoritePath)&&replacePathPrefix(entry.v,oldRoot,newRoot))changed=true;
    }
    for(auto &record:g_libraryIdentities){
      identitiesChanged|=replacePathPrefix(record.canonicalPath,oldRoot,newRoot);
      identitiesChanged|=replacePathPrefix(record.currentPath,oldRoot,newRoot);
      for(auto &path:record.previousPaths)
        identitiesChanged|=replacePathPrefix(path,oldRoot,newRoot);
    }
  }
  g_libraryIdentitiesDirty|=identitiesChanged;
  return changed||identitiesChanged;
}

static void removeSavedPathsBelow(const std::string &root) {
  auto sources=loadGameSources();
  sources.erase(std::remove_if(sources.begin(),sources.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),sources.end());
  saveGameSources(sources);
  auto favorites=loadFavoriteFolders();
  favorites.erase(std::remove_if(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathAtOrBelow(path,root); }),favorites.end());
  saveFavoriteFolders(favorites);
  if(!g_fileClipboard.path.empty()&&pathAtOrBelow(g_fileClipboard.path,root)) g_fileClipboard={};
  g_rescanAfterSettings=true;
}

static bool validEntryName(const std::string &name) {
  if(name.empty()||name=="."||name==".."||name.size()>255) return false;
  for(unsigned char c:name) if(c<' '||c=='/'||c=='\\'||c==':') return false;
  return true;
}

static bool removeTreeInternal(const std::string &path) {
  if(filesystemRoot(path)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0) return errno==ENOENT;
  if(S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)) return remove(path.c_str())==0;
  if(!S_ISDIR(st.st_mode)) return false;
  DIR *dir=opendir(path.c_str()); if(!dir) return false;
  bool ok=true; struct dirent *entry;
  while(ok&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=removeTreeInternal(join(path,entry->d_name));
  }
  if(closedir(dir)!=0) ok=false;
  return ok&&rmdir(path.c_str())==0;
}

struct TransferState {
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> done{0};
  std::string current,error;
  std::vector<unsigned char> buffer=std::vector<unsigned char>(1<<18);
  std::mutex detailMutex;
  std::atomic<bool> cancelled{false};
};

static void setTransferDetail(TransferState &state,const std::string &current,const std::string &error={}) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  if(!current.empty()) state.current=current;
  if(!error.empty()) state.error=error;
}

static std::string transferError(TransferState &state) {
  std::lock_guard<std::mutex> lock(state.detailMutex);
  return state.error;
}

static bool transferFrame(TransferState &state) {
  if(!beginUiFrame()){ state.cancelled.store(true); return false; }
  SDL_Event event;
  while(pollUiEvent(event)){
    pumpStick(event);
    int tx=0,ty=0;
    if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-100) state.cancelled.store(true);
    if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL) state.cancelled.store(true);
  }
  std::string current;
  { std::lock_guard<std::mutex> lock(state.detailMutex); current=state.current; }
  clearUiBackground();
  drawTextC(g_font_big,SW/2,80,"File transfer",COL_HI);
  drawTextC(g_font_sm,SW/2,150,ellipsizedText(g_font_sm,current,SW-180).c_str(),COL_DIM);
  int bw=SW*2/3,bx=(SW-bw)/2,by=SH/2-24,bh=42;
  border(bx,by,bw,bh,2,COL_SEL);
  uint64_t done=state.done.load(std::memory_order_relaxed);
  uint64_t total=state.total.load(std::memory_order_relaxed);
  uint64_t progress=total?std::min(done,total):0;
  int fill=total?(int)((bw-6)*progress/total):0;
  fillRect(bx+3,by+3,fill,bh-6,COL_HI);
  char text[96];
  int percent=total?(int)(progress*100/total):0;
  snprintf(text,sizeof(text),"%d%%  -  %.1f / %.1f MiB",percent,done/1048576.0,total/1048576.0);
  drawTextC(g_font,SW/2,by+66,text,COL_TXT);
  if(state.cancelled.load())
    drawTextC(g_font_sm,SW/2,SH-72,"Cancelling...",COL_VAL);
  else
    drawFooterText("B  Cancel",SH-72+TTF_FontHeight(g_font_sm)/2);
  presentUi();
  return !state.cancelled.load();
}

static bool measureTree(const std::string &path,TransferState &state) {
  if(state.cancelled.load(std::memory_order_relaxed)) return false;
  struct stat st{};
  if(lstat(path.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)){ state.total.fetch_add((uint64_t)st.st_size,std::memory_order_relaxed); return true; }
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  DIR *dir=opendir(path.c_str()); if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=measureTree(join(path,entry->d_name),state);
  }
  if(closedir(dir)!=0) ok=false;
  return ok;
}

static bool copyFileAtomic(const std::string &source,const std::string &destination,TransferState &state) {
  setTransferDetail(state,fileNameOf(source));
  const std::string partial=destination+".nx-part", backup=destination+".nx-old";
  remove(partial.c_str());
  FILE *input=fopen(source.c_str(),"rb");
  if(!input){ setTransferDetail(state,{},"Could not open the source file"); return false; }
  FILE *output=fopen(partial.c_str(),"wb");
  if(!output){ fclose(input); setTransferDetail(state,{},"Could not create the destination file"); return false; }
  bool ok=true;
  while(ok&&!state.cancelled.load(std::memory_order_relaxed)){
    size_t count=fread(state.buffer.data(),1,state.buffer.size(),input);
    if(count){
      if(fwrite(state.buffer.data(),1,count,output)!=count){ setTransferDetail(state,{},"Write failed; check free space and permissions"); ok=false; break; }
      state.done.fetch_add(count,std::memory_order_relaxed);
    }
    if(count<state.buffer.size()){
      if(ferror(input)){ setTransferDetail(state,{},"Read failed"); ok=false; }
      break;
    }
  }
  if(state.cancelled.load()) ok=false;
  if(ok&&fflush(output)!=0){ setTransferDetail(state,{},"Could not flush the destination file"); ok=false; }
  if(ok&&fsync(fileno(output))!=0){ setTransferDetail(state,{},"Could not commit the destination file"); ok=false; }
  if(fclose(input)!=0&&ok){ setTransferDetail(state,{},"Could not close the source file"); ok=false; }
  if(fclose(output)!=0&&ok){ setTransferDetail(state,{},"Could not close the destination file"); ok=false; }
  if(!ok||state.cancelled.load()){ remove(partial.c_str()); return false; }
  struct stat destinationStat{}; bool existed=stat(destination.c_str(),&destinationStat)==0;
  if(existed){
    struct stat backupStat{};
    if(lstat(backup.c_str(),&backupStat)==0){ setTransferDetail(state,{},"A previous backup file blocks this operation"); remove(partial.c_str()); return false; }
    if(rename(destination.c_str(),backup.c_str())!=0){ setTransferDetail(state,{},"Could not preserve the existing destination"); remove(partial.c_str()); return false; }
  }
  if(rename(partial.c_str(),destination.c_str())!=0){
    if(existed) rename(backup.c_str(),destination.c_str());
    setTransferDetail(state,{},"Could not finalize the copied file"); remove(partial.c_str()); return false;
  }
  if(existed) remove(backup.c_str());
  return true;
}

static bool copyTree(const std::string &source,const std::string &destination,TransferState &state) {
  struct stat st{};
  if(lstat(source.c_str(),&st)!=0){ setTransferDetail(state,{},"Source is no longer available"); return false; }
  if(S_ISREG(st.st_mode)) return copyFileAtomic(source,destination,state);
  if(!S_ISDIR(st.st_mode)){ setTransferDetail(state,{},"Unsupported file type"); return false; }
  if(mkdir(destination.c_str(),0777)!=0){ setTransferDetail(state,{},"Could not create a destination folder"); return false; }
  DIR *dir=opendir(source.c_str());
  if(!dir){ setTransferDetail(state,{},"Could not open a source folder"); return false; }
  bool ok=true; struct dirent *entry;
  while(ok&&!state.cancelled.load()&&(entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    ok=copyTree(join(source,entry->d_name),join(destination,entry->d_name),state);
  }
  if(closedir(dir)!=0&&ok){ setTransferDetail(state,{},"Could not close a source folder"); ok=false; }
  return ok&&!state.cancelled.load();
}

static bool enoughFreeSpace(const std::string &folder,uint64_t bytes) {
  struct statvfs info{};
  if(statvfs(folder.c_str(),&info)!=0||!info.f_frsize) return true;
  return bytes<=static_cast<uint64_t>(info.f_bavail)*info.f_frsize;
}

static bool executePaste(const std::string &folder) {
  if(g_fileClipboard.path.empty()) return false;
  struct stat sourceStat{};
  if(lstat(g_fileClipboard.path.c_str(),&sourceStat)!=0){ modalMessage("Paste failed",{"The copied item is no longer available."}); g_fileClipboard={}; return false; }
  const std::string destination=join(folder,fileNameOf(g_fileClipboard.path));
  if(pathIdentity(destination)==pathIdentity(g_fileClipboard.path) ||
     (S_ISDIR(sourceStat.st_mode)&&pathAtOrBelow(destination,g_fileClipboard.path))){
    modalMessage("Paste failed",{"The destination cannot be inside the source."}); return false;
  }
  struct stat destinationStat{}; bool destinationExists=lstat(destination.c_str(),&destinationStat)==0;
  if(destinationExists&&S_ISDIR(sourceStat.st_mode)){
    modalMessage("Folder already exists",{"Choose another destination or rename the folder first.",destination}); return false;
  }
  if(destinationExists&&!S_ISREG(destinationStat.st_mode)){
    modalMessage("Paste failed",{"The destination is not a regular file."}); return false;
  }
  if(destinationExists&&!confirmBox("Replace existing file?",{fileNameOf(destination),"","The existing file will be replaced."})) return false;

  bool sameDevice=deviceOf(g_fileClipboard.path)==deviceOf(destination);
  if(g_fileClipboard.move&&sameDevice){
    const std::string backup=destination+".nx-old";
    bool preserved=false;
    if(destinationExists){
      struct stat backupStat{};
      if(lstat(backup.c_str(),&backupStat)==0||rename(destination.c_str(),backup.c_str())!=0){ modalMessage("Move failed",{"Could not preserve the existing destination."}); return false; }
      preserved=true;
    }
    if(rename(g_fileClipboard.path.c_str(),destination.c_str())==0){
      if(preserved) remove(backup.c_str());
      replaceSavedPathPrefix(g_fileClipboard.path,destination);
      g_fileClipboard={}; toast("Move complete",700); return true;
    }
    if(preserved) rename(backup.c_str(),destination.c_str());
  }

  TransferState state;
  setTransferDetail(state,"Preparing transfer...");
  bool ok=false;
  std::atomic<bool> complete{false};
  appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
  std::thread worker([&](){
    ok=measureTree(g_fileClipboard.path,state);
    if(ok&&!state.cancelled.load()&&!enoughFreeSpace(folder,state.total.load(std::memory_order_relaxed))){
      setTransferDetail(state,{},"The destination does not have enough available space");
      ok=false;
    }
    if(ok&&!state.cancelled.load()){
      setTransferDetail(state,fileNameOf(g_fileClipboard.path));
      ok=copyTree(g_fileClipboard.path,destination,state);
    }
    complete.store(true,std::memory_order_release);
  });
  while(!complete.load(std::memory_order_acquire)){
    transferFrame(state);
    waitForNextUiFrame();
  }
  worker.join();
  appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
  if(!ok&&S_ISDIR(sourceStat.st_mode)) removeTreeInternal(destination);
  if(ok&&g_fileClipboard.move){
    if(removeTreeInternal(g_fileClipboard.path)) replaceSavedPathPrefix(g_fileClipboard.path,destination);
    else { modalMessage("Move incomplete",{"The copy completed, but the original could not be removed completely.","Review both locations before trying again."}); ok=false; }
  }
  if(ok){ g_rescanAfterSettings=true; if(g_fileClipboard.move) g_fileClipboard={}; toast("Transfer complete",700); }
  else if(state.cancelled.load()){ toast("Transfer cancelled",700); }
  else { std::string error=transferError(state); modalMessage("Transfer failed",{error.empty()?"The file transfer could not be completed.":error}); }
  return ok;
}

static bool editSmbShare(SwitchStorage::SmbShare &share,bool creating) {
  SwitchStorage::SmbShare edited=share;
  constexpr int fieldCount=7,saveRow=7,totalRows=8;
  int sel=0;
  bool done=false,saved=false;
  beginScreenFx();

  auto cleanServer=[&](){
    edited.server=trim(edited.server);
    if(edited.server.rfind("smb://",0)==0) edited.server.erase(0,6);
    while(!edited.server.empty()&&edited.server.back()=='/') edited.server.pop_back();
  };
  auto cleanShare=[&](){
    std::string combined=trim(edited.share);
    if(!edited.path.empty()) combined+="/"+edited.path;
    std::replace(combined.begin(),combined.end(),'\\','/');
    while(!combined.empty()&&combined.front()=='/') combined.erase(combined.begin());
    while(!combined.empty()&&combined.back()=='/') combined.pop_back();
    std::string normalized; bool slash=false;
    for(char value:combined){
      if(value=='/'){ if(slash) continue; slash=true; }
      else slash=false;
      normalized+=value;
    }
    size_t separator=normalized.find('/');
    edited.share=trim(normalized.substr(0,separator));
    edited.path=separator==std::string::npos?std::string{}:trim(normalized.substr(separator+1));
  };
  auto sharedFolder=[&](){ return edited.path.empty()?edited.share:edited.share+"/"+edited.path; };
  auto validate=[&](){
    edited.name=trim(edited.name); cleanServer(); cleanShare();
    if(edited.name.empty()){ modalMessage("Display name required",{"Enter a name used to identify this share in Drastic DS."}); return false; }
    if(edited.server.empty()||edited.server.find('/')!=std::string::npos||edited.server.find('\\')!=std::string::npos){
      modalMessage("Invalid SMB server",{"Enter only a host name or IP address.","Example: 192.168.1.20"}); return false;
    }
    bool invalidPath=edited.share.empty()||edited.share.find(':')!=std::string::npos;
    size_t start=0;
    while(!invalidPath&&start<=edited.path.size()){
      size_t slash=edited.path.find('/',start);
      std::string component=trim(edited.path.substr(start,slash==std::string::npos?std::string::npos:slash-start));
      if((component.empty()&&!edited.path.empty())||component=="."||component==".."||component.find(':')!=std::string::npos) invalidPath=true;
      if(slash==std::string::npos) break;
      start=slash+1;
    }
    if(invalidPath){
      modalMessage("Invalid SMB share",{"Enter a share name, optionally followed by folders.","Do not include a drive letter or smb:// prefix."}); return false;
    }
    return true;
  };
  auto editField=[&](int index){
    char value[256]; bool accepted=false;
    if(index==0) accepted=promptTextMode("SMB display name",edited.name.c_str(),value,sizeof(value),false,false,
      "Friendly name shown in the Drastic DS file browser.","Example: Living room NAS");
    else if(index==1) accepted=promptTextMode("Server or IP address",edited.server.c_str(),value,sizeof(value),false,false,
      "Enter the network host only. Do not include smb:// or a folder.","Example: 192.168.1.20 or NAS.local");
    else if(index==2){ std::string folder=sharedFolder(); accepted=promptTextMode("Shared folder",folder.c_str(),value,sizeof(value),false,false,
      "Enter the share and an optional folder path inside it.","Nested folders are supported."); }
    else if(index==3) accepted=promptTextMode("Username",edited.user.c_str(),value,sizeof(value),false,true,
      "Account used by the SMB server. Leave blank for guest access.","Leave blank for guest");
    else if(index==4) accepted=promptTextMode("Password",edited.password.c_str(),value,sizeof(value),true,true,
      "Password for the SMB account. It is stored in launcher.ini.","Leave blank when no password is required");
    else if(index==5) accepted=promptTextMode("Workgroup",edited.domain.c_str(),value,sizeof(value),false,true,
      "Usually optional on a home network.","Example: WORKGROUP, or leave blank");
    if(!accepted) return;
    if(index==0) edited.name=value;
    else if(index==1){ edited.server=value; cleanServer(); }
    else if(index==2){ edited.share=value; edited.path.clear(); cleanShare(); }
    else if(index==3) edited.user=value;
    else if(index==4) edited.password=value;
    else if(index==5) edited.domain=value;
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel<6) editField(sel);
    else if(sel==6) edited.autoMount=!edited.autoMount;
    else if(validate()){
      if(creating){
        std::unordered_set<std::string> ids;
        for(const auto &existing:loadSmbSharesFromStore()) ids.insert(existing.id);
        uint64_t seed=armGetSystemTick();
        do { char id[17]; snprintf(id,sizeof(id),"%08llx",(unsigned long long)(seed&0xffffffffULL)); edited.id=id; seed=seed*6364136223846793005ULL+1; } while(ids.count(edited.id));
      }
      share=std::move(edited); saved=true; done=true;
    }
  };

  while(!done){
    if(!beginUiFrame()) break;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      int scale=highResolutionUi()?3:2;
      int rowHeight=g_launcherPortrait?settingsRowH():27*scale,y0=topBarH()+26;
      int margin=g_launcherPortrait?(highResolutionUi()?72:36):(highResolutionUi()?90:56);
      int helpWidth=g_launcherPortrait?SW-margin*2:(highResolutionUi()?570:420);
      int gap=g_launcherPortrait?24:(highResolutionUi()?44:28);
      int formWidth=g_launcherPortrait?SW-margin*2:SW-margin*2-helpWidth-gap;
      if(touch==TOUCH_TAP){
        if(ty>=SH-42){ done=true; continue; }
        for(int index=0;index<fieldCount;index++) if(tx>=margin&&tx<margin+formWidth&&ty>=y0+index*rowHeight&&ty<y0+(index+1)*rowHeight){ sel=index; activate(); break; }
        int buttonY=y0+fieldCount*rowHeight+10;
        if(tx>=margin&&tx<margin+formWidth&&ty>=buttonY&&ty<buttonY+rowHeight){ sel=saveRow; activate(); }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+totalRows-1)%totalRows;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%totalRows;
      else if((event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT)&&sel==6) edited.autoMount=!edited.autoMount;
      else if(event.cbutton.button==BTN_CONFIRM) activate();
      else if(event.cbutton.button==BTN_CANCEL) done=true;
    }

    clearUiBackground();
    drawHeader(creating?"Add SMB network share":"Edit SMB network share",edited.name.empty()?nullptr:edited.name.c_str());
    int scale=highResolutionUi()?3:2;
    int rowHeight=g_launcherPortrait?settingsRowH():27*scale,y0=topBarH()+26;
    int margin=g_launcherPortrait?(highResolutionUi()?72:36):(highResolutionUi()?90:56);
    int helpWidth=g_launcherPortrait?SW-margin*2:(highResolutionUi()?570:420);
    int gap=g_launcherPortrait?24:(highResolutionUi()?44:28);
    int formWidth=g_launcherPortrait?SW-margin*2:SW-margin*2-helpWidth-gap;
    int panelHeight=fieldCount*rowHeight+rowHeight+30;
    int helpX=g_launcherPortrait?margin:margin+formWidth+gap;
    int helpY=g_launcherPortrait?y0+panelHeight+gap:y0-10;
    int helpHeight=g_launcherPortrait?std::max(250,std::min(highResolutionUi()?430:360,SH-helpY-64)):panelHeight;
    glassPanel(margin,y0-10,formWidth,panelHeight);
    glassPanel(helpX,helpY,helpWidth,helpHeight);
    const char *labels[fieldCount]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup"};
    std::string password=edited.password.empty()?"Not set":std::string(std::min<size_t>(16,edited.password.size()),'*');
    const std::string values[fieldCount]={
      edited.name.empty()?"Not set":edited.name,
      edited.server.empty()?"Not set":edited.server,
      edited.share.empty()?"Not set":sharedFolder(),
      edited.user.empty()?"Guest":edited.user,
      password,
      edited.domain.empty()?"Optional":edited.domain,
      edited.autoMount?"On":"Off"
    };
    for(int index=0;index<fieldCount;index++){
      int y=y0+index*rowHeight; bool current=sel==index;
      if(current){ fillRect(margin+8,y,formWidth-16,rowHeight-2,COL_FOCUS); fillRect(margin+8,y,5,rowHeight-2,COL_SEL); }
      if(g_launcherPortrait)
        drawSettingsRowText(labels[index],values[index].c_str(),y,formWidth-16,
                            margin+30,margin+formWidth-24,current,
                            current?COL_VAL:COL_DIM,current?COL_VAL:COL_TXT,true);
      else {
        drawText(g_font_sm,margin+30,y+(rowHeight-TTF_FontHeight(g_font_sm))/2,labels[index],current?COL_VAL:COL_DIM);
        drawScrollTextR(g_font,margin+formWidth-24,y+(rowHeight-TTF_FontHeight(g_font))/2,formWidth/2-30,values[index].c_str(),current?COL_VAL:COL_TXT);
      }
    }
    int buttonY=y0+fieldCount*rowHeight+10; bool buttonSelected=sel==saveRow;
    fillRect(margin+14,buttonY,formWidth-28,rowHeight-4,buttonSelected?COL_FOCUS:COL_CARD);
    if(buttonSelected) border(margin+14,buttonY,formWidth-28,rowHeight-4,2,COL_SEL);
    drawTextC(g_font,margin+formWidth/2,buttonY+(rowHeight-TTF_FontHeight(g_font))/2-2,
              creating?"Connect and save":"Save changes",buttonSelected?COL_VAL:COL_HI);

    static const char *helpTitle[totalRows]={"Display name","Server / IP address","Shared folder","Username","Password","Workgroup","Connect at startup","Save share"};
    static const char *helpLine1[totalRows]={
      "A friendly name shown only in Drastic DS.","The host name or IP of your SMB server.","The share name and optional folder path.","Leave blank when the share allows guests.",
      "The password for the selected account.","Usually optional on home networks.","Reconnect this share when the launcher opens.","Validate the fields and connect to the share."
    };
    static const char *helpLine2[totalRows]={
      "Example: Living room NAS","Example: 192.168.1.20 or NAS.local","Nested folders are supported.","Use the account configured on your NAS or PC.",
      "The value is masked on this screen.","Example: WORKGROUP","Turn this off for manually connected shares.","Connection errors will be shown after saving."
    };
    const int helpContentY=g_launcherPortrait?helpY:y0;
    drawText(g_font_big,helpX+28,helpContentY+22,
             fittedText(g_font_big,helpTitle[sel],helpWidth-56).c_str(),COL_HI);
    int helpLineHeight=TTF_FontHeight(g_font_sm)+4;
    drawWrapped(g_font_sm,helpX+28,helpContentY+92,helpWidth-56,helpLineHeight,2,helpLine1[sel],COL_TXT);
    drawWrapped(g_font_sm,helpX+28,helpContentY+156,helpWidth-56,helpLineHeight,2,helpLine2[sel],COL_DIM);
    std::string address="smb://"+(edited.server.empty()?std::string("server"):edited.server)+"/"+(edited.share.empty()?std::string("share"):sharedFolder());
    drawText(g_font_sm,helpX+28,helpContentY+210,"Connection preview",COL_DIM);
    drawScrollTextL(g_font,helpX+28,helpContentY+244,helpWidth-56,address.c_str(),COL_VAL);
    drawButtonHint(helpX+28,helpY+helpHeight-66,"A","Edit / toggle");
    drawButtonHint(helpX+28,helpY+helpHeight-32,"B","Cancel");
    drawFadeIn(); presentUi(); waitForNextUiFrame();
  }
  return saved;
}

static void networkSharesScreen() {
  int sel=0,top=0;
  for(;;){
    auto shares=loadSmbSharesFromStore(); int n=1+(int)shares.size();
    const int listY=g_launcherPortrait?settingsListY():112;
    const int rowHeight=g_launcherPortrait?(highResolutionUi()?104:84):60;
    int vis=std::max(1,(SH-listY-settingsFooterReserve())/rowHeight);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowHeight; if(ty>=y&&ty<y+rowHeight-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(shares.size()>=8){ toast("Maximum of 8 SMB shares",900); continue; }
            SwitchStorage::SmbShare share;
            if(editSmbShare(share,true)){
              shares.push_back(share); saveSmbShares(shares);
              std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error});
              sel=(int)shares.size(); rebuild=true;
            }
          } else {
            auto &share=shares[sel-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
            const char *actions[]={mounted?"Disconnect":"Connect","Edit","Toggle connect at startup","Remove"};
            int action=dropdown(share.name.c_str(),actions,4,0);
            if(action==0){
              if(mounted) SwitchStorage::UnmountSmb(share.id);
              else { std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error}); }
              rebuild=true;
            } else if(action==1){
              SwitchStorage::SmbShare edited=share;
              if(editSmbShare(edited,false)){
                bool reconnect=mounted||edited.autoMount;
                SwitchStorage::UnmountSmb(share.id); share=std::move(edited); saveSmbShares(shares);
                if(reconnect){ std::string error; if(!SwitchStorage::MountSmb(share,&error)) modalMessage("SMB connection failed",{error}); }
                rebuild=true;
              }
            } else if(action==2){ share.autoMount=!share.autoMount; saveSmbShares(shares); rebuild=true; }
            else if(action==3&&confirmBox("Remove SMB share?",{share.name,"","Saved folders on this share will also be removed."})){
              std::string root=SwitchStorage::SmbRootPath(share.id); SwitchStorage::UnmountSmb(share.id);
              shares.erase(shares.begin()+sel-1); saveSmbShares(shares); removeSavedPathsBelow(root);
              sel=std::max(0,sel-1); rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      std::string summary=std::to_string(shares.size())+(shares.size()==1?" saved share":" saved shares");
      drawHeader("SMB network shares",summary.c_str());
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,y=listY+row*rowHeight; bool current=index==sel;
        if(current){ fillRect(56,y-3,SW-112,rowHeight-4,COL_FOCUS); fillRect(56,y-3,5,rowHeight-4,COL_SEL); }
        if(index==0) drawText(g_font,82,y+(rowHeight-TTF_FontHeight(g_font))/2-2,"[ Add SMB share ]",current?COL_VAL:COL_HI);
        else { const auto &share=shares[index-1]; bool mounted=SwitchStorage::IsSmbMounted(share.id);
          int nameY=y+(g_launcherPortrait?8:0);
          drawText(g_font,82,nameY,
                   fittedText(g_font,share.name,g_launcherPortrait?SW-340:SW/2).c_str(),current?COL_VAL:COL_TXT);
          std::string status=mounted?"Connected":(share.autoMount?"Disconnected - auto":"Disconnected");
          drawTextR(g_font_sm,SW-82,nameY+4,status.c_str(),mounted?(SDL_Color){120,220,120,255}:COL_DIM);
          std::string address="smb://"+share.server+"/"+share.share+(share.path.empty()?std::string{}:"/"+share.path);
          drawText(g_font_sm,82,y+(g_launcherPortrait?48:31),
                   fittedText(g_font_sm,address,g_launcherPortrait?SW-164:SW-340).c_str(),COL_DIM); }
      }
      drawFooterText("A  Select       B  Back");
      presentUi(); waitForNextUiFrame();
    }
  }
}

enum class BrowserMode { SelectFolder, SelectImage, Manage };
enum class BrowserItemKind { Use, Up, Paste, Favorite, Directory, File, Location, Smb, ManageSmb };
struct BrowserItem {
  std::string label,path;
  BrowserItemKind kind=BrowserItemKind::File;
  bool directory=false;
};

static bool ensurePathMounted(const std::string &path) {
  for(const auto &share:loadSmbSharesFromStore()){
    std::string root=SwitchStorage::SmbRootPath(share.id);
    if(pathAtOrBelow(path,root)){
      if(SwitchStorage::IsSmbMounted(share.id)) return true;
      std::string error;
      if(SwitchStorage::MountSmb(share,&error)) return true;
      modalMessage("SMB connection failed",{share.name,error}); return false;
    }
  }
  return true;
}

static bool isUsbStoragePath(const std::string &path) {
  size_t colon=path.find(':');
  if(colon<4) return false;
  if(tolower((unsigned char)path[0])!='u'||tolower((unsigned char)path[1])!='m'||tolower((unsigned char)path[2])!='s') return false;
  for(size_t index=3;index<colon;index++) if(!isdigit((unsigned char)path[index])) return false;
  return true;
}

static bool isConfiguredSmbStoragePath(
    const std::string &path,const std::vector<SwitchStorage::SmbShare> &shares) {
  return std::any_of(shares.begin(),shares.end(),[&](const auto &share){
    const std::string root=SwitchStorage::SmbRootPath(share.id);
    return !root.empty()&&pathAtOrBelow(path,root);
  });
}

static std::string usbIdForPath(const std::string &path){
  for(const auto &location:SwitchStorage::ListUsbLocations())
    if(pathAtOrBelow(path,normalizeLocationPath(location.path)))return location.id;
  return {};
}

static bool hasConfiguredUsbSource(const std::vector<std::string> &paths) {
  return std::any_of(paths.begin(),paths.end(),[](const std::string &path){
    return isUsbStoragePath(path)||path.rfind(UNAVAILABLE_USB_PREFIX,0)==0;
  });
}

static void removeUnavailableUsbGames(const std::unordered_set<std::string> &connectedIds){
  g_games.erase(std::remove_if(g_games.begin(),g_games.end(),[&](Game &game){
    if(game.sourceStableId.empty()||connectedIds.count(game.sourceStableId))return false;
    if(game.cover)SDL_DestroyTexture(game.cover);
    return true;
  }),g_games.end());
  applySort();
}

static bool refreshConfiguredUsbSources(std::vector<std::string> &paths) {
  if(!hasConfiguredUsbSource(paths)) return false;
  const auto locations=SwitchStorage::ListUsbLocations();
  std::unordered_map<std::string,std::string> roots;
  for(const auto &location:locations)roots[location.id]=normalizeLocationPath(location.path);
  bool changed=false;
  for(auto &path:paths){
    std::string stableId,relative;
    if(decodeUnavailableUsbSource(path,&stableId,&relative)){
      const auto root=roots.find(stableId);
      if(root!=roots.end()){
        path=normalizeLocationPath(root->second+relative);
        changed=true;
      }
      continue;
    }
    if(!isUsbStoragePath(path)) continue;
    // A mutable umsN: path is accepted only while it is currently backed by a
    // known volume.  Never search every disk for a coincidentally matching
    // directory: an alias can be reused immediately after unplug/replug.
    bool bound=false;
    for(const auto &location:locations){
      const std::string root=normalizeLocationPath(location.path);
      if(pathAtOrBelow(path,root)){bound=true;break;}
    }
    if(!bound)continue;
  }
  if(changed){ saveGameSources(paths); storeSave(g_global,LAUNCHER_INI); }
  return changed;
}

static void renderUsbForwarderWait() {
  clearUiBackground();
  const int panelWidth=std::min(720,SW-64),panelHeight=220;
  const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
  glassPanel(panelX,panelY,panelWidth,panelHeight);
  border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
  drawTextC(g_font_big,SW/2,panelY+42,"Connecting USB storage",COL_SEL);
  drawTextC(g_font,SW/2,panelY+108,"Waiting for the game drive...",COL_TXT);
  drawTextC(g_font_sm,SW/2,panelY+146,"The game will start automatically",COL_DIM);
  drawFooterText("B  Cancel",panelY+181);
  presentUi();
}

static void ensureSavedPathMountedAtStartup(const std::string &path) {
  auto shares=loadSmbSharesFromStore();
  bool changed=false;
  for(auto &share:shares){
    if(pathAtOrBelow(path,SwitchStorage::SmbRootPath(share.id))&&!share.autoMount){
      share.autoMount=true;
      changed=true;
    }
  }
  if(changed) saveSmbShares(shares);
}

static std::vector<BrowserItem> browserItems(const std::string &current,BrowserMode mode,bool &opened) {
  std::vector<BrowserItem> items; opened=true;
  if(current.empty()){
    SwitchStorage::InitializeUsb();
    items.push_back({"SD card","sdmc:/",BrowserItemKind::Location,true});
    for(const auto &usb:SwitchStorage::ListUsbLocations()) items.push_back({usb.label,usb.path,BrowserItemKind::Location,true});
    for(const auto &share:loadSmbSharesFromStore()){
      bool mounted=SwitchStorage::IsSmbMounted(share.id);
      std::string label="SMB - "+(share.name.empty()?share.share:share.name)+(mounted?"":" (disconnected)");
      items.push_back({label,SwitchStorage::SmbBrowsePath(share),BrowserItemKind::Smb,true});
    }
    for(const auto &favorite:loadFavoriteFolders()) items.push_back({"Pinned - "+favorite,favorite,BrowserItemKind::Location,true});
    items.push_back({"Manage SMB shares","",BrowserItemKind::ManageSmb,true});
    return items;
  }
  if(mode==BrowserMode::SelectFolder) items.push_back({"[ Use this folder ]",current,BrowserItemKind::Use,true});
  if(mode==BrowserMode::Manage&&!g_fileClipboard.path.empty()) items.push_back({std::string("[ Paste ")+(g_fileClipboard.move?"moved":"copied")+" item here ]",current,BrowserItemKind::Paste,true});
  if(mode==BrowserMode::Manage){
    auto favorites=loadFavoriteFolders();
    bool pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(current); });
    items.push_back({pinned?"[ Unpin this folder ]":"[ Pin this folder ]",current,BrowserItemKind::Favorite,true});
  }
  items.push_back({"[ .. locations / parent ]",parentFolder(current),BrowserItemKind::Up,true});
  DIR *dir=opendir(current.c_str());
  if(!dir){ opened=false; return items; }
  std::vector<BrowserItem> entries; struct dirent *entry;
  while((entry=readdir(dir))){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    std::string path=join(current,entry->d_name); bool directory=entry->d_type==DT_DIR;
    if(entry->d_type==DT_UNKNOWN){ struct stat st{}; if(stat(path.c_str(),&st)!=0) continue; directory=S_ISDIR(st.st_mode); }
    if(!directory&&mode==BrowserMode::SelectFolder) continue;
    if(!directory&&mode==BrowserMode::SelectImage){
      const size_t dot=path.find_last_of('.');std::string extension=dot==std::string::npos?std::string{}:path.substr(dot);
      std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char value){return (char)std::tolower(value);});
      if(extension!=".png"&&extension!=".jpg"&&extension!=".jpeg"&&extension!=".webp"&&extension!=".bmp")continue;
    }
    entries.push_back({std::string(entry->d_name)+(directory?"/":""),path,directory?BrowserItemKind::Directory:BrowserItemKind::File,directory});
  }
  closedir(dir);
  std::sort(entries.begin(),entries.end(),[](const BrowserItem &left,const BrowserItem &right){
    if(left.directory!=right.directory) return left.directory>right.directory;
    return strcasecmp(left.label.c_str(),right.label.c_str())<0;
  });
  items.insert(items.end(),std::make_move_iterator(entries.begin()),std::make_move_iterator(entries.end()));
  return items;
}

static bool toggleFavorite(const std::string &path) {
  auto favorites=loadFavoriteFolders(); std::string identity=pathIdentity(path);
  auto iterator=std::find_if(favorites.begin(),favorites.end(),[&](const std::string &entry){ return pathIdentity(entry)==identity; });
  bool pinned=iterator==favorites.end();
  if(pinned){
    if(favorites.size()>=24){ toast("Maximum of 24 pinned folders",900); return false; }
    ensureSavedPathMountedAtStartup(path);
    favorites.push_back(normalizeLocationPath(path));
  }
  else favorites.erase(iterator);
  saveFavoriteFolders(favorites); toast(pinned?"Folder pinned":"Folder unpinned",650); return true;
}

static bool browserActions(const BrowserItem &item,BrowserMode mode) {
  if(item.kind!=BrowserItemKind::Directory&&item.kind!=BrowserItemKind::File&&item.kind!=BrowserItemKind::Use) return false;
  std::vector<std::string> labels;
  if(mode==BrowserMode::Manage){ labels={"Copy","Move","Rename"}; }
  bool canPin=item.directory;
  bool pinned=false;
  if(canPin){
    auto favorites=loadFavoriteFolders();
    pinned=std::any_of(favorites.begin(),favorites.end(),[&](const std::string &path){ return pathIdentity(path)==pathIdentity(item.path); });
    labels.push_back(pinned?"Unpin folder":"Pin folder");
  }
  if(labels.empty()) return false;
  std::vector<const char*> choices; for(const auto &label:labels) choices.push_back(label.c_str());
  int action=dropdown("File options",choices.data(),(int)choices.size(),0);
  if(action<0) return false;
  if(mode==BrowserMode::Manage&&action==0){ g_fileClipboard={item.path,false}; toast("Copied to clipboard",600); return false; }
  if(mode==BrowserMode::Manage&&action==1){ g_fileClipboard={item.path,true}; toast("Move queued",600); return false; }
  if(mode==BrowserMode::Manage&&action==2){
    char name[256]; std::string oldName=fileNameOf(item.path);
    if(!promptText("Rename",oldName.c_str(),name,sizeof(name))) return false;
    std::string newName=trim(name);
    if(!validEntryName(newName)){ modalMessage("Invalid name",{"Names cannot contain /, \\, :, or control characters."}); return false; }
    std::string destination=join(parentFolder(item.path),newName); struct stat st{};
    if(lstat(destination.c_str(),&st)==0){ modalMessage("Rename failed",{"An item with that name already exists."}); return false; }
    if(rename(item.path.c_str(),destination.c_str())!=0){ modalMessage("Rename failed",{strerror(errno)}); return false; }
    replaceSavedPathPrefix(item.path,destination); toast("Renamed",600); return true;
  }
  if(canPin) return toggleFavorite(item.path);
  return false;
}

static std::string runFileBrowser(const std::string &start,BrowserMode mode) {
  std::string current=normalizeLocationPath(start);
  if(!current.empty()&&!ensurePathMounted(current)) current.clear();
  int sel=0,top=0;
  for(;;){
    bool opened=false; auto items=browserItems(current,mode,opened);
    if(!opened){ modalMessage("Folder unavailable",{current,"","The device may be disconnected."}); current.clear(); sel=top=0; continue; }
    const int rowHeight=g_launcherPortrait?settingsRowH():46;
    const int listY=g_launcherPortrait?settingsListY():112;
    int n=(int)items.size();
    int vis=std::max(1,(SH-listY-settingsFooterReserve())/rowHeight);
    if(n==0){ current.clear(); continue; }
    sel=std::max(0,std::min(sel,n-1)); if(sel<top)top=sel; if(sel>=top+vis)top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return {};
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48){ uiAudioPlay(UiSound::Back); return {}; }
          for(int row=0;row<vis&&top+row<n;row++){ int y=listY+row*rowHeight; if(ty>=y&&ty<y+rowHeight-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL){ if(current.empty()) return {}; current=parentFolder(current); sel=top=0; rebuild=true; }
        else if(event.cbutton.button==BTN_SETTINGS){ if(browserActions(items[sel],mode)) rebuild=true; }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&mode==BrowserMode::Manage&&!current.empty()&&!g_fileClipboard.path.empty()){ executePaste(current); rebuild=true; }
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_START&&mode==BrowserMode::Manage){
          const std::string target=current.empty()?items[sel].path:current;
          const std::string usbId=usbIdForPath(target);
          if(!usbId.empty()&&confirmBox("Safely eject USB drive?",{target,"Close any game or transfer using this drive first."})){
            // A scan can own an open devoptab handle under this volume. Cancel
            // and join it before unmount, then let the main library reconcile
            // configured sources when returning from the file manager.
            stopGameScan();
            std::string error;if(SwitchStorage::SafelyEjectUsb(usbId,&error)){g_rescanAfterSettings=true;current.clear();sel=top=0;rebuild=true;toast("USB drive ejected safely");}
            else modalMessage("USB eject failed",{error});
          }
        }
        else if(event.cbutton.button==BTN_CONFIRM){
          const BrowserItem item=items[sel];
          if(item.kind==BrowserItemKind::Use) return item.path;
          if(item.kind==BrowserItemKind::Paste){ executePaste(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Favorite){ toggleFavorite(current); rebuild=true; }
          else if(item.kind==BrowserItemKind::Up){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::ManageSmb){ networkSharesScreen(); sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::Directory){ current=item.path; sel=top=0; rebuild=true; }
          else if(item.kind==BrowserItemKind::File&&mode==BrowserMode::SelectImage)return item.path;
          else if(item.kind==BrowserItemKind::Location||item.kind==BrowserItemKind::Smb){
            if(ensurePathMounted(item.path)){ DIR *test=opendir(item.path.c_str()); if(test){ closedir(test); current=item.path; sel=top=0; rebuild=true; } else modalMessage("Location unavailable",{item.path}); }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      const char *title=mode==BrowserMode::Manage?"File manager":mode==BrowserMode::SelectImage?"Select local cover":"Select game folder";
      drawHeader(title,current.empty()?"Locations":current.c_str());
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,slotY=listY+row*rowHeight;
        int y=slotY+(rowHeight-TTF_FontHeight(g_font))/2;
        bool selected=index==sel; const auto &item=items[index];
        if(selected){ fillRect(54,slotY-3,SW-108,rowHeight-4,COL_FOCUS); fillRect(54,slotY-3,5,rowHeight-4,COL_SEL); }
        SDL_Color color=item.kind==BrowserItemKind::Use||item.kind==BrowserItemKind::Paste||item.kind==BrowserItemKind::Favorite?COL_HI:(item.directory?COL_TXT:(SDL_Color){120,220,120,255});
        drawText(g_font,80,y,ellipsizedText(g_font,item.label,SW-180).c_str(),selected?COL_VAL:color);
      }
      std::string footer=mode==BrowserMode::Manage?"A  Open       X  Actions       Y  Paste       +  Eject USB       B  Back":"A  Open / Select       X  Pin       B  Back";
      drawFooterText(footer.c_str());
      presentUi(); waitForNextUiFrame();
    }
  }
}

static std::string browseFolder(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectFolder);
}

static std::string browseCoverImage(const std::string &start) {
  return runFileBrowser(start,BrowserMode::SelectImage);
}

static void runFileManager() {
  runFileBrowser({},BrowserMode::Manage);
}

struct CustomShaderInfo {
  std::string name;
  std::string relative;
  int passes=0;
  bool vulkanReady=false;
};

static bool readSmallTextFile(const std::string &path,std::string &text) {
  FILE *file=fopen(path.c_str(),"rb");
  if(!file) return false;
  if(fseek(file,0,SEEK_END)!=0){ fclose(file); return false; }
  long size=ftell(file);
  if(size<0||size>2*1024*1024||fseek(file,0,SEEK_SET)!=0){ fclose(file); return false; }
  text.resize((size_t)size);
  bool ok=!size||fread(text.data(),1,(size_t)size,file)==(size_t)size;
  fclose(file);
  if(!ok||text.find('\0')!=std::string::npos){ text.clear(); return false; }
  return true;
}

static bool spirvFileReady(const std::string &path) {
  FILE *file=fopen(path.c_str(),"rb");
  if(!file) return false;
  unsigned char magic[4]={};
  bool ok=fread(magic,1,sizeof(magic),file)==sizeof(magic)&&
      magic[0]==0x03&&magic[1]==0x02&&magic[2]==0x23&&magic[3]==0x07;
  fclose(file);
  return ok;
}

static bool parseCustomShaderInfo(const std::string &absolute,
                                  const std::string &relative,
                                  CustomShaderInfo &info) {
  std::string text;
  if(!readSmallTextFile(absolute,text)) return false;
  size_t options=text.find("<options>");
  size_t optionsEnd=options==std::string::npos?std::string::npos:
      text.find("</options>",options+9);
  if(optionsEnd==std::string::npos) return false;
  std::string body=text.substr(options+9,optionsEnd-options-9);
  size_t cursor=0;
  while(cursor<=body.size()){
    size_t end=body.find('\n',cursor);
    std::string line=trim(body.substr(cursor,end==std::string::npos?
        std::string::npos:end-cursor));
    size_t comment=line.find("//"); if(comment!=std::string::npos) line.erase(comment);
    size_t equals=line.find('=');
    if(equals!=std::string::npos&&trim(line.substr(0,equals))=="name")
      info.name=trim(line.substr(equals+1));
    if(end==std::string::npos) break;
    cursor=end+1;
  }
  if(info.name.empty()||info.name.size()>95) return false;
  cursor=0;
  while((cursor=text.find("<pass>",cursor))!=std::string::npos){ info.passes++; cursor+=6; }
  if(info.passes<1||info.passes>16) return false;
  info.relative=relative;
  info.vulkanReady=true;
  const std::string pack=absolute+".nxvk/pass";
  for(int pass=0;pass<info.passes;pass++){
    const std::string base=pack+std::to_string(pass);
    if(!spirvFileReady(base+".vert.spv")||!spirvFileReady(base+".frag.spv")){
      info.vulkanReady=false;
      break;
    }
  }
  if(g_uiTarget && SDL_GetRenderTarget(g_ren)!=g_uiTarget)
    SDL_SetRenderTarget(g_ren,g_uiTarget);
  return true;
}

static void scanCustomShaderDirectory(const std::string &absolute,
                                      const std::string &relative,int depth,
                                      std::vector<CustomShaderInfo> &shaders) {
  if(depth>8||shaders.size()>=512) return;
  DIR *directory=opendir(absolute.c_str()); if(!directory) return;
  while(dirent *entry=readdir(directory)){
    if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,"..")) continue;
    std::string name=entry->d_name;
    std::string childAbsolute=absolute+"/"+name;
    std::string childRelative=relative.empty()?name:relative+"/"+name;
    struct stat status{}; if(stat(childAbsolute.c_str(),&status)!=0) continue;
    if(S_ISDIR(status.st_mode)){
      if(name.size()>=5&&!strcasecmp(name.c_str()+name.size()-5,".nxvk")) continue;
      scanCustomShaderDirectory(childAbsolute,childRelative,depth+1,shaders);
    }else if(S_ISREG(status.st_mode)&&name.size()>=4&&
             !strcasecmp(name.c_str()+name.size()-4,".dfx")){
      CustomShaderInfo shader;
      if(parseCustomShaderInfo(childAbsolute,childRelative,shader))
        shaders.push_back(std::move(shader));
    }
  }
  closedir(directory);
}

static std::vector<CustomShaderInfo> customShaderList() {
  std::vector<CustomShaderInfo> shaders;
  scanCustomShaderDirectory(SHADERS_DIR,{},0,shaders);
  std::sort(shaders.begin(),shaders.end(),[](const auto &left,const auto &right){
    return strcasecmp(left.name.c_str(),right.name.c_str())<0;
  });
  return shaders;
}

static std::string customShaderValue(const char *value) {
  if(!value||!*value) return "(not selected)";
  const char *slash=strrchr(value,'/');
  std::string name=slash?slash+1:value;
  if(name.size()>4&&!strcasecmp(name.c_str()+name.size()-4,".dfx"))
    name.resize(name.size()-4);
  return name;
}

static bool validateCustomShaderSelection(const Store &settings,
                                          const std::string &renderer,
                                          std::string &error) {
  auto value=[&](const char *key,const char *fallback){
    for(const KV &entry:settings.kv) if(entry.k==key) return entry.v.c_str();
    return fallback;
  };
  if(strcmp(value("Wrapper/VideoFilter","nearest"),"custom")) return true;
  const char *selected=value("Wrapper/CustomShader","");
  if(!selected||!*selected){
    error="Select a custom shader in Settings -> Graphics";
    return false;
  }
  std::vector<CustomShaderInfo> shaders=customShaderList();
  for(const CustomShaderInfo &shader:shaders){
    if(shader.relative!=selected) continue;
    if(renderer=="vk"&&!shader.vulkanReady){
      error="Custom shader Vulkan pack is missing";
      return false;
    }
    return true;
  }
  error="Selected custom .dfx is missing or invalid";
  return false;
}

static int choiceIdx(const Opt &o) {
  const char *cur = iniGet(o.key, o.def);
  for (int i=0;i<o.nch;i++) if (!strcmp(o.ch[i].val, cur)) return i;
  return -1;
}
static bool optEnabled(const Opt &o) {
  if(o.type==OT_STATUS) return true;
  if(o.key && !strncmp(o.key,"Wrapper/LSFG",12) && !lsfgDllInstalled())
    return false;
  if(!o.gateKey) return true;
  const char *value=iniGet(o.gateKey,"");
  /* A leading '=' makes a gate opt-in for one choice; existing gates remain
     enabled for every value except gateOff. */
  return o.gateOff&&o.gateOff[0]=='='
      ? strcmp(value,o.gateOff+1)==0
      : strcmp(value,o.gateOff)!=0;
}

static bool localTimeFromMillis(const char *text, struct tm &result) {
  char *end=nullptr;
  long long millis=std::strtoll(text?text:"",&end,10);
  if(!end||*end||millis<=0) return false;
  time_t seconds=(time_t)(millis/1000);
  return localtime_r(&seconds,&result)!=nullptr;
}

static void optValue(const Opt &o, char *out, int n) {
  out[0]=0;
  if (o.type==OT_CHOICE){ int i=choiceIdx(o); snprintf(out,n,"%s", i>=0?o.ch[i].label:iniGet(o.key,o.def)); }
  else if (o.type==OT_RANGE) snprintf(out,n,"%s", iniGet(o.key,o.def));
  else if (o.type==OT_SCALED_RANGE) {
    int value=(int)std::lround(std::strtod(iniGet(o.key,o.def),nullptr)*o.multiplier);
    snprintf(out,n,"%d%s",value,o.suffix?o.suffix:"");
  }
  else if (o.type==OT_TEXT){ const char *v=iniGet(o.key,o.def); snprintf(out,n,"%s", (v&&*v)?v:"(auto)"); }
  else if (o.type==OT_HOTKEY){ const char *v=iniGet(o.key,o.def); snprintf(out,n,"%s", (v&&*v)?v:"None"); }
  else if (o.type==OT_STATUS) snprintf(out,n,"%s",lsfgDllInstalled()?"Installed":"Missing");
  else if (o.type==OT_SHADER) snprintf(out,n,"%s",customShaderValue(iniGet(o.key,o.def)).c_str());
  else if (o.type==OT_DATETIME) {
    struct tm value{};
    if(localTimeFromMillis(iniGet(o.key,o.def),value))
      strftime(out,(size_t)n,"%Y-%m-%d  %H:%M",&value);
    else snprintf(out,n,"Set date and time");
  }
  else if (o.type==OT_SUBMENU) snprintf(out,n,">");
}
static void optAdjust(const Opt &o, int dir) {
  if (!optEnabled(o)) return;
  if (o.type==OT_CHOICE){
    int i=choiceIdx(o); if(i<0)i=0; i=(i+dir+o.nch)%o.nch;
    iniSet(o.key,o.ch[i].val);
    if(!strcmp(o.key,"Drastic/CustomClockEnable") &&
       !strcmp(o.ch[i].val,"true")) {
      iniSet("Drastic/RtcSystemTime","false");
      struct tm unused{};
      if(!localTimeFromMillis(iniGet("Drastic/CustomClock","0"),unused)) {
        char timestamp[32];
        snprintf(timestamp,sizeof(timestamp),"%lld",
                 (long long)time(nullptr)*1000LL);
        iniSet("Drastic/CustomClock",timestamp);
      }
    } else if(!strcmp(o.key,"Drastic/RtcSystemTime") &&
              !strcmp(o.ch[i].val,"true")) {
      iniSet("Drastic/CustomClockEnable","false");
    }
  }
  else if (o.type==OT_RANGE){ int v=atoi(iniGet(o.key,o.def))+dir*o.step; if(v<o.lo)v=o.lo; if(v>o.hi)v=o.hi; char b[24]; snprintf(b,sizeof(b),"%d",v); iniSet(o.key,b); }
  else if (o.type==OT_SCALED_RANGE){
    int v=(int)std::lround(std::strtod(iniGet(o.key,o.def),nullptr)*o.multiplier)+dir*o.step;
    if(v<o.lo)v=o.lo;
    if(v>o.hi)v=o.hi;
    char b[24]; snprintf(b,sizeof(b),"%g",(double)v/o.multiplier); iniSet(o.key,b);
  }
  else if (o.type==OT_HOTKEY) iniSet(o.key,"None");
}

static const char *captureButton(SDL_GameController *pad) {
  struct M { SDL_GameControllerButton b; const char *tok; };
  static const M map[] = {
    {SDL_CONTROLLER_BUTTON_B,"A"},{SDL_CONTROLLER_BUTTON_A,"B"},{SDL_CONTROLLER_BUTTON_Y,"X"},{SDL_CONTROLLER_BUTTON_X,"Y"}, // Nintendo labels
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,"L"},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,"R"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK,"StickL"},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,"StickR"},
    {SDL_CONTROLLER_BUTTON_START,"Plus"},{SDL_CONTROLLER_BUTTON_BACK,"Minus"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,"Up"},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,"Down"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,"Left"},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,"Right"},
  };
  (void)pad;
  Uint32 start = SDL_GetTicks();
  const Uint32 acceptAfter=start+120;
  // Ignore the button press that opened this screen without blocking the event
  // pump; new input is accepted after the short release/debounce deadline.
  SDL_Event e;
  while (SDL_PollEvent(&e)) { /* flush */ }
  for (;;) {
    while (SDL_PollEvent(&e)) {
      if (!SDL_TICKS_PASSED(SDL_GetTicks(),acceptAfter)) continue;
      if (e.type == SDL_CONTROLLERBUTTONDOWN) {
        for (auto &m : map) if (e.cbutton.button == m.b) return m.tok;
      } else if (e.type == SDL_CONTROLLERAXISMOTION) { // ZL/ZR are analog triggers on Switch
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT  && e.caxis.value > 16000) return "ZL";
        if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT && e.caxis.value > 16000) return "ZR";
      } else if (e.type == SDL_QUIT) return "";
    }
    int remain = 6 - (int)((SDL_GetTicks() - start) / 1000);
    if (remain <= 0) return ""; // timed out -> cancel, keep current binding
    clearUiBackground();
    int pw=std::min(780,SW-64),ph=210,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big,SW/2,py+50,"Press a button to bind", COL_HI);
    char sub[64]; snprintf(sub,sizeof(sub),"wait %ds to cancel", remain);
    drawTextC(g_font,SW/2,py+126,sub, COL_DIM);
    presentUi();
    const Uint32 now=SDL_GetTicks();
    const Uint32 nextTick=std::min(start+6000,now+1000);
    waitForNextUiFrame(false,nextTick);
  }
}

static std::string captureButtonCombo(SDL_GameController *pad,const char *label) {
  struct M { SDL_GameControllerButton button; const char *token; };
  static const M buttons[] = {
    {SDL_CONTROLLER_BUTTON_B,"A"},{SDL_CONTROLLER_BUTTON_A,"B"},
    {SDL_CONTROLLER_BUTTON_Y,"X"},{SDL_CONTROLLER_BUTTON_X,"Y"},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,"L"},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,"R"},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK,"StickL"},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,"StickR"},
    {SDL_CONTROLLER_BUTTON_START,"Plus"},{SDL_CONTROLLER_BUTTON_BACK,"Minus"},
    {SDL_CONTROLLER_BUTTON_DPAD_UP,"Up"},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,"Down"},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT,"Left"},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,"Right"},
  };
  if(!pad) return {};
  constexpr unsigned triggerLeftBit=(unsigned)(sizeof(buttons)/sizeof(*buttons));
  constexpr unsigned triggerRightBit=triggerLeftBit+1;
  auto heldMask=[&](){
    Uint32 mask=0;
    SDL_GameControllerUpdate();
    for(unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);i++)
      if(SDL_GameControllerGetButton(pad,buttons[i].button)) mask|=(Uint32)1u<<i;
    if(SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERLEFT)>16000) mask|=(Uint32)1u<<triggerLeftBit;
    if(SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>16000) mask|=(Uint32)1u<<triggerRightBit;
    return mask;
  };
  auto maskText=[&](Uint32 mask){
    std::string value;
    auto append=[&](const char *token){ if(!value.empty()) value+='+'; value+=token; };
    for(unsigned i=0;i<sizeof(buttons)/sizeof(*buttons);i++) if(mask&((Uint32)1u<<i)) append(buttons[i].token);
    if(mask&((Uint32)1u<<triggerLeftBit)) append("ZL");
    if(mask&((Uint32)1u<<triggerRightBit)) append("ZR");
    return value;
  };

  const Uint32 start=SDL_GetTicks();
  bool armed=false,started=false;
  Uint32 captured=0;
  SDL_Event event;
  for(;;){
    while(SDL_PollEvent(&event)) if(event.type==SDL_QUIT) return {};
    const Uint32 held=heldMask();
    if(!armed){
      if(!held) armed=true;
    } else if(held){
      started=true;
      captured|=held;
    } else if(started){
      return maskText(captured);
    }

    int remain=10-(int)((SDL_GetTicks()-start)/1000);
    if(remain<=0) return {};
    clearUiBackground();
    int pw=std::min(840,SW-64),ph=250,px=(SW-pw)/2,py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    std::string title="Bind "; title+=label?label:"hotkey";
    drawTextC(g_font_big,SW/2,py+42,title.c_str(),COL_HI);
    drawTextC(g_font,SW/2,py+104,armed?"Hold every button, then release them":"Release the button used to open this screen",COL_TXT);
    std::string current=maskText(captured|held);
    drawTextC(g_font,SW/2,py+148,current.empty()?"Waiting...":current.c_str(),current.empty()?COL_DIM:COL_VAL);
    char sub[64]; snprintf(sub,sizeof(sub),"%ds to cancel",remain);
    drawTextC(g_font_sm,SW/2,py+204,sub,COL_DIM);
    presentUi();
    const Uint32 now=SDL_GetTicks();
    waitForNextUiFrame(false,std::min(start+10000,now+1000));
  }
}

static float g_hy = -1;
static void beginScreenFx(){ g_fxT = SDL_GetTicks(); g_hy = -1; }
static void drawFadeIn(){
  if(!g_uiAnimations) return;
  const int D = 160; int el = (int)(SDL_GetTicks() - g_fxT);
  if (el < D) fillRect(0,0,SW,SH,(SDL_Color){0,0,0,(Uint8)(200*(D-el)/D)});
}
static bool highResolutionUi(){ return g_outputW>=1600; }
static int topBarH(){
  if(g_launcherPortrait) return highResolutionUi()?132:104;
  return highResolutionUi()?112:80;
}
static void drawHeader(const char *title, const char *ctx){
  int bandH = topBarH() - 4;
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  if(g_launcherPortrait){
    const int titleY=ctx&&*ctx?(highResolutionUi()?18:12):
                     (bandH-TTF_FontHeight(g_font_big))/2;
    const int logoH=std::min(bandH-18,highResolutionUi()?62:48);
    const int logoW=logoH*16/9;
    if(g_logo){
      SDL_Rect logoRect={18,ctx&&*ctx?10:(bandH-logoH)/2,logoW,logoH};
      SDL_RenderCopy(g_ren,g_logo,nullptr,&logoRect);
    }
    const int titleWidth=std::max(80,SW-2*(logoW+34));
    const std::string shownTitle=fittedText(g_font_big,title,titleWidth);
    drawTextC(g_font_big,SW/2,titleY,shownTitle.c_str(),COL_VAL);
    if(ctx&&*ctx){
      const int contextWidth=SW-52;
      const std::string shownContext=fittedText(g_font_sm,ctx,contextWidth);
      drawTextC(g_font_sm,SW/2,bandH-TTF_FontHeight(g_font_sm)-12,
                shownContext.c_str(),COL_DIM);
    }
    return;
  }
  int lh = bandH - 12;
  if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh*16/9,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
  drawTextC(g_font_big,SW/2,(bandH-TTF_FontHeight(g_font_big))/2,title,COL_VAL);
  if (ctx&&*ctx) {
    int titleRight=SW/2+textW(g_font_big,title)/2;
    int maxWidth=(SW-28)-titleRight-30;
    if(maxWidth>40) drawScrollTextR(g_font_sm,SW-28,(bandH-TTF_FontHeight(g_font_sm))/2,maxWidth,ctx,COL_VAL);
  }
}
static int settingsRowH(){
  return g_launcherPortrait?(highResolutionUi()?78:64):46;
}
static int settingsListY(){
  return g_launcherPortrait?topBarH()+(highResolutionUi()?38:28):118;
}
static int settingsFooterReserve(){
  return g_launcherPortrait?(highResolutionUi()?104:88):72;
}
static int portraitRowInset(){
  /* Keep the selected row aligned with the same one-pixel gutter used by the
     landscape layout.  A larger portrait-only inset made the highlight look
     detached from its text after the UI target was rotated. */
  return 1;
}
static void listCol(int *colX,int *colW,int *labelX,int *valX){
  int margin=g_launcherPortrait?(highResolutionUi()?72:36):180;
  int w = SW-margin; if (w>980) w=980;
  *colW=w; *colX=(SW-w)/2;
  int inset=g_launcherPortrait?(highResolutionUi()?34:26):40;
  *labelX=*colX+inset; *valX=*colX+w-inset;
}
static int listVis(){
  int v=(SH-settingsListY()-settingsFooterReserve())/settingsRowH();
  return v<1?1:v;
}

static bool settingsRowNeedsStackedText(const char *label,const char *value,
                                        int labelX,int valX){
  if(!g_launcherPortrait) return false;
  const int gap=highResolutionUi()?32:24;
  return textW(g_font,label)+gap+textW(g_font,value)>valX-labelX;
}

static void drawSettingsRowText(const char *label,const char *value,
                                int slotY,int colW,int labelX,int valX,
                                bool current,SDL_Color labelColor,
                                SDL_Color valueColor,bool scrollValue,
                                int rowHeight){
  const int actualRowHeight=rowHeight>0?rowHeight:settingsRowH();
  if(settingsRowNeedsStackedText(label,value,labelX,valX)){
    const int maxWidth=std::max(40,valX-labelX);
    const int labelHeight=TTF_FontHeight(g_font);
    const int valueHeight=TTF_FontHeight(g_font_sm);
    const int gap=highResolutionUi()?5:3;
    const int blockHeight=labelHeight+gap+valueHeight;
    const int labelY=slotY+(actualRowHeight-blockHeight)/2;
    if(current)
      drawScrollTextL(g_font,labelX,labelY,maxWidth,label,labelColor);
    else
      drawText(g_font,labelX,labelY,
               fittedText(g_font,label,maxWidth).c_str(),labelColor);
    const int valueY=labelY+labelHeight+gap;
    drawScrollTextR(g_font_sm,valX,valueY,maxWidth,value,valueColor);
    return;
  }
  const int y=slotY+(actualRowHeight-TTF_FontHeight(g_font))/2;
  drawText(g_font,labelX,y,label,labelColor);
  if(scrollValue) drawScrollTextR(g_font,valX,y,colW/2-40,value,valueColor);
  else drawTextR(g_font,valX,y,value,valueColor);
}

static void drawFooterText(const char *text,int centerY){
  if(!text||!*text) return;
  std::vector<std::string> tokens;
  const std::string input=text;
  size_t start=0;
  while(start<input.size()){
    while(start<input.size()&&input[start]==' ') start++;
    if(start>=input.size()) break;
    size_t split=input.size(),next=input.size();
    for(size_t index=start+1;index<input.size();index++){
      if(input[index-1]==' '&&input[index]==' '){
        split=index-1; next=index+1;
        while(next<input.size()&&input[next]==' ') next++;
        break;
      }
    }
    std::string token=trim(input.substr(start,split-start));
    if(!token.empty()) tokens.push_back(std::move(token));
    start=next;
  }
  if(tokens.size()>=2&&tokens.size()%2==0){
    std::vector<FootItem> hints;
    hints.reserve(tokens.size()/2);
    for(size_t index=0;index+1<tokens.size()&&hints.size()<10;index+=2)
      hints.push_back({tokens[index].c_str(),tokens[index+1].c_str(),FA_NONE});
    drawFooterHints(hints.data(),(int)hints.size(),centerY>=0?centerY:SH-26);
    return;
  }
  g_footN=0;
  const int maxWidth=SW-32;
  const int y=centerY>=0?centerY-TTF_FontHeight(g_font_sm)/2:SH-38;
  drawTextC(g_font_sm,SW/2,y,fittedText(g_font_sm,text,maxWidth).c_str(),COL_DIM);
}

static void showHelpCard(const char *section,const char *title,const char *kind,
                         const std::string &description,const char *current,
                         const char *scope) {
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event;
    while(pollUiEvent(event)){
      pumpStick(event);
      int touchX=0,touchY=0;
      if(touchFeed(event,&touchX,&touchY)==TOUCH_TAP) return;
      if(event.type==SDL_CONTROLLERBUTTONDOWN &&
         (event.cbutton.button==BTN_CONFIRM ||
          event.cbutton.button==BTN_CANCEL ||
          event.cbutton.button==BTN_SETTINGS)) return;
    }

    clearUiBackground();
    const int panelWidth=std::min(SW-(g_launcherPortrait?64:120),1000);
    const int panelHeight=std::min(SH-96,500);
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
    drawText(g_font_sm,panelX+40,panelY+24,section&&*section?section:"Settings",COL_DIM);
    const char *helpTitle=title&&*title?title:"Setting help";
    drawText(g_font_big,panelX+40,panelY+58,
             fittedText(g_font_big,helpTitle,panelWidth-80).c_str(),COL_VAL);

    std::string metadata=kind&&*kind?kind:"Setting";
    if(scope&&*scope){ metadata+="  |  "; metadata+=scope; }
    drawText(g_font_sm,panelX+40,panelY+114,
             fittedText(g_font_sm,metadata,panelWidth-80).c_str(),COL_SEL);
    int bodyY=panelY+164;
    if(current&&*current){
      const char *prefix="Current: ";
      drawText(g_font_sm,panelX+40,panelY+146,prefix,COL_DIM);
      drawScrollTextL(g_font_sm,panelX+40+textW(g_font_sm,prefix),panelY+146,
                      panelWidth-80-textW(g_font_sm,prefix),current,COL_TXT);
      bodyY=panelY+198;
    }
    fillRect(panelX+40,bodyY-18,panelWidth-80,2,(SDL_Color){70,78,92,210});
    drawWrapped(g_font,panelX+40,bodyY,panelWidth-80,32,7,
                description.c_str(),COL_TXT);
    FootItem closeHints[]={{"A","Close",FA_NONE},{"B","",FA_NONE},{"X","",FA_NONE}};
    drawFooterHints(closeHints,3,panelY+panelHeight-50);
    drawTextC(g_font_sm,SW/2,panelY+panelHeight-24,"Touch anywhere to close",COL_DIM);
    presentUi();
    waitForNextUiFrame();
  }
}

static void showOptionHelp(const char *section,const Opt &option,
                           const char *scope) {
  SettingHelpInfo help=settingHelpFor(option);
  char value[256]={};
  const char *current=nullptr;
  if(option.type!=OT_SUBMENU){ optValue(option,value,sizeof(value)); current=value; }
  showHelpCard(LauncherLocalization::Translate(section).data(),
               LauncherLocalization::Translate(option.label).data(),
               LauncherLocalization::Translate(help.kind).data(),
               std::string(LauncherLocalization::Translate(help.text)),current,
               LauncherLocalization::Translate(scope).data());
}

static const char *settingsScreenDescription(int screen) {
  switch(screen){
    case SCR_GRAPHICS: return "Selects the renderer, screen arrangement, scaling, post-processing, FPS display, and DraStic's 3D or visual compatibility options.";
    case SCR_ENHANCE: return "Contains true 3D enhancements alongside performance trade-offs and game-specific display workarounds. Use X Help on each option before changing it.";
    case SCR_FRAMEGEN: return "Configures Vulkan-only LSFG 2x frame generation. It creates intermediate display frames but does not make Nintendo DS emulation run faster.";
    case SCR_AUDIO: return "Controls emulated sound, output volume, buffering latency, and the simulated Nintendo DS microphone.";
    case SCR_EMU: return "Controls DraStic CPU use, ROM loading, saves, the emulated clock, firmware identity, frame skipping, and fast-forward behavior.";
    case SCR_FRAMERATE: return "Controls frame skipping, fast-forward limits, and auto-fire timing. Frame skipping is a performance trade-off and can break visuals in some games.";
    case SCR_NETWORK: return "Controls optional DraStic gameplay features, Slot-2 accessories, Lua, and how in-game save data interacts with savestates or other emulators.";
    case SCR_CONTROLLER: return "Maps Nintendo DS controls, configures stick, motion, and USB-mouse stylus input, and assigns unique in-game hotkey combinations.";
    case SCR_FIRMWARE: return "Edits the user information reported by the emulated Nintendo DS firmware, including language, nickname, favorite color, and birthday.";
    default: return "Opens this group of emulator settings.";
  }
}

static void renderSettings(int scr,int sel,int top,const char *ctx){
  clearUiBackground();
  const Screen &S=g_screens[scr];
  drawHeader(LauncherLocalization::Translate(S.title).data(), ctx);
  int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
  int vis=listVis();
  const int rowH=settingsRowH(),listY=settingsListY();
  glassPanel(colX-12,listY-10,colW+24,vis*rowH+18);
  const int rowInset=g_launcherPortrait?portraitRowInset():1;
  float ty = (float)(listY + (sel-top)*rowH + rowInset);
  g_hy = (!g_uiAnimations||g_hy<0) ? ty : g_hy + (ty-g_hy)*0.30f;
  fillRect(colX,(int)g_hy,colW,rowH-rowInset*2,COL_FOCUS);
  fillRect(colX,(int)g_hy,5,rowH-rowInset*2,COL_SEL);
  for(int r=0;r<vis && top+r<S.n;r++){
    int i=top+r,slotY=listY+r*rowH; bool cur=(i==sel); bool en=optEnabled(S.opts[i]);
    SDL_Color lc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_TXT);
    SDL_Color vc = !en?(SDL_Color){92,98,110,255}:(cur?COL_VAL:COL_DIM);
    char v[256]; optValue(S.opts[i],v,sizeof(v));
    const std::string value=std::string(LauncherLocalization::Translate(v));
    drawSettingsRowText(LauncherLocalization::Translate(S.opts[i].label).data(),value.c_str(),slotY,colW,labelX,valX,
                        cur,lc,vc,S.opts[i].type==OT_SHADER);
    if(S.opts[i].key && !strcmp(S.opts[i].key,"Drastic/FirmwareColor")){
      int colorIndex=choiceIdx(S.opts[i]);
      if(colorIndex>=0 && colorIndex<(int)(sizeof(C_firmwareColorRgb)/sizeof(*C_firmwareColorRgb))){
        const bool stacked=settingsRowNeedsStackedText(S.opts[i].label,v,labelX,valX);
        TTF_Font *valueFont=stacked?g_font_sm:g_font;
        const int swatchSize=g_launcherPortrait?20:24;
        int valueY=slotY+(rowH-TTF_FontHeight(valueFont))/2;
        if(stacked){
          const int gap=highResolutionUi()?5:3;
          const int blockHeight=TTF_FontHeight(g_font)+gap+TTF_FontHeight(g_font_sm);
          valueY=slotY+(rowH-blockHeight)/2+TTF_FontHeight(g_font)+gap;
        }
        const int swatchX=valX-textW(valueFont,v)-swatchSize-12;
        const int swatchY=valueY+(TTF_FontHeight(valueFont)-swatchSize)/2;
        fillRect(swatchX,swatchY,swatchSize,swatchSize,C_firmwareColorRgb[colorIndex]);
        border(swatchX,swatchY,swatchSize,swatchSize,1,(SDL_Color){225,230,240,255});
      }
    }
  }
  if(S.n>vis){
    int trH=vis*rowH, trX=colX+colW+16, trY=listY-2;
    fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
    int thH=trH*vis/S.n, denom=(S.n-vis>0?S.n-vis:1);
    fillRect(trX,trY+(trH-thH)*top/denom,4,thH,COL_SEL);
  }
  drawFooterText("Left / Right  Change       A  Choose       X  Help       B  Back");
  drawFadeIn();
  presentUi();
}

static int dropdown(const char *title, const char *const *labels, int n, int cur,
                    const SDL_Color *swatches) {
  int sel = (cur < 0 || cur >= n) ? 0 : cur, top = 0;
  const int rowH = 52;
  int vis = (SH - 200) / rowH; if (vis < 1) vis = 1; if (vis > n) vis = n;
  beginScreenFx();
  for (;;) {
    if(!beginUiFrame()) return cur;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(touchScrollList(tk,sel,top,n,vis)) continue;
        if(tk==TOUCH_TAP){ int pw=SW>760?760:SW-160,px=(SW-pw)/2,ly=(SH-(90+vis*rowH))/2+70;
          for(int r=0;r<vis&&top+r<n;r++){ int y=ly+r*rowH; if(ty>=y&&ty<y+rowH&&tx>=px&&tx<px+pw){ return top+r; } }
        } }
      if (e.type != SDL_CONTROLLERBUTTONDOWN) continue;
      switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n;   break;
        case BTN_CONFIRM: return sel;
        case BTN_CANCEL:  return cur;
      }
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
      if(top<0) top=0;
    }
    clearUiBackground();
    int pw = SW>760?760:SW-160, ph = 90 + vis*rowH, px=(SW-pw)/2, py=(SH-ph)/2;
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+18, title, COL_VAL);
    int ly = py+70;
    for(int r=0;r<vis && top+r<n;r++){
      int i=top+r, y=ly+r*rowH; bool curr=(i==sel);
      if(curr){ fillRect(px+8,y,pw-16,rowH-4,COL_FOCUS); fillRect(px+8,y,5,rowH-4,COL_SEL); }
      int textX=px+34;
      const int textY=y+(rowH-TTF_FontHeight(g_font))/2;
      if(swatches){
        const int swatchSize=28;
        const int swatchY=y+(rowH-swatchSize)/2;
        fillRect(textX,swatchY,swatchSize,swatchSize,swatches[i]);
        border(textX,swatchY,swatchSize,swatchSize,1,(SDL_Color){225,230,240,255});
        textX+=swatchSize+14;
      }
      const int textWidth=px+pw-34-textX;
      if(curr)
        drawScrollTextL(g_font,textX,textY,textWidth,labels[i],COL_VAL);
      else
        drawText(g_font,textX,textY,
                 ellipsizedText(g_font,labels[i],textWidth).c_str(),COL_TXT);
    }
    if(n>vis){ int trH=vis*rowH,trX=px+pw-12,trY=ly; fillRect(trX,trY,4,trH,(SDL_Color){40,44,54,255});
      int thH=trH*vis/n,dn=(n-vis>0?n-vis:1); fillRect(trX,trY+(trH-thH)*top/dn,4,thH,COL_SEL); }
    drawFadeIn();
    presentUi();
    waitForNextUiFrame();
  }
}
static void optChoosePopup(const Opt &o) {
  if(o.type!=OT_CHOICE || o.nch<=0) return;
  const char* labels[32];std::array<std::string,32> localized; int n = o.nch>32?32:o.nch;
  for(int i=0;i<n;i++){localized[i]=LauncherLocalization::Translate(o.ch[i].label);labels[i]=localized[i].c_str();}
  const SDL_Color *swatches=o.key&&!strcmp(o.key,"Drastic/FirmwareColor")
      ? C_firmwareColorRgb : nullptr;
  int idx = dropdown(LauncherLocalization::Translate(o.label).data(), labels, n, choiceIdx(o), swatches);
  if(idx>=0 && idx<o.nch) iniSet(o.key, o.ch[idx].val);
}

static void chooseCustomShader(const Opt &option) {
  std::vector<CustomShaderInfo> shaders=customShaderList();
  if(shaders.empty()){
    modalMessage("No custom shaders found",
                 {"Copy DraStic .dfx/.dsd shader files to:",
                  "/switch/drastic/shaders/"});
    return;
  }
  const bool vulkan=!strcmp(iniGet("Wrapper/Renderer","vk"),"vk");
  std::vector<std::string> labelStorage;
  std::vector<const char*> labels;
  labelStorage.reserve(shaders.size()); labels.reserve(shaders.size());
  int current=-1;
  const char *configured=iniGet(option.key,option.def);
  for(size_t index=0;index<shaders.size();index++){
    std::string label=shaders[index].name;
    if(vulkan&&!shaders[index].vulkanReady) label+="  [Vulkan pack missing]";
    labelStorage.push_back(std::move(label));
    if(!strcmp(shaders[index].relative.c_str(),configured)) current=(int)index;
  }
  for(const std::string &label:labelStorage) labels.push_back(label.c_str());
  int selected=dropdown("Custom DraStic shader",labels.data(),
                        (int)labels.size(),current);
  if(selected<0||selected>=(int)shaders.size()) return;
  if(vulkan&&!shaders[selected].vulkanReady){
    modalMessage("Vulkan shader pack missing",
                 {shaders[selected].name,
                  "Compile the .dfx with tools/compile_custom_shader.py,",
                  "then keep its .dfx.nxvk folder beside the shader."});
    return;
  }
  iniSet(option.key,shaders[selected].relative.c_str());
  iniSet("Wrapper/VideoFilter","custom");
}

static bool leapYear(int year) {
  return (year%4==0 && year%100!=0) || year%400==0;
}

static int daysInMonth(int year,int month) {
  static const int days[]={31,28,31,30,31,30,31,31,30,31,30,31};
  return month==2 ? days[1]+(leapYear(year)?1:0) : days[month-1];
}

static void editCustomClock(const Opt &option) {
  struct tm date{};
  if(!localTimeFromMillis(iniGet(option.key,option.def),date)) {
    time_t now=time(nullptr);
    localtime_r(&now,&date);
  }
  date.tm_sec=0;
  int values[]={date.tm_year+1900,date.tm_mon+1,date.tm_mday,
                date.tm_hour,date.tm_min};
  const char *labels[]={"Year","Month","Day","Hour","Minute"};
  int selected=0;
  beginScreenFx();
  for(;;) {
    if(!beginUiFrame()) return;
    SDL_Event event;
    navRepeat();
    while(pollUiEvent(event)) {
      pumpStick(event);
      int touchX=0,touchY=0;
      TouchKind touch=touchFeed(event,&touchX,&touchY);
      if(touch==TOUCH_TAP) {
        int panelY=(SH-430)/2;
        for(int row=0;row<5;row++)
          if(touchY>=panelY+92+row*54 && touchY<panelY+146+row*54)
            selected=row;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)
        selected=(selected+4)%5;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        selected=(selected+1)%5;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
              event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
        int delta=event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT?1:-1;
        int lo[]={2000,1,1,0,0};
        int hi[]={2099,12,daysInMonth(values[0],values[1]),23,59};
        values[selected]+=delta;
        if(values[selected]<lo[selected]) values[selected]=hi[selected];
        if(values[selected]>hi[selected]) values[selected]=lo[selected];
        int maxDay=daysInMonth(values[0],values[1]);
        if(values[2]>maxDay) values[2]=maxDay;
      } else if(event.cbutton.button==BTN_CONFIRM) {
        struct tm result{};
        result.tm_year=values[0]-1900;
        result.tm_mon=values[1]-1;
        result.tm_mday=values[2];
        result.tm_hour=values[3];
        result.tm_min=values[4];
        result.tm_isdst=-1;
        time_t seconds=mktime(&result);
        if(seconds!=(time_t)-1) {
          char timestamp[32];
          snprintf(timestamp,sizeof(timestamp),"%lld",
                   (long long)seconds*1000LL);
          iniSet(option.key,timestamp);
        }
        return;
      } else if(event.cbutton.button==BTN_CANCEL) return;
    }

    clearUiBackground();
    drawHeader("Custom clock",nullptr);
    int panelW=660,panelH=430,panelX=(SW-panelW)/2,panelY=(SH-panelH)/2;
    glassPanel(panelX,panelY,panelW,panelH);
    border(panelX,panelY,panelW,panelH,3,COL_SEL);
    drawTextC(g_font_sm,SW/2,panelY+30,
              "Starting date and time (local)",COL_DIM);
    for(int row=0;row<5;row++) {
      int y=panelY+92+row*54;
      bool current=row==selected;
      if(current) {
        fillRect(panelX+24,y-8,panelW-48,48,COL_FOCUS);
        fillRect(panelX+24,y-8,5,48,COL_SEL);
      }
      drawText(g_font,panelX+58,y,labels[row],current?COL_VAL:COL_TXT);
      char value[16];
      if(row==0) snprintf(value,sizeof(value),"%04d",values[row]);
      else snprintf(value,sizeof(value),"%02d",values[row]);
      drawTextR(g_font,panelX+panelW-58,y,value,current?COL_VAL:COL_DIM);
    }
    drawFooterText("Left / Right  Change       A  Save       B  Cancel",
                   panelY+panelH-28);
    drawFadeIn();
    presentUi();
    waitForNextUiFrame();
  }
}

static const Opt *findHotkeyConflict(const Opt &option,
                                     const std::string &combo) {
  if(combo.empty() || !strcasecmp(combo.c_str(), "None")) return nullptr;
  for(int screen=0;screen<SCR_COUNT;screen++){
    const Screen &candidateScreen=g_screens[screen];
    for(int index=0;index<candidateScreen.n;index++){
      const Opt &candidate=candidateScreen.opts[index];
      if(candidate.type!=OT_HOTKEY || !candidate.key ||
         !strcmp(candidate.key,option.key)) continue;
      if(!strcasecmp(iniGet(candidate.key,candidate.def),combo.c_str()))
        return &candidate;
    }
  }
  return nullptr;
}

static int s_setSel[SCR_COUNT]={0}, s_setTop[SCR_COUNT]={0};
static bool resetOption(const Opt &option){
  if(!option.key||!option.def)return false;
  if(g_active==&g_game){
    const bool existed=storeHas(g_game,option.key);storeRemove(g_game,option.key);return existed;
  }
  const bool changed=strcmp(storeGet(*g_active,option.key,option.def),option.def)!=0;
  iniSet(option.key,option.def);return changed;
}
static void runSettings(int scr, SDL_GameController *pad, const char *ctx) {
  if(scr==SCR_FRAMEGEN){
    normalizeLsfgStore(g_global);
    if(g_active!=&g_global) normalizeLsfgStore(*g_active);
  }
  const Screen &S=g_screens[scr];
  int sel=s_setSel[scr],top=s_setTop[scr];
  if(sel<0||sel>=S.n) sel=0;
  if(top<0||top>=S.n) top=0;
  while(sel<S.n-1 && !optEnabled(S.opts[sel])) sel++;
  auto nav=[&](int dir){ for(int k=0;k<S.n;k++){ sel=(sel+dir+S.n)%S.n; if(optEnabled(S.opts[sel])) break; } };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        int visible=listVis();
        if(touchScrollList(tk,sel,top,S.n,visible)){ s_setSel[scr]=sel; s_setTop[scr]=top; continue; }
        if(tk==TOUCH_SWIPE_L){ optAdjust(S.opts[sel],-1); continue; }
        if(tk==TOUCH_SWIPE_R){ optAdjust(S.opts[sel],+1); continue; }
        if(tk==TOUCH_TAP){
          if(ty<topBarH() || ty>=SH-40){ return; }
          int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX); int vis=listVis();
          const int rowH=settingsRowH(),listY=settingsListY();
          for(int r=0;r<vis && top+r<S.n;r++){ int y=listY+r*rowH;
            if(ty>=y && ty<y+rowH){ int ni=top+r; if(optEnabled(S.opts[ni])){ sel=ni;
              if(tx>=colX+colW/2){ SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); } }
              break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP:   nav(-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: nav(+1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  optAdjust(S.opts[sel],-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: optAdjust(S.opts[sel], 1); break;
        case BTN_CONFIRM: {
          const Opt &o=S.opts[sel];
          if(o.type==OT_SUBMENU){ runSettings(o.sub,pad,ctx); beginScreenFx(); }
          else if(o.type==OT_SHADER){ chooseCustomShader(o); beginScreenFx(); }
          else if(o.type==OT_DATETIME){ editCustomClock(o); beginScreenFx(); }
          else if(o.type==OT_TEXT){
            if(optEnabled(o)){
              char buf[128];
              if(promptText(o.label, iniGet(o.key,o.def), buf, sizeof(buf))) iniSet(o.key,buf);
            }
            beginScreenFx();
          }
          else if(o.type==OT_HOTKEY){
            if(optEnabled(o)){
              std::string combo=captureButtonCombo(pad,o.label);
              if(!combo.empty()){
                const Opt *conflict=findHotkeyConflict(o,combo);
                if(conflict){
                  modalMessage("Hotkey already assigned",
                               {combo,"This combination is already used by:",
                                conflict->label});
                } else {
                  iniSet(o.key,combo.c_str());
                }
              }
            }
            beginScreenFx();
          }
          else if(S.binds && o.type==OT_CHOICE && o.ch==C_btn){
            const char *tok=captureButton(pad);
            if(tok&&*tok) iniSet(o.key,tok);
            beginScreenFx();
          }
          else if(o.type==OT_CHOICE && o.nch>2 && optEnabled(o)){ optChoosePopup(o); beginScreenFx(); }
          else optAdjust(o,1);
          break;
        }
        case BTN_SETTINGS:
          showOptionHelp(S.title,S.opts[sel],ctx&&*ctx?"Per-game setting":"Global setting");
          beginScreenFx();
          break;
        case SDL_CONTROLLER_BUTTON_X:
          if(resetOption(S.opts[sel])) toast("Setting reset to default",450);
          break;
        case BTN_CANCEL: return;
      }
      int vis=listVis(); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1; if(top<0)top=0;
      s_setSel[scr]=sel; s_setTop[scr]=top;
    }
    renderSettings(scr,sel,top,ctx);
    waitForNextUiFrame();
  }
}
static std::string launcherUpdateStatusText() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  switch(snapshot.state){
    case LauncherUpdateState::Checking: return "Checking...";
    case LauncherUpdateState::UpdateAvailable: return snapshot.release.tag+" available";
    case LauncherUpdateState::UpToDate: return "Up to date";
    case LauncherUpdateState::Downloading: {
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const uint64_t percent=total?std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      return "Downloading "+std::to_string(percent)+"%";
    }
    case LauncherUpdateState::ReadyToInstall: return "Ready to install";
    case LauncherUpdateState::Installing: return "Installing...";
    case LauncherUpdateState::Installed: return "Ready to exit";
    case LauncherUpdateState::Cancelled: return "Cancelled";
    case LauncherUpdateState::Error: return "Check failed";
    case LauncherUpdateState::Idle: break;
  }
  return std::string("Installed ")+installedReleaseTag();
}

static void launcherSettingsScreen() {
  static int savedSelection=0;
  const int optionCount=(int)(sizeof(S_launcher)/sizeof(Opt));
  const int apiKeyRow=optionCount,listCount=optionCount+1;
  const int updateRow=listCount,selectionCount=listCount+1;
  int sel=std::max(0,std::min(savedSelection,selectionCount-1)),top=0;
  auto applyChange=[&](){
    LauncherLocalization::Initialize(storeGet(g_global,"Wrapper/Language","system"));
    applyLauncherAppearance();
    const int requested=atoi(storeGet(g_global,"Wrapper/LauncherRotation","0"));
    if(!configureLauncherOrientation(requested)){
      storeSet(g_global,"Wrapper/LauncherRotation",
               std::to_string(g_launcherRotation).c_str());
      toast("Could not change launcher orientation");
    } else {
      g_touch={};
      beginScreenFx();
    }
    uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  };
  auto finish=[&](){ savedSelection=sel; storeSave(g_global,LAUNCHER_INI); };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ finish(); return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      const int rowH=settingsRowH(),listY=settingsListY();
      const int visible=std::min(std::max(1,(SH-listY-190)/rowH),listCount);
      const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
      const int buttonX=(SW-buttonWidth)/2;
      const int buttonY=std::min(SH-buttonHeight-104,listY+visible*rowH+24);
      if(touchScrollList(touch,sel,top,listCount,visible)) continue;
      if(touch==TOUCH_SWIPE_L&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); continue; }
      if(touch==TOUCH_SWIPE_R&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); continue; }
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ finish(); return; }
        if(tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight){
          sel=updateRow;
          SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press);
          continue;
        }
        for(int row=0;row<visible&&top+row<listCount;row++){
          int y=listY+row*rowH;
          if(ty>=y&&ty<y+rowH){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+selectionCount-1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%selectionCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT&&sel<optionCount){ optAdjust(S_launcher[sel],-1); applyChange(); }
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT&&sel<optionCount){ optAdjust(S_launcher[sel],1); applyChange(); }
      else if(event.cbutton.button==BTN_CONFIRM){
        if(sel==updateRow){ runUpdateScreen(); beginScreenFx(); }
        else if(sel==apiKeyRow){
          char key[192];snprintf(key,sizeof(key),"%s",storeGet(g_global,"Wrapper/SteamGridDBKey",""));
          const std::string currentKey=key;
          if(promptTextMode("SteamGridDB API key",currentKey.c_str(),key,sizeof(key),true,true)){
            storeSet(g_global,"Wrapper/SteamGridDBKey",trim(key).c_str());storeSave(g_global,LAUNCHER_INI);
          }
          beginScreenFx();
        }
        else {
          const Opt &option=S_launcher[sel];
          if(option.type==OT_CHOICE&&option.nch>2){ optChoosePopup(option); beginScreenFx(); }
          else optAdjust(option,1);
          applyChange();
        }
      } else if(event.cbutton.button==BTN_SETTINGS){
        if(sel<optionCount)
          showOptionHelp("Launcher",S_launcher[sel],"Launcher setting");
        else if(sel==apiKeyRow)
          showHelpCard("Launcher","SteamGridDB API key","Online account",
                       "Stores the API key used to search and download game covers and shortcut icons. Select this row again to replace an incorrect key.",
                       nullptr,"Launcher setting");
        else
          showHelpCard("Launcher","Check for Updates","Launcher updates",
                       "Checks the latest published DrasticDS_nx release, displays its notes, verifies the downloaded NRO, and safely replaces this launcher.",
                       nullptr,"Launcher action");
        beginScreenFx();
      } else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X){
        if(sel<optionCount){if(resetOption(S_launcher[sel])){applyChange();toast("Setting reset to default",450);}}
        else{storeRemove(g_global,"Wrapper/SteamGridDBKey");toast("Setting reset to default",450);}
      } else if(event.cbutton.button==BTN_CANCEL){ finish(); return; }
      if(sel<listCount){
        if(sel<top) top=sel;
        if(sel>=top+visible) top=sel-visible+1;
      }
    }

    clearUiBackground();
    drawHeader("Launcher",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    const int rowH=settingsRowH(),listY=settingsListY();
    const int visible=std::min(std::max(1,(SH-listY-190)/rowH),listCount);
    glassPanel(colX-12,listY-10,colW+24,visible*rowH+18);
    const int rowInset=g_launcherPortrait?portraitRowInset():1;
    if(sel<listCount){
      float target=(float)(listY+(sel-top)*rowH+rowInset);
      g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
      fillRect(colX,(int)g_hy,colW,rowH-rowInset*2,COL_FOCUS);
      fillRect(colX,(int)g_hy,5,rowH-rowInset*2,COL_SEL);
    }
    for(int row=0;row<visible&&top+row<listCount;row++){
      int index=top+row,slotY=listY+row*rowH; bool current=index==sel;
      if(index==apiKeyRow){
        const bool configured=storeGet(g_global,"Wrapper/SteamGridDBKey","")[0];
        drawSettingsRowText("SteamGridDB API key",configured?"Configured":"Not configured",slotY,colW,labelX,valX,
                            current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM);
      } else {
        char value[96]; optValue(S_launcher[index],value,sizeof(value));
        drawSettingsRowText(S_launcher[index].label,value,slotY,colW,labelX,valX,
                            current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM);
      }
    }
    const int buttonWidth=std::min(500,SW-80),buttonHeight=58;
    const int buttonX=(SW-buttonWidth)/2;
    const int buttonY=std::min(SH-buttonHeight-104,listY+visible*rowH+24);
    const bool updateSelected=sel==updateRow;
    fillRect(buttonX,buttonY,buttonWidth,buttonHeight,updateSelected?COL_FOCUS:(SDL_Color){35,40,50,225});
    border(buttonX,buttonY,buttonWidth,buttonHeight,2,updateSelected?COL_SEL:COL_DIM);
    const int fontHeight=TTF_FontHeight(g_font);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-fontHeight)/2,"Check for Updates",updateSelected?COL_VAL:COL_TXT);
    const std::string updateStatus=launcherUpdateStatusText();
    drawTextC(g_font_sm,SW/2,buttonY+buttonHeight+8,updateStatus.c_str(),updateSelected?COL_VAL:COL_DIM);
  drawFooterText("Left / Right  Change       A  Choose       X  Help       Y  Reset       B  Back");
    drawFadeIn(); presentUi(); waitForNextUiFrame();
  }
}

static std::string installedReleaseTag() {
  const std::string built=LauncherUpdate_BuiltReleaseTag();
#if defined(DRASTIC_NX_UPDATE_TEST)
  return built;
#else
  const std::string stored=storeGet(g_global,"Wrapper/InstalledReleaseTag","");
  if(stored.empty()) return built;
  return LauncherUpdate_IsNewer(stored,built)?stored:built;
#endif
}

static std::vector<size_t> utf8Boundaries(const std::string &text) {
  std::vector<size_t> boundaries{0};
  for(size_t index=0;index<text.size();){
    const unsigned char lead=(unsigned char)text[index];
    size_t length=lead<0x80?1:(lead&0xe0)==0xc0?2:(lead&0xf0)==0xe0?3:(lead&0xf8)==0xf0?4:1;
    if(index+length>text.size()) length=1;
    for(size_t part=1;part<length;part++) if(((unsigned char)text[index+part]&0xc0)!=0x80){ length=1; break; }
    index+=length;
    boundaries.push_back(index);
  }
  return boundaries;
}

static std::vector<std::string> wrapReleaseNotes(const std::string &notes,int maxWidth) {
  std::vector<std::string> lines;
  size_t paragraphStart=0;
  while(paragraphStart<=notes.size()){
    size_t paragraphEnd=notes.find('\n',paragraphStart);
    if(paragraphEnd==std::string::npos) paragraphEnd=notes.size();
    std::string paragraph=notes.substr(paragraphStart,paragraphEnd-paragraphStart);
    if(!paragraph.empty()&&paragraph.back()=='\r') paragraph.pop_back();
    for(char &value:paragraph) if(value=='\t'||(unsigned char)value<0x20) value=' ';
    while(!paragraph.empty()&&paragraph.back()==' ') paragraph.pop_back();
    if(paragraph.empty()) lines.emplace_back();
    else {
      bool continuation=false;
      while(!paragraph.empty()){
        while(!paragraph.empty()&&paragraph.front()==' ') paragraph.erase(paragraph.begin());
        if(paragraph.empty()) break;
        const std::string prefix=continuation&&paragraph.rfind("- ",0)!=0?"  ":"";
        if(textW(g_font_sm,(prefix+paragraph).c_str())<=maxWidth){ lines.push_back(prefix+paragraph); break; }
        const auto boundaries=utf8Boundaries(paragraph);
        size_t low=1,high=boundaries.size()-1;
        while(low<high){
          size_t middle=(low+high+1)/2;
          if(textW(g_font_sm,(prefix+paragraph.substr(0,boundaries[middle])).c_str())<=maxWidth) low=middle;
          else high=middle-1;
        }
        size_t split=boundaries[low];
        size_t space=paragraph.rfind(' ',split);
        if(space!=std::string::npos&&space>0&&space>=split/3) split=space;
        lines.push_back(prefix+paragraph.substr(0,split));
        paragraph.erase(0,split);
        continuation=true;
      }
    }
    if(paragraphEnd==notes.size()) break;
    paragraphStart=paragraphEnd+1;
  }
  if(lines.empty()) lines.emplace_back("No release notes were provided.");
  return lines;
}

static void requestLauncherExitAfterUpdate() {
  g_exitRequested=true;
}

static void runUpdateScreen() {
  if(g_launcherNroPath.empty()){
    modalMessage("Update check unavailable",{
      "Could not determine the current launcher NRO path.",
      "Launch DrasticDS.nro directly from the SD card and try again."
    });
    return;
  }
  if(!g_griddbReady){
    modalMessage("Update check unavailable",{
      "The launcher could not initialize its network connection.",
      "Check the connection and try again."
    });
    return;
  }
  LauncherUpdateSnapshot initial=LauncherUpdate_GetSnapshot();
  if(initial.state==LauncherUpdateState::Idle)
    LauncherUpdate_StartCheck(installedReleaseTag());

  int scroll=0;
  bool cancelRequested=false,installedSaved=false;
  std::string wrappedTag,wrappedBody;
  std::vector<std::string> wrappedLines;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){
      LauncherUpdate_Cancel();
      return;
    }
    LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
    if(snapshot.state==LauncherUpdateState::ReadyToInstall){
      if(g_romfsReady){ romfsExit(); g_romfsReady=false; }
      LauncherUpdate_InstallDownloaded(g_launcherNroPath);
      snapshot=LauncherUpdate_GetSnapshot();
      if(snapshot.state==LauncherUpdateState::Error&&!g_romfsReady&&R_SUCCEEDED(romfsInit()))
        g_romfsReady=true;
    }
    if(snapshot.state==LauncherUpdateState::Installed&&!installedSaved){
      storeSet(g_global,"Wrapper/InstalledReleaseTag",snapshot.release.tag.c_str());
      storeSave(g_global,LAUNCHER_INI);
      g_updateNoticeTag.clear();
      installedSaved=true;
    }

    const int panelWidth=SW*7/8,panelHeight=SH*4/5;
    const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
    const int bodyX=panelX+42,bodyY=panelY+126,bodyWidth=panelWidth-84;
    const int footerHeight=108,bodyBottom=panelY+panelHeight-footerHeight;
    const int lineHeight=TTF_FontHeight(g_font_sm)+8;
    const int visibleLines=std::max(1,(bodyBottom-bodyY)/lineHeight);
    if(snapshot.release.tag!=wrappedTag||snapshot.release.notes!=wrappedBody){
      wrappedTag=snapshot.release.tag;
      wrappedBody=snapshot.release.notes;
      wrappedLines=wrapReleaseNotes(wrappedBody.empty()?"Release notes will appear here.":wrappedBody,bodyWidth-20);
      scroll=0;
    }
    const int maxScroll=std::max(0,(int)wrappedLines.size()-visibleLines);
    scroll=std::max(0,std::min(scroll,maxScroll));

    SDL_Event event;
    navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SCROLL_UP) scroll=std::min(maxScroll,scroll+std::max(1,g_touchScrollSteps));
      else if(touch==TOUCH_SCROLL_DOWN) scroll=std::max(0,scroll-std::max(1,g_touchScrollSteps));
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) scroll=std::max(0,scroll-1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) scroll=std::min(maxScroll,scroll+1);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER) scroll=std::max(0,scroll-visibleLines);
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) scroll=std::min(maxScroll,scroll+visibleLines);
      else if(event.cbutton.button==BTN_CANCEL){
        if(snapshot.state==LauncherUpdateState::Downloading){ LauncherUpdate_Cancel(); cancelRequested=true; }
        else if(snapshot.state!=LauncherUpdateState::Installed) return;
      } else if(event.cbutton.button==BTN_CONFIRM){
        if(snapshot.state==LauncherUpdateState::UpdateAvailable){
          if(LauncherUpdate_StartDownload(g_launcherNroPath)) cancelRequested=false;
        } else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled){
          cancelRequested=false;
          LauncherUpdate_StartCheck(installedReleaseTag());
        } else if(snapshot.state==LauncherUpdateState::Installed){
          requestLauncherExitAfterUpdate();
          return;
        }
      }
    }

    snapshot=LauncherUpdate_GetSnapshot();
    clearUiBackground();
    fillRect(0,0,SW,SH,(SDL_Color){0,0,0,105});
    glassPanel(panelX,panelY,panelWidth,panelHeight);
    border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
    drawTextC(g_font_big,SW/2,panelY+24,"Drastic DS Update",COL_SEL);

    std::string status;
    switch(snapshot.state){
      case LauncherUpdateState::Idle: status="Ready to check for updates"; break;
      case LauncherUpdateState::Checking: status="Checking GitHub for the latest release..."; break;
      case LauncherUpdateState::UpdateAvailable:
        status="Version "+snapshot.release.tag+" is available    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::UpToDate:
        status="You are up to date    Installed: "+installedReleaseTag(); break;
      case LauncherUpdateState::Downloading:
        status=cancelRequested?"Cancelling download...":"Downloading "+snapshot.release.assetName; break;
      case LauncherUpdateState::ReadyToInstall: status="Preparing installation..."; break;
      case LauncherUpdateState::Installing: status="Installing update..."; break;
      case LauncherUpdateState::Installed: status="Update installed successfully - relaunch Drastic DS manually"; break;
      case LauncherUpdateState::Cancelled: status="Update cancelled"; break;
      case LauncherUpdateState::Error: status=snapshot.error.empty()?"Update failed":snapshot.error; break;
    }
    drawScrollTextL(g_font_sm,bodyX,panelY+92,bodyWidth,status.c_str(),
      snapshot.state==LauncherUpdateState::Error?(SDL_Color){235,125,115,255}:COL_VAL);

    SDL_Rect clip={bodyX,bodyY,bodyWidth,bodyBottom-bodyY};
    SDL_RenderSetClipRect(g_ren,&clip);
    for(int row=0;row<visibleLines&&scroll+row<(int)wrappedLines.size();row++)
      drawText(g_font_sm,bodyX,bodyY+row*lineHeight,wrappedLines[scroll+row].c_str(),COL_TXT);
    SDL_RenderSetClipRect(g_ren,nullptr);
    if((int)wrappedLines.size()>visibleLines){
      const int trackX=panelX+panelWidth-25,trackHeight=bodyBottom-bodyY;
      fillRect(trackX,bodyY,4,trackHeight,(SDL_Color){40,44,54,255});
      const int thumbHeight=std::max(18,trackHeight*visibleLines/(int)wrappedLines.size());
      fillRect(trackX,bodyY+(trackHeight-thumbHeight)*scroll/std::max(1,maxScroll),4,thumbHeight,COL_SEL);
    }

    if(snapshot.state==LauncherUpdateState::Downloading){
      const uint64_t total=snapshot.total?snapshot.total:snapshot.release.assetSize;
      const int percent=total?(int)std::min<uint64_t>(100,snapshot.downloaded*100/total):0;
      const int barX=bodyX,barY=panelY+panelHeight-82,barWidth=bodyWidth,barHeight=24;
      border(barX,barY,barWidth,barHeight,2,COL_SEL);
      fillRect(barX+3,barY+3,(barWidth-6)*percent/100,barHeight-6,COL_HI);
      char progress[96];
      snprintf(progress,sizeof(progress),"%d%%    %.1f / %.1f MiB",percent,
        snapshot.downloaded/(1024.0*1024.0),total/(1024.0*1024.0));
      drawTextC(g_font_sm,SW/2,barY+30,progress,COL_DIM);
    } else {
      const char *controls="B  Back       Up / Down  Scroll       L / R  Page";
      if(snapshot.state==LauncherUpdateState::UpdateAvailable) controls="A  Download       B  Back       Up / Down  Scroll";
      else if(snapshot.state==LauncherUpdateState::Error||snapshot.state==LauncherUpdateState::Cancelled) controls="A  Retry       B  Back";
      else if(snapshot.state==LauncherUpdateState::Installed) controls="A  Exit Drastic DS";
      drawFooterText(controls,panelY+panelHeight-38);
    }
    drawFadeIn();
    presentUi();
    waitForNextUiFrame();
  }
}

static void pollUpdateNotification() {
  const LauncherUpdateSnapshot snapshot=LauncherUpdate_GetSnapshot();
  if(snapshot.state==LauncherUpdateState::UpdateAvailable&&!snapshot.release.tag.empty()&&
     snapshot.release.tag!=g_updateNotifiedTag){
    g_updateNotifiedTag=snapshot.release.tag;
    g_updateNoticeTag=snapshot.release.tag;
    g_updateNoticeUntil=SDL_GetTicks()+9000;
  }
}

static void drawUpdateNotification() {
  if(g_updateNoticeTag.empty()||SDL_TICKS_PASSED(SDL_GetTicks(),g_updateNoticeUntil)){
    g_updateNoticeTag.clear();
    return;
  }
  const int width=std::min(540,SW-40),height=92,x=SW-width-24,y=SH-height-58;
  glassPanel(x,y,width,height);
  border(x,y,width,height,2,COL_SEL);
  const std::string title="Drastic DS "+g_updateNoticeTag+" is available";
  drawText(g_font,x+22,y+16,ellipsizedText(g_font,title,width-44).c_str(),COL_VAL);
  drawText(g_font_sm,x+22,y+54,"Open Settings > Launcher > Check for Updates",COL_TXT);
}

static void gameSourcesScreen() {
  int sel=0,top=0;
  for(;;){
    const int rowHeight=g_launcherPortrait?settingsRowH():50;
    const int startY=g_launcherPortrait?settingsListY():112;
    auto sources=loadGameSources(); int n=1+(int)sources.size();
    int vis=std::max(1,(SH-startY-settingsFooterReserve())/rowHeight);
    sel=std::max(0,std::min(sel,n-1)); if(sel<top) top=sel; if(sel>=top+vis) top=sel-vis+1;
    bool rebuild=false;
    while(!rebuild){
      if(!beginUiFrame()) return;
      SDL_Event event; navRepeat();
      while(pollUiEvent(event)){
        pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
        if(touchScrollList(touch,sel,top,n,vis)) continue;
        if(touch==TOUCH_TAP){
          if(ty>=SH-48) return;
          for(int row=0;row<vis&&top+row<n;row++){ int y=startY+row*rowHeight; if(ty>=y&&ty<y+rowHeight-4){ sel=top+row; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
          continue;
        }
        if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
        if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
        else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
        else if(event.cbutton.button==BTN_CANCEL) return;
        else if(event.cbutton.button==BTN_CONFIRM){
          if(sel==0){
            if(sources.size()>=16){ toast("Maximum of 16 game folders",900); continue; }
            std::string selected=browseFolder({});
            if(!selected.empty()){
              std::string identity=pathIdentity(selected);
              if(std::any_of(sources.begin(),sources.end(),[&](const std::string &path){ return pathIdentity(path)==identity; })){
                toast("Folder already added",800);
              } else {
                ensureSavedPathMountedAtStartup(selected); sources.push_back(selected); saveGameSources(sources); g_rescanAfterSettings=true; sel=(int)sources.size();
              }
              rebuild=true;
            }
          } else {
            const char *actions[]={"Change folder","Move up","Move down","Remove"};
            int action=dropdown("Game folder",actions,4,0); size_t index=(size_t)(sel-1);
            if(action==0){
              std::string selected=browseFolder(sources[index]);
              if(!selected.empty()){
                std::string identity=pathIdentity(selected); bool duplicate=false;
                for(size_t i=0;i<sources.size();i++) if(i!=index&&pathIdentity(sources[i])==identity) duplicate=true;
                if(duplicate) toast("Folder already added",800);
                else { ensureSavedPathMountedAtStartup(selected); sources[index]=selected; saveGameSources(sources); g_rescanAfterSettings=true; }
                rebuild=true;
              }
            } else if(action==1&&index>0){ std::swap(sources[index],sources[index-1]); saveGameSources(sources); sel--; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==2&&index+1<sources.size()){ std::swap(sources[index],sources[index+1]); saveGameSources(sources); sel++; g_rescanAfterSettings=true; rebuild=true; }
            else if(action==3&&confirmBox("Remove game folder?",{sources[index],"","No files will be deleted."})){
              sources.erase(sources.begin()+index); saveGameSources(sources); sel=std::max(0,sel-1); g_rescanAfterSettings=true; rebuild=true;
            }
          }
        }
        if(sel<top) top=sel;
        if(sel>=top+vis) top=sel-vis+1;
      }
      if(rebuild) break;
      clearUiBackground();
      drawHeader("Game folders","All folders are scanned by Drastic DS");
      for(int row=0;row<vis&&top+row<n;row++){
        int index=top+row,slotY=startY+row*rowHeight;
        int y=slotY+(rowHeight-TTF_FontHeight(g_font))/2; bool current=index==sel;
        if(current){
          const int rowInset=g_launcherPortrait?portraitRowInset():-3;
          const int highlightHeight=g_launcherPortrait?rowHeight-rowInset*2:rowHeight-4;
          fillRect(56,slotY+rowInset,SW-112,highlightHeight,COL_FOCUS);
          fillRect(56,slotY+rowInset,5,highlightHeight,COL_SEL);
        }
        std::string label=index==0?"[ Add game folder ]":sources[index-1];
        drawText(g_font,82,y,ellipsizedText(g_font,label,SW-170).c_str(),current?COL_VAL:(index==0?COL_HI:COL_TXT));
      }
      drawFooterText("A  Select       B  Back");
      presentUi(); waitForNextUiFrame();
    }
  }
}

static void libraryStorageScreen() {
  static int savedSelection=0;
  constexpr int rowCount=4;
  const int rowHeight=g_launcherPortrait?settingsRowH():64;
  const int startY=g_launcherPortrait?settingsListY():126;
  int sel=std::max(0,std::min(savedSelection,rowCount-1));
  auto openRow=[&](){
    if(sel==0) gameSourcesScreen();
    else if(sel==1) runFileManager();
    else if(sel==2) networkSharesScreen();
    else downloadAllCovers();
    beginScreenFx();
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()){ savedSelection=sel; return; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40){ savedSelection=sel; return; }
        for(int row=0;row<rowCount;row++){ int y=startY+row*rowHeight; if(ty>=y&&ty<y+rowHeight){ sel=row; openRow(); break; } }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+rowCount-1)%rowCount;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%rowCount;
      else if(event.cbutton.button==BTN_CONFIRM) openRow();
      else if(event.cbutton.button==BTN_CANCEL){ savedSelection=sel; return; }
    }

    clearUiBackground();
    drawHeader("Library & storage",nullptr);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    glassPanel(colX-12,startY-10,colW+24,rowCount*rowHeight+18);
    const int rowInset=g_launcherPortrait?portraitRowInset():2;
    float target=(float)(startY+sel*rowHeight+rowInset);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowHeight-rowInset*2,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowHeight-rowInset*2,COL_SEL);
    auto shares=loadSmbSharesFromStore(); size_t mounted=0;
    for(const auto &share:shares) if(SwitchStorage::IsSmbMounted(share.id)) mounted++;
    size_t folderCount=loadGameSources().size();
    std::string folderValue=std::to_string(folderCount)+(folderCount==1?" folder":" folders");
    std::string smbValue=std::to_string(mounted)+" / "+std::to_string(shares.size())+" connected";
    size_t missing=0;for(const Game &game:g_games)if(!regularFileExists(existingCoverPath(game)))missing++;
    const std::string coverValue=missing?std::to_string(missing)+" missing":"Complete";
    const char *labels[rowCount]={"Game folders","File manager","SMB network shares","Download covers"};
    const char *values[rowCount]={folderValue.c_str(),"SD / USB / SMB",smbValue.c_str(),coverValue.c_str()};
    for(int row=0;row<rowCount;row++){
      int slot=startY+row*rowHeight; bool current=row==sel;
      drawSettingsRowText(labels[row],values[row],slot,colW,labelX,valX,
                          current,current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,
                          false,rowHeight);
    }
    drawFooterText("A  Open       B  Back");
    drawFadeIn(); presentUi(); waitForNextUiFrame();
  }
}

static void runSettingsRoot(SDL_GameController *pad, const char *ctx) {
  bool global=!(ctx&&*ctx);
  static const int globalOrder[] = { SCR_EMU, SCR_GRAPHICS, SCR_AUDIO,
                                     SCR_NETWORK, SCR_CONTROLLER };
  static const int gameOrder[] = { SCR_FRAMEGEN, SCR_EMU, SCR_GRAPHICS,
                                   SCR_AUDIO, SCR_NETWORK, SCR_CONTROLLER };
  const int *order=global?globalOrder:gameOrder;
  const int nscr=global?(int)(sizeof(globalOrder)/sizeof(*globalOrder)):
                        (int)(sizeof(gameOrder)/sizeof(*gameOrder));
  int launcherRow=0,libraryRow=1,framegenRow=2,screenStart=3;
  int n=nscr+(global?screenStart:0),sel=0,top=0;
  const int rowH=g_launcherPortrait?settingsRowH():58;
  const int y0=g_launcherPortrait?settingsListY():92;
  const int sectionGap=g_launcherPortrait?(highResolutionUi()?44:36):34;
  const int vis=std::max(1,(SH-y0-settingsFooterReserve()-sectionGap)/rowH);
  auto rowY=[&](int index){ return y0+(index-top)*rowH+(global&&index>=screenStart?sectionGap:0); };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty);
      if(touchScrollList(touch,sel,top,n,vis)) continue;
      if(touch==TOUCH_TAP){
        if(ty<topBarH()||ty>=SH-40) return;
        for(int row=0;row<vis&&top+row<n;row++){ int index=top+row,y=rowY(index); if(ty>=y&&ty<y+rowH){ sel=index; SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN; press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break; } }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+n-1)%n;
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%n;
      else if(event.cbutton.button==BTN_CONFIRM){
        if(global&&sel==launcherRow) launcherSettingsScreen();
        else if(global&&sel==libraryRow) libraryStorageScreen();
        else if(global&&sel==framegenRow) runSettings(SCR_FRAMEGEN,pad,ctx);
        else runSettings(order[global?sel-screenStart:sel],pad,ctx);
        beginScreenFx();
      } else if(event.cbutton.button==BTN_SETTINGS){
        if(global&&sel==launcherRow)
          showHelpCard("Settings","Launcher","Launcher appearance",
                       "Changes the SDL launcher's orientation, theme, library grid, labels, animations, sounds, and cover downloads.",
                       nullptr,"Settings category");
        else if(global&&sel==libraryRow)
          showHelpCard("Settings","Library & storage","Game and file management",
                       "Manages game folders, local or removable storage, files, and SMB network shares used by the launcher.",
                       nullptr,"Settings category");
        else {
          int screen=(global&&sel==framegenRow)?SCR_FRAMEGEN:
                     order[global?sel-screenStart:sel];
          showHelpCard(global?"Settings":"Game settings",g_screens[screen].title,
                       "Settings category",settingsScreenDescription(screen),nullptr,
                       global?"Global settings":"Per-game overrides");
        }
        beginScreenFx();
      } else if(event.cbutton.button==BTN_CANCEL) return;
      if(sel<top) top=sel;
      if(sel>=top+vis) top=sel-vis+1;
    }

    clearUiBackground();
    drawHeader(global?"Settings":"Game settings",global?nullptr:ctx);
    int colX,colW,labelX,valX; listCol(&colX,&colW,&labelX,&valX);
    int shown=std::min(vis,n);
    if(global){
      glassPanel(colX-12,y0-10,colW+24,screenStart*rowH+18);
      glassPanel(colX-12,y0+screenStart*rowH+sectionGap-10,colW+24,(shown-screenStart)*rowH+18);
    } else glassPanel(colX-12,y0-10,colW+24,shown*rowH+18);
    const int rowInset=g_launcherPortrait?portraitRowInset():2;
    float target=(float)(rowY(sel)+rowInset);
    g_hy=(!g_uiAnimations||g_hy<0)?target:g_hy+(target-g_hy)*0.30f;
    fillRect(colX,(int)g_hy,colW,rowH-rowInset*2,COL_FOCUS);
    fillRect(colX,(int)g_hy,5,rowH-rowInset*2,COL_SEL);
    for(int row=0;row<vis&&top+row<n;row++){
      int index=top+row,slot=rowY(index); bool current=index==sel;
      if(global&&index==launcherRow){
        const char *theme=storeGet(g_global,"Wrapper/Theme","animated");
        const char *value=!strcmp(theme,"xmb")?"XMB (PS3)":(!strcmp(theme,"animated")?"Glow":(!strcmp(theme,"classic")?"Classic":(!strcmp(theme,"oled")?"OLED black":"Bubbles")));
        drawSettingsRowText("Launcher",value,slot,colW,labelX,valX,current,
                            current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowH);
      } else if(global&&index==libraryRow){
        drawSettingsRowText("Library & storage","games / files / network",slot,colW,labelX,valX,current,
                            current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowH);
      } else if(global&&index==framegenRow){
        drawSettingsRowText("Frame Generation","LSFG 2x / Vulkan",slot,colW,labelX,valX,current,
                            current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowH);
      } else {
        drawSettingsRowText(g_screens[order[global?index-screenStart:index]].title,">",slot,colW,labelX,valX,current,
                            current?COL_VAL:COL_TXT,current?COL_VAL:COL_DIM,false,rowH);
      }
    }
    if(n>vis){ int trackH=vis*rowH,trackX=colX+colW+16; fillRect(trackX,y0,4,trackH,(SDL_Color){40,44,54,255}); int thumbH=std::max(16,trackH*vis/n),denom=std::max(1,n-vis); fillRect(trackX,y0+(trackH-thumbH)*top/denom,4,thumbH,COL_SEL); }
    drawFooterText("A  Open       X  Help       B  Back");
    drawFadeIn(); presentUi(); waitForNextUiFrame();
  }
}

static void toast(const char *msg, Uint32 durationMs) {
  g_toastMessage=msg?msg:"";
  g_toastUntil=g_toastMessage.empty()?0:SDL_GetTicks()+durationMs;
  wakeUiFromWorker(0x544f4153);
}

static void drawPendingToast() {
  if(g_toastMessage.empty()) return;
  if(SDL_TICKS_PASSED(SDL_GetTicks(),g_toastUntil)){
    g_toastMessage.clear();g_toastUntil=0;return;
  }
  const int pw=std::min(820,SW-64),ph=120,px=(SW-pw)/2,py=(SH-ph)/2;
  glassPanel(px,py,pw,ph);border(px,py,pw,ph,2,COL_HI);
  drawTextC(g_font,SW/2,py+46,fittedText(g_font,g_toastMessage,pw-48).c_str(),COL_TXT);
}

static std::vector<std::string> wrapDialogLines(const std::vector<std::string> &lines,
                                                 int maxWidth){
  std::vector<std::string> wrapped;
  for(const std::string &source:lines){
    if(source.empty()){ wrapped.emplace_back(); continue; }
    std::string line;
    size_t cursor=0;
    while(cursor<source.size()){
      while(cursor<source.size()&&source[cursor]==' ') cursor++;
      size_t end=source.find(' ',cursor);
      std::string word=source.substr(cursor,end==std::string::npos?
                                    std::string::npos:end-cursor);
      std::string candidate=line.empty()?word:line+" "+word;
      if(!line.empty()&&textW(g_font,candidate.c_str())>maxWidth){
        wrapped.push_back(std::move(line));
        line=std::move(word);
      } else line=std::move(candidate);
      if(end==std::string::npos) break;
      cursor=end+1;
    }
    if(!line.empty()) wrapped.push_back(
        textW(g_font,line.c_str())<=maxWidth?line:
        fittedText(g_font,line,maxWidth));
  }
  return wrapped;
}

static void modalMessage(const char *title, const std::vector<std::string> &lines) {
  const int pw=SW*3/4;
  const std::vector<std::string> displayLines=wrapDialogLines(lines,pw-64);
  const int lineHeight=40;
  const int ph=std::min(SH-64,180+(int)displayLines.size()*lineHeight);
  const int px=(SW-pw)/2,py=(SH-ph)/2;
  for (;;) {
    if(!beginUiFrame()) return;
    SDL_Event e;
    navRepeat();
    while (pollUiEvent(e)) {
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP) return; }
      if (e.type == SDL_CONTROLLERBUTTONDOWN &&
          (e.cbutton.button == BTN_CONFIRM || e.cbutton.button == BTN_CANCEL)) return;
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,COL_SEL);
    drawTextC(g_font_big, SW/2, py+34,
              fittedText(g_font_big,title,pw-48).c_str(),COL_SEL);
    int y = py+108;
    for (const std::string &line : displayLines) {
      if(y+TTF_FontHeight(g_font)>=py+ph-54) break;
      drawTextC(g_font,SW/2,y,line.c_str(),COL_TXT);
      y+=lineHeight;
    }
    drawFooterText("A  Continue",py+ph-30);
      presentUi(); waitForNextUiFrame();
  }
}

static bool confirmBox(const char *title, const std::vector<std::string> &lines) {
  int pw=SW*3/4;
  const std::vector<std::string> displayLines=wrapDialogLines(lines,pw-64);
  int ph=std::min(SH-64,220+(int)displayLines.size()*40),px=(SW-pw)/2,py=(SH-ph)/2;
  int bw=std::min(210,(pw-54)/2),bh=56,bby=py+ph-bh-22;
  int yesx=SW/2-bw-12,nox=SW/2+12;
  for(;;){
    if(!beginUiFrame()) return false;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; if(touchFeed(e,&tx,&ty)==TOUCH_TAP && ty>=bby && ty<bby+bh){
          if(tx>=yesx && tx<yesx+bw) return true;
          if(tx>=nox  && tx<nox+bw)  return false;
      } }
      if(e.type==SDL_CONTROLLERBUTTONDOWN){
        if(e.cbutton.button==BTN_CONFIRM) return true;
        if(e.cbutton.button==BTN_CANCEL) return false;
      }
    }
    clearUiBackground();
    glassPanel(px,py,pw,ph);
    border(px,py,pw,ph,3,(SDL_Color){210,70,70,255});
    drawTextC(g_font_big,SW/2,py+34,
              fittedText(g_font_big,title,pw-48).c_str(),
              (SDL_Color){235,120,120,255});
    int y=py+112;
    for(const std::string &line:displayLines){
      if(y+40>=bby-8) break;
      drawTextC(g_font,SW/2,y,line.c_str(),COL_TXT);
      y+=40;
    }
    fillRect(yesx,bby,bw,bh,(SDL_Color){150,50,50,255}); border(yesx,bby,bw,bh,2,(SDL_Color){215,95,95,255});
    int yesHintWidth=footerHintWidth("A","Yes");
    drawButtonHint(yesx+(bw-yesHintWidth)/2,bby+bh/2,"A","Yes");
    fillRect(nox,bby,bw,bh,(SDL_Color){48,54,64,255}); border(nox,bby,bw,bh,2,COL_DIM);
    int noHintWidth=footerHintWidth("B","No");
    drawButtonHint(nox+(bw-noHintWidth)/2,bby+bh/2,"B","No");
      presentUi(); waitForNextUiFrame();
  }
}

static bool bundledResourcesPresent() {
  const std::string system=SYSTEM_DIR;
  return regularFileExists(system + "/game_database.xml") &&
         regularFileExists(system + "/usrcheat.dat");
}

static bool supersededBundledCheatDatabase(const std::string &path) {
  struct stat status{};
  if(stat(path.c_str(),&status)!=0||!S_ISREG(status.st_mode)) return false;
  unsigned char header[96]={};
  FILE *file=fopen(path.c_str(),"rb");
  if(!file) return false;
  const bool valid=fread(header,1,sizeof(header),file)==sizeof(header);
  fclose(file);
  if(!valid||memcmp(header,"R4 CheatCode",12)) return false;
  static const char legacyName[]="CMP NDS Cheat Database 220713";
  if(status.st_size==13739796&&
     !memcmp(header+16,legacyName,sizeof(legacyName)-1)) return true;
  static const char incompatibleName[]=
      "DeadSkullzJr's NDS(i) Cheat Database (20250812)";
  static const unsigned char emptyEncoding[4]={0,0,0,0};
  return status.st_size==55503268&&
         !memcmp(header+16,incompatibleName,sizeof(incompatibleName)-1)&&
         !memcmp(header+0x4c,emptyEncoding,sizeof(emptyEncoding));
}

static bool userSystemFilesPresent() {
  const std::string system=SYSTEM_DIR;
  const bool arm7=regularFileExists(system + "/nds_bios_arm7.bin") ||
                  regularFileExists(system + "/drastic_bios_arm7.bin");
  const bool arm9=regularFileExists(system + "/nds_bios_arm9.bin") ||
                  regularFileExists(system + "/drastic_bios_arm9.bin");
  const bool firmware=regularFileExists(system + "/nds_firmware_modified.bin") ||
                      regularFileExists(system + "/nds_firmware.bin");
  return arm7&&arm9&&firmware;
}

static void drawWrapped(TTF_Font *font,int x,int y,int maxWidth,int lineHeight,int maxLines,const char *text,SDL_Color color) {
  if(!text||!*text) return;
  std::string input=text,line; int drawn=0;
  auto emit=[&](const std::string &value){ if(drawn<maxLines){ drawText(font,x,y+drawn*lineHeight,value.c_str(),color); drawn++; } };
  size_t index=0;
  while(index<input.size()&&drawn<maxLines){
    size_t end=index; while(end<input.size()&&input[end]!=' '&&input[end]!='\n') end++;
    std::string word=input.substr(index,end-index);
    std::string candidate=line.empty()?word:line+" "+word;
    if(textW(font,candidate.c_str())>maxWidth&&!line.empty()){ emit(line); line=word; }
    else line=std::move(candidate);
    if(end<input.size()&&input[end]=='\n'){ emit(line); line.clear(); }
    index=end+1;
  }
  if(!line.empty()&&drawn<maxLines) emit(line);
}

static SDL_Texture *loadScaledTexture(const std::string &path,int width,int height) {
  if(width<1||height<1) return nullptr;
  SDL_Surface *source=IMG_Load(path.c_str());
  if(!source) return nullptr;
  SDL_Surface *scaled=SDL_CreateRGBSurfaceWithFormat(0,width,height,32,SDL_PIXELFORMAT_RGBA32);
  if(!scaled){ SDL_FreeSurface(source); return nullptr; }
  SDL_BlendMode blend=SDL_BLENDMODE_NONE;
  SDL_GetSurfaceBlendMode(source,&blend);
  SDL_SetSurfaceBlendMode(source,SDL_BLENDMODE_NONE);
  bool ok=SDL_BlitScaled(source,nullptr,scaled,nullptr)==0;
  SDL_SetSurfaceBlendMode(source,blend);
  SDL_FreeSurface(source);
  if(!ok){ SDL_FreeSurface(scaled); return nullptr; }
  SDL_Texture *texture=SDL_CreateTextureFromSurface(g_ren,scaled);
  SDL_FreeSurface(scaled);
  if(texture) SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
  return texture;
}

static const char *gridDbErrorText(int result) {
  if(result==GRIDDB_NO_KEY) return "The SteamGridDB API key was rejected.";
  if(result==GRIDDB_NO_NET) return "Could not connect to SteamGridDB.";
  if(result==GRIDDB_NOT_FOUND) return "No matching artwork was found.";
  return "SteamGridDB returned an unexpected error.";
}

static int chooseCoverArtwork(const std::vector<GridDbArtwork> &artworks,const char *gameName) {
  if(artworks.empty()) return -1;
  const int rowHeight=52;
  const int listX=g_launcherPortrait?48:56;
  const int listWidth=g_launcherPortrait?SW-96:SW/2-78;
  const int previewX=g_launcherPortrait?48:SW/2+28;
  const int previewAreaWidth=g_launcherPortrait?SW-96:SW-previewX-56;
  const int portraitPreviewLimit=highResolutionUi()?720:510;
  const int previewHeight=g_launcherPortrait?
      std::min(portraitPreviewLimit,(previewAreaWidth*3)/2):
      std::min(SH-210,highResolutionUi()?720:510);
  const int previewWidth=previewHeight*2/3;
  const int previewY=g_launcherPortrait?topBarH()+20:116;
  const int startY=g_launcherPortrait?previewY+previewHeight+30:116;
  const int visible=std::max(1,(SH-startY-settingsFooterReserve())/rowHeight);
  const std::string temporary=std::string(COVERS_DIR)+"/.sgdb-preview.img";
  int sel=0,top=0,loaded=-1;
  SDL_Texture *preview=nullptr;
  bool previewFailed=false;
  auto releasePreview=[&](){ if(preview) SDL_DestroyTexture(preview); preview=nullptr; remove(temporary.c_str()); };
  auto loadPreview=[&](int index){
    releasePreview(); loaded=index; previewFailed=false;
    clearUiBackground(); drawHeader("Choose cover artwork",gameName);
    drawTextC(g_font,previewX+previewAreaWidth/2,
              previewY+previewHeight/2-18,"Loading preview...",COL_DIM);
    presentUi();
    const std::string &url=artworks[index].thumbnailUrl.empty()?artworks[index].url:artworks[index].thumbnailUrl;
    if(griddb_download_image(url,temporary)==GRIDDB_OK) preview=loadScaledTexture(temporary,previewWidth,previewHeight);
    previewFailed=preview==nullptr; remove(temporary.c_str()); beginScreenFx();
  };
  mkdir(COVERS_DIR,0777);
  loadPreview(0);
  for(;;){
    if(!beginUiFrame()){ releasePreview(); return -1; }
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event); int tx=0,ty=0; TouchKind touch=touchFeed(event,&tx,&ty); int oldSelection=sel;
      if(touchScrollList(touch,sel,top,(int)artworks.size(),visible)){ if(sel!=oldSelection) loadPreview(sel); continue; }
      if(touch==TOUCH_TAP){
        if(ty>=SH-48){ releasePreview(); return -1; }
        if(tx>=listX&&tx<listX+listWidth) for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
          int itemY=startY+row*rowHeight;
          if(ty>=itemY&&ty<itemY+rowHeight){ sel=top+row; if(loaded!=sel) loadPreview(sel); break; }
        }
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      int previous=sel;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP) sel=(sel+(int)artworks.size()-1)%(int)artworks.size();
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN) sel=(sel+1)%(int)artworks.size();
      else if(event.cbutton.button==BTN_CONFIRM){ releasePreview(); return sel; }
      else if(event.cbutton.button==BTN_CANCEL){ releasePreview(); return -1; }
      if(sel<top) top=sel;
      if(sel>=top+visible) top=sel-visible+1;
      if(sel!=previous) loadPreview(sel);
    }
    clearUiBackground(); drawHeader("Choose cover artwork",gameName);
    glassPanel(listX-10,startY-10,listWidth+20,std::min(visible,(int)artworks.size())*rowHeight+18);
    for(int row=0;row<visible&&top+row<(int)artworks.size();row++){
      int index=top+row,itemY=startY+row*rowHeight,textY=itemY+(rowHeight-TTF_FontHeight(g_font))/2; bool current=index==sel;
      if(current){ fillRect(listX,itemY,listWidth,rowHeight-3,COL_FOCUS); fillRect(listX,itemY,5,rowHeight-3,COL_SEL); }
      std::string label="Artwork "+std::to_string(index+1);
      drawText(g_font,listX+26,textY,label.c_str(),current?COL_VAL:COL_TXT);
      if(artworks[index].width>0&&artworks[index].height>0){
        std::string dimensions=std::to_string(artworks[index].width)+"x"+std::to_string(artworks[index].height);
        drawTextR(g_font_sm,listX+listWidth-20,textY+(TTF_FontHeight(g_font)-TTF_FontHeight(g_font_sm))/2,dimensions.c_str(),current?COL_VAL:COL_DIM);
      }
    }
    int imageX=previewX+(previewAreaWidth-previewWidth)/2,imageY=previewY;
    fillRect(imageX,imageY,previewWidth,previewHeight,COL_CARD);
    if(loaded==sel&&preview){ SDL_Rect destination={imageX,imageY,previewWidth,previewHeight}; SDL_RenderCopy(g_ren,preview,nullptr,&destination); }
    else if(loaded==sel&&previewFailed) drawTextC(g_font_sm,imageX+previewWidth/2,imageY+previewHeight/2,"Preview unavailable",COL_DIM);
    border(imageX,imageY,previewWidth,previewHeight,2,loaded==sel?COL_SEL:COL_DIM);
    drawFooterText("A  Use artwork       B  Back");
    drawFadeIn(); presentUi(); waitForNextUiFrame();
  }
}

static void downloadCover(Game &g) {
  std::string key=storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(key.empty()){
    char buffer[128];
    if(promptText("Enter your free SteamGridDB API key","",buffer,sizeof(buffer))){ key=buffer; storeSet(g_global,"Wrapper/SteamGridDBKey",buffer); storeSave(g_global,LAUNCHER_INI); }
    else { toast("A SteamGridDB API key is required",1200); return; }
  }
  mkdir(COVERS_DIR,0777);
  std::string query=g.title;
  GridDbGameResult selectedGame;
  for(;;){
    toast("Searching SteamGridDB...");
    std::vector<GridDbGameResult> matches;
    int result=griddb_search_games(key,query,matches);
    if(result!=GRIDDB_OK&&result!=GRIDDB_NOT_FOUND){ modalMessage("Cover search failed",{gridDbErrorText(result)}); return; }
    std::vector<std::string> labels={"Custom search..."};
    for(const auto &match:matches) labels.push_back(match.name);
    std::vector<const char*> names; names.reserve(labels.size());
    for(const auto &label:labels) names.push_back(label.c_str());
    int gameIndex=dropdown("Choose matching title",names.data(),(int)names.size(),-1);
    if(gameIndex<0) return;
    if(gameIndex==0){
      char custom[256];
      if(!promptText("Custom SteamGridDB search",query.c_str(),custom,sizeof(custom))) continue;
      std::string nextQuery=trim(custom); if(!nextQuery.empty()) query=std::move(nextQuery);
      continue;
    }
    selectedGame=matches[gameIndex-1]; break;
  }
  toast("Loading available artwork...");
  std::vector<GridDbArtwork> artworks;
  int result=griddb_fetch_artworks(key,selectedGame.id,artworks);
  if(result!=GRIDDB_OK){ modalMessage("Artwork search failed",{gridDbErrorText(result)}); return; }
  int artworkIndex=chooseCoverArtwork(artworks,selectedGame.name.c_str());
  if(artworkIndex<0) return;
  toast("Downloading selected cover...");
  result=griddb_download_image(artworks[artworkIndex].url,coverPath(g));
  if(result==GRIDDB_OK){ reloadCover(g); toast("Cover downloaded"); }
  else toast("Cover download failed");
}

static bool runCoverImportTask(const char *title,const std::string &detail,
                               const std::function<void(const std::atomic_bool&)> &task){
  std::atomic_bool cancel{false},complete{false};
  std::thread worker([&]{task(cancel);complete.store(true,std::memory_order_release);wakeUiFromWorker(0x434f5649);});
  beginScreenFx();while(!complete.load(std::memory_order_acquire)){
    if(!beginUiFrame()){cancel.store(true);break;}SDL_Event event;while(pollUiEvent(event)){pumpStick(event);int x=0,y=0;
      if((event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)||(touchFeed(event,&x,&y)==TOUCH_TAP&&y>=SH-80))cancel.store(true);}
    clearUiBackground();drawHeader(LauncherLocalization::Translate(title).data(),nullptr);drawTextC(g_font,SW/2,SH/2-10,detail.c_str(),COL_TXT);
    const std::string back=LauncherLocalization::Translate("Cancel").data();FootItem footer[]={{"B",back.c_str(),FA_NONE}};drawFooterHints(footer,1,SH-26);presentUi();waitForNextUiFrame();}
  if(worker.joinable())worker.join();
  return !cancel.load();
}

static void importCoverFromFile(Game &g){
  const std::string selected=browseCoverImage(parentFolder(g.path));if(selected.empty())return;
  mkdir(COVERS_DIR,0777);const std::string destination=coverPath(g),temporary=destination+".tmp";bool imported=false;std::string reason,detail;
  if(!runCoverImportTask("Importing local cover",fileNameOf(selected),[&](const std::atomic_bool &cancel){
    const auto fail=[&](const char *message,const char *technical=nullptr){reason=message;if(technical)detail=technical;remove(temporary.c_str());};struct stat info{};
    if(cancel.load())return;
    if(stat(selected.c_str(),&info)!=0||!S_ISREG(info.st_mode)){fail("The selected cover file is unavailable.",strerror(errno));return;}
    if(info.st_size<1||(uint64_t)info.st_size>32ull*1024*1024){fail("The selected cover file is too large.");return;}
    if(!recoverAtomicFile(destination)){fail("DraStic could not prepare the cover file safely.",strerror(errno));return;}
    using Surface=std::unique_ptr<SDL_Surface,decltype(&SDL_FreeSurface)>;Surface source{IMG_Load(selected.c_str()),SDL_FreeSurface};
    if(!source){fail("The selected file is not a supported image.",IMG_GetError());return;}
    if(source->w<=0||source->h<=0||source->w>8192||source->h>8192||(uint64_t)source->w*(uint64_t)source->h>16ull*1024*1024){fail("The selected image dimensions are too large.");return;}
    if(cancel.load())return;
    Surface converted{SDL_ConvertSurfaceFormat(source.get(),SDL_PIXELFORMAT_RGBA32,0),SDL_FreeSurface};source.reset();
    if(!converted||IMG_SavePNG(converted.get(),temporary.c_str())!=0){fail("DraStic could not convert the selected image to PNG.",IMG_GetError());return;}
    converted.reset();if(cancel.load()){remove(temporary.c_str());return;}Surface verify{IMG_Load(temporary.c_str()),SDL_FreeSurface};
    if(!verify||verify->w<=0||verify->h<=0){fail("DraStic could not verify the converted cover.",IMG_GetError());return;}verify.reset();
    FILE *saved=fopen(temporary.c_str(),"rb+");if(!saved){fail("DraStic could not save the converted cover.",strerror(errno));return;}
    const bool synced=fsync(fileno(saved))==0,closed=fclose(saved)==0;if(!synced||!closed){fail("DraStic could not save the converted cover.",strerror(errno));return;}
    if(cancel.load()){remove(temporary.c_str());return;}if(!replaceAtomic(destination,temporary)){fail("DraStic could not replace the current cover safely.",strerror(errno));return;}imported=true;
  }))return;
  if(imported){reloadCover(g);toast(LauncherLocalization::Translate("Cover imported").data());return;}
  std::vector<std::string> lines{std::string(LauncherLocalization::Translate(reason.empty()?"The selected cover could not be imported safely.":reason))};if(!detail.empty())lines.push_back(detail);
  modalMessage(LauncherLocalization::Translate("Cover import failed").data(),lines);
}

static void coverSettings(Game &g){
  int selection=0;const bool portrait=g_launcherPortrait;const int margin=portrait?36:70,gap=portrait?24:30,cardsTop=topBarH()+40,cardsBottom=SH-settingsFooterReserve();SDL_Rect cards[2];
  if(portrait){const int height=(cardsBottom-cardsTop-gap)/2;cards[0]={margin,cardsTop,SW-margin*2,height};cards[1]={margin,cardsTop+height+gap,SW-margin*2,height};}
  else{const int width=(SW-margin*2-gap)/2;cards[0]={margin,cardsTop,width,cardsBottom-cardsTop};cards[1]={margin+width+gap,cardsTop,width,cardsBottom-cardsTop};}
  const char *titles[2]={"Download from SteamGridDB","Import cover from file"};const char *kinds[2]={"Online artwork","Local image"};
  const char *descriptions[2]={"Search SteamGridDB and replace this game's custom cover with selected online artwork.","Choose a PNG, JPEG, WebP or BMP image from SD, USB or SMB storage. It is validated and saved safely as PNG."};
  const auto inside=[](const SDL_Rect&r,int x,int y){return x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;};
  const auto removeCustom=[&]{const std::string path=existingCoverPath(g);if(!regularFileExists(path)||!confirmBox(LauncherLocalization::Translate("Remove custom cover?").data(),{std::string(LauncherLocalization::Translate("The downloaded or imported cover will be deleted.")),std::string(LauncherLocalization::Translate("The launcher will use the game's embedded artwork when available."))}))return;
    if(remove(path.c_str())!=0&&errno!=ENOENT)modalMessage(LauncherLocalization::Translate("Cover removal failed").data(),{strerror(errno)});else{if(path!=coverPath(g))remove(coverPath(g).c_str());fsdevCommitDevice("sdmc");reloadCover(g);toast(LauncherLocalization::Translate("Custom cover removed").data());}};
  beginScreenFx();for(;;){if(!beginUiFrame())return;const bool hasCustom=regularFileExists(existingCoverPath(g));SDL_Event event;navRepeat();
    while(pollUiEvent(event)){pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);bool choose=false;
      if(touch==TOUCH_TAP){if(inside(cards[0],tx,ty)){selection=0;choose=true;}else if(inside(cards[1],tx,ty)){selection=1;choose=true;}else if(ty>=SH-40)return;}
      if(event.type==SDL_CONTROLLERBUTTONDOWN){if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_LEFT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)selection=0;else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_RIGHT||event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)selection=1;
        else if(event.cbutton.button==BTN_CONFIRM)choose=true;else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_X&&hasCustom){removeCustom();beginScreenFx();}else if(event.cbutton.button==BTN_CANCEL)return;}
      if(choose){if(selection==0)downloadCover(g);else importCoverFromFile(g);beginScreenFx();}}
    clearUiBackground();drawHeader(LauncherLocalization::Translate("Cover settings").data(),g.title.c_str());for(int index=0;index<2;index++){const SDL_Rect&card=cards[index];const bool current=index==selection;
      fillRect(card.x+5,card.y+7,card.w,card.h,(SDL_Color){0,0,0,62});fillRect(card.x,card.y,card.w,card.h,current?COL_FOCUS:COL_CARD);border(card.x,card.y,card.w,card.h,current?4:2,current?COL_SEL:COL_DIM);if(current)fillRect(card.x,card.y,8,card.h,COL_SEL);
      const std::string title=LauncherLocalization::Translate(titles[index]).data();drawTextC(g_font_big,card.x+card.w/2,card.y+34,fittedText(g_font_big,title,card.w-60).c_str(),current?COL_VAL:COL_TXT);
      drawTextC(g_font,card.x+card.w/2,card.y+(portrait?92:126),LauncherLocalization::Translate(kinds[index]).data(),current?COL_HI:COL_DIM);drawWrapped(g_font_sm,card.x+38,card.y+(portrait?148:194),card.w-76,TTF_FontHeight(g_font_sm)+7,portrait?4:5,LauncherLocalization::Translate(descriptions[index]).data(),current?COL_TXT:COL_DIM);}
    const std::string choose=LauncherLocalization::Translate("Choose").data(),removeLabel=LauncherLocalization::Translate("Remove custom cover").data(),back=LauncherLocalization::Translate("Back").data();
    if(hasCustom){FootItem footer[]={{"A",choose.c_str(),FA_NONE},{"Y",removeLabel.c_str(),FA_NONE},{"B",back.c_str(),FA_NONE}};drawFooterHints(footer,3,SH-26);}else{FootItem footer[]={{"A",choose.c_str(),FA_NONE},{"B",back.c_str(),FA_NONE}};drawFooterHints(footer,2,SH-26);}drawFadeIn();presentUi();waitForNextUiFrame();}
}

static void downloadAllCovers() {
  std::string key=storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(key.empty()){
    char buffer[128];
    if(promptText("Enter your free SteamGridDB API key","",buffer,sizeof(buffer))){ key=buffer; storeSet(g_global,"Wrapper/SteamGridDBKey",buffer); storeSave(g_global,LAUNCHER_INI); }
    else { toast("A SteamGridDB API key is required",1200); return; }
  }
  mkdir(COVERS_DIR,0777);
  struct CoverJob{std::string stableId,title,path;};
  std::vector<CoverJob> pending;
  for(const Game &game:g_games)if(!regularFileExists(existingCoverPath(game)))pending.push_back({game.key,game.title,coverPath(game)});
  if(pending.empty()){toast("All covers already downloaded",1200);return;}
  const int total=(int)pending.size();std::atomic<int> done{0},ok{0},fail{0};std::atomic<bool> cancel{false},complete{false};
  std::mutex progressMutex;std::string currentTitle;std::vector<std::string> downloaded;
  std::thread worker([&]{
    for(const CoverJob &job:pending){
      if(cancel.load())break;
      {std::lock_guard<std::mutex> lock(progressMutex);currentTitle=job.title;}
      wakeUiFromWorker(0x434f5645);
      const int result=griddb_fetch_cover(key,job.title,job.path,&cancel);
      if(result==GRIDDB_OK){ok++;std::lock_guard<std::mutex> lock(progressMutex);downloaded.push_back(job.stableId);}else fail++;
      done++;wakeUiFromWorker(0x434f5645);
    }
    complete=true;wakeUiFromWorker(0x434f5645);
  });
  while(!complete.load()){
    if(!beginUiFrame()){cancel=true;break;}
    SDL_Event event;while(pollUiEvent(event)){pumpStick(event);if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)cancel=true;int tx=0,ty=0;if(touchFeed(event,&tx,&ty)==TOUCH_TAP&&ty>=SH-90)cancel=true;}
    std::string title;{std::lock_guard<std::mutex> lock(progressMutex);title=currentTitle;}
    clearUiBackground();drawHeader("Download covers",nullptr);
    drawTextC(g_font,SW/2,SH/2-96,("Downloading  "+std::to_string(std::min(total,done.load()+1))+" / "+std::to_string(total)).c_str(),COL_VAL);
    drawTitleCell(SW/2,SW-260,SH/2-44,title,true,COL_TXT);
    int width=SW-360,x=180,y=SH/2+16,height=26;fillRect(x,y,width,height,(SDL_Color){40,44,54,255});border(x,y,width,height,2,COL_DIM);
    fillRect(x,y,total?width*done.load()/total:0,height,COL_SEL);
    char status[64];snprintf(status,sizeof(status),"%d downloaded    %d failed",ok.load(),fail.load());drawTextC(g_font_sm,SW/2,y+46,status,COL_DIM);
    FootItem footer[]={{"B","Cancel",FA_NONE}};drawFooterHints(footer,1,SH-24);presentUi();waitForNextUiFrame();
  }
  cancel=true;if(worker.joinable())worker.join();
  // Only invalidate successful entries here. The cover worker decodes them
  // again when their page becomes visible; GPU uploads stay frame-budgeted.
  {std::lock_guard<std::mutex> lock(progressMutex);for(const std::string &stableId:downloaded)if(Game *game=findGameByKey(stableId)){if(game->cover){SDL_DestroyTexture(game->cover);game->cover=nullptr;}game->coverIsRomIcon=false;game->coverUse=0;game->coverRequest=0;game->coverQueued=false;game->triedCover=false;}}
  char message[96];snprintf(message,sizeof(message),"Covers: %d downloaded, %d failed%s",ok.load(),fail.load(),cancel.load()&&done.load()<total?" (cancelled)":"");
  toast(message,1600);
}

static bool pickIcon(Game &g, char *outPath, size_t outSize) {
  std::string base = std::string(DATA_DIR) + "/forwarders", tmp = base + "/iconpick";
  mkdir(base.c_str(),0777); mkdir(tmp.c_str(),0777);
  if(DIR*d=opendir(tmp.c_str())){ struct dirent*e; while((e=readdir(d))) if(e->d_name[0]!='.') remove((tmp+"/"+std::string(e->d_name)).c_str()); closedir(d); }
  std::vector<std::string> paths; struct stat st;
  { std::string cp=existingCoverPath(g); if(stat(cp.c_str(),&st)==0) paths.push_back(cp); }
  std::string key = storeGet(g_global,"Wrapper/SteamGridDBKey","");
  if(!key.empty()){
    clearUiBackground();
    drawHeader("Choose an icon", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Fetching icons from SteamGridDB...", COL_TXT);
    presentUi();
    int nf=griddb_fetch_icons(key,g.title,tmp,14);
    for(int i=0;i<nf;i++){ char p[300]; snprintf(p,sizeof(p),"%s/gicon_%d.png",tmp.c_str(),i); paths.push_back(p); }
  }
  if(paths.empty()){ toast("No icon found - add a SteamGridDB key or download a cover first",1800); return false; }
  int n=(int)paths.size();
  int cols=n<5?n:5; if(cols<1)cols=1;
  int rows=(n+cols-1)/cols, gap=18, top=150, bot=40;
  int cw=(SW-80-(cols-1)*gap)/cols, ch=(SH-top-bot-(rows-1)*gap)/rows;
  int cell=cw<ch?cw:ch; if(cell>200)cell=200; if(cell<90)cell=90;
  int x0=(SW-(cols*cell+(cols-1)*gap))/2, y0=top;
  std::vector<SDL_Texture*> tex(n,nullptr);
  for(int i=0;i<n;i++) tex[i]=loadScaledTexture(paths[i],cell,cell);
  int sel=0, chosen=-1; bool done=false; beginScreenFx();
  while(!done){
    if(!beginUiFrame()){ done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){ pumpStick(e);
      { int tx=0,ty=0; TouchKind touch=touchFeed(e,&tx,&ty);
        if(touch==TOUCH_SCROLL_UP){ sel=std::min(n-1,sel+cols); continue; }
        if(touch==TOUCH_SCROLL_DOWN){ sel=std::max(0,sel-cols); continue; }
        if(touch==TOUCH_TAP){
          for(int i=0;i<n;i++){ int row=i/cols,column=i%cols,x=x0+column*(cell+gap),y=y0+row*(cell+gap);
            if(tx>=x&&tx<x+cell&&ty>=y&&ty<y+cell){ sel=i; chosen=i; done=true; break; } }
          if(done) continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=(sel+1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel+cols)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel-cols+n)%n; break;
        case BTN_CONFIRM: chosen=sel; done=true; break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeader("Choose an icon", g.title.c_str());
    for(int i=0;i<n;i++){ int r=i/cols,c=i%cols, x=x0+c*(cell+gap), y=y0+r*(cell+gap);
      if(i==sel) fillRect(x-6,y-6,cell+12,cell+12,COL_SEL);
      fillRect(x,y,cell,cell,COL_CARD);
      if(tex[i]){ SDL_Rect d{x,y,cell,cell}; SDL_RenderCopy(g_ren,tex[i],nullptr,&d); }
      else drawTextC(g_font_sm,x+cell/2,y+cell/2,"?",COL_DIM);
    }
    drawFadeIn(); presentUi(); waitForNextUiFrame();
  }
  for(auto t:tex) if(t) SDL_DestroyTexture(t);
  if(chosen>=0 && chosen<n){ snprintf(outPath,outSize,"%s",paths[chosen].c_str()); return true; }
  return false;
}

static void forwarderWizard(Game &g) {
  char name[256]; snprintf(name,sizeof(name),"%s",g.title.c_str());
  char icon[300]={0};
  { struct stat st; std::string cp=existingCoverPath(g);
    if(stat(cp.c_str(),&st)==0) snprintf(icon,sizeof(icon),"%s",cp.c_str()); }
  SDL_Texture *iconTex = icon[0] ? loadScaledTexture(icon,280,280) : nullptr;

  const int isz=g_launcherPortrait?std::min(280,SW-160):280;
  const int ix=g_launcherPortrait?(SW-isz)/2:110;
  const int iy=g_launcherPortrait?topBarH()+30:176;
  const int rx=g_launcherPortrait?56:ix+isz+70;
  const int rw=g_launcherPortrait?SW-112:SW-rx-90;
  const int nameY=g_launcherPortrait?iy+isz+62:196;
  const int createY=g_launcherPortrait?nameY+116:350;
  const int fieldH=64,createH=58;
  int sel=0; bool done=false; beginScreenFx();

  auto edit=[&](const char *header,char *buffer,size_t size){
    char value[256];
    if(promptText(header,buffer,value,sizeof(value))&&value[0]&&size){ size_t length=std::min(strlen(value),size-1); memcpy(buffer,value,length); buffer[length]=0; }
  };
  auto build=[&](){
    if(!icon[0]){ toast("Pick an icon first",1200); return; }
    clearUiBackground();
    drawHeader("Creating HOME shortcut", g.title.c_str());
    drawTextC(g_font, SW/2, SH/2, "Building + installing forwarder...", COL_TXT);
    presentUi();
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    const std::string &shortcutKey=g.key;
    std::vector<std::string> legacyKeys;
    const auto appendLegacy=[&](const std::string &key){if(!key.empty()&&key!=shortcutKey&&std::find(legacyKeys.begin(),legacyKeys.end(),key)==legacyKeys.end())legacyKeys.push_back(key);};
    appendLegacy(g.pathKey);if(g.legacyUnique)appendLegacy(g.legacyKey);
    // A short-lived 1.0.8 test build used the content fingerprint as identity.
    appendLegacy(stableGameKey(g.gameCode,g.fingerprint));
    char err[256]={0}; bool ok=forwarder_create(shortcutKey,name,icon,legacyKeys,err,sizeof(err));
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    if(ok){ toast("HOME shortcut installed",1800); done=true; }
    else modalMessage("Shortcut failed", { err[0]?err:"Unknown error" });
    beginScreenFx();
  };
  auto activate=[&](){
    if(sel==0){ char p[300]; if(pickIcon(g,p,sizeof(p))){ snprintf(icon,sizeof(icon),"%s",p); if(iconTex)SDL_DestroyTexture(iconTex); iconTex=loadScaledTexture(icon,isz,isz); } beginScreenFx(); }
    else if(sel==1) edit("Shortcut name", name, sizeof(name));
    else build();
  };

  while(!done){
    if(!beginUiFrame()){ done=true; break; }
    SDL_Event e; navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(tx>=ix&&tx<ix+isz&&ty>=iy&&ty<iy+isz){ sel=0; activate(); }
          else if(ty>=nameY-6&&ty<nameY+fieldH){ sel=1; activate(); }
          else if(ty>=createY-6&&ty<createY+createH){ sel=2; activate(); }
          else if(ty>=SH-40) done=true;
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  sel=0; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if(sel==0) sel=1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:    sel=(sel==0)?2:(sel==1?2:sel-1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  sel=(sel==0)?1:(sel==2?1:sel+1); break;
        case BTN_CONFIRM: activate(); break;
        case BTN_CANCEL:  done=true; break;
      }
    }
    clearUiBackground();
    drawHeader("Create HOME shortcut", g.title.c_str());
    if(sel==0) fillRect(ix-6,iy-6,isz+12,isz+12,COL_SEL);
    fillRect(ix,iy,isz,isz,COL_CARD);
    if(iconTex){ SDL_Rect d{ix,iy,isz,isz}; SDL_RenderCopy(g_ren,iconTex,nullptr,&d); }
    else drawTextC(g_font_sm,ix+isz/2,iy+isz/2,"(no icon)",COL_DIM);
    drawTextC(g_font_sm, ix+isz/2, iy+isz+20, "Icon", sel==0?COL_VAL:COL_DIM);
    auto field=[&](int idx,int y,const char*label,const char*val){ bool cur=sel==idx;
      if(cur){ fillRect(rx-10,y-6,rw+20,fieldH,COL_FOCUS); fillRect(rx-10,y-6,5,fieldH,COL_SEL); }
      drawText(g_font_sm, rx, y, label, cur?COL_VAL:COL_DIM);
      drawScrollTextL(g_font,rx,y+26,rw-8,val,cur?COL_VAL:COL_TXT); };
    field(1,nameY,"Name",name);
    { bool cur=sel==2;
      fillRect(rx-10,createY-6,rw+20,createH, cur?(SDL_Color){44,86,44,240}:(SDL_Color){30,46,32,200});
      if(cur) fillRect(rx-10,createY-6,5,createH,COL_SEL);
      drawTextC(g_font, rx+rw/2, createY+12, "Create shortcut", cur?COL_VAL:(SDL_Color){150,225,150,255}); }
    drawFadeIn(); presentUi(); waitForNextUiFrame();
  }
  if(iconTex) SDL_DestroyTexture(iconTex);
}

static int chooseLibraryAction(const char *title,const std::vector<std::string> &items){
  if(items.empty())return -1;
  int selection=0,top=0;beginScreenFx();
  for(;;){
    if(!beginUiFrame())return -1;
    const int rowH=settingsRowH(),listY=settingsListY();
    const int visible=std::max(1,std::min((int)items.size(),(SH-listY-settingsFooterReserve())/rowH));
    SDL_Event event{};navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);int tx=0,ty=0;TouchKind touch=touchFeed(event,&tx,&ty);
      if(touchScrollList(touch,selection,top,(int)items.size(),visible))continue;
      if(touch==TOUCH_TAP){if(ty>=SH-44)return -1;for(int row=0;row<visible&&top+row<(int)items.size();row++)if(ty>=listY+row*rowH&&ty<listY+(row+1)*rowH)return top+row;}
      if(event.type!=SDL_CONTROLLERBUTTONDOWN)continue;
      if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_UP)selection=(selection+(int)items.size()-1)%items.size();
      else if(event.cbutton.button==SDL_CONTROLLER_BUTTON_DPAD_DOWN)selection=(selection+1)%items.size();
      else if(event.cbutton.button==BTN_CONFIRM)return selection;
      else if(event.cbutton.button==BTN_CANCEL)return -1;
      if(selection<top)top=selection;
      if(selection>=top+visible)top=selection-visible+1;
    }
    clearUiBackground();drawHeader(title,nullptr);
    int colX,colW,labelX,valX;listCol(&colX,&colW,&labelX,&valX);
    glassPanel(colX-12,listY-10,colW+24,visible*rowH+18);
    for(int row=0;row<visible&&top+row<(int)items.size();row++){
      const int index=top+row,y=listY+row*rowH;if(index==selection){fillRect(colX,y,colW,rowH-2,COL_FOCUS);fillRect(colX,y,5,rowH-2,COL_SEL);}
      drawText(g_font,labelX,y+(rowH-TTF_FontHeight(g_font))/2,items[index].c_str(),index==selection?COL_VAL:COL_TXT);
    }
    FootItem footer[]={{"A","Choose",FA_NONE},{"B","Back",FA_NONE}};drawFooterHints(footer,2,SH-24);
    drawFadeIn();presentUi();waitForNextUiFrame();
  }
}

static void manageCollections(){
  for(;;){
    std::vector<std::string> choices{"New collection..."};for(const Collection &collection:g_collections)choices.push_back(collection.name);
    const int selected=chooseLibraryAction("Manage collections",choices);if(selected<0)return;
    if(selected==0){char name[96]{};if(promptText("Collection name","",name,sizeof(name))&&!trim(name).empty()){
      const std::string entered=trim(name);const bool duplicate=std::any_of(g_collections.begin(),g_collections.end(),[&](const Collection &c){return !strcasecmp(c.name.c_str(),entered.c_str());});
      if(duplicate)modalMessage("Collection already exists",{entered});else{g_collections.push_back({entered,{}});saveLibraryOrganization();}}
      beginScreenFx();continue;}
    const int index=selected-1;const int action=chooseLibraryAction(g_collections[index].name.c_str(),{"Rename","Delete collection"});
    if(action==0){char renamed[96]{};if(promptText("Rename collection",g_collections[index].name.c_str(),renamed,sizeof(renamed))&&!trim(renamed).empty()){
      if(g_activeCollection==g_collections[index].name)g_activeCollection=trim(renamed);
      g_collections[index].name=trim(renamed);saveLibraryOrganization();rebuildLibraryView();}}
    else if(action==1&&confirmBox("Delete collection?",{g_collections[index].name,"No games will be deleted."})){
      if(g_activeCollection==g_collections[index].name)g_activeCollection.clear();
      g_collections.erase(g_collections.begin()+index);saveLibraryOrganization();rebuildLibraryView();}
    beginScreenFx();
  }
}

static void organizeGame(Game &game){
  for(;;){
    std::vector<std::string> choices{"Favorite"+(g_favorites.count(game.key)?std::string("  ✓"):std::string{})};
    for(const Collection &collection:g_collections)choices.push_back(collection.name+(collection.games.count(game.key)?"  ✓":""));
    choices.push_back("New collection...");const int selected=chooseLibraryAction("Favorites & collections",choices);if(selected<0)return;
    if(selected==0){if(!g_favorites.erase(game.key))g_favorites.insert(game.key);}
    else if(selected==(int)choices.size()-1){char name[96]{};if(promptText("Collection name","",name,sizeof(name))&&!trim(name).empty())g_collections.push_back({trim(name),{game.key}});}
    else{Collection &collection=g_collections[selected-1];if(!collection.games.erase(game.key))collection.games.insert(game.key);}
    saveLibraryOrganization();rebuildLibraryView();beginScreenFx();
  }
}

static void chooseLibraryFilter(){
  std::vector<std::string> choices{"All games","Favorites"};for(const Collection &collection:g_collections)choices.push_back(collection.name);
  choices.push_back("Search...");choices.push_back("Manage collections");const int selected=chooseLibraryAction("Filter library",choices);if(selected<0)return;
  if(selected==0){g_activeCollection.clear();g_searchQuery.clear();}
  else if(selected==1){g_activeCollection="favorites";g_searchQuery.clear();}
  else if(selected<2+(int)g_collections.size()){g_activeCollection=g_collections[selected-2].name;g_searchQuery.clear();}
  else if(selected==2+(int)g_collections.size()){char query[128]{};if(promptText("Search games",g_searchQuery.c_str(),query,sizeof(query))){g_searchQuery=trim(query);g_activeCollection.clear();}}
  else manageCollections();
  rebuildLibraryView();beginScreenFx();
}

static int perGameMenu(Game &g, SDL_GameController *pad) {
  const char *items[] = { "Launch", "Game settings", "Rename game", "Cover settings", "Create HOME shortcut", "Clear game settings", "Favorite / collections", "Delete game" };
  int n=8, sel=0;
  std::string gp = std::string(GAMECFG_DIR) + "/" + g.key + ".ini";
  std::string pathGp = std::string(GAMECFG_DIR) + "/" + g.pathKey + ".ini";
  std::string legacyGp = std::string(GAMECFG_DIR) + "/" + g.legacyKey + ".ini";
  if(regularFileExists(gp)) storeLoad(g_game,gp.c_str());
  else if(!g.pathKey.empty()&&regularFileExists(pathGp)) storeLoad(g_game,pathGp.c_str());
  else if(g.legacyUnique&&!g.legacyKey.empty()&&regularFileExists(legacyGp)) storeLoad(g_game,legacyGp.c_str());
  else g_game.kv.clear();
  normalizeCpuThreads(g_game);
  migrateStylusMode(g_game,false);
  storeRemove(g_game,"Wrapper/CpuBoost");
  const int coverWidth=g_launcherPortrait?(highResolutionUi()?300:240):300;
  const int coverHeight=coverWidth*3/2;
  const int coverX=g_launcherPortrait?(SW-coverWidth)/2:90;
  const int coverY=g_launcherPortrait?topBarH()+30:(SH-coverHeight)/2;
  const int menuX=g_launcherPortrait?56:coverX+coverWidth+64;
  const int menuWidth=g_launcherPortrait?SW-112:SW-menuX-70;
  const int menuRowH=g_launcherPortrait?(highResolutionUi()?72:62):56;
  const int menuStartY=g_launcherPortrait?coverY+coverHeight+40:210;
  // Use one row rectangle for hit testing, the animated selection overlay and
  // text placement.  The old landscape overlay started six pixels before the
  // row while the text was centered in the unshifted 56-pixel slot, leaving
  // the selected label visibly below the overlay's centre.
  const int menuRowInset=g_launcherPortrait?portraitRowInset():4;
  const int menuContentH=menuRowH-menuRowInset*2;
  const auto menuRowTop=[&](int index){
    return menuStartY+index*menuRowH+menuRowInset;
  };
  beginScreenFx();
  for(;;){
    if(!beginUiFrame()) return 0;
    SDL_Event e;
    navRepeat();
    while(pollUiEvent(e)){
      pumpStick(e);
      { int tx=0,ty=0; TouchKind tk=touchFeed(e,&tx,&ty);
        if(tk==TOUCH_TAP){
          if(ty>=SH-40){ return 0; }
          for(int i=0;i<n;i++){
            const int hitTop=menuRowTop(i);
            if(ty>=hitTop && ty<hitTop+menuContentH){ sel=i;
            SDL_Event a; memset(&a,0,sizeof(a)); a.type=SDL_CONTROLLERBUTTONDOWN; a.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&a); break; } }
          continue;
        }
      }
      if(e.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(e.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=(sel+n-1)%n; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=(sel+1)%n; break;
        case BTN_CANCEL: return 0;
        case BTN_CONFIRM:
          if(sel==0) return 1;
          else if(sel==1){
            g_active=&g_game;
            runSettingsRoot(pad,g.title.c_str());
            g_active=&g_global;
            mkdir(GAMECFG_DIR,0777);
            bool saved=true;
            if(g_game.kv.empty()){
              if(remove(gp.c_str())!=0&&errno!=ENOENT) saved=false;
            } else saved=storeSave(g_game,gp.c_str());
            if(saved&&g.legacyUnique&&legacyGp!=gp) remove(legacyGp.c_str());
            if(saved&&pathGp!=gp) remove(pathGp.c_str());
            g.hasCfg=saved&&!g_game.kv.empty();
            if(!saved) modalMessage("Game settings",{"Could not save the per-game settings."});
            beginScreenFx();
          }
          else if(sel==2){
            char buf[128];
            if(promptText("Rename game", g.title.c_str(), buf, sizeof(buf))){
              g.title = buf;
              storeSet(g_titles, g.key.c_str(), buf);
              if(g.legacyUnique&&!g.legacyKey.empty()) storeRemove(g_titles,g.legacyKey.c_str());
              storeSave(g_titles, TITLES_INI);
            }
          }
          else if(sel==3){ coverSettings(g); beginScreenFx(); }
          else if(sel==4){ forwarderWizard(g); beginScreenFx(); }
          else if(sel==5){
            g_game.kv.clear(); remove(gp.c_str());
            if(g.legacyUnique&&!g.legacyKey.empty()) remove(legacyGp.c_str());
            g.hasCfg=false; toast("Game settings cleared",700); beginScreenFx();
          }
          else if(sel==6){ organizeGame(g); beginScreenFx(); }
          else if(sel==7){
            if(confirmBox("Delete game?", { g.title, "", "This permanently deletes the game file from",
                                            "its storage device. This cannot be undone." })){
              if(remove(g.path.c_str())!=0){
                modalMessage("Delete failed",{strerror(errno)});
                beginScreenFx();
                break;
              }
              remove(coverPath(g).c_str());
              remove(gp.c_str());
              if(!g.pathKey.empty()){
                remove((std::string(COVERS_DIR)+"/"+g.pathKey+".png").c_str());
                remove(pathGp.c_str());storeRemove(g_titles,g.pathKey.c_str());storeRemove(g_recent,g.pathKey.c_str());
              }
              if(g.legacyUnique&&!g.legacyKey.empty()){
                remove((std::string(COVERS_DIR)+"/"+g.legacyKey+".png").c_str());
                remove(legacyGp.c_str());
                storeRemove(g_titles,g.legacyKey.c_str());
                storeRemove(g_recent,g.legacyKey.c_str());
              }
              storeRemove(g_titles,g.key.c_str()); storeSave(g_titles,TITLES_INI);
              storeRemove(g_recent,g.key.c_str()); storeSave(g_recent,RECENT_INI);
              toast("Game deleted",800);
              return 2;
            }
          }
          break;
      }
    }
    clearUiBackground();
    if(g_launcherPortrait) drawHeader("Game menu",g.title.c_str());
    g_cover_budget = 1;
    ensureCover(g,true);
    int cw=coverWidth,chh=coverHeight,cx=coverX,cy=coverY;
    fillRect(cx+5,cy+7,cw,chh,(SDL_Color){0,0,0,60}); fillRect(cx+2,cy+3,cw,chh,(SDL_Color){0,0,0,75});
    if(g.cover){ drawGameArtwork(g,cx,cy,cw,chh,255,255); border(cx,cy,cw,chh,2,COL_DIM); }
    else { fillRect(cx,cy,cw,chh,(SDL_Color){40,44,54,255}); border(cx,cy,cw,chh,2,COL_DIM); drawTextC(g_font,cx+cw/2,cy+chh/2,"NO COVER",COL_DIM); }
    if(!g_launcherPortrait)
      drawText(g_font_big,cx+cw+70,120,
               fittedText(g_font_big,g.title,SW-(cx+cw+140)).c_str(),COL_TXT);
    int mx=menuX,mw=menuWidth;
    float ty=(float)menuRowTop(sel);
    g_hy=(!g_uiAnimations||g_hy<0)?ty:g_hy+(ty-g_hy)*0.30f;
    fillRect(mx,(int)g_hy,mw,menuContentH,COL_FOCUS);
    fillRect(mx,(int)g_hy,5,menuContentH,COL_SEL);
    for(int i=0;i<n;i++){
      const int y=menuRowTop(i)+(menuContentH-TTF_FontHeight(g_font))/2;
      bool cur=i==sel;
      SDL_Color rc = (i==n-1) ? (SDL_Color){228,120,120,255} : COL_TXT;
      drawText(g_font,mx+30,y,
               fittedText(g_font,items[i],mw-52).c_str(),cur?COL_VAL:rc);
    }
    drawFadeIn();
    presentUi();
    waitForNextUiFrame();
  }
}

static bool extractFromRomfs(const char *src, const char *dst, bool force=false) {
  struct stat ss{},ds{};
  if(stat(src,&ss)!=0||!S_ISREG(ss.st_mode)||!recoverAtomicFile(dst)) return false;
  if (!force && stat(dst,&ds)==0 && ds.st_size==ss.st_size) return true;
  std::string tmp = std::string(dst) + ".tmp";
  FILE *in=fopen(src,"rb"), *out=fopen(tmp.c_str(),"wb");
  if(!in||!out){ if(in)fclose(in); if(out)fclose(out); return false; }
  static char buf[1<<16]; size_t n; bool ok=true;
  while((n=fread(buf,1,sizeof(buf),in))>0){ if(fwrite(buf,1,n,out)!=n){ ok=false; break; } }
  if(ferror(in)) ok=false;
  if(fflush(out)!=0||fsync(fileno(out))!=0) ok=false;
  if(fclose(in)!=0) ok=false;
  if(fclose(out)!=0) ok=false;
  if(!ok){ remove(tmp.c_str()); return false; }
  struct stat temporary{};
  if(stat(tmp.c_str(),&temporary)!=0||temporary.st_size!=ss.st_size||!replaceAtomic(dst,tmp)){
    remove(tmp.c_str());
    return false;
  }
  return stat(dst,&ds)==0 && ds.st_size==ss.st_size;
}

static bool extractTree(const std::string &src, const std::string &dst, bool force) {
  mkdir(dst.c_str(), 0777);
  DIR *d = opendir(src.c_str());
  if (!d) return false;
  bool ok = true;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    std::string s = src + "/" + e->d_name, t = dst + "/" + e->d_name;
    struct stat st;
    if (stat(s.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      ok = extractTree(s, t, force) && ok;
    else
      ok = extractFromRomfs(s.c_str(), t.c_str(), force) && ok;
  }
  closedir(d);
  return ok;
}

static const char *BUILD_STAMP = __DATE__ " " __TIME__;
static const char *RES_MARKER = "sdmc:/switch/drastic/system/.drastic_build";
static const char *BUNDLED_SHADER_MARKER =
    "sdmc:/switch/drastic/shaders/Bundled/.drasticds_nx_build";

static bool bundledShadersPresent() {
  static const char *manifests[] = {
    "lcd1x+natural_vision.dfx",
    "lcd1x+nds_color+natural_vision.dfx",
    "lcd1x+nds_color.dfx",
    "lcd1x.dfx",
    "natural_vision.dfx",
    "nds_color.dfx",
    "sharp_bilinear+natural_vision.dfx",
    "sharp_bilinear+nds_color+natural_vision.dfx",
    "sharp_bilinear+nds_color.dfx",
    "sharp_bilinear.dfx",
    "zfast_lcd+dsi_color.dfx",
    "zfast_lcd+dslite_color+natural_vision.dfx",
    "zfast_lcd+dslite_color.dfx",
    "zfast_lcd+natural_vision.dfx",
    "zfast_lcd+nds_color+natural_vision.dfx",
    "zfast_lcd+nds_color.dfx",
    "zfast_lcd.dfx",
    "xbr-sabr/4XBR_v1.1_Low configuration.dfx",
    "xbr-sabr/SABR_v3.0.dfx",
  };
  const std::string root=BUNDLED_SHADERS_DIR;
  if(!regularFileExists(root+"/README.md")||
     !regularFileExists(root+"/NOTICE.md")||
     !regularFileExists(root+"/COPYING")) return false;
  for(const char *manifest:manifests){
    const std::string dfx=root+"/"+manifest;
    std::string dsd=dfx.substr(0,dfx.size()-4)+".dsd";
    const std::string pack=dfx+".nxvk";
    if(!regularFileExists(dfx)||!regularFileExists(dsd)||
       !regularFileExists(pack+"/pack.info")||
       !regularFileExists(pack+"/pass0.vert.spv")||
       !regularFileExists(pack+"/pass0.frag.spv")) return false;
  }
  return true;
}

static bool ensureBundledShaders() {
  char current[64]={0};
  FILE *file=fopen(BUNDLED_SHADER_MARKER,"r");
  if(file){
    if(!fgets(current,sizeof(current),file)) current[0]=0;
    fclose(file);
  }
  const std::string marker=std::string("1 ")+BUILD_STAMP;
  if(trim(current)==marker&&bundledShadersPresent()) return true;
  toast("Installing bundled custom shaders (one-time)...");
  const bool ok=extractTree("romfs:/shaders",BUNDLED_SHADERS_DIR,true)&&
                bundledShadersPresent();
  if(ok) writeAtomicText(BUNDLED_SHADER_MARKER,marker+"\n");
  return ok;
}

static bool ensureResources() {
  char cur[64] = {0};
  FILE *f = fopen(RES_MARKER, "r");
  if (f) { if (!fgets(cur, sizeof(cur), f)) cur[0] = 0; fclose(f); }
  const bool present = bundledResourcesPresent();
  const std::string marker=std::string("109-pokemon-save-v1-cheats-compatible-merged-v3 ")+BUILD_STAMP;
  if(trim(cur)==marker&&present) return true;
  mkdir(SYSTEM_DIR, 0777);
  const std::string system=SYSTEM_DIR;
  bool ok=extractFromRomfs("romfs:/res/game_database.xml",
                           (system+"/game_database.xml").c_str(),true);
  const std::string cheatDatabase=system+"/usrcheat.dat";
  if(!regularFileExists(cheatDatabase)||
     supersededBundledCheatDatabase(cheatDatabase))
    ok=extractFromRomfs("romfs:/res/usrcheat.dat",
                        cheatDatabase.c_str(),true)&&ok;
  ok=bundledResourcesPresent()&&ok;
  if(ok) writeAtomicText(RES_MARKER,marker+"\n");
  return ok;
}

static bool ensureBundledFile(const char *src,const char *dst,const std::string &marker) {
  char cur[48] = {0};
  FILE *f = fopen(marker.c_str(), "r");
  if (f) { if (!fgets(cur, sizeof(cur), f)) cur[0] = 0; fclose(f); }
  struct stat st;
  if(trim(cur)==BUILD_STAMP&&stat(dst,&st)==0&&S_ISREG(st.st_mode)) return true;
  if(!extractFromRomfs(src,dst,true)) return false;
  writeAtomicText(marker,std::string(BUILD_STAMP)+"\n");
  return true;
}

static bool ensureCore(const char *src,const char *dst,const std::string &build) {
  return ensureBundledFile(src,dst,std::string(CORES_DIR)+"/.core_build_"+build);
}

static bool sameNroBuild(const char *first,const char *second) {
  struct stat firstStat{},secondStat{};
  if(stat(first,&firstStat)!=0||stat(second,&secondStat)!=0||
     !S_ISREG(firstStat.st_mode)||!S_ISREG(secondStat.st_mode)||
     firstStat.st_size!=secondStat.st_size) return false;
  auto readIdentity=[](const char *path,u8 identity[32]){
    u8 header[0x50];
    FILE *file=fopen(path,"rb");
    if(!file) return false;
    bool ok=fseek(file,0x10,SEEK_SET)==0&&fread(header,1,sizeof(header),file)==sizeof(header);
    if(fclose(file)!=0) ok=false;
    if(!ok||memcmp(header,"NRO0",4)!=0) return false;
    memcpy(identity,header+0x30,32);
    return std::any_of(identity,identity+32,[](u8 byte){ return byte!=0; });
  };
  u8 firstId[32],secondId[32];
  return readIdentity(first,firstId)&&readIdentity(second,secondId)&&
         memcmp(firstId,secondId,sizeof(firstId))==0;
}

static bool ensureEmu(const char *src,const char *dst) {
  if(sameNroBuild(src,dst)) return true;
  return extractFromRomfs(src,dst,true)&&sameNroBuild(src,dst);
}

static void cleanupLegacyEmuHosts() {
  static const char *directories[]={DATA_DIR,EMU_HOST_DIR};
  static const char *filenames[]={"DrasticDS_nx_vk.nro",
                                  "DrasticDS_nx_gl.nro",
                                  "DrasticDS_nx_zink.nro"};
  static const char *suffixes[]={"",".tmp",".old"};
  bool removed=false;
  for(const char *directory:directories) for(const char *filename:filenames)
    for(const char *suffix:suffixes){
      const std::string path=std::string(directory)+"/"+filename+suffix;
      if(remove(path.c_str())==0) removed=true;
    }
  if(removed) fsdevCommitDevice("sdmc");
}

struct GLay { int cols, rows, cw, chh, gapx, gapy, x0, y0, titleH; };
static GLay gridLayout(){
  GLay g;
  const bool big=highResolutionUi();
  g.gapx=big?24:18;
  g.gapy=big?18:14;
  if(g_launcherPortrait){ g.gapx=big?20:14; g.gapy=big?20:16; }
  g.titleH=g_showGameTitles?(big?30:24):0;
  int topBar=topBarH();
  int footer=g_launcherPortrait?(big?124:96):(big?54:38);
  g.cols=g_gridColumns;
  g.rows=g_gridRows;
  if(g_launcherPortrait){
    /* Preserve the configured number of games per page, but choose the
       factorisation that gives covers the largest usable portrait footprint. */
    const int capacity=g_gridColumns*g_gridRows;
    long long bestArea=-1;
    int bestColumns=1,bestRows=capacity;
    const int margin=big?60:32;
    const int caption=g.titleH?g.titleH+8:0;
    const int availableHeight=SH-topBar-footer;
    for(int columns=1;columns<=capacity;columns++){
      if(capacity%columns) continue;
      const int rows=capacity/columns;
      const int width=(SW-2*margin-(columns-1)*g.gapx)/columns;
      const int height=(availableHeight-(rows-1)*g.gapy-rows*caption)/rows;
      if(width<48||height<72) continue;
      const int coverHeight=std::min(height,width*3/2);
      const int coverWidth=coverHeight*2/3;
      const long long area=(long long)coverWidth*coverHeight;
      if(area>bestArea){ bestArea=area; bestColumns=columns; bestRows=rows; }
    }
    g.cols=bestColumns;
    g.rows=bestRows;
  }
  int availH = SH - topBar - footer;
  int caption=g.titleH?g.titleH+8:0;
  int maxCoverH=(availH-(g.rows-1)*g.gapy-g.rows*caption)/g.rows;
  if(maxCoverH<72) maxCoverH=72;
  int margin = big?60:(g_launcherPortrait?32:40);
  int autoWidth=maxCoverH*2/3;
  int maxCoverW=(SW-2*margin-(g.cols-1)*g.gapx)/g.cols;
  g.cw=std::max(48,std::min(autoWidth,maxCoverW));
  g.chh=std::min(maxCoverH,g.cw*3/2);
  g.cw=g.chh*2/3;
  int gridW = g.cols*g.cw + (g.cols-1)*g.gapx;
  g.x0 = (SW - gridW)/2;
  int gridH=g.rows*(g.chh+caption)+(g.rows-1)*g.gapy;
  g.y0=topBar+std::max(0,(availH-gridH)/2);
  return g;
}
static int gridHitTest(int px,int py,int top){
  GLay L=gridLayout(); int n=(int)g_games.size();
  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c; if(idx>=n) continue;
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    if(px>=x-4 && px<x+L.cw+4 && py>=y-4 && py<y+L.chh+(L.titleH?L.titleH+8:0)) return idx;
  }
  return -1;
}
static void drawTitleCell(int cx,int cellW,int y,const std::string&title,bool sel,SDL_Color col){
  TTF_Font*f=g_font_sm;
  int tw=textW(f,title.c_str());
  if(tw<=cellW){ drawTextC(f,cx,y,title.c_str(),col); return; }
  int x0=cx-cellW/2;
  if(!sel){
    const std::string &shortened=ellipsizedText(f,title,cellW);
    drawTextC(f,cx,y,shortened.c_str(),col);
    return;
  }
  SDL_Rect clip={x0,y-2,cellW,(f?TTF_FontHeight(f):26)+8};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-cellW;
  float t=(SDL_GetTicks()%5000)/5000.0f;
  float pp = t<0.5f ? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,title.c_str(),col);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawScrollTextR(TTF_Font*f,int xRight,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawTextR(f,xRight,y,s,c); return; }
  int x0=xRight-maxW;
  SDL_Rect clip={x0,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x0-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void drawScrollTextL(TTF_Font*f,int x,int y,int maxW,const char*s,SDL_Color c){
  if(maxW<=0 || !s || !*s) return;
  int tw=textW(f,s);
  if(tw<=maxW){ drawText(f,x,y,s,c); return; }
  SDL_Rect clip={x,y-2,maxW,(f?TTF_FontHeight(f):26)+6};
  SDL_RenderSetClipRect(g_ren,&clip);
  int span=tw-maxW;
  float t=(SDL_GetTicks()%6000)/6000.0f;
  float pp=t<0.5f? t*2.f : (1.f-t)*2.f;
  drawText(f,x-(int)(pp*span),y,s,c);
  SDL_RenderSetClipRect(g_ren,nullptr);
}

static void renderGrid(int sel,int top,const char*gamedirLabel){
  clearUiBackground();
  g_cover_budget = COVER_REQUEST_BUDGET;
  if(sel>=0 && sel<(int)g_libraryView.size()) ensureCover(*g_libraryView[sel],true);
  GLay L=gridLayout();
  int n=(int)g_libraryView.size(), per=L.cols*L.rows;
  int pages=n?(n+per-1)/per:1, pageIndex=n?sel/per:0, page=pageIndex+1;
  int bandH = g_launcherPortrait?topBarH()-4:L.y0-4;
  fillRect(0,0,SW,bandH,COL_PANEL);
  if(!hasAnimatedBackground()) fillRect(0,bandH,SW,2,COL_SEL);
  char pinfo[160]; snprintf(pinfo,sizeof(pinfo),"%d / %d    \xc2\xb7    Page %d / %d    \xc2\xb7    Sort: %s",n?sel+1:0,n,page,pages,SORT_NAME[g_sort]);
  if(g_launcherPortrait){
    int logoH=highResolutionUi()?62:48,logoW=logoH*16/9;
    if(g_logo){ SDL_Rect logoRect={18,10,logoW,logoH}; SDL_RenderCopy(g_ren,g_logo,nullptr,&logoRect); }
    const int infoWidth=std::max(80,SW-2*(logoW+34));
    const std::string shownInfo=fittedText(g_font_sm,pinfo,infoWidth);
    drawTextC(g_font_sm,SW/2,highResolutionUi()?22:15,shownInfo.c_str(),COL_VAL);
    const std::string shownFolder=fittedText(g_font_sm,gamedirLabel,SW-52);
    drawTextC(g_font_sm,SW/2,bandH-TTF_FontHeight(g_font_sm)-12,
              shownFolder.c_str(),COL_DIM);
  } else {
    int lh = bandH - 12;
    if(g_logo){ SDL_Rect ld={26,(bandH-lh)/2,lh*16/9,lh}; SDL_RenderCopy(g_ren,g_logo,nullptr,&ld); }
    drawTextC(g_font,SW/2,(bandH-TTF_FontHeight(g_font))/2,pinfo,COL_VAL);
    int pinfoRight=SW/2+textW(g_font,pinfo)/2;
    int folderMaxW=(SW-34)-(pinfoRight+24);
    drawScrollTextR(g_font_sm,SW-34,(bandH-TTF_FontHeight(g_font_sm))/2,folderMaxW,gamedirLabel,COL_DIM);
  }

  int rowStride=L.chh+(L.titleH?L.titleH+8:0)+L.gapy;
  for(int r=0;r<L.rows;r++) for(int c=0;c<L.cols;c++){
    int idx=(top+r)*L.cols+c;
    if(idx>=n) continue;
    Game&g=*g_libraryView[idx];
    int x=L.x0+c*(L.cw+L.gapx), y=L.y0+r*rowStride;
    bool cur=(idx==sel);
    ensureCover(g,true);
    fillRect(x+4,y+6,L.cw,L.chh,(SDL_Color){0,0,0,55});
    fillRect(x+2,y+3,L.cw,L.chh,(SDL_Color){0,0,0,70});
    if(g.cover){
      Uint32 el=SDL_GetTicks()-g.coverAt; Uint8 fa=!g_uiAnimations?255:(el<180?(Uint8)(255*el/180):255);
      drawGameArtwork(g,x,y,L.cw,L.chh,fa,cur?255:150);
    }
    else { fillRect(x,y,L.cw,L.chh,COL_CARD); drawTextC(g_font_sm,x+L.cw/2,y+L.chh/2-8,"NO COVER",COL_DIM); }
    border(x,y,L.cw,L.chh,1,(SDL_Color){12,13,18,255});
    fillRect(x,y,L.cw,1,(SDL_Color){255,255,255,26});
    if(cur){ const int G=6;
      for(int i=G;i>=1;i--){ Uint8 a=(Uint8)(150*(G-i+1)/G); border(x-2-i,y-2-i,L.cw+4+2*i,L.chh+4+2*i,1,(SDL_Color){255,170,0,a}); }
      border(x-2,y-2,L.cw+4,L.chh+4,2,COL_SEL);
    }
    if(g_showRegionFlags && g.region>0 && g_flag[g.region]){
      int fw=L.cw*26/100; if(fw>30)fw=30; if(fw<16)fw=16; int fh=fw*2/3;
      SDL_Rect fd={x+6,y+6,fw,fh}; SDL_RenderCopy(g_ren,g_flag[g.region],nullptr,&fd);
      border(x+6,y+6,fw,fh,1,(SDL_Color){10,12,18,255});
    }
    if(g_showCustomSettingsBadges && g.hasCfg){ int ds=L.cw/11<12?12:L.cw/11; fillRect(x+L.cw-ds-8,y+8,ds,ds,COL_SEL); border(x+L.cw-ds-8,y+8,ds,ds,2,(SDL_Color){10,12,18,255}); }
    if(g_showGameTitles) drawTitleCell(x+L.cw/2,L.cw,y+L.chh+6,g.title,cur,cur?COL_VAL:COL_DIM);
  }
  if(sel>=0&&sel<n)ensureCover(*g_libraryView[sel],true);
  // Decode the following page while the current one is being viewed.  Jobs
  // run on the cover worker and are lower priority than the selected/current
  // page, so changing pages no longer performs the first decode on the UI
  // thread and rapid navigation still favors what is actually visible.
  const int prefetchStart=(pageIndex+1)*per;
  for(int index=prefetchStart;index<std::min(n,prefetchStart+per);index++)
    ensureCover(*g_libraryView[index]);
  if(n==0) drawTextC(g_font,SW/2,SH/2,"No games found -- open Settings > Library & storage",COL_DIM);
  drawUpdateNotification();
  FootItem foot[] = {
    { "A", "Launch", FA_LAUNCH }, { "Y", "Sort", FA_SORT },
    { "X", "Settings", FA_SETTINGS }, { "+", "Game Menu", FA_OPTIONS },
    { "-", "Filter", FA_FILTER }, { "L", "", FA_PAGEL }, { "R", "Page", FA_PAGER }, { "B", "Quit", FA_QUIT },
  };
  drawFooterHints(foot, 8, SH-26);
  presentUi();
}

static int gridNav(int sel,int dx,int dy,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, page=sel/per, pos=sel%per, cr=pos/cols, cc=pos%cols;
  auto clamp=[&](int i){ return i>=n? n-1 : (i<0?0:i); };
  if(dx>0){
    if(cc<cols-1 && page*per+cr*cols+cc+1 < n) return page*per+cr*cols+cc+1;
    if((page+1)*per < n) return clamp((page+1)*per + cr*cols);
    return sel;
  }
  if(dx<0){
    if(cc>0) return sel-1;
    if(page>0) return clamp((page-1)*per + cr*cols + (cols-1));
    return sel;
  }
  if(dy>0){
    if(cr<rows-1 && page*per+(cr+1)*cols+cc < n) return page*per+(cr+1)*cols+cc;
    return sel;
  }
  if(dy<0){
    if(cr>0) return sel-cols;
    return sel;
  }
  return sel;
}

static int gridPage(int sel,int dir,int cols,int rows,int n){
  if(n<=0) return 0;
  int per=cols*rows, pos=sel%per, maxpage=(n-1)/per;
  int np=sel/per + dir; if(np<0) np=0; if(np>maxpage) np=maxpage;
  int i=np*per+pos; return i>=n? n-1 : i;
}

static bool ensureDirectory(const char *path) {
  if(mkdir(path,0777)==0) return true;
  if(errno!=EEXIST) return false;
  struct stat st{};
  return stat(path,&st)==0&&S_ISDIR(st.st_mode);
}

static void cleanupLauncher() {
  // Fence the updater's SDL wake callback before any SDL object is destroyed.
  LauncherUpdate_SetWakeCallback(nullptr,nullptr);
  LauncherUpdate_Shutdown();
  // The worker may still be decoding an SD image into a software surface.  It
  // must finish before game records and SDL_image are torn down.
  stopCoverDecodeWorker();
  if(g_ren) SDL_SetRenderTarget(g_ren,nullptr);
  for(auto &game:g_games){ if(game.cover) SDL_DestroyTexture(game.cover); game.cover=nullptr; }
  clearTextCaches();
  for(int index=1;index<4;index++){ if(g_flag[index]) SDL_DestroyTexture(g_flag[index]); g_flag[index]=nullptr; }
  SDL_Texture **glyphs[]={&g_gA,&g_gB,&g_gX,&g_gY,&g_gPlus,&g_gMinus,&g_gLeftRight,&g_gUpDown,&g_gL,&g_gR};
  for(SDL_Texture **glyph:glyphs){ if(*glyph) SDL_DestroyTexture(*glyph); *glyph=nullptr; }
  if(g_logo) SDL_DestroyTexture(g_logo);
  g_logo=nullptr;
  if(g_glowTexture) SDL_DestroyTexture(g_glowTexture);
  g_glowTexture=nullptr;
  if(g_uiTarget) SDL_DestroyTexture(g_uiTarget);
  g_uiTarget=nullptr;
  if(g_font) TTF_CloseFont(g_font);
  if(g_font_sm) TTF_CloseFont(g_font_sm);
  if(g_font_big) TTF_CloseFont(g_font_big);
  g_font=g_font_sm=g_font_big=nullptr;
  if(g_plReady) plExit();
  g_plReady=false;
  uiAudioShutdown();
  SwitchStorage::Shutdown();
  closeController();
  if(g_ren) SDL_DestroyRenderer(g_ren);
  if(g_win) SDL_DestroyWindow(g_win);
  g_ren=nullptr; g_win=nullptr;
  if(g_imgReady) IMG_Quit();
  if(g_ttfReady) TTF_Quit();
  if(g_sdlReady) SDL_Quit();
  g_imgReady=g_ttfReady=g_sdlReady=false;
  if(g_griddbReady) griddb_global_exit();
  g_griddbReady=false;
  if(g_storageSocketReady) socketExit();
  g_storageSocketReady=false;
  if(g_romfsReady) romfsExit();
  g_romfsReady=false;
}

static int startupFailure(const char *message) {
  if(g_sdlReady) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Drastic DS Launcher",message,g_win);
  cleanupLauncher();
  /* A handled startup error is not a process crash. Returning zero prevents
     hbloader from turning it into a generic Atmosphere user-break report. */
  return 0;
}

static int earlyStartupFailure(const char *message, Result result) {
  consoleInit(nullptr);
  printf("DrasticDS_nx - early startup failure\n\n%s\n\n", message);
  printf("Result: 0x%08X (module %u, description %u)\n\n",
         result, R_MODULE(result), R_DESCRIPTION(result));
  printf("Press A to exit.");
  consoleUpdate(nullptr);

  PadState failurePad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&failurePad);
  while(appletMainLoop()) {
    padUpdate(&failurePad);
    if(padGetButtonsDown(&failurePad)&HidNpadButton_A) break;
    svcSleepThread(16000000ULL);
  }
  consoleExit(nullptr);
  return 0;
}

static bool isAppletMode(){
  const AppletType type=appletGetAppletType();
  return type!=AppletType_Application&&type!=AppletType_SystemApplication;
}

static void runAppletInstaller(){
  LauncherLocalization::Initialize("system");
  auto tr=[](const char *value){return std::string(LauncherLocalization::Translate(value));};
  const int panelWidth=std::min(SW-96,960),panelHeight=std::min(SH-96,500);
  const int panelX=(SW-panelWidth)/2,panelY=(SH-panelHeight)/2;
  const int buttonWidth=std::min(520,panelWidth-96),buttonHeight=76;
  const int buttonX=(SW-buttonWidth)/2,buttonY=panelY+panelHeight-buttonHeight-48;
  beginScreenFx();
  for(;;){
    if(!beginUiFrame())return;
    SDL_Event event{};navRepeat();
    while(pollUiEvent(event)){
      int tx=0,ty=0;const TouchKind touch=touchFeed(event,&tx,&ty);
      const bool pressed=event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CONFIRM;
      const bool touched=touch==TOUCH_TAP&&tx>=buttonX&&tx<buttonX+buttonWidth&&ty>=buttonY&&ty<buttonY+buttonHeight;
      if(pressed||touched){
        clearUiBackground();drawHeader(LauncherLocalization::Translate("Applet mode installer").data(),nullptr);drawTextC(g_font,SW/2,SH/2,LauncherLocalization::Translate("Installing...").data(),COL_VAL);presentUi();
        appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);char error[256]{};const bool installed=forwarder_create_launcher(error,sizeof(error));appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        if(installed)modalMessage("Drastic DS",{tr("HOME Menu shortcut installed.")});else modalMessage(tr("Shortcut failed").c_str(),{error[0]?error:tr("Unknown error")});beginScreenFx();
      }
      if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL)return;
    }
    clearUiBackground();glassPanel(panelX,panelY,panelWidth,panelHeight);border(panelX,panelY,panelWidth,panelHeight,3,COL_SEL);
    drawTextC(g_font_big,SW/2,panelY+44,LauncherLocalization::Translate("Applet mode installer").data(),COL_SEL);
    const auto lines=wrapDialogLines({tr("Drastic DS is running in applet mode."),tr("Install a HOME Menu shortcut to run it with full memory and normal performance.")},panelWidth-96);
    int lineY=panelY+132;for(const std::string &line:lines){if(lineY+TTF_FontHeight(g_font)>=buttonY-28)break;drawTextC(g_font,SW/2,lineY,line.c_str(),COL_TXT);lineY+=TTF_FontHeight(g_font)+12;}
    fillRect(buttonX,buttonY,buttonWidth,buttonHeight,COL_FOCUS);border(buttonX,buttonY,buttonWidth,buttonHeight,3,COL_SEL);
    drawTextC(g_font,SW/2,buttonY+(buttonHeight-TTF_FontHeight(g_font))/2,LauncherLocalization::Translate("Install").data(),COL_VAL);
    const std::string install=tr("Install"),back=tr("Back");FootItem footer[]={{"A",install.c_str(),FA_NONE},{"B",back.c_str(),FA_NONE}};drawFooterHints(footer,2,panelY+panelHeight-18);
    drawFadeIn();presentUi();waitForNextUiFrame();
  }
}

int main(int argc, char **argv){
  extern std::string g_forwarderSelfPath;
  if(argc>=1&&argv[0]&&argv[0][0]) g_forwarderSelfPath=argv[0];
  g_launcherNroPath=launcherNroPath();
  std::string updateRecoveryError;
  const bool updateRecoveryOk=g_launcherNroPath.empty()||
      LauncherUpdate_RecoverInstallation(g_launcherNroPath,updateRecoveryError);
  const Result romfsResult=romfsInit();
  if(R_FAILED(romfsResult))
    return earlyStartupFailure("Could not mount the embedded launcher RomFS.",romfsResult);
  g_romfsReady=true;
  updateAutoFirmwareLanguageLabel();
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY,"linear");
  if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_AUDIO)!=0) return startupFailure("SDL initialization failed.");
  g_sdlReady=true;
  LauncherUpdate_SetWakeCallback([](void*){wakeUiFromWorker(0x55504454);},nullptr);
  uiAudioInit();
  if(TTF_Init()!=0) return startupFailure("Font initialization failed.");
  g_ttfReady=true;
  const int imageFlags=IMG_INIT_PNG|IMG_INIT_JPG;
  if((IMG_Init(imageFlags)&imageFlags)!=imageFlags) return startupFailure("Image initialization failed.");
  g_imgReady=true;
  if(appletGetOperationMode()==AppletOperationMode_Console){ SW=1920; SH=1080; }
  g_win=SDL_CreateWindow("Drastic DS",0,0,SW,SH,SDL_WINDOW_FULLSCREEN);
  if(!g_win) return startupFailure("Could not create the launcher window.");
  g_ren=SDL_CreateRenderer(g_win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if(!g_ren) return startupFailure("Could not create the launcher renderer.");
  SDL_SetRenderDrawBlendMode(g_ren,SDL_BLENDMODE_BLEND);
  if(SDL_GetRendererOutputSize(g_ren,&SW,&SH)!=0) return startupFailure("Could not query the display size.");
  g_outputW=SW;
  g_outputH=SH;
  if(SDL_Surface *logo=IMG_Load("romfs:/logo.png")){ g_logo=SDL_CreateTextureFromSurface(g_ren,logo); SDL_FreeSurface(logo); }
  makeFlags();
  for(int index=0;index<SDL_NumJoysticks();index++) if(SDL_IsGameController(index)){ openController(index); break; }

  if(R_FAILED(plInitialize(PlServiceType_User))) return startupFailure("System font service initialization failed.");
  g_plReady=true;
  PlFontData fontData{};
  if(R_FAILED(plGetSharedFontByType(&fontData,PlSharedFontType_Standard))||!fontData.address||!fontData.size||fontData.size>INT_MAX)
    return startupFailure("Could not load the system font.");
  int scale=SH>=1080?1:0;
  auto openFont=[&](int size)->TTF_Font*{ SDL_RWops *rw=SDL_RWFromConstMem(fontData.address,(int)fontData.size); return rw?TTF_OpenFontRW(rw,1,size):nullptr; };
  g_font_sm=openFont(scale?26:20);
  g_font=openFont(scale?32:26);
  g_font_big=openFont(scale?52:40);
  if(!g_font_sm||!g_font||!g_font_big) return startupFailure("Could not open the system font.");
  makeGlyphs();
  if(isAppletMode()){
    (void)ensureDirectory("sdmc:/switch");(void)ensureDirectory(DATA_DIR);
    runAppletInstaller();cleanupLauncher();return 0;
  }

  g_griddbReady=griddb_global_init();
  if(!g_griddbReady&&R_SUCCEEDED(socketInitializeDefault())) g_storageSocketReady=true;
  const char *directories[]={"sdmc:/switch",DATA_DIR,EMU_HOST_DIR,COVERS_DIR,CORES_DIR,GAMECFG_DIR,DEF_GAMEDIR,SYSTEM_DIR,USER_DIR,CACHE_DIR,LSFG_DIR,
                             "sdmc:/switch/drastic/cheats","sdmc:/switch/drastic/scripts",SHADERS_DIR,
                             "sdmc:/switch/drastic/slot2","sdmc:/switch/drastic/microphone",
                             "sdmc:/switch/drastic/user/savestates","sdmc:/switch/drastic/user/backup"};
  for(const char *directory:directories) if(!ensureDirectory(directory)) return startupFailure("Could not create the Drastic DS data directories.");
  /* Earlier builds used separate Vulkan and OpenGL executables. The unified
     host supersedes them, so remove every known stale copy on the first
     launcher boot after updating. */
  cleanupLegacyEmuHosts();

  if(!updateRecoveryOk)
    modalMessage("Update recovery failed",{updateRecoveryError,"The installed launcher was left unchanged."});

  const bool missingSystemFilesAtStartup=!userSystemFilesPresent();

  struct stat configStat{};
  bool firstRun=stat(LAUNCHER_INI,&configStat)!=0;
  storeLoad(g_global,LAUNCHER_INI);
  storeLoad(g_titles,TITLES_INI);
  storeLoad(g_recent,RECENT_INI);
  storeLoad(g_metadata,METADATA_INI);
  const bool legacyIdentityStore=storeHas(g_global,"Library/IdentityCount");
  const bool migrateIdentities=loadLibraryIdentities();
  if(legacyIdentityStore)storeRemovePrefix(g_global,"Library/Identity");
  const bool smbPathsMigrated=migrateV109SmbPaths();
  if(migrateIdentities||g_libraryIdentitiesDirty){
    saveLibraryIdentities();
    if(!storeSave(g_metadata,METADATA_INI))
      return startupFailure("Could not migrate the library identity cache.");
  }
  bool runtimeKeysRemoved=false;
  for(const char *key:{"Wrapper/CoreSo","Drastic/RomPath"})if(storeHas(g_global,key)){
    storeRemove(g_global,key);runtimeKeysRemoved=true;
  }
  loadLibraryOrganization();
  const int previousSettingsVersion=atoi(storeGet(
      g_global,"Wrapper/LauncherSettingsVersion","0"));
  bool settingsMigrated=migrateLauncherSettings(g_global)||legacyIdentityStore||
                        smbPathsMigrated||runtimeKeysRemoved;
  if(previousSettingsVersion<3&&!migrateFastForwardProfiles())
    return startupFailure("Could not migrate per-game fast-forward settings.");
  settingsMigrated=normalizeLsfgStore(g_global)||settingsMigrated;
  int sortMode=atoi(storeGet(g_global,"Wrapper/SortMode","0"));
  if(sortMode>=0&&sortMode<SORT_COUNT) g_sort=sortMode;
  if(firstRun){
    g_active=&g_global;
    saveGameSources({DEF_GAMEDIR});
    storeSet(g_global,"Wrapper/SteamGridDBKey","");
    storeSet(g_global,"Wrapper/UiSounds","true");
    storeSet(g_global,"Wrapper/Theme","animated");
    storeSet(g_global,"Wrapper/Language","system");
    storeSet(g_global,"Wrapper/LauncherRotation","0");
    storeSet(g_global,"Wrapper/GridColumns","6");
    storeSet(g_global,"Wrapper/GridRows","2");
    storeSet(g_global,"Wrapper/Renderer","vk");
    storeSet(g_global,"Wrapper/AnalogDeadzone","35");
    storeSet(g_global,"Wrapper/ShowGameTitles","true");
    storeSet(g_global,"Wrapper/ShowRegionFlags","true");
    storeSet(g_global,"Wrapper/ShowCustomSettingsBadges","true");
    storeSet(g_global,"Wrapper/UiAnimations","true");
    storeSet(g_global,"Wrapper/CheckUpdatesAtBoot","true");
    storeSet(g_global,"Wrapper/InstalledReleaseTag",LauncherUpdate_BuiltReleaseTag());
    commitAll();
    if(!storeSave(g_global,LAUNCHER_INI)) return startupFailure("Could not create launcher.ini.");
  } else {
    bool changed=settingsMigrated;
    if(!storeHas(g_global,"Wrapper/GamePathCount")){ saveGameSources(loadGameSources()); changed=true; }
    int columns=atoi(storeGet(g_global,"Wrapper/GridColumns","6"));
    int rows=atoi(storeGet(g_global,"Wrapper/GridRows","2"));
    if(columns<3||columns>8){ storeSet(g_global,"Wrapper/GridColumns","6"); changed=true; }
    if(rows<1||rows>3){ storeSet(g_global,"Wrapper/GridRows","2"); changed=true; }
    if(changed&&!storeSave(g_global,LAUNCHER_INI)) return startupFailure("Could not update launcher.ini.");
  }
  LauncherLocalization::Initialize(storeGet(g_global,"Wrapper/Language","system"));
  applyLauncherAppearance();
  const int launcherRotation=atoi(
      storeGet(g_global,"Wrapper/LauncherRotation","0"));
  if(!configureLauncherOrientation(launcherRotation))
    return startupFailure("Could not create the launcher interface surface.");
  uiAudioSetEnabled(strcmp(storeGet(g_global,"Wrapper/UiSounds","true"),"false")!=0);
  if(missingSystemFilesAtStartup){
    modalMessage("Nintendo DS system files required",
                 {"Nintendo DS BIOS and firmware are not bundled.","",
                  "Copy these files to /switch/drastic/system/:",
                  "nds_bios_arm7.bin", "nds_bios_arm9.bin",
                  "nds_firmware.bin"});
  }
  if(!ensureBundledShaders())
    return startupFailure("Could not install the bundled custom shaders.");
  startCoverDecodeWorker();
  std::vector<std::string> gamePaths=loadGameSources();
  bool hasUsbSource=hasConfiguredUsbSource(gamePaths);
  const bool startupHasUsbSource=hasUsbSource;
  const std::vector<SwitchStorage::SmbShare> startupSmbShares=loadSmbSharesFromStore();
  std::atomic<bool> storageInitDone{false},storageInitCancel{false};
  std::thread storageInitWorker([&,startupHasUsbSource,startupSmbShares]{
    SwitchStorage::SetUsbStatusCallback(usbStatusWake,nullptr);
    if(startupHasUsbSource&&!storageInitCancel.load())SwitchStorage::InitializeUsb();
    for(const auto &share:startupSmbShares){
      if(storageInitCancel.load())break;
      if(share.autoMount){std::string error;SwitchStorage::MountSmb(share,&error,&storageInitCancel);}
    }
    storageInitDone=true;wakeUiFromWorker(0x53544f52);
  });
  uint64_t usbGeneration=0;Uint32 usbRefreshAt=0;bool storageIntegrated=false;
  std::vector<std::string> initialGamePaths;
  for(const std::string &source:gamePaths)
    if(!isUsbStoragePath(source)&&source.rfind(UNAVAILABLE_USB_PREFIX,0)!=0&&
       !isConfiguredSmbStoragePath(source,startupSmbShares))
      initialGamePaths.push_back(source);
  startGameScan(std::move(initialGamePaths),true);

  int sel=0,top=0,rows=1;
  bool running=true,launch=false,userExit=false;
  std::string launchKey,launchPathKey,launchLegacyKey,launchPath;
  bool launchLegacyUnique=false;
  auto selectGame=[&](Game &game){
    recordPlayed(game);
    launchKey=game.key;
    launchPathKey=game.pathKey;
    launchLegacyKey=game.legacyKey;
    launchLegacyUnique=game.legacyUnique;
    launchPath=game.path;
    launch=true;
    running=false;
  };
  auto requestExit=[&](){
    auto tr=[](const char *value){return std::string(LauncherLocalization::Translate(value));};
    if(!confirmBox(tr("Exit Drastic DS?").c_str(),{tr("Active scans and network operations will be cancelled safely."),tr("Return to the HOME Menu?")})){beginScreenFx();return false;}
    userExit=true;running=false;return true;
  };

  bool forwarderRequested=false,forwarderMatched=false;
  std::string forwarderKey;
  for(int argument=1;argument+1<argc;argument++) if(!strcmp(argv[argument],"-g")){
    forwarderRequested=true;
    forwarderKey=argv[argument+1];
    if(Game *game=findGameByKey(forwarderKey)){ selectGame(*game); forwarderMatched=true; }
    break;
  }
  if(!forwarderRequested&&g_griddbReady&&!g_launcherNroPath.empty()&&
     strcmp(storeGet(g_global,"Wrapper/CheckUpdatesAtBoot","true"),"false")!=0)
    LauncherUpdate_StartCheck(installedReleaseTag());
  bool forwarderPending=forwarderRequested&&!forwarderMatched;
  const Uint32 forwarderDeadline=forwarderPending?SDL_GetTicks()+10000:0;
  if(forwarderPending&&!usbRefreshAt) usbRefreshAt=SDL_GetTicks()+300;
  std::vector<std::string> pendingMountedSources;

  while(running&&beginUiFrame()){
    pumpGameScan();
    if(!g_libraryScan&&!pendingMountedSources.empty()){startGameScan(std::move(pendingMountedSources),false);pendingMountedSources.clear();}
    if(sel>=(int)g_libraryView.size())sel=std::max(0,(int)g_libraryView.size()-1);
    if(storageInitDone.load()&&!storageIntegrated){
      if(storageInitWorker.joinable())storageInitWorker.join();
      storageIntegrated=true;
      usbGeneration=SwitchStorage::UsbStatusGeneration();gamePaths=loadGameSources();refreshConfiguredUsbSources(gamePaths);
      for(const std::string &source:gamePaths)
        if(isUsbStoragePath(source)||isConfiguredSmbStoragePath(source,startupSmbShares))
          pendingMountedSources.push_back(source);
    }
    if(forwarderPending)if(Game *game=findGameByKey(forwarderKey)){selectGame(*game);forwarderPending=false;}
    if(!running)break;
    pollUpdateNotification();
    if(hasUsbSource&&storageIntegrated){
      const Uint32 now=SDL_GetTicks();
      const uint64_t generation=SwitchStorage::UsbStatusGeneration();
      if(generation!=usbGeneration){ usbGeneration=generation; usbRefreshAt=now+300; }
      if(usbRefreshAt&&SDL_TICKS_PASSED(now,usbRefreshAt)){
        usbRefreshAt=0;
        const std::string selected=!g_libraryView.empty()?g_libraryView[sel]->key:std::string{};
        std::unordered_set<std::string> connectedUsbIds;
        for(const auto &location:SwitchStorage::ListUsbLocations())connectedUsbIds.insert(location.id);
        removeUnavailableUsbGames(connectedUsbIds);
        // Reconstruct every source from its persisted stable binding before
        // considering the new topology.  Keeping an old resolved umsN: string
        // across this boundary can silently bind it to a different disk.
        gamePaths=loadGameSources();
        refreshConfiguredUsbSources(gamePaths);
        std::vector<std::string> usbSources;for(const std::string &source:gamePaths)if(isUsbStoragePath(source))usbSources.push_back(source);
        if(!usbSources.empty())pendingMountedSources=std::move(usbSources);
        sel=0;
        if(!selected.empty()) for(size_t index=0;index<g_libraryView.size();index++) if(g_libraryView[index]->key==selected){ sel=(int)index; break; }
        top=0;
        if(forwarderPending) if(Game *game=findGameByKey(forwarderKey)){
          selectGame(*game);
          forwarderPending=false;
        }
      }
      if(!running) break;
    }
    if(forwarderPending&&SDL_TICKS_PASSED(SDL_GetTicks(),forwarderDeadline)){
      forwarderPending=false;modalMessage("Game not found",{"The shortcut's game is not in the current library.","","Reconnect its storage or update the game folders."});running=false;
    }
    if(forwarderPending){
      SDL_Event event;
      while(pollUiEvent(event)){
        pumpStick(event);
        if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==BTN_CANCEL){
          requestExit();
          break;
        }
      }
      if(!running) break;
      renderUsbForwarderWait();
    Uint32 nextDeadline=forwarderDeadline;
    if(usbRefreshAt&&(!nextDeadline||SDL_TICKS_PASSED(nextDeadline,usbRefreshAt)))nextDeadline=usbRefreshAt;
    waitForNextUiFrame(true,nextDeadline);
      continue;
    }
    GLay layout=gridLayout(); int cols=layout.cols; rows=layout.rows;
    SDL_Event event; navRepeat();
    while(pollUiEvent(event)){
      pumpStick(event);
      int tx=0,ty=0,n=(int)g_libraryView.size(); TouchKind touch=touchFeed(event,&tx,&ty);
      if(touch==TOUCH_SWIPE_L||touch==TOUCH_SWIPE_R){ sel=gridPage(sel,touch==TOUCH_SWIPE_L?1:-1,cols,rows,n); top=n?(sel/(cols*rows))*rows:0; continue; }
      if(touch==TOUCH_TAP){
        int action=footTapAct(tx,ty);
        if(action==FA_NONE){
          int hit=gridHitTest(tx,ty,top);
          if(hit>=0){ if(hit==sel&&n) selectGame(*g_libraryView[sel]); else sel=hit; }
        } else {
          SDL_Event press{}; press.type=SDL_CONTROLLERBUTTONDOWN;
          switch(action){
            case FA_LAUNCH: press.cbutton.button=BTN_CONFIRM; SDL_PushEvent(&press); break;
            case FA_SORT: press.cbutton.button=SDL_CONTROLLER_BUTTON_X; SDL_PushEvent(&press); break;
            case FA_OPTIONS: press.cbutton.button=SDL_CONTROLLER_BUTTON_START; SDL_PushEvent(&press); break;
            case FA_SETTINGS: press.cbutton.button=BTN_SETTINGS; SDL_PushEvent(&press); break;
            case FA_FILTER: chooseLibraryFilter();sel=top=0;break;
            case FA_PAGEL: sel=gridPage(sel,-1,cols,rows,n); break;
            case FA_PAGER: sel=gridPage(sel,1,cols,rows,n); break;
            case FA_QUIT: requestExit(); break;
          }
        }
        top=n?(sel/(cols*rows))*rows:0;
        if(!running) break;
        continue;
      }
      if(event.type!=SDL_CONTROLLERBUTTONDOWN) continue;
      switch(event.cbutton.button){
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: sel=gridNav(sel,-1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: sel=gridNav(sel,1,0,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: sel=gridNav(sel,0,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: sel=gridNav(sel,0,1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: sel=gridPage(sel,-1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: sel=gridPage(sel,1,cols,rows,n); break;
        case SDL_CONTROLLER_BUTTON_X:
          if(n){
            std::string keep=g_libraryView[sel]->key; g_sort=(g_sort+1)%SORT_COUNT;
            storeSet(g_global,"Wrapper/SortMode",std::to_string(g_sort).c_str()); storeSave(g_global,LAUNCHER_INI);
            applySort(); sel=0; for(int index=0;index<(int)g_libraryView.size();index++) if(g_libraryView[index]->key==keep){ sel=index; break; }
          }
          break;
        case BTN_CONFIRM: if(n) selectGame(*g_libraryView[sel]); break;
        case SDL_CONTROLLER_BUTTON_START:
          if(n){ Game *game=g_libraryView[sel];int result=perGameMenu(*game,g_pad); if(result==1) selectGame(*game); else if(result==2){ startGameScan(gamePaths,true); sel=top=0; } }
          break;
        case SDL_CONTROLLER_BUTTON_BACK: chooseLibraryFilter();sel=top=0;break;
        case BTN_SETTINGS: {
          std::vector<std::string> oldPaths=gamePaths;
          g_active=&g_global; runSettingsRoot(g_pad,nullptr); storeSave(g_global,LAUNCHER_INI);
          layout=gridLayout(); cols=layout.cols; rows=layout.rows;
          gamePaths=loadGameSources();
          if(gamePaths!=oldPaths||g_rescanAfterSettings){
            hasUsbSource=hasConfiguredUsbSource(gamePaths);
            if(hasUsbSource) SwitchStorage::InitializeUsb();
            usbGeneration=SwitchStorage::UsbStatusGeneration();
            usbRefreshAt=0;
            refreshConfiguredUsbSources(gamePaths);
            startGameScan(gamePaths,true);
            sel=top=0;
            g_rescanAfterSettings=false;
          }
          break;
        }
        case BTN_CANCEL: requestExit(); break;
      }
      top=n?(sel/(cols*rows))*rows:0;
    }
    const std::string location=!g_libraryView.empty()?gameLocationLabel(*g_libraryView[sel]):"No game selected";
    renderGrid(sel,top,location.c_str());
    waitForNextUiFrame(true,usbRefreshAt);
  }

  if(userExit&&g_ren){clearUiBackground();drawTextC(g_font_big,SW/2,SH/2-42,LauncherLocalization::Translate("Closing Drastic DS...").data(),COL_VAL);drawTextC(g_font_sm,SW/2,SH/2+28,LauncherLocalization::Translate("Finishing background operations safely.").data(),COL_DIM);presentUi();}
  storageInitCancel=true;stopGameScan();
  if(storageInitWorker.joinable())storageInitWorker.join();
  SwitchStorage::SetUsbStatusCallback(nullptr,nullptr);

  g_active=&g_global;
  if(launch) commitAll();
  storeSave(g_global,LAUNCHER_INI);
  storeSave(g_recent,RECENT_INI);

  bool willChain=false;
  std::string emulatorNro;
  if(launch&&envHasNextLoad()){
    Store effective=makeRuntimeConfig(g_global,toEmu(launchPath));
    if(!launchKey.empty()){
      std::string profile=std::string(GAMECFG_DIR)+"/"+launchKey+".ini";
      if(!regularFileExists(profile)&&!launchPathKey.empty())profile=std::string(GAMECFG_DIR)+"/"+launchPathKey+".ini";
      if(!regularFileExists(profile)&&launchLegacyUnique&&!launchLegacyKey.empty()) profile=std::string(GAMECFG_DIR)+"/"+launchLegacyKey+".ini";
      Store overrides; storeLoad(overrides,profile.c_str());
      migrateStylusMode(overrides,false);
      storeRemove(overrides,"Wrapper/CpuBoost");
      for(const auto &entry:overrides.kv)
        if(runtimeConfigKey(entry.k)&&!runtimeOwnedKey(entry.k))
          storeSet(effective,entry.k.c_str(),entry.v.c_str());
    }
    normalizeLsfgStore(effective);
    normalizeCpuThreads(effective);
    /* Renderer remains a per-game option. The unified host reads this merged
       profile and selects Vulkan, native NVC0 or Zink at process startup. */
    const char *configuredRenderer=storeGet(effective,"Wrapper/Renderer","vk");
    std::string renderer=!strcmp(configuredRenderer,"gl")?"gl":
                         !strcmp(configuredRenderer,"zink")?"zink":"vk";
    std::string shaderError;
    const bool shaderReady=validateCustomShaderSelection(
        effective,renderer,shaderError);
    appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    std::string coreSource="romfs:/cores/libdrastic_arm64.so";
    std::string coreDestination=std::string(CORES_DIR)+"/libdrastic_arm64.so";
    std::string emulatorSource="romfs:/emu/DrasticDS_nx.nro";
    std::string emulatorDestination=std::string(EMU_HOST_DIR)+"/DrasticDS_nx.nro";
    emulatorNro=emulatorDestination;
    bool haveCore=ensureCore(coreSource.c_str(),coreDestination.c_str(),"109");
    bool haveEmulator=ensureEmu(emulatorSource.c_str(),emulatorDestination.c_str());
    bool haveResources=ensureResources();
    bool haveSystemFiles=userSystemFilesPresent();
    appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    storeSet(effective,"Wrapper/CoreSo",CORE_SO_PATH);
    storeSet(effective,"Drastic/RomPath",toEmu(launchPath).c_str());
    if(!launchKey.empty()){
      storeSet(effective,"Wrapper/GameKey",launchKey.c_str());
      std::string runtimeProfile=std::string("/switch/drastic/gamecfg/")+launchKey+".ini";
      storeSet(effective,"Wrapper/GameConfigPath",runtimeProfile.c_str());
    }else{
      storeRemove(effective,"Wrapper/GameKey");
      storeRemove(effective,"Wrapper/GameConfigPath");
    }
    storeRemove(effective,"Wrapper/LauncherPath");
    storeRemove(effective,"Wrapper/CpuBoost");
    const std::string launcherPath=launcherNroPath();
    if(!launcherPath.empty()) storeSet(effective,"Wrapper/LauncherPath",launcherPath.c_str());
    const bool lsfgRequested=!strcmp(
        storeGet(effective,"Wrapper/LSFGEnabled","false"),"true");
    const char *lsfgWarning=nullptr;
    if(lsfgRequested&&renderer!="vk"){
      // LSFG is a Vulkan-only presentation feature.  Keep the user's saved
      // preference intact for Vulkan, but silently disable it in the merged
      // launch profile when native OpenGL or Zink is selected.
      storeSet(effective,"Wrapper/LSFGEnabled","false");
    } else if(lsfgRequested&&!lsfgDllInstalled()){
      storeSet(effective,"Wrapper/LSFGEnabled","false");
      lsfgWarning="LSFG disabled: Lossless.dll is missing";
    }
    bool configSaved=storeSave(effective,EMU_INI);
    willChain=haveCore&&haveEmulator&&haveResources&&haveSystemFiles&&
              configSaved&&shaderReady;
    if(willChain&&lsfgWarning){
      modalMessage("Launch warning",{lsfgWarning});
    } else if(!willChain){
      std::string failure;
      if(!shaderReady) failure=shaderError;
      else if(!haveSystemFiles) failure="Missing DS BIOS/firmware in /switch/drastic/system";
      else if(!haveResources) failure="Could not extract Drastic resources (SD full?)";
      else if(!haveCore||!haveEmulator) failure="Could not extract emulator files (SD full?)";
      else failure="Could not write the launch configuration";
      modalMessage("Launch failed",{failure});
    }
  }

  cleanupLauncher();
  if(willChain)
    envSetNextLoad(emulatorNro.c_str(),emulatorNro.c_str());
  return 0;
}
