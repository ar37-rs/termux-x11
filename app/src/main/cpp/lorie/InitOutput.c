/*

Copyright 1993, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall
not be used in advertising or otherwise to promote the sale, use or
other dealings in this Software without prior written authorization
from The Open Group.

*/

#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#pragma ide diagnostic ignored "cppcoreguidelines-narrowing-conversions"
#pragma ide diagnostic ignored "cert-err34-c"
#pragma ide diagnostic ignored "ConstantConditionsOC"
#pragma ide diagnostic ignored "ConstantFunctionResult"
#pragma ide diagnostic ignored "bugprone-integer-division"
#pragma clang diagnostic ignored "-Wmissing-noreturn"
#pragma clang diagnostic ignored "-Wformat-nonliteral"

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/errno.h>
#include <libxcvt/libxcvt.h>
#include <X11/X.h>
#include <X11/Xos.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/hardware_buffer.h>
#include <sys/wait.h>
#include <selection.h>
#include <X11/Xatom.h>
#include <present.h>
#include <present_priv.h>
#include <dri3.h>
#include <sys/mman.h>
#include <busfault.h>
#include <linux/ashmem.h>
#include "scrnintstr.h"
#include "servermd.h"
#include "fb.h"
#include "input.h"
#include "mipointer.h"
#include "micmap.h"
#include "dix.h"
#include "miline.h"
#include "glx_extinit.h"
#include "randrstr.h"
#include "damagestr.h"
#include "cursorstr.h"
#include "propertyst.h"
#include "shmint.h"
#include "misyncshm.h"
#include "glxserver.h"
#include "glxutil.h"
#include "fbconfigs.h"

#include "renderer.h"
#include "inpututils.h"
#include "lorie.h"

#define unused __attribute__((unused))
#define wrap(priv, real, mem, func) { priv->mem = real->mem; real->mem = func; }
#define unwrap(priv, real, mem) { real->mem = priv->mem; }
#define USAGE (AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN)
#define log(prio, ...) __android_log_print(ANDROID_LOG_ ## prio, "LorieNative", __VA_ARGS__)

extern DeviceIntPtr lorieMouse, lorieKeyboard;

#define lorieGCPriv(pGC) LorieGCPrivPtr pGCPriv = dixLookupPrivate(&(pGC)->devPrivates, &lorieGCPrivateKey)
static DevPrivateKeyRec lorieGCPrivateKey;
typedef struct {
    const GCOps *ops;
    const GCFuncs *funcs;
} LorieGCPrivRec, *LorieGCPrivPtr;
static Bool lorieCreateGC(GCPtr pGC);

struct vblank {
    struct xorg_list list;
    uint64_t id, msc;
};

static struct present_screen_info loriePresentInfo;

typedef struct {
    bool ready;
    CreateGCProcPtr CreateGC;
    CloseScreenProcPtr CloseScreen;
    CreateScreenResourcesProcPtr CreateScreenResources;

    DamagePtr damage;
    OsTimerPtr redrawTimer;
    OsTimerPtr fpsTimer;

    int eventFd, stateFd;

    struct lorie_shared_server_state* state;
    struct {
        AHardwareBuffer* buffer;
        Bool locked;
        Bool legacyDrawing;
        uint8_t flip;
        uint32_t width, height;
        char name[1024];
        uint32_t framerate;
    } root;

    JavaVM* vm;
    JNIEnv* env;
    Bool dri3;

    uint64_t vblank_interval;
    struct xorg_list vblank_queue;
    uint64_t current_msc;
} lorieScreenInfo;

ScreenPtr pScreenPtr;
static lorieScreenInfo lorieScreen = {
        .root.width = 1050,
        .root.height = 1024
        .root.framerate = 60,
        .root.name = "screen",
        .dri3 = TRUE,
        .vblank_queue = { &lorieScreen.vblank_queue, &lorieScreen.vblank_queue },
}, *pvfb = &lorieScreen;
static char *xstartup = NULL;

#pragma clang diagnostic push
#pragma ide diagnostic ignored "UnreachableCallsOfFunction"
static int create_shmem_region(char const* name, size_t size)
{
    int fd = memfd_create("Xlorie", MFD_CLOEXEC|MFD_ALLOW_SEALING);
    if (fd) {
        ftruncate (fd, size);
        return fd;
    }

    fd = open("/dev/ashmem", O_RDWR);
    if (fd < 0)
        return fd;

    char name_buffer[ASHMEM_NAME_LEN] = {0};
    strncpy(name_buffer, name, sizeof(name_buffer));
    name_buffer[sizeof(name_buffer)-1] = 0;

    int ret = ioctl(fd, ASHMEM_SET_NAME, name_buffer);
    if (ret < 0) goto error;

    ret = ioctl(fd, ASHMEM_SET_SIZE, size);
    if (ret < 0) goto error;

    return fd;
    error:
    close(fd);
    return ret;
}
#pragma clang diagnostic pop

__attribute((constructor())) static void initSharedServerState() {
    pthread_mutexattr_t mutex_attr;
    pthread_condattr_t cond_attr;
    if (-1 == (lorieScreen.stateFd = create_shmem_region("xserver", sizeof(*lorieScreen.state)))) {
        dprintf(2, "FATAL: Failed to allocate server state.\n");
        _exit(1);
    }

    if (!(lorieScreen.state = mmap(NULL, sizeof(*lorieScreen.state), PROT_READ|PROT_WRITE, MAP_SHARED, lorieScreen.stateFd, 0))) {
        dprintf(2, "FATAL: Failed to map server state.\n");
        _exit(1);
    }

    pthread_mutexattr_init(&mutex_attr);
    pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&lorieScreen.state->lock, &mutex_attr);
    pthread_mutex_init(&lorieScreen.state->cursor.lock, &mutex_attr);

    pthread_condattr_init(&cond_attr);
    pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&lorieScreen.state->cond, &cond_attr);
    renderer_set_shared_state(lorieScreen.state);
}

