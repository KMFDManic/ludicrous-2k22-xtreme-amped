#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GLES2/gl2.h>

/* Ensure we call real GLES2 symbols, not rgl wrappers */
#ifdef glDisable
#undef glDisable
#endif
#ifdef glClear
#undef glClear
#endif
#ifdef glClearColor
#undef glClearColor
#endif
#ifdef glEnable
#undef glEnable
#endif
#ifdef __cplusplus
extern "C" {
#endif
int GLideN64_GetBGMode(void);
#ifdef __cplusplus
}
#endif

#include "libretro.h"
#include "libretro_private.h"
#include "GLideN64_libretro.h"

#ifdef HAVE_LIBNX
#include <switch.h>
#endif

#include <libco.h>

#include <glsm/glsmsym.h>

#include "api/m64p_frontend.h"
#include "plugin/plugin.h"
#include "api/m64p_types.h"
#include "r4300/r4300.h"
#include "memory/memory.h"
#include "main/main.h"
#include "main/cheat.h"
#include "main/version.h"
#include "main/savestates.h"
#include "main/mupen64plus.ini.h"
#include "api/m64p_config.h"
#include "osal_files.h"
#include "main/rom.h"
#include "pi/pi_controller.h"
#include "si/pif.h"
#include "libretro_memory.h"
#include "GLideN64_libretro.h"

#include "audio_plugin.h"

#ifndef PRESCALE_WIDTH
#define PRESCALE_WIDTH  640
#endif

#ifndef PRESCALE_HEIGHT
#define PRESCALE_HEIGHT 625
#endif

#define PATH_SIZE 2048

#define ISHEXDEC ((codeLine[cursor]>='0') && (codeLine[cursor]<='9')) || ((codeLine[cursor]>='a') && (codeLine[cursor]<='f')) || ((codeLine[cursor]>='A') && (codeLine[cursor]<='F'))

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

retro_log_printf_t log_cb = NULL;
retro_video_refresh_t video_cb = NULL;
retro_input_poll_t poll_cb = NULL;
retro_input_state_t input_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t environ_cb = NULL;


/* ---- Xtreme live-apply helpers ---- */
static bool av_info_dirty = false;

static void xt_push_av_info_if_needed(void)
{
    if (!av_info_dirty || !environ_cb) return;
    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    /* Push new timing/geometry so changes take effect without restart */
    environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
    av_info_dirty = false;
}
/* ----------------------------------- */


struct retro_rumble_interface rumble;

save_memory_data saved_memory;

static cothread_t game_thread;
cothread_t retro_thread;

int astick_deadzone;
int astick_sensitivity;
int r_cbutton;
int l_cbutton;
int d_cbutton;
int u_cbutton;
int dpad_as_analog_p1 = 0;

static uint8_t* game_data = NULL;
static uint32_t game_size = 0;

static bool     emu_initialized     = false;
static unsigned initial_boot        = true;
static unsigned audio_buffer_size   = 2048;

static unsigned retro_filtering     = 0;
static bool     first_context_reset = false;
static bool     initializing        = true;

uint32_t retro_screen_width = 320;
uint32_t retro_screen_height = 240;
float retro_screen_aspect = 4.0 / 3.0;

uint32_t bilinearMode = 0;
uint32_t EnableHWLighting = 0;
uint32_t CorrectTexrectCoords = 0;
uint32_t enableNativeResTexrects = 0;
uint32_t enableLegacyBlending = 0;
uint32_t EnableCopyColorToRDRAM = 0;
uint32_t EnableCopyDepthToRDRAM = 0;
uint32_t AspectRatio = 0;
uint32_t txFilterMode = 0;
uint32_t txEnhancementMode = 0;
uint32_t txHiresEnable = 0;
uint32_t txHiresFullAlphaChannel = 0;
uint32_t txFilterIgnoreBG = 0;
uint32_t MultiSampling = 0;
uint32_t EnableFragmentDepthWrite = 0;
uint32_t EnableShadersStorage = 0;
uint32_t CropMode = 0;
uint32_t EnableFBEmulation = 0;
uint32_t ForceDisableExtraMem = 0;
uint32_t CountPerOp = 0;
uint32_t TurboBoost = 0;
static bool TurboBoost_NoSync = false;
uint32_t CountPerScanlineOverride = 0;
uint32_t GLideN64IniBehaviour = 0;
uint32_t EnableCopyAuxToRDRAM = 0;
uint32_t BGMode = 1;

/* Xtreme Fog/Draw Distance: 0.0 = disabled, 1.0 = normal */
float xt_fog_scale = 1.0f;


/* Xtreme Fog Color override: 0=Default, 1=Purple, 2=Red, 3=Green, 4=Blue, 5=Yellow, 6=Cyan, 7=Magenta, 8=Orange, 9=Pink, 10=Black, 11=White */
uint32_t xt_fog_color_mode = 0;
unsigned xt_purge_interval_vi = 600;


/* Xtreme Langoliers helpers */
void xt_vram_purge_soft(void);
int xt_purge_button_id = -1; /* RETRO_DEVICE_ID_JOYPAD_* or -1 disabled */
static int16_t xt_purge_button_last = 0;
static unsigned xt_seconds_to_vi(double seconds)
{
    /* Use the core’s reported VI refresh as FPS-ish; fall back to 60 if weird */
    double fps = vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype);
    if (fps < 1.0 || fps > 1000.0) fps = 60.0;

    if (seconds <= 0.0)
        return 0;

    unsigned vi = (unsigned)(seconds * fps + 0.5);
    return vi ? vi : 1; /* minimum 1 tick */
}

int rspMode = 0;
// after the controller's CONTROL* member has been assigned we can update
// them straight from here...
extern struct
{
    CONTROL *control;
    BUTTONS buttons;
} controller[4];
// ...but it won't be at least the first time we're called, in that case set
// these instead for input_plugin to read.
int pad_pak_types[4];
int pad_present[4] = {1, 1, 1, 1};

