// cgltf.cpp -- single translation unit that emits the cgltf v1.15
// implementation.  RISE uses cgltf to parse glTF 2.0 (.gltf and .glb)
// files for the gltfmesh_geometry chunk (see
// src/Library/Geometry/TriangleMeshLoaderGLTF.{h,cpp}); every other
// TU just includes "cgltf.h" without defining CGLTF_IMPLEMENTATION.
//
// File extension is .cpp rather than the upstream-conventional .c so
// the existing make / cmake / vcxproj / pbxproj recipes -- all
// C++-only -- pick it up without bespoke C-language rules.  cgltf is
// happy to compile as either; its declarations are wrapped in
// `extern "C"`.

// Suppress vendored-code warnings inside this TU only.  CLAUDE.md's
// "compiler warnings are bugs" rule applies to RISE source; modifying
// cgltf upstream would defeat the point of vendoring.  Scope is
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

#define CGLTF_IMPLEMENTATION

#include "cgltf.h"

#if defined(__clang__)
	#pragma clang diagnostic pop
#elif defined(__GNUC__)
	#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
	#pragma warning(pop)
#endif
