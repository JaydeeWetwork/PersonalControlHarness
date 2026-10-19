/* stb_image implementation unit (PNG only; third-party code, warnings off). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_MAX_DIMENSIONS (1 << 14)  /* a corrupt header cannot ask for gigabytes */
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"