static void n64DebugCallback(void* aContext, int aLevel, const char* aMessage)
{
    char buffer[1024];
    snprintf(buffer, 1024, "mupen64plus: %s\n", aMessage);
    if (log_cb)
        log_cb(RETRO_LOG_INFO, buffer);
}

extern m64p_rom_header ROM_HEADER;

static void xt_apply_sync_state(void)
{
#ifdef RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE
    struct retro_fastforwarding_override ff = {0};
    if (TurboBoost_NoSync && TurboBoost > 0) {
        ff.ratio = TurboBoost;
        ff.inhibit_frame_time = true;
        ff.notify_frontend = true;
    }
    environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff);
#endif
#ifdef HAVE_GLSM
    int swap = (TurboBoost_NoSync && TurboBoost > 0) ? 0 : 1;
    glsm_ctl(GLSM_CTL_STATE_SET_SWAP_INTERVAL, &swap);
#endif
}

static void parse_turboboost_value(const char *val)
{
    TurboBoost_NoSync = false;
    TurboBoost = 0;
    if (!val) return;

    if (!strcmp(val, "X6")) TurboBoost = 6;
    else if (!strcmp(val, "X5")) TurboBoost = 5;
    else if (!strcmp(val, "X4")) TurboBoost = 4;
    else if (!strcmp(val, "X3")) TurboBoost = 3;
    else if (!strcmp(val, "X2")) TurboBoost = 2;
    else if (!strcmp(val, "X1")) TurboBoost = 1;
    else if (!strcmp(val, "EX9")) { TurboBoost = 9; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX8")) { TurboBoost = 8; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX7")) { TurboBoost = 7; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX6")) { TurboBoost = 6; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX5")) { TurboBoost = 5; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX4")) { TurboBoost = 4; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX3")) { TurboBoost = 3; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX2")) { TurboBoost = 2; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX1")) { TurboBoost = 1; TurboBoost_NoSync = true; }
    else if (!strcmp(val, "EX0")) { TurboBoost = 0; TurboBoost_NoSync = true; }
    else TurboBoost = 0;

    xt_apply_sync_state();
}

static void setup_variables(void)
{
    struct retro_variable variables[] = {
        { "LudicrousN64-cpucore",
#ifdef DYNAREC
#ifdef HAVE_LIBNX
            "CPU Core; dynamic_recompiler|pure_interpreter|cached_recompiler" },
#else
            "CPU Core; dynamic_recompiler|pure_interpreter|cached_interpreter" },
#endif
#else
            "CPU Core; dynamic_recompiler|pure_interpreter" },
#endif
        { "LudicrousN64-rspmode",
#ifndef VC
            "RSP Mode; HLE|LLE" },
#else
            "RSP Mode; HLE" },
#endif
        { "LudicrousN64-43screensize",
            "4:3 Resolution; 512x384|640x480|720x576|832x624|960x720|1152x864|1280x960|1400x1050|1440x1080|1600x1200|1920x1440|2240x1680|2560x1920|2880x2160|3200x2400|3520x2640|3840x2880|160x120|256x192|320x240|384x288|400x300" },
        { "LudicrousN64-169screensize",
            "16:9 Resolution; 512×288|640x360|854×480|960x540|1280x720|1920x1080|2560x1440|3840x2160|4096x2160|7680x4320|256×144|426×240" },
        { "LudicrousN64-aspect",
            "Aspect Ratio; 4:3|16:9|16:9 adjusted" },
        {"LudicrousN64-DPadMode",
           "Xtreme D-Pad Mode; Off|P1 D-Pad as Analog"},
        {"LudicrousN64-astick-sensitivity",
           "Xtreme Analog/D-Pad Mode Sensitivity (percent); 100|105|110|115|120|125|130|135|140|145|150|200|50|55|60|65|70|75|80|85|90|95"},           
        { "LudicrousN64-LangoliersPurge",
            "Xtreme Langoliers Purge (periodically unleash time-eaters to devour stale VRAM); Off|Always (Constant – The Ledge)|50 ms (Flash Fiction – The Jaunt)|100 ms (Short Story – Survivor Type)|250 ms (Short Story – The Raft)|500 ms (Short Story – The Boogeyman)|750 ms (Short Story – Children of the Corn)|900 ms (Short Story – The Monkey)|1 sec (Voracious – The Langoliers)|2 sec (Telekinetic – Carrie)|3 sec (Rabid – Cujo)|5 sec (Ravenous – It)|7 sec (Sinister – The Dark Half)|10 sec (Creeping – The Mist)|15 sec (Relentless – Christine)|20 sec (Restless – Pet Sematary)|30 sec (Obsessive – Misery)|45 sec (Prophetic – The Dead Zone)|60 sec (Watchful – The Shining)|90 sec (Nocturnal – ’Salem’s Lot)|120 sec (Ominous – Needful Things)|180 sec (Haunting – 1408)|240 sec (Pursuit – The Running Man)|300 sec (Searing – Firestarter)|600 sec (Drowsy – Doctor Sleep)|900 sec (Nostalgic – Stand by Me)|1200 sec (Hopeful – The Shawshank Redemption)|1500 sec (Enduring – The Long Walk)|1800 sec (Expansive – The Stand)|2400 sec (Epic – The Dark Tower)" },
        { "LudicrousN64-LangoliersPress",
            "Xtreme MF Langoliers Press (tap a button to instantly unleash a one-shot VRAM purge); Disabled|A|B|X|Y|L|R|Select|Start|L2|R2|L3|R3" },
        { "LudicrousN64-virefresh",
            "Xtreme Langoliers TimeSlip VIRefresh (discharge time-eaters to make stale FPS...vanish); Auto|2400|2500|2600|2700|2800|2900|3000|3100|3200|3300|3400|3500|3600|3700|3800|3900|4000|4100|4200|4300|4400|4500|50|100|150|200|250|300|350|400|450|500|550|600|650|700|750|800|850|900|950|1000|1050|1100|1150|1200|1250|1300|1350|1400|1450|1500|1600|1700|1800|1900|2000|2100|2200|2300" },
        { "LudicrousN64-TurboBoost",
            "Xtreme TurboBoost; 0|X1|X2|X3|X4|X5|X6|EX0|EX1|EX2|EX3|EX4|EX5|EX6|EX7|EX8|EX9" },
        { "LudicrousN64-CountPerOp",
            "Xtreme OverClock; 0|X1|X2|X3|X4|X5|X6|X7|X8|X9|XX" },
        { "LudicrousN64-GLideN64IniBehaviour",
            "Xtreme Ini Control; ini_config_priority|core_options_priority|disabled"},
        { "LudicrousN64-xtreme_fog", "Xtreme Fog/Draw Distance; 1.00x (Default)|0.75x (Farther)|0.50x (Farthest)|1.25x (Nearer)|1.50x (Near)|2.00x (Spooky)|3.00x (Thick)|4.00x (In-Your-Face)|6.00x (Wall)|8.00x (Whiteout)|12.00x (Near Wall)|16.00x (Face Fog)|24.00x (Blind)|32.00x (Total Whiteout)|Off (Disabled)" },
        { "LudicrousN64-xtreme_fog_color", "Xtreme Fog Color; Default|Purple|Red|Green|Blue|Yellow|Cyan|Magenta|Orange|Pink|Black|White" },
        { "LudicrousN64-BilinearMode",
            "Bilinear filtering mode; standard|3point" },