void lorieActivityConnected(void) {
    lorieSendSharedServerState(pvfb->stateFd);
    lorieSendRootWindowBuffer(pvfb->root.buffer);
}

static Bool TrueNoop() { return TRUE; }
static Bool FalseNoop() { return FALSE; }
static void VoidNoop() {}

void
ddxGiveUp(unused enum ExitCode error) {
    log(ERROR, "Server stopped (%d)", error);
    CloseWellKnownConnections();
    UnlockServer();
    exit(error);
}

static void* ddxReadyThread(unused void* cookie) {
    if (xstartup && serverGeneration == 1) {
        pid_t pid = fork();

        if (!pid) {
            char DISPLAY[16] = "";
            sprintf(DISPLAY, ":%s", display);
            setenv("DISPLAY", DISPLAY, 1);

#define INHERIT_VAR(v) char *v = getenv("XSTARTUP_" #v); if (v && strlen(v)) setenv(#v, v, 1); unsetenv("XSTARTUP_" #v);
            INHERIT_VAR(CLASSPATH)
            INHERIT_VAR(LD_LIBRARY_PATH)
            INHERIT_VAR(LD_PRELOAD)
#undef INHERIT_VAR

            execlp(xstartup, xstartup, NULL);
            execlp("sh", "sh", "-c", xstartup, NULL);
            dprintf(2, "Failed to start command `sh -c \"%s\"`: %s\n", xstartup, strerror(errno));
            abort();
        } else {
            int status;
            do {
                pid_t w = waitpid(pid, &status, 0);
                if (w == -1) {
                    perror("waitpid");
                    GiveUp(SIGKILL);
                }

                if (WIFEXITED(status)) {
                    printf("%d exited, status=%d\n", w, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                    printf("%d killed by signal %d\n", w, WTERMSIG(status));
                } else if (WIFSTOPPED(status)) {
                    printf("%d stopped by signal %d\n", w, WSTOPSIG(status));
                } else if (WIFCONTINUED(status)) {
                    printf("%d continued\n", w);
                }
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));
            GiveUp(SIGINT);
        }
    }

    return NULL;
}

void
ddxReady(void) {
    if (!xstartup)
        xstartup = getenv("TERMUX_X11_XSTARTUP");
    if (!xstartup)
        return;

    pthread_t t;
    pthread_create(&t, NULL, ddxReadyThread, NULL);
}

void
OsVendorInit(void) {
}

void
OsVendorFatalError(unused const char *f, unused va_list args) {
    log(ERROR, f, args);
}

#if defined(DDXBEFORERESET)
void
ddxBeforeReset(void) {
    return;
}
#endif

#if INPUTTHREAD
/** This function is called in Xserver/os/inputthread.c when starting
    the input thread. */
void
ddxInputThreadInit(void) {}
#endif

void ddxUseMsg(void) {
    ErrorF("-xstartup \"command\"    start `command` after server startup\n");
    ErrorF("-legacy-drawing        use legacy drawing, without using AHardwareBuffers\n");
    ErrorF("-force-bgra            force flipping colours (RGBA->BGRA)\n");
    ErrorF("-disable-dri3          disabling DRI3 support (to let lavapipe work)\n");
}

int ddxProcessArgument(unused int argc, unused char *argv[], unused int i) {
    if (strcmp(argv[i], "-xstartup") == 0) {  /* -xstartup "command" */
        CHECK_FOR_REQUIRED_ARGUMENTS(1);
        xstartup = argv[++i];
        return 2;
    }

    if (strcmp(argv[i], "-legacy-drawing") == 0) {
        pvfb->root.legacyDrawing = TRUE;
        return 1;
    }

    if (strcmp(argv[i], "-force-bgra") == 0) {
        pvfb->root.flip = TRUE;
        return 1;
    }

    if (strcmp(argv[i], "-disable-dri3") == 0) {
        pvfb->dri3 = FALSE;
        return 1;
    }

    return 0;
}

static RRModePtr lorieCvt(int width, int height, int framerate) {
    struct libxcvt_mode_info *info;
    char name[128];
    xRRModeInfo modeinfo = {0};
    RRModePtr mode;

    info = libxcvt_gen_mode_info(width, height, framerate, 0, 0);

    snprintf(name, sizeof name, "%dx%d", info->hdisplay, info->vdisplay);
    modeinfo.nameLength = strlen(name);
    modeinfo.width      = info->hdisplay;
    modeinfo.height     = info->vdisplay;
    modeinfo.dotClock   = info->dot_clock * 1000.0;
    modeinfo.hSyncStart = info->hsync_start;
    modeinfo.hSyncEnd   = info->hsync_end;
    modeinfo.hTotal     = info->htotal;
    modeinfo.vSyncStart = info->vsync_start;
    modeinfo.vSyncEnd   = info->vsync_end;
    modeinfo.vTotal     = info->vtotal;
    modeinfo.modeFlags  = info->mode_flags;

    mode = RRModeGet(&modeinfo, name);
    free(info);
    return mode;
}

static Bool resetRootCursor(unused ClientPtr pClient, unused void *closure) {
    CursorVisible = TRUE;
    pScreenPtr->DisplayCursor(lorieMouse, pScreenPtr, NullCursor);
    pScreenPtr->DisplayCursor(lorieMouse, pScreenPtr, rootCursor);
    return TRUE;
}

static void lorieMoveCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, int x, int y) {
    pthread_mutex_lock(&pvfb->state->lock);
    pvfb->state->cursor.x = x;
    pvfb->state->cursor.y = y;
    pvfb->state->cursor.moved = TRUE;
    pthread_cond_signal(&pvfb->state->cond);
    pthread_mutex_unlock(&pvfb->state->lock);
}

