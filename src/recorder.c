/*
 * recorder.c – Screen recording via mpv stream callback + encoding
 *
 * Captures the desktop using BitBlt and feeds raw BGRA frames to
 * mpv through a custom stream protocol ("capture://").  mpv's
 * rawvideo demuxer reads from our callback, and its built-in
 * libx264 encoder writes the output to an MP4 file.
 *
 * System audio is captured via WASAPI loopback and fed to mpv as
 * a WAV stream through a second protocol ("capture-audio://").
 *
 * Architecture:
 *   video capture thread:  BitBlt → latest_frame double-buffer
 *   audio capture thread:  WASAPI loopback → audio ring buffer
 *   mpv video demuxer:     read_fn() ← latest_frame (wall-clock paced) → rawvideo → libx264 → MP4
 *   mpv audio demuxer:     read_fn() ← audio ring → WAV → aac → MP4
 *
 * No external binaries are needed – everything runs through
 * libmpv-2.dll which already contains the FFmpeg encoding stack.
 */
#include "recorder.h"
#include "constants.h"

#include <mpv/client.h>
#include <mpv/stream_cb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* WASAPI loopback capture */
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

/* ── COM GUIDs (avoid linking uuid.lib) ──────────────────────── */

static const GUID CLSID_MMDevEnum_ =
    {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID IID_IMMDevEnum_ =
    {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID IID_IAudioCli_ =
    {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID IID_IAudioCapCli_ =
    {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};
static const GUID GUID_KSDATAFORMAT_IEEE_FLOAT_ =
    {0x00000003,0x0000,0x0010,{0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71}};

#ifndef AUDCLNT_STREAMFLAGS_LOOPBACK
#define AUDCLNT_STREAMFLAGS_LOOPBACK 0x00020000
#endif
#ifndef AUDCLNT_STREAMFLAGS_EVENTCALLBACK
#define AUDCLNT_STREAMFLAGS_EVENTCALLBACK 0x00040000
#endif
#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif
#ifndef WAVE_FORMAT_EXTENSIBLE
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE
#endif

/* ── Ring buffer for frame/sample data ───────────────────────── */

/*
 * Simple single-producer / single-consumer ring buffer.
 * Producer: capture thread writes full frames/samples.
 * Consumer: mpv's read_fn reads arbitrary byte chunks.
 *
 * We size it for ~4 frames so the capture thread can run slightly
 * ahead of the encoder without blocking.
 */
#define RING_FRAME_SLOTS 4

typedef struct {
    BYTE           *buf;
    size_t          capacity;     /* total bytes                     */
    volatile size_t write_pos;    /* producer position               */
    volatile size_t read_pos;     /* consumer position               */
    volatile BOOL   eof;          /* producer signals no more data   */
    volatile BOOL   cancel;       /* consumer abort                  */
    HANDLE          hDataReady;   /* event: data available or EOF    */
    HANDLE          hSpaceReady;  /* event: space available          */
    CRITICAL_SECTION cs;
} RingBuf;

static RingBuf *ring_create(size_t frame_bytes)
{
    RingBuf *rb = (RingBuf *)calloc(1, sizeof(*rb));
    if (!rb) return NULL;
    rb->capacity   = frame_bytes * RING_FRAME_SLOTS;
    rb->buf        = (BYTE *)malloc(rb->capacity);
    if (!rb->buf) { free(rb); return NULL; }
    rb->write_pos  = 0;
    rb->read_pos   = 0;
    rb->eof        = FALSE;
    rb->cancel     = FALSE;
    rb->hDataReady  = CreateEventW(NULL, FALSE, FALSE, NULL);
    rb->hSpaceReady = CreateEventW(NULL, FALSE, FALSE, NULL);
    InitializeCriticalSection(&rb->cs);
    return rb;
}

static void ring_destroy(RingBuf *rb)
{
    if (!rb) return;
    if (rb->buf)         free(rb->buf);
    if (rb->hDataReady)  CloseHandle(rb->hDataReady);
    if (rb->hSpaceReady) CloseHandle(rb->hSpaceReady);
    DeleteCriticalSection(&rb->cs);
    free(rb);
}

/* Bytes available for reading. */
static size_t ring_readable(const RingBuf *rb)
{
    size_t w = rb->write_pos, r = rb->read_pos;
    return (w >= r) ? (w - r) : (rb->capacity - r + w);
}

/* Bytes available for writing. */
static size_t ring_writable(const RingBuf *rb)
{
    /* Keep 1 byte gap so full != empty. */
    return rb->capacity - 1 - ring_readable(rb);
}

/* Producer: write exactly `len` bytes.  Blocks if space unavailable. */
static BOOL ring_write(RingBuf *rb, const BYTE *data, size_t len)
{
    while (len > 0) {
        if (rb->cancel) return FALSE;

        EnterCriticalSection(&rb->cs);
        size_t avail = ring_writable(rb);
        LeaveCriticalSection(&rb->cs);

        if (avail == 0) {
            WaitForSingleObject(rb->hSpaceReady, 100);
            continue;
        }

        size_t chunk = (len < avail) ? len : avail;
        size_t wp = rb->write_pos;

        /* Handle wrap-around. */
        size_t to_end = rb->capacity - wp;
        if (chunk <= to_end) {
            memcpy(rb->buf + wp, data, chunk);
        } else {
            memcpy(rb->buf + wp, data, to_end);
            memcpy(rb->buf, data + to_end, chunk - to_end);
        }

        EnterCriticalSection(&rb->cs);
        rb->write_pos = (wp + chunk) % rb->capacity;
        LeaveCriticalSection(&rb->cs);

        SetEvent(rb->hDataReady);

        data += chunk;
        len  -= chunk;
    }
    return TRUE;
}

/* Producer: try to write `len` bytes.  Never blocks – drops data if
   there is insufficient space.  Returns TRUE if all data was written,
   FALSE if some or all was dropped.  Used by the audio capture thread
   so it never stalls and always keeps WASAPI drained. */
static BOOL ring_try_write(RingBuf *rb, const BYTE *data, size_t len)
{
    if (rb->cancel) return FALSE;

    EnterCriticalSection(&rb->cs);
    size_t avail = ring_writable(rb);
    LeaveCriticalSection(&rb->cs);

    if (avail < len) {
        /* Not enough space – drop the data entirely. */
        return FALSE;
    }

    size_t wp = rb->write_pos;
    size_t to_end = rb->capacity - wp;
    if (len <= to_end) {
        memcpy(rb->buf + wp, data, len);
    } else {
        memcpy(rb->buf + wp, data, to_end);
        memcpy(rb->buf, data + to_end, len - to_end);
    }

    EnterCriticalSection(&rb->cs);
    rb->write_pos = (wp + len) % rb->capacity;
    LeaveCriticalSection(&rb->cs);

    SetEvent(rb->hDataReady);
    return TRUE;
}

/* Producer: signal that no more data will be written. */
static void ring_set_eof(RingBuf *rb)
{
    rb->eof = TRUE;
    SetEvent(rb->hDataReady);
}

/* Consumer: read up to `nbytes` bytes.  Blocks until data or EOF.
   Returns bytes read, 0 on EOF, -1 on cancel. */
static int64_t ring_read(RingBuf *rb, char *buf, size_t nbytes)
{
    for (;;) {
        if (rb->cancel) return -1;

        EnterCriticalSection(&rb->cs);
        size_t avail = ring_readable(rb);
        LeaveCriticalSection(&rb->cs);

        if (avail > 0) {
            size_t chunk = (nbytes < avail) ? nbytes : avail;
            size_t rp = rb->read_pos;
            size_t to_end = rb->capacity - rp;
            if (chunk <= to_end) {
                memcpy(buf, rb->buf + rp, chunk);
            } else {
                memcpy(buf, rb->buf + rp, to_end);
                memcpy(buf + to_end, rb->buf, chunk - to_end);
            }

            EnterCriticalSection(&rb->cs);
            rb->read_pos = (rp + chunk) % rb->capacity;
            LeaveCriticalSection(&rb->cs);

            SetEvent(rb->hSpaceReady);
            return (int64_t)chunk;
        }

        if (rb->eof) return 0;  /* EOF */

        WaitForSingleObject(rb->hDataReady, 100);
    }
}

/* ── WAV header builder ──────────────────────────────────────── */

/* Build a minimal 44-byte WAV header for 16-bit PCM streaming. */
static void build_wav_header(BYTE *h, int rate, int channels, int bits)
{
    int   byte_rate   = rate * channels * (bits / 8);
    int   block_align = channels * (bits / 8);
    DWORD data_sz     = 0x7FFFFFFF;         /* unknown / streaming */
    DWORD riff_sz     = 36 + data_sz;

    memcpy(h,    "RIFF", 4);  memcpy(h+ 4, &riff_sz,      4);
    memcpy(h+ 8, "WAVE", 4);
    memcpy(h+12, "fmt ", 4);
    DWORD fmt_sz = 16;            memcpy(h+16, &fmt_sz,      4);
    WORD  fmt_tag = 1;            memcpy(h+20, &fmt_tag,     2); /* PCM */
    WORD  nch = (WORD)channels;   memcpy(h+22, &nch,         2);
    DWORD sr = (DWORD)rate;       memcpy(h+24, &sr,          4);
    DWORD br = (DWORD)byte_rate;  memcpy(h+28, &br,          4);
    WORD  ba = (WORD)block_align; memcpy(h+32, &ba,          2);
    WORD  bps = (WORD)bits;       memcpy(h+34, &bps,         2);
    memcpy(h+36, "data", 4);     memcpy(h+40, &data_sz,     4);
}

/* ── Internal state ──────────────────────────────────────────── */

struct Recorder {
    mpv_handle *mpv;

    /* Video capture */
    HANDLE       hCaptureThread;
    HDC          hdcScreen;
    HDC          hdcMem;
    HBITMAP      hBitmap;
    void        *pBits;
    int          capX, capY, capW, capH;
    int          fps;

    /* Double-buffer for latest captured frame */
    BYTE            *latest_frame;     /* latest capture (cs_frame-protected) */
    size_t           frame_bytes;      /* capW * capH * 4                     */
    CRITICAL_SECTION cs_frame;         /* guards latest_frame                 */

    /* stream_read_fn state (mpv demuxer thread only, except stopped)  */
    BYTE            *read_buf;         /* local copy being served to mpv      */
    size_t           read_offset;      /* bytes served from current frame     */
    BOOL             read_started;     /* TRUE after first read_fn call       */
    LARGE_INTEGER    qpc_freq;         /* QPC frequency                       */
    LARGE_INTEGER    qpc_read_start;   /* QPC at first read_fn call           */
    int64_t          frames_served;    /* completed frames delivered to mpv   */
    LONGLONG         read_pause_ticks; /* accumulated pause in QPC ticks      */
    BOOL             read_was_paused;  /* pause tracking state                */
    LARGE_INTEGER    read_pause_qpc;   /* QPC when pause started in read_fn   */

    /* WASAPI audio capture */
    HANDLE       hAudioThread;
    HANDLE       hAudioFormatReady;  /* signaled when format is known */
    RingBuf     *audio_ring;         /* created by audio thread       */
    int          audio_rate;         /* filled by audio thread        */
    int          audio_channels;     /* filled by audio thread        */
    BOOL         audio_src_float;    /* filled by audio thread        */
    BOOL         audio_init_failed;  /* TRUE if WASAPI init fails     */
    BOOL         audio_active;       /* TRUE if audio was requested   */

    /* Audio WAV header state (used by audio_stream_read_fn) */
    BYTE         audio_wav_hdr[44];
    int          audio_hdr_len;
    int          audio_hdr_pos;
    BOOL         audio_hdr_built;

    /* State */
    volatile BOOL stopped;
    volatile BOOL paused;
    volatile BOOL capture_mouse;  /* draw cursor in captured frames */
    volatile BOOL error_flag;
    char          last_error[2048];
    CRITICAL_SECTION cs_err;
};

/* Static buffer for errors during recorder_create. */
static char s_create_error[512];
const char *recorder_create_error(void) { return s_create_error; }

/* Global pointer so the stream callback can reach the Recorder.
   Only one recording session is active at a time. */
static Recorder *s_active_rec = NULL;

/* ── Audio loopback device detection (WASAPI) ────────────────── */

BOOL recorder_find_audio_device(wchar_t *device_name, int max_len)
{
    if (!device_name || max_len < 2) return FALSE;
    device_name[0] = L'\0';

    /* Check if the default audio render endpoint exists.
       If it does, WASAPI loopback capture is available. */
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL need_uninit = (hr == S_OK || hr == S_FALSE);

    IMMDeviceEnumerator *pEnum = NULL;
    hr = CoCreateInstance(&CLSID_MMDevEnum_, NULL, CLSCTX_ALL,
                          &IID_IMMDevEnum_, (void **)&pEnum);
    if (SUCCEEDED(hr) && pEnum) {
        IMMDevice *pDev = NULL;
        hr = pEnum->lpVtbl->GetDefaultAudioEndpoint(pEnum,
                                                     eRender, eConsole,
                                                     &pDev);
        if (SUCCEEDED(hr) && pDev) {
            wcsncpy(device_name, L"System Audio (WASAPI Loopback)",
                    max_len - 1);
            device_name[max_len - 1] = L'\0';
            pDev->lpVtbl->Release(pDev);
        }
        pEnum->lpVtbl->Release(pEnum);
    }

    if (need_uninit) CoUninitialize();
    return device_name[0] != L'\0';
}

/* ── mpv video stream callbacks ──────────────────────────────── */

static int64_t stream_read_fn(void *cookie, char *buf, uint64_t nbytes)
{
    Recorder *rec = (Recorder *)cookie;

    /* Initialise wall-clock timer on the very first call.
       This excludes mpv's startup latency from the timeline. */
    if (!rec->read_started) {
        QueryPerformanceFrequency(&rec->qpc_freq);
        QueryPerformanceCounter(&rec->qpc_read_start);
        rec->frames_served     = 0;
        rec->read_offset       = 0;
        rec->read_started      = TRUE;
        rec->read_pause_ticks  = 0;
        rec->read_was_paused   = FALSE;

        /* Grab first frame */
        EnterCriticalSection(&rec->cs_frame);
        memcpy(rec->read_buf, rec->latest_frame, rec->frame_bytes);
        LeaveCriticalSection(&rec->cs_frame);
    }

    /* If current frame fully served, advance to next. */
    if (rec->read_offset >= rec->frame_bytes) {
        rec->frames_served++;
        rec->read_offset = 0;

        /* EOF: recording finished and this frame is done */
        if (rec->stopped) return 0;

        /* Pause: block here until unpaused, track pause duration */
        if (rec->paused) {
            if (!rec->read_was_paused) {
                rec->read_was_paused = TRUE;
                QueryPerformanceCounter(&rec->read_pause_qpc);
            }
            while (rec->paused && !rec->stopped) Sleep(50);
            if (rec->stopped) return 0;
            if (rec->read_was_paused) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                rec->read_pause_ticks += now.QuadPart
                                         - rec->read_pause_qpc.QuadPart;
                rec->read_was_paused = FALSE;
            }
        }

        /* Wall-clock pacing: sleep until this frame's presentation time.
           This is the key mechanism: we block mpv's demuxer thread so
           it reads frames at exactly 1/fps intervals, matching real
           time regardless of encoder speed or capture speed. */
        double next_sec = (double)rec->frames_served / (double)rec->fps;
        for (;;) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsed = (double)(now.QuadPart
                                      - rec->qpc_read_start.QuadPart
                                      - rec->read_pause_ticks)
                             / (double)rec->qpc_freq.QuadPart;
            double wait = next_sec - elapsed;
            if (wait <= 0.001) break;   /* close enough */
            if (rec->stopped) return 0;
            DWORD ms = (DWORD)(wait * 1000.0);
            if (ms > 100) ms = 100;     /* stay responsive to stop */
            if (ms < 1) break;
            Sleep(ms);
        }

        /* Grab latest captured frame */
        EnterCriticalSection(&rec->cs_frame);
        memcpy(rec->read_buf, rec->latest_frame, rec->frame_bytes);
        LeaveCriticalSection(&rec->cs_frame);
    }

    /* Serve bytes from current frame */
    size_t remain = rec->frame_bytes - rec->read_offset;
    size_t chunk  = ((size_t)nbytes < remain) ? (size_t)nbytes : remain;
    memcpy(buf, rec->read_buf + rec->read_offset, chunk);
    rec->read_offset += chunk;
    return (int64_t)chunk;
}

static int64_t stream_seek_fn(void *cookie, int64_t offset)
{
    (void)cookie;
    /* Rawvideo streams aren't seekable.  mpv probes seekability
       by seeking to 0 right after open — we allow that one case. */
    if (offset == 0) return 0;
    return MPV_ERROR_UNSUPPORTED;
}

static int64_t stream_size_fn(void *cookie)
{
    (void)cookie;
    return MPV_ERROR_UNSUPPORTED;  /* unknown / infinite stream */
}

static void stream_close_fn(void *cookie)
{
    (void)cookie;
}

static void stream_cancel_fn(void *cookie)
{
    Recorder *rec = (Recorder *)cookie;
    rec->stopped = TRUE;
}

static int stream_open_fn(void *user_data, char *uri,
                          mpv_stream_cb_info *info)
{
    (void)uri;
    Recorder *rec = (Recorder *)user_data;

    info->cookie    = rec;
    info->read_fn   = stream_read_fn;
    info->seek_fn   = stream_seek_fn;
    info->size_fn   = stream_size_fn;
    info->close_fn  = stream_close_fn;
    info->cancel_fn = stream_cancel_fn;
    return 0;
}

/* ── mpv audio stream callbacks ──────────────────────────────── */

static int64_t audio_stream_read_fn(void *cookie, char *buf, uint64_t nbytes)
{
    Recorder *rec = (Recorder *)cookie;

    /* Wait for the audio capture thread to finish WASAPI init
       and fill in the format info + create the ring buffer. */
    if (!rec->audio_hdr_built) {
        DWORD wait = WaitForSingleObject(rec->hAudioFormatReady, 2000);
        if (wait != WAIT_OBJECT_0 || rec->audio_init_failed) {
            return 0;   /* EOF — mpv will proceed without audio */
        }
        build_wav_header(rec->audio_wav_hdr,
                         rec->audio_rate, rec->audio_channels, 16);
        rec->audio_hdr_len   = 44;
        rec->audio_hdr_pos   = 0;
        rec->audio_hdr_built = TRUE;
    }

    /* Serve WAV header bytes first */
    if (rec->audio_hdr_pos < rec->audio_hdr_len) {
        int rem = rec->audio_hdr_len - rec->audio_hdr_pos;
        int n = (int)((nbytes < (uint64_t)rem) ? nbytes : (uint64_t)rem);
        memcpy(buf, rec->audio_wav_hdr + rec->audio_hdr_pos, n);
        rec->audio_hdr_pos += n;
        return n;
    }

    /* Then PCM data from the audio ring buffer */
    return ring_read(rec->audio_ring, buf, (size_t)nbytes);
}

static int64_t audio_stream_seek_fn(void *cookie, int64_t offset)
{
    (void)cookie;
    if (offset == 0) return 0;
    return MPV_ERROR_UNSUPPORTED;
}

static int64_t audio_stream_size_fn(void *cookie)
{
    (void)cookie;
    return MPV_ERROR_UNSUPPORTED;
}

static void audio_stream_close_fn(void *cookie)
{
    (void)cookie;
}

static void audio_stream_cancel_fn(void *cookie)
{
    Recorder *rec = (Recorder *)cookie;
    if (rec->audio_ring) {
        rec->audio_ring->cancel = TRUE;
        SetEvent(rec->audio_ring->hDataReady);
    }
    /* Also signal format event so read_fn doesn't hang */
    if (rec->hAudioFormatReady)
        SetEvent(rec->hAudioFormatReady);
}

static int audio_stream_open_fn(void *user_data, char *uri,
                                mpv_stream_cb_info *info)
{
    Recorder *rec = (Recorder *)user_data;

    info->cookie    = rec;
    info->read_fn   = audio_stream_read_fn;
    info->seek_fn   = audio_stream_seek_fn;
    info->size_fn   = audio_stream_size_fn;
    info->close_fn  = audio_stream_close_fn;
    info->cancel_fn = audio_stream_cancel_fn;
    return 0;
}

/* ── Video capture thread ────────────────────────────────────── */

static DWORD WINAPI capture_thread_proc(LPVOID param)
{
    Recorder *rec = (Recorder *)param;
    DWORD frame_ms = 1000 / rec->fps;

    while (!rec->stopped) {
        if (rec->paused) {
            Sleep(50);
            continue;
        }

        DWORD t0 = GetTickCount();

        /* Grab one frame via GDI */
        if (!BitBlt(rec->hdcMem, 0, 0, rec->capW, rec->capH,
                    rec->hdcScreen, rec->capX, rec->capY, SRCCOPY))
        {
            DWORD e = GetLastError();
            EnterCriticalSection(&rec->cs_err);
            snprintf(rec->last_error, sizeof(rec->last_error),
                     "Screen capture (BitBlt) failed: error %lu", e);
            LeaveCriticalSection(&rec->cs_err);
            rec->error_flag = TRUE;
            rec->stopped    = TRUE;
            break;
        }

        /* Draw the mouse cursor onto the captured frame.
           BitBlt+SRCCOPY does not include the cursor, so we
           overlay it manually using GetCursorInfo + DrawIconEx. */
        if (rec->capture_mouse) {
            CURSORINFO ci;
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                int cx = ci.ptScreenPos.x - rec->capX;
                int cy = ci.ptScreenPos.y - rec->capY;
                ICONINFO ii;
                if (GetIconInfo(ci.hCursor, &ii)) {
                    cx -= (int)ii.xHotspot;
                    cy -= (int)ii.yHotspot;
                    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                }
                DrawIconEx(rec->hdcMem, cx, cy, ci.hCursor,
                           0, 0, 0, NULL, DI_NORMAL);
            }
        }

        /* Publish frame to double buffer.  stream_read_fn grabs a
           copy under the same critical section when it's ready. */
        EnterCriticalSection(&rec->cs_frame);
        memcpy(rec->latest_frame, rec->pBits, rec->frame_bytes);
        LeaveCriticalSection(&rec->cs_frame);

        /* Frame-rate pacing */
        DWORD elapsed = GetTickCount() - t0;
        if (elapsed < frame_ms) Sleep(frame_ms - elapsed);
    }

    return 0;
}

