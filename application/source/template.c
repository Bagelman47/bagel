#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <grrlib.h>
#include <asndlib.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h> // gettime(), ticks_to_millisecs()
#include "Bagel_jpg.h"
#include "brururrur_wav.h"
#include "frames_tpl.h"
#include "frame_delays.h"
#include "stdio.h"

#define SOUND_VOICE 0

//----------------
// wav parsing
//----------------
typedef struct {
	const u8 *pcm;     // start of the raw pcm samples (inside the built-in file)
	u32 size;          // size of the pcm data in bytes
	u32 sampleRate;    // native sample rate in hz
	s32 format;        // asnd voice format
} WavInfo;

// reading byte-by-byte here because wav is little-endian and the Wii is big-endian
static u16 rd16le(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32le(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

// walk the riff chunks to find "fmt " and "data". should say true if it works
static bool parseWav(const u8 *wav, size_t len, WavInfo *out) {
	if (!wav || !out || len < 12 || memcmp(wav, "RIFF", 4) != 0 || memcmp(wav + 8, "WAVE", 4) != 0)
		return false;

	u16 audioFormat = 0, channels = 0, bits = 0;
	u32 rate = 0;
	bool haveFmt = false;
	size_t pos = 12;

	// every read below is checked against len, using subtraction so a bogus chunk size cant wrap the position around and read out of bounds
	while (pos <= len && len - pos >= 8) {
		const u8 *chunk = wav + pos;
		u32 chunkSize = rd32le(chunk + 4);
		size_t body = pos + 8;
		size_t avail = len - body;

		if (memcmp(chunk, "fmt ", 4) == 0) {
			if (chunkSize < 16 || avail < 16) return false; // truncated fmt chunk
			audioFormat = rd16le(wav + body + 0);
			channels    = rd16le(wav + body + 2);
			rate        = rd32le(wav + body + 4);
			bits        = rd16le(wav + body + 14);
			haveFmt = true;
		} else if (memcmp(chunk, "data", 4) == 0) {
			if (!haveFmt) return false;
			// clamp in case the header claims more data than the file can hold
			out->pcm  = wav + body;
			out->size = (chunkSize > avail) ? (u32)avail : chunkSize;
			out->sampleRate = rate;

			if (audioFormat != 1) return false; // only uncompressed pcm
			if      (channels == 1 && bits == 8)  out->format = VOICE_MONO_8BIT_U;    // 8-bit wav is unsigned
			else if (channels == 2 && bits == 8)  out->format = VOICE_STEREO_8BIT_U;
			else if (channels == 1 && bits == 16) out->format = VOICE_MONO_16BIT_LE;  // 16-bit wav is little-endian
			else if (channels == 2 && bits == 16) out->format = VOICE_STEREO_16BIT_LE;
			else return false;

			// asnd accepts up to 144 khz; reject anything nonsensical
			return out->size > 0 && rate > 0 && rate <= 144000;
		}

		if (chunkSize > avail) break;
		size_t skip = (size_t)chunkSize + (chunkSize & 1);
		if (skip > avail) break; // padding byte would be past the end
		pos = body + skip;
	}
	return false;
}

//----------------
// sound playback
//----------------
// ansd needs a 32-byte aligned, 32-byte padded buffer. The pcm data inside the built-in wav starts right after the header (not aligned), so copy it once at bootup and keep it for the life of the program. freeing it right after asnd_stopvoice() is unsafe but the DSP may still be mid-read of that buffer
static u8 *soundBuf = NULL;
static u32 soundBufSize = 0;
static WavInfo soundInfo;

static bool prepareSound(void) {
	if (!parseWav(brururrur_wav, brururrur_wav_size, &soundInfo)) return false;

	u32 padded = (soundInfo.size + 31) & ~31u;
	soundBuf = memalign(32, padded);
	if (!soundBuf) return false;

	memcpy(soundBuf, soundInfo.pcm, soundInfo.size);
	memset(soundBuf + soundInfo.size, 0, padded - soundInfo.size); // silence in the padding
	DCFlushRange(soundBuf, padded);
	soundBufSize = padded;
	return true;
}

static void stopSound(void) {
	ASND_StopVoice(SOUND_VOICE);
}

static void startSound(void) {
	if (!soundBuf) return; // wav failed to parse/allocate: run silently
	ASND_StopVoice(SOUND_VOICE);
	ASND_SetVoice(SOUND_VOICE, soundInfo.format, soundInfo.sampleRate, 0,
	              soundBuf, soundBufSize, 255, 255, NULL);
}

//----------------
// Animation frames
//----------------
// the frames are pre-converted at build time to cmpr which is the gpus compressed format (4 bits per pixel), and built in the dol as one tpl. grrlib_loadtexturetpl just points at that data so there is no decoding at all: showing a frame is only a matter of drawing a different texture
static GRRLIB_texImg *frameTex[FRAME_COUNT];
static u32 frameStartMs[FRAME_COUNT]; // when each frame starts, from play start
static u32 animLengthMs = 0;

static void loadFrames(void) {
	// tpl header: magic 0x0020AF30, then the number of images (big-endian, which is the Wii's native byte order). grrlibs own id check is off by one, so bounds-check here
	u32 count = (frames_tpl_size >= 8) ? *(const u32 *)(frames_tpl + 4) : 0;

	u32 t = 0;
	for (int i = 0; i < FRAME_COUNT; i++) {
		frameTex[i] = (i < (int)count) ? GRRLIB_LoadTextureTPL(frames_tpl, i) : NULL;
		frameStartMs[i] = t;
		t += frameDelayMs[i];
	}
	animLengthMs = t;
}

static void freeFrames(void) {
	for (int i = 0; i < FRAME_COUNT; i++) {
		free(frameTex[i]); // only the struct; the pixels live in the dol
		frameTex[i] = NULL;
	}
}

static u32 nowMs(void) {
	return (u32)ticks_to_millisecs(gettime());
}

//----------------
// main
//----------------
static void drawCentered(GRRLIB_texImg *tex) {
	if (!tex) return;
	f32 x = ((int)rmode->fbWidth   - (int)tex->w) / 2.0f;
	f32 y = ((int)rmode->efbHeight - (int)tex->h) / 2.0f;
	GRRLIB_DrawImg(x, y, tex, 0, 1, 1, 0xFFFFFFFF);
}

int main(int argc, char **argv) {
	// get the graphics system and the controllers working
	GRRLIB_Init();
	WPAD_Init();

	// audio: audio_init should come before asnd_init, and asnd opens paused
	AUDIO_Init(NULL);
	ASND_Init();
	ASND_Pause(0);
	prepareSound();

	// bagel.jpg is a jpg so use the JPEG loader with its exact size
	GRRLIB_texImg *bagel = GRRLIB_LoadTextureJPGEx(Bagel_jpg, Bagel_jpg_size);

	// every frame is ready to draw
	loadFrames();

	// animation state. frames are timed against the real clock using each frames delay, so it runs at the correct speed (30 FPS or so) on both 50 hz and 60 hz video modes
	bool playing = false;
	int frameIndex = 0;
	u32 playStartMs = 0;

	while (SYS_MainLoop()) {
		WPAD_ScanPads();
		u32 pressed = WPAD_ButtonsDown(0);

		// exit the bagel when HOME is pressed
		if (pressed & WPAD_BUTTON_HOME) break;

		// B starts (or restarts) the meme and its sound
		if (pressed & WPAD_BUTTON_B) {
			playing = true;
			frameIndex = 0;
			startSound();
			playStartMs = nowMs();
		}

		if (playing) {
			u32 elapsed = nowMs() - playStartMs;
			if (elapsed >= animLengthMs) {
				// finished: back to the bagel. the sound is not cut off it is a voice and ends by itself a bit later
				playing = false;
			} else {
				// skip to the frame for the time it is on (drops frames instead of slowing down if a render ever goes late)
				while (frameIndex + 1 < FRAME_COUNT && elapsed >= frameStartMs[frameIndex + 1])
					frameIndex++;
			}
		}

		// don't pass null to grrlib_drawimg: fall back to the bagel if it needs to
		GRRLIB_texImg *toDraw = (playing && frameTex[frameIndex]) ? frameTex[frameIndex] : bagel;
		if (toDraw) drawCentered(toDraw);

		GRRLIB_Render();
	}

	// get resources and close grrlib
	stopSound();
	ASND_End();          // the buffer can stop now
	free(soundBuf);
	soundBuf = NULL;
	freeFrames();
	GRRLIB_FreeTexture(bagel);
	GRRLIB_Exit();

	// go to a console
	GXRModeObj *conMode = VIDEO_GetPreferredMode(NULL);
	void *conXfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(conMode));
	console_init(conXfb, 20, 20, conMode->fbWidth, conMode->xfbHeight,
	             conMode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(conMode);
	VIDEO_ClearFrameBuffer(conMode, conXfb, COLOR_BLACK);
	VIDEO_SetNextFramebuffer(conXfb);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if (conMode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();

	printf("\x1b[2;0H");
	printf("Bye!\n");

	u32 byeStartMs = nowMs();
	while (nowMs() - byeStartMs < 1000)
		VIDEO_WaitVSync();

	exit(0);
}