static void lorieConvertCursor(CursorPtr pCurs, uint32_t *data) {
    CursorBitsPtr bits = pCurs->bits;
    if (bits->argb) {
        for (int i = 0; i < bits->width * bits->height; i++) {
            /* Convert bgra to rgba */
            CARD32 p = bits->argb[i];
            data[i] = (p & 0xFF000000) | ((p & 0x00FF0000) >> 16) | (p & 0x0000FF00) | ((p & 0x000000FF) << 16);
        }
    } else {
        uint32_t d, fg, bg, *p;
        int x, y, stride, i, bit;

        p = data;
        fg = ((pCurs->foreBlue & 0xff00) << 8) | (pCurs->foreGreen & 0xff00) | (pCurs->foreRed >> 8);
        bg = ((pCurs->backBlue & 0xff00) << 8) | (pCurs->backGreen & 0xff00) | (pCurs->backRed >> 8);
        stride = BitmapBytePad(bits->width);
        for (y = 0; y < bits->height; y++)
            for (x = 0; x < bits->width; x++) {
                i = y * stride + x / 8;
                bit = 1 << (x & 7);
                d = (bits->source[i] & bit) ? fg : bg;
                d = (bits->mask[i] & bit) ? d | 0xff000000 : 0x00000000;
                *p++ = d;
            }
    }
}

static void lorieSetCursor(unused DeviceIntPtr pDev, unused ScreenPtr pScr, CursorPtr pCurs, int x0, int y0) {
    CursorBitsPtr bits = pCurs ? pCurs->bits : NULL;
    if (pCurs && (pCurs->bits->width >= 512 || pCurs->bits->height >= 512)) {
        // We do not have enough memory allocated for such a big cursor, let's display default "X" cursor
        QueueWorkProc(resetRootCursor, NULL, NULL);
        return;
    }

    pthread_mutex_lock(&pvfb->state->cursor.lock);
    if (pCurs && bits) {
        pvfb->state->cursor.xhot = bits->xhot;
        pvfb->state->cursor.yhot = bits->yhot;
        pvfb->state->cursor.width = bits->width;
        pvfb->state->cursor.height = bits->height;
        lorieConvertCursor(pCurs, pvfb->state->cursor.bits);
    } else {
        pvfb->state->cursor.xhot = pvfb->state->cursor.yhot = 0;
        pvfb->state->cursor.width = pvfb->state->cursor.height = 0;
    }
    pvfb->state->cursor.updated = true;
    pthread_cond_signal(&pvfb->state->cond);
    pthread_mutex_unlock(&pvfb->state->cursor.lock);

    if (x0 >= 0 && y0 >= 0)
        lorieMoveCursor(NULL, NULL, x0, y0);
}

static miPointerSpriteFuncRec loriePointerSpriteFuncs = {
    .RealizeCursor = TrueNoop,
    .UnrealizeCursor = TrueNoop,
    .SetCursor = lorieSetCursor,
    .MoveCursor = lorieMoveCursor,
    .DeviceCursorInitialize = TrueNoop,
    .DeviceCursorCleanup = VoidNoop
};

static miPointerScreenFuncRec loriePointerCursorFuncs = {
    .CursorOffScreen = FalseNoop,
    .CrossScreen = VoidNoop,
    .WarpCursor = miPointerWarpCursor
};

static void lorieUpdateBuffer(void) {
    AHardwareBuffer_Desc d0 = {}, d1 = {};
    AHardwareBuffer *new = NULL, *old = pvfb->root.buffer;
    int status, wasLocked = pvfb->root.locked;
    void *data0 = NULL, *data1 = NULL;

    if (pvfb->root.legacyDrawing) {
        PixmapPtr pixmap = (PixmapPtr) pScreenPtr->devPrivate;
        DrawablePtr draw = &pixmap->drawable;
        data1 = (draw->width && draw->height) ? pixmap->devPrivate.ptr : NULL;
        if (data1)
            data0 = malloc(pScreenPtr->width * pScreenPtr->height * 4);
            pixman_blt(data1, data0, draw->width, pScreenPtr->width, 32, 32, 0, 0, 0, 0,
                       min(draw->width, pScreenPtr->width), min(draw->height, pScreenPtr->height));
        pScreenPtr->ModifyPixmapHeader(pScreenPtr->devPrivate, pScreenPtr->width, pScreenPtr->height, 32, 32, pScreenPtr->width * 4, data0);
        free(data1);
        return;
    }

    if (pScreenPtr->devPrivate) {
        d0.width = pScreenPtr->width;
        d0.height = pScreenPtr->height;
        d0.layers = 1;
        d0.usage = USAGE | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        d0.format = pvfb->root.flip
                ? AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM
                : AHARDWAREBUFFER_FORMAT_B8G8R8A8_UNORM;

        /* I could use this, but in this case I must swap colours in the shader. */
        // desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;

        status = AHardwareBuffer_allocate(&d0, &new);
        if (status != 0)
            FatalError("Failed to allocate root window pixmap (error %d)", status);

        AHardwareBuffer_describe(new, &d0);
        status = AHardwareBuffer_lock(new, USAGE, -1, NULL, &data0);
        if (status != 0)
            FatalError("Failed to lock root window pixmap (error %d)", status);

        pvfb->root.buffer = new;
        pvfb->root.locked = TRUE;

        pScreenPtr->ModifyPixmapHeader(pScreenPtr->devPrivate, d0.width, d0.height, 32, 32, d0.stride * 4, data0);

        renderer_set_buffer(pvfb->env, new);
        lorieSendRootWindowBuffer(pvfb->root.buffer);
    }

    if (old) {
        if (wasLocked)
            AHardwareBuffer_unlock(old, NULL);

        if (new && pvfb->root.locked) {
            /*
             * It is pretty easy. If there is old pixmap we should copy it's contents to new pixmap.
             * If it is impossible we should simply request root window exposure.
             */
            AHardwareBuffer_describe(old, &d1);
            status = AHardwareBuffer_lock(old, USAGE, -1, NULL, &data1);
            if (status == 0) {
                pixman_blt(data1, data0, d1.stride, d0.stride,
                           32, 32, 0, 0, 0, 0,
                           min(d1.width, d0.width), min(d1.height, d0.height));
                AHardwareBuffer_unlock(old, NULL);
            } else {
                RegionRec reg;
                BoxRec box = {.x1 = 0, .y1 = 0, .x2 = d0.width, .y2 = d0.height};
                RegionInit(&reg, &box, 1);
                pScreenPtr->WindowExposures(pScreenPtr->root, &reg);
                RegionUninit(&reg);
                AHardwareBuffer_release(old);
                return;
            }
        }
        AHardwareBuffer_release(old);
    }
}