#ifndef HAVE_OPENGLES2
        { "LudicrousN64-MultiSampling",
            "MSAA level; 0|2|4|8|16" },
#endif
#if !defined(VC) && !defined(CLASSIC)
        { "LudicrousN64-EnableFBEmulation",
            "Framebuffer Emulation; False|True" },
#endif				
        { "LudicrousN64-EnableCopyColorToRDRAM",
#ifndef HAVE_OPENGLES
            "Color buffer to RDRAM; Off|ASync|Sync" },
#else
            "Color buffer to RDRAM; Off|Async|Sync" },
#endif
#if !defined(VC) && !defined(CLASSIC)
        { "LudicrousN64-EnableCopyDepthToRDRAM",
            "Depth buffer to RDRAM; Off|Software|FromMem" },
#endif
        { "LudicrousN64-EnableCopyAuxToRDRAM",
            "Copy auxiliary buffers to RDRAM; False|True" },
        { "LudicrousN64-EnableHWLighting",
            "Hardware per-pixel lighting; False|True" },
        { "LudicrousN64-CorrectTexrectCoords",
            "Continuous texrect coords; Off|Auto|Force" },
        { "LudicrousN64-EnableNativeResTexrects",
            "Native res. 2D texrects; False|True" },
        { "LudicrousN64-BackgroundMode",
            "Background mode (GLideN64); One piece|Stripped" },            
#if defined(HAVE_OPENGLES)
        { "LudicrousN64-EnableLegacyBlending",
            "Less accurate blending mode; True|False" },
        { "LudicrousN64-EnableFragmentDepthWrite",
            "GPU shader depth write; False|True" },
#else
        { "LudicrousN64-EnableLegacyBlending",
            "Less accurate blending mode; True|False" },
        { "LudicrousN64-EnableFragmentDepthWrite",
            "GPU shader depth write; False|True" },
#endif
        { "LudicrousN64-EnableShadersStorage",
            "Cache GPU Shaders; False|True" },
        { "LudicrousN64-CropMode",
            "Crop Mode; Auto|Off" },
        { "LudicrousN64-txFilterMode",
            "Texture filter; None|Smooth filtering 1|Smooth filtering 2|Smooth filtering 3|Smooth filtering 4|Sharp filtering 1|Sharp filtering 2" },
        { "LudicrousN64-txEnhancementMode",
            "Texture Enhancement; None|As Is|X2|X2SAI|HQ2X|HQ2XS|LQ2X|LQ2XS|HQ4X|2xBRZ|3xBRZ|4xBRZ|5xBRZ|6xBRZ" },
        { "LudicrousN64-txFilterIgnoreBG",
            "Filter background textures; True|False" },
        { "LudicrousN64-txHiresEnable",
            "Use High-Res textures; False|True" },
        { "LudicrousN64-txHiresFullAlphaChannel",
            "Use High-Res Full Alpha Channel; False|True" },
        {"LudicrousN64-astick-deadzone",
           "Analog Deadzone (percent); 15|20|25|30|0|5|10"},
        {"LudicrousN64-r-cbutton",
           "Right C Button; C1|C2|C3|C4"},
        {"LudicrousN64-l-cbutton",
           "Left C Button; C2|C3|C4|C1"},
        {"LudicrousN64-d-cbutton",
           "Down C Button; C3|C4|C1|C2"},
        {"LudicrousN64-u-cbutton",
           "Up C Button; C4|C1|C2|C3"},
        {"LudicrousN64-ForceDisableExtraMem",
           "Disable Expansion Pak; False|True"},
        {"LudicrousN64-pak1",
           "Player 1 Pak; memory|rumble|none"},
        {"LudicrousN64-pak2",
           "Player 2 Pak; none|memory|rumble"},
        {"LudicrousN64-pak3",
           "Player 3 Pak; none|memory|rumble"},
        {"LudicrousN64-pak4",
           "Player 4 Pak; none|memory|rumble"},
        { NULL, NULL },
    };

    static const struct retro_controller_description port[] = {
        { "Controller", RETRO_DEVICE_JOYPAD },
        { "RetroPad", RETRO_DEVICE_JOYPAD },
    };

    static const struct retro_controller_info ports[] = {
        { port, 2 },
        { port, 2 },
        { port, 2 },
        { port, 2 },
        { 0, 0 }
    };

    environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}


static bool emu_step_load_data()
{
    if(CoreStartup(FRONTEND_API_VERSION, ".", ".", "Core", n64DebugCallback, 0, 0) && log_cb)
        log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to initialize core\n");

    log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_ROM_OPEN\n");

    if(CoreDoCommand(M64CMD_ROM_OPEN, game_size, (void*)game_data))
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "mupen64plus: Failed to load ROM\n");
        goto load_fail;
    }

    free(game_data);
    game_data = NULL;

    log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_ROM_GET_HEADER\n");

    if(CoreDoCommand(M64CMD_ROM_GET_HEADER, sizeof(ROM_HEADER), &ROM_HEADER))
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "mupen64plus; Failed to query ROM header information\n");
        goto load_fail;
    }

    return true;