/* ── Audio capture thread (WASAPI loopback) ──────────────────── */

static DWORD WINAPI audio_capture_thread_proc(LPVOID param)
{
    Recorder *rec = (Recorder *)param;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE && hr != S_FALSE) {
        rec->audio_init_failed = TRUE;
        SetEvent(rec->hAudioFormatReady);
        return 1;
    }
    BOOL need_uninit = (hr == S_OK || hr == S_FALSE);

    IMMDeviceEnumerator *pEnum    = NULL;
    IMMDevice           *pDev     = NULL;
    IAudioClient        *pClient  = NULL;
    IAudioCaptureClient *pCapture = NULL;
    WAVEFORMATEX        *pwfx     = NULL;

    /* Get default render endpoint (speakers) for loopback capture. */
    hr = CoCreateInstance(&CLSID_MMDevEnum_, NULL, CLSCTX_ALL,
                          &IID_IMMDevEnum_, (void **)&pEnum);
    if (FAILED(hr) || !pEnum) {
        goto fail;
    }

    hr = pEnum->lpVtbl->GetDefaultAudioEndpoint(pEnum, eRender, eConsole,
                                                 &pDev);
    if (FAILED(hr) || !pDev) {
        goto fail;
    }

    hr = pDev->lpVtbl->Activate(pDev, &IID_IAudioCli_,
                                 CLSCTX_ALL, NULL, (void **)&pClient);
    if (FAILED(hr) || !pClient) {
        goto fail;
    }

    hr = pClient->lpVtbl->GetMixFormat(pClient, &pwfx);
    if (FAILED(hr) || !pwfx) {
        goto fail;
    }

    rec->audio_rate     = (int)pwfx->nSamplesPerSec;
    rec->audio_channels = (int)pwfx->nChannels;

    /* Detect float32 source format */
    rec->audio_src_float = FALSE;
    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        rec->audio_src_float = TRUE;
    } else if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pwfx->cbSize >= 22) {
        /* WAVEFORMATEXTENSIBLE: SubFormat is at offset 24 in the struct
           (after WAVEFORMATEX + the Samples union + dwChannelMask).
           But it's safer to cast: */
        BYTE *extra = (BYTE *)pwfx + sizeof(WAVEFORMATEX);
        /* Samples (2) + dwChannelMask (4) + SubFormat (16) */
        GUID *sub = (GUID *)(extra + 2 + 4);
        if (memcmp(sub, &GUID_KSDATAFORMAT_IEEE_FLOAT_, sizeof(GUID)) == 0)
            rec->audio_src_float = TRUE;
    }

    /* Initialise in loopback mode with event-driven capture */
    HANDLE hCaptureEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    REFERENCE_TIME bufDuration = 10000000;  /* 1 second buffer */
    hr = pClient->lpVtbl->Initialize(pClient,
                                      AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_LOOPBACK
                                        | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                      bufDuration, 0, pwfx, NULL);
    if (FAILED(hr)) {
        /* Fall back to polling mode if event callback not supported */
        CloseHandle(hCaptureEvent);
        hCaptureEvent = NULL;
        hr = pClient->lpVtbl->Initialize(pClient,
                                          AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_LOOPBACK,
                                          bufDuration, 0, pwfx, NULL);
    }
    if (FAILED(hr)) {
        if (hCaptureEvent) CloseHandle(hCaptureEvent);
        goto fail;
    }

    if (hCaptureEvent) {
        hr = pClient->lpVtbl->SetEventHandle(pClient, hCaptureEvent);
        if (FAILED(hr)) {
            CloseHandle(hCaptureEvent);
            hCaptureEvent = NULL;
        }
    }

    hr = pClient->lpVtbl->GetService(pClient, &IID_IAudioCapCli_,
                                      (void **)&pCapture);
    if (FAILED(hr) || !pCapture) {
        goto fail;
    }

    /* Create audio ring buffer: ~30 seconds of 16-bit PCM.
       We need a large buffer because mpv's encoder may burst on
       video keyframes and temporarily fall behind on audio reads.
       With frame duplication the video stream is busier, so the
       audio consumer can fall further behind — 30 s headroom
       prevents data drops from ring_try_write. */
    {
        size_t one_sec = (size_t)rec->audio_rate * rec->audio_channels * 2;
        /* ring_create multiplies by RING_FRAME_SLOTS (4), so pass
           7.5 seconds to get 30 seconds total capacity. */
        rec->audio_ring = ring_create(one_sec * 15 / 2);
    }
    if (!rec->audio_ring) {
        goto fail;
    }

    /* Pre-fill ring with ~0.5 s of silence so mpv’s audio demuxer
       can probe the format without blocking on live WASAPI data.
       Without this, WASAPI loopback produces no packets when the
       system is silent, stalling the entire encoding pipeline for
       10–40 seconds until something generates audio. */
    {
        size_t prefill = (size_t)rec->audio_rate
                         * rec->audio_channels * 2 / 2;  /* 0.5 s */
        BYTE *zeros = (BYTE *)calloc(1, prefill);
        if (zeros) {
            ring_try_write(rec->audio_ring, zeros, prefill);
            free(zeros);
        }
    }

    /* Signal that format info and ring buffer are ready */
    SetEvent(rec->hAudioFormatReady);

    /* Start capture loop */
    hr = pClient->lpVtbl->Start(pClient);
    if (FAILED(hr)) {
        ring_set_eof(rec->audio_ring);
        goto cleanup;
    }

    /* Pre-allocate a staging buffer for one full WASAPI buffer period.
       Query the actual buffer size (in frames) from the audio client. */
    UINT32 audioBufFrames = 0;
    pClient->lpVtbl->GetBufferSize(pClient, &audioBufFrames);
    size_t stage_cap = (size_t)audioBufFrames * rec->audio_channels * 2;
    BYTE *stage_buf = (BYTE *)malloc(stage_cap);
    if (!stage_buf) {
        ring_set_eof(rec->audio_ring);
        if (hCaptureEvent) CloseHandle(hCaptureEvent);
        goto cleanup;
    }

    BYTE silence[4096];
    ZeroMemory(silence, sizeof(silence));

    /* Bytes of 16-bit PCM per ~100 ms (the WASAPI wait interval).
       When no system audio is playing, WASAPI loopback produces zero
       packets.  We inject this much silence each iteration so mpv's
       audio reader never starves — without this the encoding pipeline
       stalls until something generates sound. */
    size_t silence_per_tick = (size_t)rec->audio_rate
                              * rec->audio_channels * 2 / 10;  /* 100 ms */

    while (!rec->stopped) {
        /* Wait for WASAPI to signal data (event) or poll fallback */
        if (hCaptureEvent)
            WaitForSingleObject(hCaptureEvent, 100);
        else
            Sleep(10);

        UINT32 pktLen = 0;
        pCapture->lpVtbl->GetNextPacketSize(pCapture, &pktLen);

        BOOL got_data = FALSE;

        while (pktLen > 0 && !rec->stopped) {
            BYTE   *data      = NULL;
            UINT32  numFrames = 0;
            DWORD   flags     = 0;

            hr = pCapture->lpVtbl->GetBuffer(pCapture, &data,
                                              &numFrames, &flags,
                                              NULL, NULL);
            if (FAILED(hr)) { pktLen = 0; break; }

            got_data = TRUE;
            size_t out_bytes = (size_t)numFrames * rec->audio_channels * 2;

            if (rec->paused || (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                /* Release WASAPI buffer immediately — nothing to keep */
                pCapture->lpVtbl->ReleaseBuffer(pCapture, numFrames);

                /* Write silence for the SILENT case (paused just discards) */
                if (!rec->paused && (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    size_t rem = out_bytes;
                    while (rem > 0 && !rec->stopped) {
                        size_t c = (rem < sizeof(silence)) ? rem : sizeof(silence);
                        ring_try_write(rec->audio_ring, silence, c);
                        rem -= c;
                    }
                }
            } else if (rec->audio_src_float) {
                /* float32 → int16 conversion into pre-allocated stage_buf.
                   Release WASAPI buffer immediately afterwards. */
                size_t nsamples = (size_t)numFrames * rec->audio_channels;
                float *src = (float *)data;
                int16_t *dst = (int16_t *)stage_buf;
                for (size_t i = 0; i < nsamples; i++) {
                    float s = src[i];
                    if (s >  1.0f) s =  1.0f;
                    if (s < -1.0f) s = -1.0f;
                    dst[i] = (int16_t)(s * 32767.0f);
                }
                pCapture->lpVtbl->ReleaseBuffer(pCapture, numFrames);
                /* Non-blocking write — drops audio if ring full rather
                   than stalling the WASAPI drain loop */
                if (!ring_try_write(rec->audio_ring, stage_buf, out_bytes))
                    { /* ring full — dropped */ }
            } else {
                /* int16 pass-through: copy to staging, release, then ring */
                memcpy(stage_buf, data, out_bytes);
                pCapture->lpVtbl->ReleaseBuffer(pCapture, numFrames);
                if (!ring_try_write(rec->audio_ring, stage_buf, out_bytes))
                    { /* ring full — dropped */ }
            }

            pCapture->lpVtbl->GetNextPacketSize(pCapture, &pktLen);
        }

        /* No WASAPI data this iteration — inject silence so mpv's
           audio demuxer never blocks waiting for ring data. */
        if (!got_data && !rec->paused && !rec->stopped) {
            size_t rem = silence_per_tick;
            while (rem > 0) {
                size_t c = (rem < sizeof(silence)) ? rem : sizeof(silence);
                ring_try_write(rec->audio_ring, silence, c);
                rem -= c;
            }
        }
    }

    pClient->lpVtbl->Stop(pClient);
    ring_set_eof(rec->audio_ring);
    free(stage_buf);
    if (hCaptureEvent) CloseHandle(hCaptureEvent);
    goto cleanup;

fail:
    rec->audio_init_failed = TRUE;
    SetEvent(rec->hAudioFormatReady);

cleanup:
    if (pwfx)     CoTaskMemFree(pwfx);
    if (pCapture)  pCapture->lpVtbl->Release(pCapture);
    if (pClient)   pClient->lpVtbl->Release(pClient);
    if (pDev)      pDev->lpVtbl->Release(pDev);
    if (pEnum)     pEnum->lpVtbl->Release(pEnum);
    if (need_uninit) CoUninitialize();
    return rec->audio_init_failed ? 1 : 0;
}

/* ── Lifecycle ───────────────────────────────────────────────── */

/* Helper: shut down audio capture thread if it was started during
   recorder_create but an error forces early cleanup.  Safe to call
   even when the thread was never started (all fields are NULL/0). */
static void cleanup_audio_early(Recorder *rec)
{
    if (rec->hAudioThread) {
        rec->stopped = TRUE;
        if (rec->audio_ring) ring_set_eof(rec->audio_ring);
        WaitForSingleObject(rec->hAudioThread, 3000);
        CloseHandle(rec->hAudioThread);
        rec->hAudioThread = NULL;
    }
    if (rec->audio_ring) {
        ring_destroy(rec->audio_ring);
        rec->audio_ring = NULL;
    }
    if (rec->hAudioFormatReady) {
        CloseHandle(rec->hAudioFormatReady);
        rec->hAudioFormatReady = NULL;
    }
}

Recorder *recorder_create(const RECT *capture_rect,
                          const char *output_path,
                          int fps,
                          const char *audio_device,
                          BOOL capture_mouse)
{
    if (!capture_rect || !output_path) return NULL;
    s_create_error[0] = '\0';

    int w = (int)(capture_rect->right  - capture_rect->left);
    int h = (int)(capture_rect->bottom - capture_rect->top);
    if (fps <= 0) fps = REC_DEFAULT_FPS;

    /* ── Allocate recorder early (needed for stream callback) ── */
    Recorder *rec = (Recorder *)calloc(1, sizeof(*rec));
    if (!rec) return NULL;

    rec->capX = (int)capture_rect->left;
    rec->capY = (int)capture_rect->top;
    rec->capW = w;
    rec->capH = h;
    rec->fps  = fps;
    rec->stopped    = FALSE;
    rec->paused     = FALSE;
    rec->capture_mouse = capture_mouse;
    rec->error_flag = FALSE;
    rec->audio_active       = FALSE;
    rec->audio_init_failed  = FALSE;
    rec->audio_hdr_built    = FALSE;
    InitializeCriticalSection(&rec->cs_err);

    /* ── Allocate video double-buffer ────────────────────────── */
    size_t frame_bytes = (size_t)w * h * 4;   /* BGRA */
    rec->frame_bytes   = frame_bytes;
    rec->latest_frame  = (BYTE *)calloc(1, frame_bytes);
    rec->read_buf      = (BYTE *)calloc(1, frame_bytes);
    if (!rec->latest_frame || !rec->read_buf) {
        snprintf(s_create_error, sizeof(s_create_error),
                 "Failed to allocate frame buffers (%zu bytes each)",
                 frame_bytes);
        free(rec->latest_frame);
        free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }
    InitializeCriticalSection(&rec->cs_frame);
    rec->read_started = FALSE;

    /* ── Start WASAPI audio init early (runs in parallel with mpv
       setup below — typically saves 2–5 s of init latency) ──── */
    if (audio_device && audio_device[0]) {
        rec->hAudioFormatReady = CreateEventW(NULL, TRUE, FALSE, NULL);
        rec->hAudioThread = CreateThread(NULL, 0,
                                          audio_capture_thread_proc,
                                          rec, 0, NULL);
        if (!rec->hAudioThread) {
            rec->audio_init_failed = TRUE;
            SetEvent(rec->hAudioFormatReady);
        }
    }

    /* ── Create mpv instance ─────────────────────────────────── */
    mpv_handle *mpv = mpv_create();
    if (!mpv) {
        snprintf(s_create_error, sizeof(s_create_error), "mpv_create() failed");
        cleanup_audio_early(rec);
        free(rec->latest_frame); free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_frame);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }
    rec->mpv = mpv;

    /* ── Register custom stream protocols ────────────────────── */
    int rc = mpv_stream_cb_add_ro(mpv, "capture", rec, stream_open_fn);
    if (rc < 0) {
        snprintf(s_create_error, sizeof(s_create_error),
                 "mpv_stream_cb_add_ro failed: %s", mpv_error_string(rc));
        mpv_destroy(mpv);
        cleanup_audio_early(rec);
        free(rec->latest_frame); free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_frame);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }

    /* Register audio stream protocol.  The WASAPI capture thread was
       started above and is initialising in parallel.  hAudioFormatReady
       will be signalled when the format info is ready. */
    if (rec->hAudioFormatReady) {
        rc = mpv_stream_cb_add_ro(mpv, "capture-audio", rec,
                                   audio_stream_open_fn);
        if (rc >= 0) {
            rec->audio_active = TRUE;
        }
        /* If registration fails the thread still runs harmlessly —
           it will be joined in cleanup_audio_early / recorder_destroy. */
    }

    /* ── Configure encoding output ───────────────────────────── */
    rc = mpv_set_option_string(mpv, "o", output_path);

    rc = mpv_set_option_string(mpv, "of", "mp4");

    rc = mpv_set_option_string(mpv, "ovc", REC_DEFAULT_CODEC);
    if (rc < 0) {
        snprintf(s_create_error, sizeof(s_create_error),
                 "Codec '%s' not available: %s",
                 REC_DEFAULT_CODEC, mpv_error_string(rc));
        mpv_destroy(mpv);
        cleanup_audio_early(rec);
        free(rec->latest_frame); free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_frame);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }

    char ovcopts[128];
    snprintf(ovcopts, sizeof(ovcopts), "crf=%d,preset=%s",
             REC_DEFAULT_CRF, REC_DEFAULT_PRESET);
    rc = mpv_set_option_string(mpv, "ovcopts", ovcopts);

    /* ── Configure rawvideo demuxer for our custom stream ────── */
    mpv_set_option_string(mpv, "demuxer", "rawvideo");

    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%d", w);
    mpv_set_option_string(mpv, "demuxer-rawvideo-w", tmp);
    snprintf(tmp, sizeof(tmp), "%d", h);
    mpv_set_option_string(mpv, "demuxer-rawvideo-h", tmp);
    snprintf(tmp, sizeof(tmp), "%d", fps);
    mpv_set_option_string(mpv, "demuxer-rawvideo-fps", tmp);
    /* BGR0 = 32-bit with blue in the lowest byte, alpha ignored.
       Using bgr0 instead of bgra prevents mpv from picking yuva420p
       which libx265 cannot encode (no alpha support). */
    mpv_set_option_string(mpv, "demuxer-rawvideo-mp-format", "bgr0");

    /* Force conversion to yuv420p before the encoder.  Without this
       mpv auto-converts bgr0 → gbrp (planar RGB 4:4:4) which libx265
       technically accepts but produces files with wrong colors in most
       players (unusual HEVC matrix_coefficients). */
    mpv_set_option_string(mpv, "vf", "format=fmt=yuv420p");

    /* ── Audio encoding (WASAPI loopback via stream callback) ── */
    if (rec->audio_active) {
        rc = mpv_set_option_string(mpv, "oac", "aac");
        rc = mpv_set_option_string(mpv, "oacopts", "b=192k");
        /* "audio-file" is a CLI alias, not valid in the C API.
           The real option name is "audio-files" (a path list). */
        rc = mpv_set_option_string(mpv, "audio-files",
                                    "capture-audio://loopback");
    }

    /* ── Misc ────────────────────────────────────────────────── */
    mpv_set_option_string(mpv, "terminal",                "no");
    mpv_set_option_string(mpv, "audio-display",           "no");
    mpv_set_option_string(mpv, "input-default-bindings",  "no");
    mpv_set_option_string(mpv, "input-vo-keyboard",       "no");

    /* ── Minimize startup latency ────────────────────────────── */
    mpv_set_option_string(mpv, "cache",                   "no");
    mpv_set_option_string(mpv, "demuxer-readahead-secs",  "0");
    mpv_set_option_string(mpv, "demuxer-max-bytes",       "65536");
    mpv_set_option_string(mpv, "demuxer-max-back-bytes",  "0");

    /* ── Initialise mpv ──────────────────────────────────────── */
    int init_rc = mpv_initialize(mpv);
    if (init_rc < 0) {
        snprintf(s_create_error, sizeof(s_create_error),
                 "mpv_initialize failed: %s", mpv_error_string(init_rc));
        mpv_destroy(mpv);
        cleanup_audio_early(rec);
        free(rec->latest_frame); free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_frame);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }

    mpv_request_log_messages(mpv, "v");

    /* ── Setup GDI capture ───────────────────────────────────── */
    rec->hdcScreen = GetDC(NULL);
    rec->hdcMem    = CreateCompatibleDC(rec->hdcScreen);

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = w;
    bmi.bmiHeader.biHeight      = -h;   /* top-down DIB */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    rec->hBitmap = CreateDIBSection(rec->hdcMem, &bmi, DIB_RGB_COLORS,
                                     &rec->pBits, NULL, 0);
    if (!rec->hBitmap || !rec->pBits) {
        snprintf(s_create_error, sizeof(s_create_error),
                 "CreateDIBSection failed: %lu", GetLastError());
        mpv_terminate_destroy(mpv);
        cleanup_audio_early(rec);
        free(rec->latest_frame); free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_frame);
        DeleteDC(rec->hdcMem);
        ReleaseDC(NULL, rec->hdcScreen);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }
    SelectObject(rec->hdcMem, rec->hBitmap);

    /* ── Start capture threads ───────────────────────────────── */
    rec->hCaptureThread = CreateThread(NULL, 0, capture_thread_proc,
                                       rec, 0, NULL);
    if (!rec->hCaptureThread) {
        snprintf(s_create_error, sizeof(s_create_error),
                 "CreateThread (video) failed: %lu", GetLastError());
        mpv_terminate_destroy(mpv);
        cleanup_audio_early(rec);
        free(rec->latest_frame); free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_frame);
        DeleteObject(rec->hBitmap);
        DeleteDC(rec->hdcMem);
        ReleaseDC(NULL, rec->hdcScreen);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }

    /* ── Wait for WASAPI init (started at the top of this function) ─ */
    if (rec->audio_active && rec->hAudioFormatReady) {
        WaitForSingleObject(rec->hAudioFormatReady, 5000);
        if (rec->audio_init_failed) {
            rec->audio_active = FALSE;
        }
    }

    /* ── Tell mpv to open our custom stream ──────────────────── */
    s_active_rec = rec;
    const char *cmd[] = {"loadfile", "capture://desktop", NULL};
    int load_rc = mpv_command(mpv, cmd);
    if (load_rc < 0) {
        snprintf(s_create_error, sizeof(s_create_error),
                 "loadfile failed: %s", mpv_error_string(load_rc));
        rec->stopped = TRUE;
        WaitForSingleObject(rec->hCaptureThread, 5000);
        CloseHandle(rec->hCaptureThread);
        cleanup_audio_early(rec);
        mpv_terminate_destroy(mpv);
        free(rec->latest_frame); free(rec->read_buf);
        DeleteCriticalSection(&rec->cs_frame);
        DeleteObject(rec->hBitmap);
        DeleteDC(rec->hdcMem);
        ReleaseDC(NULL, rec->hdcScreen);
        DeleteCriticalSection(&rec->cs_err);
        free(rec);
        return NULL;
    }

    return rec;
}