static inline void loriePixmapUnlock(PixmapPtr pixmap) {
    if (pvfb->root.legacyDrawing)
        return renderer_update_root(pixmap->drawable.width, pixmap->drawable.height, pixmap->devPrivate.ptr, pvfb->root.flip);

    if (pvfb->root.locked)
        AHardwareBuffer_unlock(pvfb->root.buffer, NULL);

    pvfb->root.locked = FALSE;
    pixmap->drawable.pScreen->ModifyPixmapHeader(pixmap, -1, -1, -1, -1, -1, NULL);
}

static inline Bool loriePixmapLock(PixmapPtr pixmap) {
    AHardwareBuffer_Desc desc = {};
    void *data;
    int status;

    if (pvfb->root.legacyDrawing)
        return TRUE;

    if (!pvfb->root.buffer) {
        pvfb->root.locked = FALSE;
        return FALSE;
    }

    AHardwareBuffer_describe(pvfb->root.buffer, &desc);
    status = AHardwareBuffer_lock(pvfb->root.buffer, USAGE, -1, NULL, &data);
    pvfb->root.locked = status == 0;
    if (pvfb->root.locked)
        pixmap->drawable.pScreen->ModifyPixmapHeader(pixmap, desc.width, desc.height, -1, -1, desc.stride * 4, data);
    else
        FatalError("Failed to lock surface: %d\n", status);

    return pvfb->root.locked;
}

static void loriePerformVblanks(void);

static Bool lorieRedraw(__unused ClientPtr pClient, __unused void *closure) {
    loriePerformVblanks();

    if (renderer_should_redraw() && RegionNotEmpty(DamageRegion(pvfb->damage))) {
        int redrawn = FALSE;

        loriePixmapUnlock(pScreenPtr->devPrivate);
        redrawn = renderer_redraw(pvfb->env, pvfb->root.flip);
        if (loriePixmapLock(pScreenPtr->devPrivate) && redrawn)
            DamageEmpty(pvfb->damage);
        if (redrawn)
            lorieRequestRender();
    } else if (pvfb->state->cursor.moved) {
        renderer_redraw(pvfb->env, pvfb->root.flip);
        lorieRequestRender();
    }

    pvfb->state->cursor.moved = FALSE;
    return TRUE;
}

static CARD32 lorieFramecounter(unused OsTimerPtr timer, unused CARD32 time, unused void *arg) {
    // renderer_print_fps(5000);
    return 5000;
}

static Bool lorieCreateScreenResources(ScreenPtr pScreen) {
    Bool ret;
    pScreen->CreateScreenResources = pvfb->CreateScreenResources;

    ret = pScreen->CreateScreenResources(pScreen);
    if (!ret)
        return FALSE;

    pScreen->devPrivate = fbCreatePixmap(pScreen, 0, 0, pScreen->rootDepth, CREATE_PIXMAP_USAGE_BACKING_PIXMAP);

    pvfb->damage = DamageCreate(NULL, NULL, DamageReportNone, TRUE, pScreen, NULL);
    if (!pvfb->damage)
        FatalError("Couldn't setup damage\n");

    DamageRegister(&(*pScreen->GetScreenPixmap)(pScreen)->drawable, pvfb->damage);
    pvfb->fpsTimer = TimerSet(NULL, 0, 5000, lorieFramecounter, pScreen);

    pthread_mutex_lock(&pvfb->state->lock);
    lorieUpdateBuffer();
    pthread_mutex_unlock(&pvfb->state->lock);

    pvfb->ready = true;

    return TRUE;
}

static Bool
lorieCloseScreen(ScreenPtr pScreen) {
    pvfb->ready = false;
    unwrap(pvfb, pScreen, CloseScreen)
    // No need to call fbDestroyPixmap since AllocatePixmap sets pixmap as PRIVATE_SCREEN so it is destroyed automatically.
    return pScreen->CloseScreen(pScreen);
}

static Bool
lorieRRScreenSetSize(ScreenPtr pScreen, CARD16 width, CARD16 height, unused CARD32 mmWidth, unused CARD32 mmHeight) {
    SetRootClip(pScreen, ROOT_CLIP_NONE);

    pvfb->root.width = pScreen->width = width;
    pvfb->root.height = pScreen->height = height;
    pScreen->mmWidth = ((double) (width)) * 25.4 / monitorResolution;
    pScreen->mmHeight = ((double) (height)) * 25.4 / monitorResolution;

    pthread_mutex_lock(&pvfb->state->lock);
    lorieUpdateBuffer();
    pthread_mutex_unlock(&pvfb->state->lock);

    pScreen->ResizeWindow(pScreen->root, 0, 0, width, height, NULL);
    DamageEmpty(pvfb->damage);
    SetRootClip(pScreen, ROOT_CLIP_FULL);

    RRScreenSizeNotify(pScreen);
    update_desktop_dimensions();
    pvfb->state->cursor.moved = TRUE;

    return TRUE;
}

static Bool
lorieRRCrtcSet(unused ScreenPtr pScreen, RRCrtcPtr crtc, RRModePtr mode, int x, int y,
               Rotation rotation, int numOutput, RROutputPtr *outputs) {
    return (crtc && mode) ? RRCrtcNotify(crtc, mode, x, y, rotation, NULL, numOutput, outputs) : FALSE;
}

static Bool
lorieRRGetInfo(unused ScreenPtr pScreen, Rotation *rotations) {
    *rotations = RR_Rotate_0;
    return TRUE;
}