load_fail:
    free(game_data);
    game_data = NULL;
    stop = 1;

    return false;
}

static void emu_step_initialize(void)
{
    if (emu_initialized)
        return;

    emu_initialized = true;

    plugin_connect_all();
}

static void EmuThreadFunction(void)
{
    log_cb(RETRO_LOG_INFO, "EmuThread: M64CMD_EXECUTE. \n");

    initializing = false;
    CoreDoCommand(M64CMD_EXECUTE, 0, NULL);
}

void reinit_gfx_plugin(void)
{
    if(first_context_reset)
    {
        first_context_reset = false;
        emu_step_initialize();
    }
}

const char* retro_get_system_directory(void)
{
    const char* dir;
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir);

    return dir ? dir : ".";
}


void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)   { }
static retro_audio_sample_batch_t audio_batch_cb_real = NULL;
static size_t audio_batch_wrapper(const int16_t *data, size_t frames)
{
    if (TurboBoost_NoSync)
        return frames; /* drop audio in EX (including EX0) to avoid audio sync pacing */
    return audio_batch_cb_real ? audio_batch_cb_real(data, frames) : frames;
}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb_real = cb; audio_batch_cb = audio_batch_wrapper; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }


void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;

    setup_variables();
}

void retro_get_system_info(struct retro_system_info *info)
{
#if defined(HAVE_OPENGLES2)
    info->library_name = "LudicrousN64 2K22 Xtreme Amped";
#elif defined(HAVE_OPENGLES3)
    info->library_name = "LudicrousN64 2K22 Xtreme Amped";
#else
    info->library_name = "LudicrousN64 2K22 Xtreme Amped";
#endif
#ifndef GIT_VERSION
#define GIT_VERSION
#endif
    info->library_version = "2K22" GIT_VERSION;
    info->valid_extensions = "n64|v64|z64|bin|u1|ndd";
    info->need_fullpath = false;
    info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    info->geometry.base_width   = retro_screen_width;
    info->geometry.base_height  = retro_screen_height;
    info->geometry.max_width    = retro_screen_width;
    info->geometry.max_height   = retro_screen_height;
    info->geometry.aspect_ratio = retro_screen_aspect;
    info->timing.fps = vi_expected_refresh_rate_from_tv_standard(ROM_PARAMS.systemtype);
    info->timing.sample_rate = 44100.0;
}

unsigned retro_get_region (void)
{
    return ((ROM_PARAMS.systemtype == SYSTEM_PAL) ? RETRO_REGION_PAL : RETRO_REGION_NTSC);
}

void copy_file(char * ininame, char * fileName)
{
    const char* filename = ConfigGetSharedDataFilepath(fileName);
    FILE *fp = fopen(filename, "w");
    if (fp != NULL)    {
        fputs(ininame, fp);
        fclose(fp);
    }
}

void retro_init(void)
{
#ifdef HAVE_LIBNX
    detectIgnoreJitKernelPatch();
#endif
    char* sys_pathname;
    wchar_t w_pathname[PATH_SIZE];
    environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys_pathname);
    char pathname[PATH_SIZE];
    strncpy(pathname, sys_pathname, PATH_SIZE);
    if (pathname[(strlen(pathname)-1)] != '/' && pathname[(strlen(pathname)-1)] != '\\')
        strcat(pathname, "/");
    strcat(pathname, "LudicrousN64/");
    mbstowcs(w_pathname, pathname, PATH_SIZE);
    if (!osal_path_existsW(w_pathname) || !osal_is_directory(w_pathname))
        osal_mkdirp(w_pathname);
    copy_file(inifile, "mupen64plus.ini");

    struct retro_log_callback log;
    unsigned colorMode = RETRO_PIXEL_FORMAT_XRGB8888;

    if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;
    else
        log_cb = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
        perf_get_cpu_features_cb = perf_cb.get_cpu_features;
    else
        perf_get_cpu_features_cb = NULL;

    environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &colorMode);
    environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);
    initializing = true;

    retro_thread = co_active();
    game_thread = co_create(65536 * sizeof(void*) * 16, EmuThreadFunction);
}

void retro_deinit(void)
{
    CoreDoCommand(M64CMD_STOP, 0, NULL);
    deinit_audio_libretro();

    if (perf_cb.perf_log)
        perf_cb.perf_log();
}

void update_controllers()
{
    struct retro_variable pk1var = { "LudicrousN64-pak1" };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk1var) && pk1var.value)
    {
        int p1_pak = PLUGIN_NONE;
        if (!strcmp(pk1var.value, "rumble"))
            p1_pak = PLUGIN_RAW;
        else if (!strcmp(pk1var.value, "memory"))
            p1_pak = PLUGIN_MEMPAK;

        // If controller struct is not initialised yet, set pad_pak_types instead
        // which will be looked at when initialising the controllers.
        if (controller[0].control)
            controller[0].control->Plugin = p1_pak;
        else
            pad_pak_types[0] = p1_pak;
    }

    struct retro_variable pk2var = { "LudicrousN64-pak2" };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk2var) && pk2var.value)
    {
        int p2_pak = PLUGIN_NONE;
        if (!strcmp(pk2var.value, "rumble"))
            p2_pak = PLUGIN_RAW;
        else if (!strcmp(pk2var.value, "memory"))
            p2_pak = PLUGIN_MEMPAK;

        if (controller[1].control)
            controller[1].control->Plugin = p2_pak;
        else
            pad_pak_types[1] = p2_pak;
    }

    struct retro_variable pk3var = { "LudicrousN64-pak3" };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk3var) && pk3var.value)
    {
        int p3_pak = PLUGIN_NONE;
        if (!strcmp(pk3var.value, "rumble"))
            p3_pak = PLUGIN_RAW;
        else if (!strcmp(pk3var.value, "memory"))
            p3_pak = PLUGIN_MEMPAK;

        if (controller[2].control)
            controller[2].control->Plugin = p3_pak;
        else
            pad_pak_types[2] = p3_pak;
    }

    struct retro_variable pk4var = { "LudicrousN64-pak4" };
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &pk4var) && pk4var.value)
    {
        int p4_pak = PLUGIN_NONE;
        if (!strcmp(pk4var.value, "rumble"))
            p4_pak = PLUGIN_RAW;
        else if (!strcmp(pk4var.value, "memory"))
            p4_pak = PLUGIN_MEMPAK;

        if (controller[3].control)
            controller[3].control->Plugin = p4_pak;
        else
            pad_pak_types[3] = p4_pak;
    }
}