/* ── Polling ─────────────────────────────────────────────────── */

int recorder_poll(Recorder *rec)
{
    if (!rec || !rec->mpv) return -1;
    if (rec->error_flag) return -1;

    /* Drain mpv events */
    for (;;) {
        mpv_event *ev = mpv_wait_event(rec->mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;

        if (ev->event_id == MPV_EVENT_LOG_MESSAGE) {
            /* ignored */
        }
        else if (ev->event_id == MPV_EVENT_SHUTDOWN) {
            rec->stopped = TRUE;
            return 1;
        }
        else if (ev->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *ef = (mpv_event_end_file *)ev->data;
            int is_err = (ef && ef->reason == MPV_END_FILE_REASON_ERROR);

            if (is_err) {
                rec->error_flag = TRUE;
                rec->stopped    = TRUE;
                /* Drain log messages for diagnostics */
                for (int j = 0; j < 50; j++) {
                    mpv_event *lev = mpv_wait_event(rec->mpv, 0.05);
                    if (lev->event_id == MPV_EVENT_NONE) break;
                    if (lev->event_id == MPV_EVENT_LOG_MESSAGE) {
                        mpv_event_log_message *lm =
                            (mpv_event_log_message *)lev->data;
                        if (lm && lm->text) {
                            EnterCriticalSection(&rec->cs_err);
                            size_t cur = strlen(rec->last_error);
                            size_t rem = sizeof(rec->last_error) - cur - 1;
                            if (rem > 2) {
                                snprintf(rec->last_error + cur, rem,
                                         "[%s] %s",
                                         lm->prefix ? lm->prefix : "?",
                                         lm->text);
                            }
                            LeaveCriticalSection(&rec->cs_err);
                        }
                    }
                }
                return -1;
            }
            rec->stopped = TRUE;
            return 1;
        }
    }
    return 0;
}

/* ── Pause / resume ──────────────────────────────────────────── */

void recorder_pause(Recorder *rec)
{
    if (!rec || rec->stopped || rec->paused) return;
    rec->paused = TRUE;
}

void recorder_resume(Recorder *rec)
{
    if (!rec || rec->stopped || !rec->paused) return;
    rec->paused = FALSE;
}

void recorder_set_mouse_capture(Recorder *rec, BOOL enable)
{
    if (rec) {
        rec->capture_mouse = enable;
    }
}

BOOL recorder_get_mouse_capture(Recorder *rec)
{
    return rec ? rec->capture_mouse : TRUE;
}

BOOL recorder_is_paused(Recorder *rec)
{
    return rec ? rec->paused : FALSE;
}

const char *recorder_last_error(Recorder *rec)
{
    return rec ? rec->last_error : "";
}

BOOL recorder_active(Recorder *rec)
{
    return rec ? rec->read_started : FALSE;
}

/* ── Stop ────────────────────────────────────────────────────── */

void recorder_stop(Recorder *rec)
{
    if (!rec) return;

    /* Mark stopped — safe to call twice. */
    BOOL was_stopped = rec->stopped;
    rec->stopped = TRUE;

    /* Signal EOF on audio ring buffer FIRST so mpv's audio reader
       unblocks and both streams can reach EOF concurrently.  Without
       this, mpv blocks on the audio read and never emits END_FILE. */
    if (rec->audio_ring) ring_set_eof(rec->audio_ring);

    if (!was_stopped) {
        /* Wait for video capture thread to finish */
        if (rec->hCaptureThread) {
            WaitForSingleObject(rec->hCaptureThread, 5000);
            CloseHandle(rec->hCaptureThread);
            rec->hCaptureThread = NULL;
        }

        /* Wait for audio capture thread to finish */
        if (rec->hAudioThread) {
            WaitForSingleObject(rec->hAudioThread, 5000);
            CloseHandle(rec->hAudioThread);
            rec->hAudioThread = NULL;
        }
    }

    /* Wait for mpv to finish encoding.  After both streams return EOF
       mpv flushes the encoder, writes remaining packets, and emits
       MPV_EVENT_END_FILE.  Only THEN do we send "quit" to finalise
       the MP4 container (moov atom).  Without this wait, "quit"
       interrupts mid-encode and the tail of the recording is lost.

       NOTE: recorder_poll() on the UI timer may have already consumed
       END_FILE and set stopped=TRUE before we get here.  We still
       need to send "quit" — otherwise mpv_terminate_destroy kills
       the muxer before it writes the moov atom. */
    if (rec->mpv) {
        /* If stopped was already set externally (e.g. by poll),
           END_FILE was already consumed — skip the wait. */
        if (!was_stopped) {
            for (int i = 0; i < 300; i++) {   /* up to 30 seconds */
                mpv_event *ev = mpv_wait_event(rec->mpv, 0.1);
                if (ev->event_id == MPV_EVENT_NONE)     continue;
                if (ev->event_id == MPV_EVENT_END_FILE) break;
                if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
            }
        }

        /* Always send quit so mpv writes the moov atom. */
        {
            const char *cmd[] = {"quit", NULL};
            mpv_command(rec->mpv, cmd);
        }

        /* Drain remaining events until SHUTDOWN */
        for (int i = 0; i < 50; i++) {
            mpv_event *ev = mpv_wait_event(rec->mpv, 0.2);
            if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
        }
    }
}

/* ── Destroy ─────────────────────────────────────────────────── */

void recorder_destroy(Recorder *rec)
{
    if (!rec) return;
    if (!rec->stopped) recorder_stop(rec);

    if (rec->mpv) {
        mpv_terminate_destroy(rec->mpv);
        rec->mpv = NULL;
    }

    /* GDI resources */
    if (rec->hBitmap)   { DeleteObject(rec->hBitmap);     rec->hBitmap   = NULL; }
    if (rec->hdcMem)    { DeleteDC(rec->hdcMem);           rec->hdcMem    = NULL; }
    if (rec->hdcScreen) { ReleaseDC(NULL, rec->hdcScreen); rec->hdcScreen = NULL; }

    /* Video double-buffer — safe now that all threads + mpv are done */
    free(rec->latest_frame);
    rec->latest_frame = NULL;
    free(rec->read_buf);
    rec->read_buf = NULL;
    DeleteCriticalSection(&rec->cs_frame);

    /* Audio ring buffer */
    ring_destroy(rec->audio_ring);
    rec->audio_ring = NULL;

    /* Audio event */
    if (rec->hAudioFormatReady) {
        CloseHandle(rec->hAudioFormatReady);
        rec->hAudioFormatReady = NULL;
    }

    if (s_active_rec == rec) s_active_rec = NULL;

    DeleteCriticalSection(&rec->cs_err);
    free(rec);
}