static Bool
lorieRandRInit(ScreenPtr pScreen) {
    rrScrPrivPtr pScrPriv;
    RROutputPtr output;
    RRCrtcPtr crtc;
    RRModePtr mode;

    if (!RRScreenInit(pScreen))
       return FALSE;

    pScrPriv = rrGetScrPriv(pScreen);
    pScrPriv->rrGetInfo = lorieRRGetInfo;
    pScrPriv->rrCrtcSet = lorieRRCrtcSet;
    pScrPriv->rrScreenSetSize = lorieRRScreenSetSize;

    RRScreenSetSizeRange(pScreen, 1, 1, 32767, 32767);

    if (FALSE
        || !(mode = lorieCvt(pScreen->width, pScreen->height, pvfb->root.framerate))
        || !(crtc = RRCrtcCreate(pScreen, NULL))
        || !RRCrtcGammaSetSize(crtc, 256)
        || !(output = RROutputCreate(pScreen, pvfb->root.name, sizeof(pvfb->root.name), NULL))
        || (output->nameLength = strlen(output->name), FalseNoop())
        || !RROutputSetClones(output, NULL, 0)
        || !RROutputSetModes(output, &mode, 1, 0)
        || !RROutputSetCrtcs(output, &crtc, 1)
        || !RROutputSetConnection(output, RR_Connected)
        || !RRCrtcNotify(crtc, mode, 0, 0, RR_Rotate_0, NULL, 1, &output))
        return FALSE;
    return TRUE;
}

void lorieTriggerWorkingQueue(void) {
    eventfd_write(pvfb->eventFd, 1);
}

static void lorieWorkingQueueCallback(int fd, int __unused ready, void __unused *data) {
    // Nothing to do here. It is needed to finish ospoll_wait.
    eventfd_t dummy;
    eventfd_read(fd, &dummy);
}

void lorieChoreographerFrameCallback(__unused long t, AChoreographer* d) {
    AChoreographer_postFrameCallback(d, (AChoreographer_frameCallback) lorieChoreographerFrameCallback, d);
    if (pvfb->ready) {
        QueueWorkProc(lorieRedraw, NULL, NULL);
        lorieTriggerWorkingQueue();
    }
}

static Bool
lorieScreenInit(ScreenPtr pScreen, unused int argc, unused char **argv) {
    static int eventFd = -1;
    pScreenPtr = pScreen;

    if (eventFd == -1)
        eventFd = eventfd(0, EFD_CLOEXEC);

    pvfb->eventFd = eventFd;
    SetNotifyFd(eventFd, lorieWorkingQueueCallback, X_NOTIFY_READ, NULL);

    miSetZeroLineBias(pScreen, 0);
    pScreen->blackPixel = 0;
    pScreen->whitePixel = 1;

    pvfb->vblank_interval = 1000000 / pvfb->root.framerate;

    if (FALSE
          || !dixRegisterPrivateKey(&lorieGCPrivateKey, PRIVATE_GC, sizeof(LorieGCPrivRec))
          || !miSetVisualTypesAndMasks(24, ((1 << TrueColor) | (1 << DirectColor)), 8, TrueColor, 0xFF0000, 0x00FF00, 0x0000FF)
          || !miSetPixmapDepths()
          || !fbScreenInit(pScreen, NULL, pvfb->root.width, pvfb->root.height, monitorResolution, monitorResolution, 0, 32)
          || !(!pvfb->dri3 || lorieInitDri3(pScreen))
          || !fbPictureInit(pScreen, 0, 0)
          || !lorieRandRInit(pScreen)
          || !miPointerInitialize(pScreen, &loriePointerSpriteFuncs, &loriePointerCursorFuncs, TRUE)
          || !fbCreateDefColormap(pScreen)
          || !present_screen_init(pScreen, &loriePresentInfo))
        return FALSE;

    wrap(pvfb, pScreen, CreateScreenResources, lorieCreateScreenResources)
    wrap(pvfb, pScreen, CloseScreen, lorieCloseScreen)
    wrap(pvfb, pScreen, CreateGC, lorieCreateGC)

    QueueWorkProc(resetRootCursor, NULL, NULL);
    ShmRegisterFbFuncs(pScreen);
    miSyncShmScreenInit(pScreen);

    return TRUE;
}                               /* end lorieScreenInit */

// From xfixes/cursor.c
static CursorPtr
CursorForDevice(DeviceIntPtr pDev) {
    if (!CursorVisible || !EnableCursor)
        return NULL;

    if (pDev && pDev->spriteInfo) {
        if (pDev->spriteInfo->anim.pCursor)
            return pDev->spriteInfo->anim.pCursor;
        return pDev->spriteInfo->sprite ? pDev->spriteInfo->sprite->current : NULL;
    }

    return NULL;
}

Bool lorieChangeWindow(unused ClientPtr pClient, void *closure) {
    jobject surface = (jobject) closure;
    renderer_set_window(pvfb->env, surface);
    lorieSetCursor(NULL, NULL, CursorForDevice(GetMaster(lorieMouse, MASTER_POINTER)), -1, -1);

    if (pvfb->root.legacyDrawing) {
        renderer_update_root(pScreenPtr->width, pScreenPtr->height, ((PixmapPtr) pScreenPtr->devPrivate)->devPrivate.ptr, pvfb->root.flip);
        renderer_redraw(pvfb->env, pvfb->root.flip);
    }

    return TRUE;
}