void update_variables()
{
    struct retro_variable var;

    var.key = "LudicrousN64-rspmode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "HLE"))
            rspMode = 0;
        else
            rspMode = 1;
    }

    var.key = "LudicrousN64-BilinearMode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "3point"))
            bilinearMode = 0;
        else
            bilinearMode = 1;
    }

    var.key = "LudicrousN64-MultiSampling";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        MultiSampling = atoi(var.value);
    }

    var.key = "LudicrousN64-EnableFBEmulation";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "False"))
            EnableFBEmulation = 0;
        else
            EnableFBEmulation = 1;
    }

    var.key = "LudicrousN64-EnableCopyColorToRDRAM";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "Async"))
            EnableCopyColorToRDRAM = 2;
        else if (!strcmp(var.value, "Sync"))
            EnableCopyColorToRDRAM = 1;
        else
            EnableCopyColorToRDRAM = 0;
    }

    var.key = "LudicrousN64-EnableCopyDepthToRDRAM";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "Software"))
            EnableCopyDepthToRDRAM = 2;
        else if (!strcmp(var.value, "FromMem"))
            EnableCopyDepthToRDRAM = 1;
        else
            EnableCopyDepthToRDRAM = 0;
    }

    var.key = "LudicrousN64-EnableCopyAuxToRDRAM";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
       EnableCopyAuxToRDRAM = !strcmp(var.value, "False") ? 0 : 1;
    }

    var.key = "LudicrousN64-BackgroundMode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
      /* 0 = One piece, 1 = Stripped */
      BGMode = (strcmp(var.value, "One piece") == 0) ? 0u : 1u;
    }

    var.key = "LudicrousN64-EnableHWLighting";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            EnableHWLighting = 1;
        else
            EnableHWLighting = 0;
    }

    var.key = "LudicrousN64-CorrectTexrectCoords";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "Force"))
            CorrectTexrectCoords = 2;
        else if (!strcmp(var.value, "Auto"))
            CorrectTexrectCoords = 1;
        else
            CorrectTexrectCoords = 0;
    }

    var.key = "LudicrousN64-EnableNativeResTexrects";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            enableNativeResTexrects = 1;
        else
            enableNativeResTexrects = 0;
    }

    var.key = "LudicrousN64-txFilterMode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "Smooth filtering 1"))
            txFilterMode = 1;
        else if (!strcmp(var.value, "Smooth filtering 2"))
            txFilterMode = 2;
        else if (!strcmp(var.value, "Smooth filtering 3"))
            txFilterMode = 3;
        else if (!strcmp(var.value, "Smooth filtering 4"))
            txFilterMode = 4;
        else if (!strcmp(var.value, "Sharp filtering 1"))
            txFilterMode = 5;
        else if (!strcmp(var.value, "Sharp filtering 2"))
            txFilterMode = 6;
        else
            txFilterMode = 0;
    }

    var.key = "LudicrousN64-txEnhancementMode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "As Is"))
            txEnhancementMode = 1;
        else if (!strcmp(var.value, "X2"))
            txEnhancementMode = 2;
        else if (!strcmp(var.value, "X2SAI"))
            txEnhancementMode = 3;
        else if (!strcmp(var.value, "HQ2X"))
            txEnhancementMode = 4;
        else if (!strcmp(var.value, "HQ2XS"))
            txEnhancementMode = 5;
        else if (!strcmp(var.value, "LQ2X"))
            txEnhancementMode = 6;
        else if (!strcmp(var.value, "LQ2XS"))
            txEnhancementMode = 7;
        else if (!strcmp(var.value, "HQ4X"))
            txEnhancementMode = 8;
        else if (!strcmp(var.value, "2xBRZ"))
            txEnhancementMode = 9;
        else if (!strcmp(var.value, "3xBRZ"))
            txEnhancementMode = 10;
        else if (!strcmp(var.value, "4xBRZ"))
            txEnhancementMode = 11;
        else if (!strcmp(var.value, "5xBRZ"))
            txEnhancementMode = 12;
        else if (!strcmp(var.value, "6xBRZ"))
            txEnhancementMode = 13;
        else
            txEnhancementMode = 0;
    }

    var.key = "LudicrousN64-txFilterIgnoreBG";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            txFilterIgnoreBG = 0;
        else
            txFilterIgnoreBG = 1;
    }

    var.key = "LudicrousN64-txHiresEnable";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            txHiresEnable = 1;
        else
            txHiresEnable = 0;
    }

    var.key = "LudicrousN64-txHiresFullAlphaChannel";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            txHiresFullAlphaChannel = 1;
        else
            txHiresFullAlphaChannel = 0;
    }

    var.key = "LudicrousN64-EnableLegacyBlending";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            enableLegacyBlending = 1;
        else
            enableLegacyBlending = 0;
    }

    var.key = "LudicrousN64-EnableFragmentDepthWrite";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            EnableFragmentDepthWrite = 1;
        else
            EnableFragmentDepthWrite = 0;
    }

    var.key = "LudicrousN64-EnableShadersStorage";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "True"))
            EnableShadersStorage = 1;
        else
            EnableShadersStorage = 0;
    }

    var.key = "mupen64plus-CropMode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "Auto"))
            CropMode = 1;
        else
            CropMode = 0;
    }

    var.key = "LudicrousN64-cpucore";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "pure_interpreter"))
             r4300emu = 0;
        else if (!strcmp(var.value, "cached_interpreter"))
             r4300emu = 1;
        else if (!strcmp(var.value, "dynamic_recompiler"))
             r4300emu = 2;
    }

    var.key = "LudicrousN64-aspect";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "16:9 adjusted")) {
             AspectRatio = 3;
             retro_screen_aspect = 16.0 / 9.0;
    av_info_dirty = true;
        } else if (!strcmp(var.value, "16:9")) {
             AspectRatio = 2;
             retro_screen_aspect = 16.0 / 9.0;
    av_info_dirty = true;
        } else {
             AspectRatio = 1;
             retro_screen_aspect = 4.0 / 3.0;
    av_info_dirty = true;
        }
    }

    if (AspectRatio == 1)
        var.key = "LudicrousN64-43screensize";
    else
        var.key = "LudicrousN64-169screensize";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        sscanf(var.value, "%dx%d", &retro_screen_width, &retro_screen_height);
    }

    var.key = "LudicrousN64-astick-deadzone";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        astick_deadzone = (int)(atoi(var.value) * 0.01f * 0x8000);

    var.key = "LudicrousN64-astick-sensitivity";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        astick_sensitivity = atoi(var.value);

    var.key = "LudicrousN64-LangoliersPurge";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "Off")) {
            xt_purge_interval_vi = 0;
        } else if (!strncmp(var.value, "Always", 6)) {
            /* Aggressive: purge every VI tick (≈ every frame at 60 Hz). */
            xt_purge_interval_vi = 1;
        } else {
            /* Expect:
             *   "<number> ms ..."
             *   "<number> sec ..."
             *   "<number>.<number> sec ..."
             * Default unit is seconds when not specified.
             */
            const char *s = var.value;
            char *end = NULL;
            double v = strtod(s, &end);

            if (v > 0.0) {
                while (end && (*end == ' ' || *end == '	'))
                    end++;

                if (end && ((end[0] == 'm' || end[0] == 'M') && (end[1] == 's' || end[1] == 'S'))) {
                    xt_purge_interval_vi = xt_seconds_to_vi(v / 1000.0);
                } else {
                    xt_purge_interval_vi = xt_seconds_to_vi(v);
                }

                /* If the computed interval is non-zero but somehow rounds to 0, clamp to 1 tick. */
                if (!xt_purge_interval_vi)
                    xt_purge_interval_vi = 1;
            } else {
                /* Fallback default if parsing fails */
                xt_purge_interval_vi = xt_seconds_to_vi(60.0);
            }
        }
    }

    
    var.key = "LudicrousN64-LangoliersPress";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        xt_purge_button_id = -1;

        if (!strcmp(var.value, "A"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_A;
        else if (!strcmp(var.value, "B"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_B;
        else if (!strcmp(var.value, "X"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_X;
        else if (!strcmp(var.value, "Y"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_Y;
        else if (!strcmp(var.value, "L"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_L;
        else if (!strcmp(var.value, "R"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_R;
        else if (!strcmp(var.value, "L2"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_L2;
        else if (!strcmp(var.value, "R2"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_R2;
        else if (!strcmp(var.value, "L3"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_L3;
        else if (!strcmp(var.value, "R3"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_R3;
        else if (!strcmp(var.value, "Select"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_SELECT;
        else if (!strcmp(var.value, "Start"))
            xt_purge_button_id = RETRO_DEVICE_ID_JOYPAD_START;

        /* Reset edge detector when option changes */
        xt_purge_button_last = 0;
    }

var.key = "LudicrousN64-virefresh";
    av_info_dirty = true;
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "Auto"))
            CountPerScanlineOverride = 0;
        else
            CountPerScanlineOverride = atoi(var.value);
    }

    var.key = "LudicrousN64-TurboBoost";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        parse_turboboost_value(var.value);
    }

    var.key = "LudicrousN64-CountPerOp";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
    if (!strcmp(var.value, "XX"))
       CountPerOp = 10;
    else if (!strcmp(var.value, "X9"))
       CountPerOp = 9;
    else if (!strcmp(var.value, "X8"))
       CountPerOp = 8;
    else if (!strcmp(var.value, "X7"))
       CountPerOp = 7;	 
    else if (!strcmp(var.value, "X6"))
       CountPerOp = 6;
    else if (!strcmp(var.value, "X5"))
       CountPerOp = 5;
    else if (!strcmp(var.value, "X4"))
       CountPerOp = 4;
    else if (!strcmp(var.value, "X3"))
       CountPerOp = 3;
    else if (!strcmp(var.value, "X2"))
       CountPerOp = 2;
    else if (!strcmp(var.value, "X1"))
       CountPerOp = 1; 
    else
       CountPerOp = 0;
    }

   var.key = "LudicrousN64-GLideN64IniBehaviour";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
       if (!strcmp(var.value, "ini_config_priority"))
          GLideN64IniBehaviour = 0;
       else if (!strcmp(var.value, "core_options_priority"))
          GLideN64IniBehaviour = 1;
       else if (!strcmp(var.value, "disabled"))
          GLideN64IniBehaviour = -1;
    }

var.key = "LudicrousN64-xtreme_fog";
var.value = NULL;
if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
{
	    /* The label includes hints; we only care about the numeric scale or Off.
	     * Parse the numeric prefix so extending the option list doesn't require
	     * hard-coding every value.
	     */
	    if (!strcmp(var.value, "Off (Disabled)"))
	    {
	       xt_fog_scale = 0.0f;
	    }
	    else
	    {
	       float scale = 1.0f;
	       if (sscanf(var.value, "%fx", &scale) == 1 && scale > 0.0f)
	          xt_fog_scale = scale;
	       else
	          xt_fog_scale = 1.00f;
	    }
}

    
var.key = "LudicrousN64-xtreme_fog_color";
var.value = NULL;
if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
{
    if (!strcmp(var.value, "Default"))       xt_fog_color_mode = 0;
    else if (!strcmp(var.value, "Purple"))  xt_fog_color_mode = 1;
    else if (!strcmp(var.value, "Red"))     xt_fog_color_mode = 2;
    else if (!strcmp(var.value, "Green"))   xt_fog_color_mode = 3;
    else if (!strcmp(var.value, "Blue"))    xt_fog_color_mode = 4;
    else if (!strcmp(var.value, "Yellow"))  xt_fog_color_mode = 5;
    else if (!strcmp(var.value, "Cyan"))    xt_fog_color_mode = 6;
    else if (!strcmp(var.value, "Magenta")) xt_fog_color_mode = 7;
    else if (!strcmp(var.value, "Orange"))  xt_fog_color_mode = 8;
    else if (!strcmp(var.value, "Pink"))    xt_fog_color_mode = 9;
    else if (!strcmp(var.value, "Black"))   xt_fog_color_mode = 10;
    else if (!strcmp(var.value, "White"))   xt_fog_color_mode = 11;
    else xt_fog_color_mode = 0;
}

var.key = "LudicrousN64-r-cbutton";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "C1"))
            r_cbutton = RETRO_DEVICE_ID_JOYPAD_A;
        else if (!strcmp(var.value, "C2"))
            r_cbutton = RETRO_DEVICE_ID_JOYPAD_Y;
        else if (!strcmp(var.value, "C3"))
            r_cbutton = RETRO_DEVICE_ID_JOYPAD_B;
        else if (!strcmp(var.value, "C4"))
            r_cbutton = RETRO_DEVICE_ID_JOYPAD_X;
    }

    var.key = "LudicrousN64-l-cbutton";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "C1"))
            l_cbutton = RETRO_DEVICE_ID_JOYPAD_A;
        else if (!strcmp(var.value, "C2"))
            l_cbutton = RETRO_DEVICE_ID_JOYPAD_Y;
        else if (!strcmp(var.value, "C3"))
            l_cbutton = RETRO_DEVICE_ID_JOYPAD_B;
        else if (!strcmp(var.value, "C4"))
            l_cbutton = RETRO_DEVICE_ID_JOYPAD_X;
    }

    var.key = "LudicrousN64-d-cbutton";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "C1"))
            d_cbutton = RETRO_DEVICE_ID_JOYPAD_A;
        else if (!strcmp(var.value, "C2"))
            d_cbutton = RETRO_DEVICE_ID_JOYPAD_Y;
        else if (!strcmp(var.value, "C3"))
            d_cbutton = RETRO_DEVICE_ID_JOYPAD_B;
        else if (!strcmp(var.value, "C4"))
            d_cbutton = RETRO_DEVICE_ID_JOYPAD_X;
    }

    var.key = "LudicrousN64-u-cbutton";
    var.value = NULL;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "C1"))
            u_cbutton = RETRO_DEVICE_ID_JOYPAD_A;
        else if (!strcmp(var.value, "C2"))
            u_cbutton = RETRO_DEVICE_ID_JOYPAD_Y;
        else if (!strcmp(var.value, "C3"))
            u_cbutton = RETRO_DEVICE_ID_JOYPAD_B;
        else if (!strcmp(var.value, "C4"))
            u_cbutton = RETRO_DEVICE_ID_JOYPAD_X;
    }

    var.key = "LudicrousN64-DPadMode";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "P1 D-Pad as Analog"))
            dpad_as_analog_p1 = 1;
        else
            dpad_as_analog_p1 = 0;
    }

    var.key = "LudicrousN64-ForceDisableExtraMem";
    var.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        ForceDisableExtraMem = !strcmp(var.value, "False") ? 0 : 1;
    }

    update_controllers();
}

