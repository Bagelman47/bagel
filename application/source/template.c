#include <stdlib.h>
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include "Bagel_jpg.h"

int main(int argc, char **argv) {
	// get the graphics and controllers working
	GRRLIB_Init();
	WPAD_Init();

	// Load the embedded JPEG into a texture
	GRRLIB_texImg *bagel = GRRLIB_LoadTexture(Bagel_jpg);

	while (SYS_MainLoop()) {
		WPAD_ScanPads();

		// close the bagel when home is pressed
		if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME) break;

		// open the bagel and show the bagel
		if (bagel) {
			f32 x = ((int)rmode->fbWidth   - (int)bagel->w) / 2.0f;
			f32 y = ((int)rmode->efbHeight - (int)bagel->h) / 2.0f;
			GRRLIB_DrawImg(x, y, bagel, 0, 1, 1, 0xFFFFFFFF);
		}

		GRRLIB_Render();
	}

	// get resources back and close grrlib
	GRRLIB_FreeTexture(bagel);
	GRRLIB_Exit();

	exit(0);
}
