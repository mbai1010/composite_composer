/*
 * Programming introduction with the SOD Embedded Image Processing API.
 * Copyright (C) PixLab | Symisc Systems, https://sod.pixlab.io
 */
/*
* Compile this file together with the SOD embedded source code to generate
* the executable. For example:
*
*  gcc sod.c canny_edge_detection.c -lm -Ofast -march=native -Wall -std=c99 -o sod_img_proc
*  
* Under Microsoft Visual Studio (>= 2015), just drop `sod.c` and its accompanying
* header files on your source tree and you're done. If you have any trouble
* integrating SOD in your project, please submit a support request at:
* https://sod.pixlab.io/support.html
*/
/*
* This simple program is a quick introduction on how to embed and start
* experimenting with SOD without having to do a lot of tedious
* reading and configuration.
*
* Make sure you have the latest release of SOD from:
*  https://pixlab.io/downloads
* The SOD Embedded C/C++ documentation is available at:
*  https://sod.pixlab.io/api.html
*/
#include <stdio.h>
#include "sod.h"


/*
 * Perform canny edge detection on an input image.
 */
int main(int argc, char *argv[])
{
	/* Input image (pass a path or use the test image shipped with the samples ZIP archive) */
	//const char *zInput = argc > 1 ? argv[1] : "./canny.jpg";
	/* Processed output image path */
	const char *zOut = argc > 1 ? argv[1] : "./out_canny.png";
	sod_img imgIn;
	imgIn.w = 632;
	imgIn.h = 395;
	imgIn.c = 1;
	imgIn.data = rawData;

    //printf("Width: %d, Height: %d, Channels: %d, size: %d\n", imgIn.w, imgIn.h, imgIn.c, sizeof(imgIn.data));
	/* Perform canny edge detection. */
	sod_img imgOut = sod_canny_edge_image(imgIn, 0 /*  Set this to 1 if you want to reduce noise */);
	/* Finally save our processed image to the specified path */
	sod_img_save_as_png(imgOut, zOut);
	/* Cleanup */
	sod_free_image(imgIn);
	sod_free_image(imgOut);
	return 0;
}