static void format_saved_memory(void)
{
    format_sram(saved_memory.sram);
    format_eeprom(saved_memory.eeprom, EEPROM_MAX_SIZE);
    format_flashram(saved_memory.flashram);
    format_mempak(saved_memory.mempack + 0 * MEMPAK_SIZE);
    format_mempak(saved_memory.mempack + 1 * MEMPAK_SIZE);
    format_mempak(saved_memory.mempack + 2 * MEMPAK_SIZE);
    format_mempak(saved_memory.mempack + 3 * MEMPAK_SIZE);
}

static void context_reset(void)
{
    static bool first_init = true;
    printf("context_reset.\n");
    glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);

    if (first_init)
    {
        glsm_ctl(GLSM_CTL_STATE_SETUP, NULL);
        first_init = false;
    }

    reinit_gfx_plugin();
}

static void context_destroy(void)
{
    glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);
}

static bool context_framebuffer_lock(void *data)
{
    if (!stop)
        return false;
    return true;
}

bool retro_load_game(const struct retro_game_info *game)
{
    glsm_ctx_params_t params = {0};
    format_saved_memory();

    update_variables();
    initial_boot = false;

    init_audio_libretro(audio_buffer_size);

    params.context_reset         = context_reset;
    params.context_destroy       = context_destroy;
    params.environ_cb            = environ_cb;
    params.stencil               = false;

    params.framebuffer_lock      = context_framebuffer_lock;

    if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
    {
        if (log_cb)
            log_cb(RETRO_LOG_ERROR, "mupen64plus: libretro frontend doesn't have OpenGL support.");
        return false;
    }

    game_data = malloc(game->size);
    memcpy(game_data, game->data, game->size);
    game_size = game->size;

    if (!emu_step_load_data())
        return false;

    first_context_reset = true;

    return true;
}