void lorieConfigureNotify(int width, int height, int framerate, size_t name_size, char* name) {
    ScreenPtr pScreen = pScreenPtr;
    RROutputPtr output = RRFirstOutput(pScreen);
    framerate = framerate ? framerate : 60;

    if (output && name) {
        // We should save this name in pvfb to make sure the name will be restored in the case if the server is being reset.
        memset(pvfb->root.name, 0, 1024);
        memset(output->name, 0, 1024);
        strncpy(pvfb->root.name, name, name_size < 1024 ? name_size : 1024);
        strncpy(output->name, name, name_size < 1024 ? name_size : 1024);
        output->name[1023] = '\0';
        output->nameLength = strlen(output->name);
    }

    if (output && width && height && (pScreen->width != width || pScreen->height != height)) {
        CARD32 mmWidth, mmHeight;
        RRModePtr mode = lorieCvt(width, height, framerate);
        mmWidth = ((double) (mode->mode.width)) * 25.4 / monitorResolution;
        mmHeight = ((double) (mode->mode.width)) * 25.4 / monitorResolution;
        RROutputSetModes(output, &mode, 1, 0);
        RRCrtcNotify(RRFirstEnabledCrtc(pScreen), mode,0, 0,RR_Rotate_0, NULL, 1, &output);
        RRScreenSizeSet(pScreen, mode->mode.width, mode->mode.height, mmWidth, mmHeight);

        log(VERBOSE, "New reported framerate is %d", framerate);
        pvfb->root.framerate = framerate;
        pvfb->vblank_interval = 1000000 / pvfb->root.framerate;
    }
}

void
InitOutput(ScreenInfo * screen_info, int argc, char **argv) {
    int depths[] = { 1, 4, 8, 15, 16, 24, 32 };
    int bpp[] =    { 1, 8, 8, 16, 16, 32, 32 };
    int i;

    if (monitorResolution == 0)
        monitorResolution = 96;

    for(i = 0; i < ARRAY_SIZE(depths); i++) {
        screen_info->formats[i].depth = depths[i];
        screen_info->formats[i].bitsPerPixel = bpp[i];
        screen_info->formats[i].scanlinePad = BITMAP_SCANLINE_PAD;
    }

    screen_info->imageByteOrder = IMAGE_BYTE_ORDER;
    screen_info->bitmapScanlineUnit = BITMAP_SCANLINE_UNIT;
    screen_info->bitmapScanlinePad = BITMAP_SCANLINE_PAD;
    screen_info->bitmapBitOrder = BITMAP_BIT_ORDER;
    screen_info->numPixmapFormats = ARRAY_SIZE(depths);

    renderer_init(pvfb->env, &pvfb->root.legacyDrawing, &pvfb->root.flip);
    xorgGlxCreateVendor();
    lorieInitClipboard();

    if (-1 == AddScreen(lorieScreenInit, argc, argv)) {
        FatalError("Couldn't add screen %d\n", i);
    }
}

// This Present implementation mostly copies the one from `present/present_fake.c`
// The only difference is performing vblanks right before redrawing root window (in lorieRedraw) instead of using timers.
static RRCrtcPtr loriePresentGetCrtc(WindowPtr w) {
    return RRFirstEnabledCrtc(w->drawable.pScreen);
}

static int loriePresentGetUstMsc(RRCrtcPtr crtc, uint64_t *ust, uint64_t *msc) {
    *ust = GetTimeInMicros();
    *msc = pvfb->current_msc;
    return Success;
}

static Bool loriePresentQueueVblank(RRCrtcPtr crtc, uint64_t event_id, uint64_t msc) {
    struct vblank* vblank;

#pragma clang diagnostic push
#pragma ide diagnostic ignored "MemoryLeak" // it is not leaked, it is destroyed in lorieRedraw
    vblank = calloc (1, sizeof (*vblank));
    if (!vblank)
        return BadAlloc;

    *vblank = (struct vblank) { .id = event_id, .msc = msc };
    xorg_list_add(&vblank->list, &pvfb->vblank_queue);

    return Success;
#pragma clang diagnostic pop
}

static void loriePresentAbortVblank(RRCrtcPtr crtc, uint64_t id, uint64_t msc) {
    struct vblank *vblank, *tmp;

    xorg_list_for_each_entry_safe(vblank, tmp, &pvfb->vblank_queue, list) {
        if (vblank->id == id) {
            xorg_list_del(&vblank->list);
            free (vblank);
            break;
        }
    }
}

static void loriePerformVblanks(void) {
    struct vblank *vblank, *tmp;
    uint64_t ust, msc;
    pvfb->current_msc++;

    xorg_list_for_each_entry_safe(vblank, tmp, &pvfb->vblank_queue, list) {
        if (vblank->msc <= pvfb->current_msc) {
            loriePresentGetUstMsc(NULL, &ust, &msc);
            present_event_notify(vblank->id, ust, msc);
            xorg_list_del(&vblank->list);
            free (vblank);
        }
    }
}

static struct present_screen_info loriePresentInfo = {
        .get_crtc = loriePresentGetCrtc,
        .get_ust_msc = loriePresentGetUstMsc,
        .queue_vblank = loriePresentQueueVblank,
        .abort_vblank = loriePresentAbortVblank,
};

void lorieSetVM(JavaVM* vm) {
    pvfb->vm = vm;
    (*vm)->AttachCurrentThread(vm, &pvfb->env, NULL);
}

#define LORIE_GC_OP_PROLOGUE(pGC) \
    lorieGCPriv(pGC);  \
    const GCFuncs *oldFuncs = pGC->funcs; \
    const GCOps *oldOps = pGC->ops; \
    unwrap(pGCPriv, pGC, funcs);  \
    unwrap(pGCPriv, pGC, ops); \

#define LORIE_GC_OP_EPILOGUE(pGC) \
    wrap(pGCPriv, pGC, funcs, oldFuncs); \
    wrap(pGCPriv, pGC, ops, oldOps)

#define LORIE_GC_FUNC_PROLOGUE(pGC) \
    lorieGCPriv(pGC); \
    unwrap(pGCPriv, pGC, funcs); \
    if (pGCPriv->ops) unwrap(pGCPriv, pGC, ops)

#define LORIE_GC_FUNC_EPILOGUE(pGC) \
    wrap(pGCPriv, pGC, funcs, &lorieGCFuncs);  \
    if (pGCPriv->ops) wrap(pGCPriv, pGC, ops, &lorieGCOps)

static const GCOps lorieGCOps;
static const GCFuncs lorieGCFuncs;

