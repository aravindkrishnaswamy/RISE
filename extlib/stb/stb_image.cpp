// stb_image.cpp -- single translation unit that emits the stb_image v2.30
// implementation.  RISE only uses stb_image to decode JPEG textures (see
// src/Library/RasterImages/JPEGReader.{h,cpp}); STBI_ONLY_JPEG keeps the
// other format-specific decoders out of the build so the binary stays
// small.  Every other TU that needs the decoder API just includes
// "stb_image.h" without defining STB_IMAGE_IMPLEMENTATION.
//
// File extension is .cpp rather than the upstream-conventional .c so the
// existing make / cmake / vcxproj / pbxproj recipes (all C++-only) pick
// it up without bespoke C-language rules.  stb_image is happy to compile
// as either; its declarations are wrapped in `extern "C"` already.

// Suppress vendored-code warnings inside this TU only.  CLAUDE.md's
// "compiler warnings are bugs" rule applies to RISE source; modifying
// stb_image upstream would defeat the point of vendoring.  Scope is
// minimal: pragmas wrap the impl-emitting include and pop right after.
#if defined(__clang__)
	#pragma clang diagnostic push
	#pragma clang diagnostic ignored "-Wunused-but-set-variable"
	#pragma clang diagnostic ignored "-Wunused-function"
	#pragma clang diagnostic ignored "-Wunused-parameter"
	#pragma clang diagnostic ignored "-Wmissing-field-initializers"
	#pragma clang diagnostic ignored "-Wcast-qual"
	#pragma clang diagnostic ignored "-Wsign-compare"
	#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
	#pragma clang diagnostic ignored "-Wdouble-promotion"
	#pragma clang diagnostic ignored "-Wcomma"
#elif defined(__GNUC__)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
	#pragma GCC diagnostic ignored "-Wunused-function"
	#pragma GCC diagnostic ignored "-Wunused-parameter"
	#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
	#pragma GCC diagnostic ignored "-Wcast-qual"
	#pragma GCC diagnostic ignored "-Wsign-compare"
	#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
	#pragma GCC diagnostic ignored "-Wdouble-promotion"
#elif defined(_MSC_VER)
	#pragma warning(push)
	#pragma warning(disable: 4244 4245 4456 4457 4459 4701 4703 4996)
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG

#include "stb_image.h"

#if defined(__clang__)
	#pragma clang diagnostic pop
#elif defined(__GNUC__)
	#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
	#pragma warning(pop)
#endif