#ifdef HAVE_LIBNX
extern Jit dynarec_jit;
extern void *jit_rw_buffer;
extern void *jit_old_addr;
#endif
void retro_unload_game(void)
{
#if defined(HAVE_LIBNX) && defined(DYNAREC)
    jitTransitionToWritable(&dynarec_jit);
    if(jit_old_addr != 0)
        dynarec_jit.rx_addr = jit_old_addr;
    jit_old_addr = 0;
    jitClose(&dynarec_jit);

    if(jit_rw_buffer != 0)
        free(jit_rw_buffer);

    jit_rw_buffer = 0;
#endif

    CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
    emu_initialized = false;
}

void retro_run (void)
{
    libretro_swap_buffer = false;
    static bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        update_variables();
    xt_push_av_info_if_needed();
    glsm_ctl(GLSM_CTL_STATE_BIND, NULL);
    /* Xtreme MF Langoliers Press: one-shot VRAM purge on button edge (P1). */
    if (xt_purge_button_id >= 0 && poll_cb && input_cb)
    {
        poll_cb();
        int16_t pressed = input_cb(0, RETRO_DEVICE_JOYPAD, 0, xt_purge_button_id);

        if (pressed && !xt_purge_button_last)
        {
            xt_vram_purge_soft();
            if (log_cb)
                log_cb(RETRO_LOG_INFO, "[xtreme] Langoliers Press: one-shot purge fired.\n");
        }

        xt_purge_button_last = pressed ? 1 : 0;
    }

    co_switch(game_thread);
    glsm_ctl(GLSM_CTL_STATE_UNBIND, NULL);
    if (libretro_swap_buffer)
        video_cb(RETRO_HW_FRAME_BUFFER_VALID, retro_screen_width, retro_screen_height, 0);
}