static void lorieValidateGC(GCPtr pGC, unsigned long stateChanges, DrawablePtr pDrawable) {
    LORIE_GC_FUNC_PROLOGUE(pGC)
    (*pGC->funcs->ValidateGC) (pGC, stateChanges, pDrawable);
    LORIE_GC_FUNC_EPILOGUE(pGC)
}

static void lorieChangeGC(GCPtr pGC, unsigned long mask) {
    LORIE_GC_FUNC_PROLOGUE(pGC)
    (*pGC->funcs->ChangeGC) (pGC, mask);
    LORIE_GC_FUNC_EPILOGUE(pGC)
}

static void lorieCopyGC(GCPtr pGCSrc, unsigned long mask, GCPtr pGCDst) {
    LORIE_GC_FUNC_PROLOGUE(pGCSrc)
    (*pGCSrc->funcs->CopyGC) (pGCSrc, mask, pGCDst);
    LORIE_GC_FUNC_EPILOGUE(pGCSrc)
}

static void lorieDestroyGC(GCPtr pGC) {
    LORIE_GC_FUNC_PROLOGUE(pGC)
    (*pGC->funcs->DestroyGC) (pGC);
    LORIE_GC_FUNC_EPILOGUE(pGC)
}

static void lorieChangeClip(GCPtr pGC, int type, void *pvalue, int nrects) {
    LORIE_GC_FUNC_PROLOGUE(pGC)
    (*pGC->funcs->ChangeClip) (pGC, type, pvalue, nrects);
    LORIE_GC_FUNC_EPILOGUE(pGC)
}

static void lorieDestroyClip(GCPtr pGC) {
    LORIE_GC_FUNC_PROLOGUE(pGC)
    (*pGC->funcs->DestroyClip) (pGC);
    LORIE_GC_FUNC_EPILOGUE(pGC)
}

static void lorieCopyClip(GCPtr pgcDst, GCPtr pgcSrc) {
    LORIE_GC_FUNC_PROLOGUE(pgcDst)
    (*pgcDst->funcs->CopyClip) (pgcDst, pgcSrc);
    LORIE_GC_FUNC_EPILOGUE(pgcDst)
}

static const GCFuncs lorieGCFuncs = {
        lorieValidateGC, lorieChangeGC, lorieCopyGC, lorieDestroyGC,
        lorieChangeClip, lorieDestroyClip, lorieCopyClip
};
#define LOCK_DRAWABLE(a) if (a == pScreenPtr->devPrivate || (a && a->type == DRAWABLE_WINDOW)) pthread_mutex_lock(&pvfb->state->lock)
#define UNLOCK_DRAWABLE(a) if (a == pScreenPtr->devPrivate || (a && a->type == DRAWABLE_WINDOW)) pthread_mutex_unlock(&pvfb->state->lock)

static void lorieFillSpans(DrawablePtr pDrawable, GCPtr pGC, int nInit, DDXPointPtr pptInit, int * pwidthInit, int fSorted) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->FillSpans) (pDrawable, pGC, nInit, pptInit, pwidthInit, fSorted);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void lorieSetSpans(DrawablePtr pDrawable, GCPtr pGC, char * psrc, DDXPointPtr ppt, int * pwidth, int nspans, int fSorted) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->SetSpans) (pDrawable, pGC, psrc, ppt, pwidth, nspans, fSorted);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePutImage(DrawablePtr pDrawable, GCPtr pGC, int depth, int x, int y, int w, int h, int leftPad, int format, char * pBits) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PutImage) (pDrawable, pGC, depth, x, y, w, h, leftPad, format, pBits);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static RegionPtr lorieCopyArea(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC, int srcx, int srcy, int w, int h, int dstx, int dsty) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDst);
    RegionPtr r = (*pGC->ops->CopyArea) (pSrc, pDst, pGC, srcx, srcy, w, h, dstx, dsty);
    UNLOCK_DRAWABLE(pDst);
    LORIE_GC_OP_EPILOGUE(pGC)
    return r;
}

static RegionPtr lorieCopyPlane(DrawablePtr pSrc, DrawablePtr pDst, GCPtr pGC, int srcx, int srcy, int width, int height, int dstx, int dsty, unsigned long bitPlane) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDst);
    RegionPtr r = (*pGC->ops->CopyPlane) (pSrc, pDst, pGC, srcx, srcy, width, height, dstx, dsty, bitPlane);
    UNLOCK_DRAWABLE(pDst);
    LORIE_GC_OP_EPILOGUE(pGC)
    return r;
}

static void loriePolyPoint(DrawablePtr pDrawable, GCPtr pGC, int mode, int npt, DDXPointPtr pptInit) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PolyPoint) (pDrawable, pGC, mode, npt, pptInit);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePolylines(DrawablePtr pDrawable, GCPtr pGC, int mode, int npt, DDXPointPtr pptInit) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->Polylines) (pDrawable, pGC, mode, npt, pptInit);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePolySegment(DrawablePtr pDrawable, GCPtr pGC, int nseg, xSegment * pSegs) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PolySegment) (pDrawable, pGC, nseg, pSegs);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePolyRectangle(DrawablePtr pDrawable, GCPtr pGC, int nrects, xRectangle * pRects) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PolyRectangle) (pDrawable, pGC, nrects, pRects);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePolyArc(DrawablePtr pDrawable, GCPtr pGC, int narcs, xArc * parcs) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PolyArc) (pDrawable, pGC, narcs, parcs);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void lorieFillPolygon(DrawablePtr pDrawable, GCPtr pGC, int shape, int mode, int count, DDXPointPtr pPts) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->FillPolygon) (pDrawable, pGC, shape, mode, count, pPts);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePolyFillRect(DrawablePtr pDrawable, GCPtr pGC, int nrectFill, xRectangle * prectInit) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PolyFillRect) (pDrawable, pGC, nrectFill, prectInit);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePolyFillArc(DrawablePtr pDrawable, GCPtr pGC, int narcs, xArc * parcs) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PolyFillArc) (pDrawable, pGC, narcs, parcs);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static int loriePolyText8(DrawablePtr pDrawable, GCPtr pGC, int x, int y, int count, char * chars) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    int r = (*pGC->ops->PolyText8) (pDrawable, pGC, x, y, count, chars);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
    return r;
}