void retro_reset (void)
{
    CoreDoCommand(M64CMD_RESET, 0, (void*)0);
}

void *retro_get_memory_data(unsigned type)
{
    switch (type)
    {
        case RETRO_MEMORY_SYSTEM_RAM: return g_rdram;
        case RETRO_MEMORY_SAVE_RAM:   return &saved_memory;
    }
    
    return NULL;
}

size_t retro_get_memory_size(unsigned type)
{
    switch (type)
    {
        case RETRO_MEMORY_SYSTEM_RAM: return RDRAM_MAX_SIZE;
        case RETRO_MEMORY_SAVE_RAM:   return sizeof(saved_memory);
    }
    
    return 0;
}

size_t retro_serialize_size (void)
{
    return 16788288 + 1024 + 4; // < 16MB and some change... ouch
}

bool retro_serialize(void *data, size_t size)
{
    if (initializing)
        return false;

    int success = savestates_save_m64p(data);
    if (success)
        return true;

    return false;
}

bool retro_unserialize(const void * data, size_t size)
{
    if (initializing)
        return false;

    int success = savestates_load_m64p(data);
    if (success)
        return true;

    return false;
}

//Needed to be able to detach controllers for Lylat Wars multiplayer
//Only sets if controller struct is initialised as addon paks do.
void retro_set_controller_port_device(unsigned in_port, unsigned device) {
    if (in_port < 4){
        switch(device)
        {
            case RETRO_DEVICE_NONE:
                if (controller[in_port].control){
                    controller[in_port].control->Present = 0;
                    break;
                } else {
                    pad_present[in_port] = 0;
                    break;
                }

            case RETRO_DEVICE_JOYPAD:
            default:
                if (controller[in_port].control){
                    controller[in_port].control->Present = 1;
                    break;
                } else {
                    pad_present[in_port] = 1;
                    break;
                }
        }
    }
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) { return false; }

void retro_cheat_reset(void)
{
    cheat_delete_all(&g_cheat_ctx);
}

void retro_cheat_set(unsigned index, bool enabled, const char* codeLine)
{
    char name[256];
    m64p_cheat_code mupenCode[256];
    uint32_t codeParts[256];
    int partCount = 0;
    int codeCount = 0;

    /* Generate a stable name per index (RetroArch may call retro_cheat_set() repeatedly). */
    snprintf(name, sizeof(name), "cheat_%u", index);

    /* Parse any consecutive hex runs in the incoming cheat line.
     *
     * RetroArch cheat lines are typically formatted like:
     *   AAAAAAAA BBBB
     * but may include separators, prefixes, or multiple code pairs.
     *
     * NOTE: The previous implementation used a VLA sized to matchLength
     * and then wrote the NUL terminator out-of-bounds (UB). This parser
     * avoids that and is bounded.
     */
    {
        char token[9]; /* up to 8 hex digits + NUL */
        int token_len = 0;
        int cursor;
        for (cursor = 0; ; cursor++)
        {
            const char c = codeLine[cursor];
            const int is_hex = ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));

            if (is_hex)
            {
                /* If a run is longer than 8 hex chars, split into 8-char chunks. */
                if (token_len == 8)
                {
                    token[token_len] = '\0';
                    if (partCount < (int)(sizeof(codeParts) / sizeof(codeParts[0])))
                        codeParts[partCount++] = (uint32_t) strtoul(token, NULL, 16);
                    token_len = 0;
                }
                token[token_len++] = c;
            }
            else
            {
                if (token_len > 0)
                {
                    token[token_len] = '\0';
                    if (partCount < (int)(sizeof(codeParts) / sizeof(codeParts[0])))
                        codeParts[partCount++] = (uint32_t) strtoul(token, NULL, 16);
                    token_len = 0;
                }

                if (c == '\0')
                    break;
            }
        }
    }

    /* We need address/value pairs. Drop a dangling odd part if present. */
    if (partCount < 2)
        return;
    if (partCount & 1)
        partCount--;

    /* Assign the parts to mupenCode */
    for (codeCount = 0; (2 * codeCount + 1) < partCount && codeCount < (int)(sizeof(mupenCode) / sizeof(mupenCode[0])); codeCount++)
    {
        mupenCode[codeCount].address = codeParts[2 * codeCount];
        mupenCode[codeCount].value   = codeParts[2 * codeCount + 1];
    }

    cheat_add_new(&g_cheat_ctx, name, mupenCode, codeCount);
    cheat_set_enabled(&g_cheat_ctx, name, enabled);
}

void retro_return(void)
{
    co_switch(retro_thread);
}

uint32_t get_retro_screen_width()
{
    return retro_screen_width;
}

uint32_t get_retro_screen_height()
{
    return retro_screen_height;
}

static int GamesharkActive = 0;

int event_gameshark_active(void)
{
    return GamesharkActive;
}

void event_set_gameshark(int active)
{
    // if boolean value doesn't change then just return
    if (!active == !GamesharkActive)
        return;

    // set the button state
    GamesharkActive = (active ? 1 : 0);

    // notify front-end application that gameshark button state has changed
    StateChanged(M64CORE_INPUT_GAMESHARK, GamesharkActive);
}