static int loriePolyText16(DrawablePtr pDrawable, GCPtr pGC, int x, int y, int count, unsigned short * chars) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    int r = (*pGC->ops->PolyText16) (pDrawable, pGC, x, y, count, chars);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
    return r;
}

static void lorieImageText8(DrawablePtr pDrawable, GCPtr pGC, int x, int y, int count, char * chars) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->ImageText8) (pDrawable, pGC, x, y, count, chars);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void lorieImageText16(DrawablePtr pDrawable, GCPtr pGC, int x, int y, int count, unsigned short * chars) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->ImageText16) (pDrawable, pGC, x, y, count, chars);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void lorieImageGlyphBlt(DrawablePtr pDrawable, GCPtr pGC, int x, int y, unsigned int nglyph, CharInfoPtr *ppci, void *pglyphBase) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->ImageGlyphBlt) (pDrawable, pGC, x, y, nglyph, ppci, pglyphBase);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePolyGlyphBlt(DrawablePtr pDrawable, GCPtr pGC, int x, int y, unsigned int nglyph, CharInfoPtr *ppci, void *pglyphBase) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDrawable);
    (*pGC->ops->PolyGlyphBlt) (pDrawable, pGC, x, y, nglyph, ppci, pglyphBase);
    UNLOCK_DRAWABLE(pDrawable);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static void loriePushPixels(GCPtr pGC, PixmapPtr pSrc, DrawablePtr pDst, int w, int h, int x, int y) {
    LORIE_GC_OP_PROLOGUE(pGC)
    LOCK_DRAWABLE(pDst);
    (*pGC->ops->PushPixels) (pGC, pSrc, pDst, w, h, x, y);
    UNLOCK_DRAWABLE(pDst);
    LORIE_GC_OP_EPILOGUE(pGC)
}

static const GCOps lorieGCOps = {
        lorieFillSpans, lorieSetSpans,
        loriePutImage, lorieCopyArea,
        lorieCopyPlane, loriePolyPoint,
        loriePolylines, loriePolySegment,
        loriePolyRectangle, loriePolyArc,
        lorieFillPolygon, loriePolyFillRect,
        loriePolyFillArc, loriePolyText8,
        loriePolyText16, lorieImageText8,
        lorieImageText16, lorieImageGlyphBlt,
        loriePolyGlyphBlt, loriePushPixels,
};

/*
 * Root window memory is shared across two processes (X server and Android activity)
 * so we should make sure there is no simultaneous reading/writing in two different processes.
 * That means we should lock out interprocess mutex before performing any operation
 * to prevent tearing and segmentation faults.
 * We lock mutex for all windows including root window because all windows share root window memory with some offset.
 */

static Bool
lorieCreateGC(GCPtr pGC) {
    ScreenPtr pScreen = pGC->pScreen;

    lorieGCPriv(pGC);
    Bool ret;

    unwrap(pvfb, pScreen, CreateGC)
    if ((ret = (*pScreen->CreateGC) (pGC))) {
        pGCPriv->funcs = pGC->funcs;
        pGCPriv->ops = pGC->ops;
        pGC->funcs = &lorieGCFuncs;
        pGC->ops = &lorieGCOps;
    }
    wrap(pvfb, pScreen, CreateGC, lorieCreateGC)

    return ret;
}

static GLboolean drawableSwapBuffers(unused ClientPtr client, unused __GLXdrawable * drawable) { return TRUE; }
static void drawableCopySubBuffer(unused __GLXdrawable * basePrivate, unused int x, unused int y, unused int w, unused int h) {}
static __GLXdrawable * createDrawable(unused ClientPtr client, __GLXscreen * screen, DrawablePtr pDraw,
                                      unused XID drawId, int type, XID glxDrawId, __GLXconfig * glxConfig) {
    __GLXdrawable *private = calloc(1, sizeof *private);
    if (private == NULL)
        return NULL;

    if (!__glXDrawableInit(private, screen, pDraw, type, glxDrawId, glxConfig)) {
        free(private);
        return NULL;
    }

    private->destroy = (void (*)(__GLXdrawable *)) free;
    private->swapBuffers = drawableSwapBuffers;
    private->copySubBuffer = drawableCopySubBuffer;

    return private;
}

static void glXDRIscreenDestroy(__GLXscreen *baseScreen) {
    free(baseScreen->GLXextensions);
    free(baseScreen->GLextensions);
    free(baseScreen->visuals);
    free(baseScreen);
}

static __GLXscreen *glXDRIscreenProbe(ScreenPtr pScreen) {
    __GLXscreen *screen;

    screen = calloc(1, sizeof *screen);
    if (screen == NULL)
        return NULL;

    screen->destroy = glXDRIscreenDestroy;
    screen->createDrawable = createDrawable;
    screen->pScreen = pScreen;
    screen->fbconfigs = configs;
    screen->glvnd = "mesa";

    __glXInitExtensionEnableBits(screen->glx_enable_bits);
    /* There is no real GLX support, but anyways swrast reports it. */
    __glXEnableExtension(screen->glx_enable_bits, "GLX_MESA_copy_sub_buffer");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_no_config_context");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context_robustness");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context_no_error");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_create_context_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_create_context_es_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_create_context_es2_profile");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_get_proc_address");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_framebuffer_sRGB");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_ARB_fbconfig_float");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_fbconfig_packed_float");
    __glXEnableExtension(screen->glx_enable_bits, "GLX_EXT_texture_from_pixmap");
    __glXScreenInit(screen, pScreen);

    return screen;
}

__GLXprovider __glXDRISWRastProvider = {
        glXDRIscreenProbe,
        "DRISWRAST",
        NULL
};
