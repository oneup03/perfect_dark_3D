// GLSL source for stereo compose fragment shaders. Used by
// gfx_opengl_compose_stereo. Shared vertex shader emits a fullscreen triangle
// from gl_VertexID with no VBO. Fragment shaders sample two eye textures
// (uTexL / uTexR) over UV in [0,1]^2 and emit the composite per the mode.
//
// LeiaSR is not represented here — it runs through a different code path
// (the SR SDK GLWeaver) and falls back to SBS when unavailable. Note that it
// still reaches the ghost-reduction block below: the LeiaSR path composes SbS
// with THIS shader into the weaver's input texture, so the range compression
// lands before the weave, which is exactly where it has to be (see below).

#ifndef _IN_GFX_OPENGL_STEREO_SHADERS_H
#define _IN_GFX_OPENGL_STEREO_SHADERS_H

// Eye FBOs are created with opengl_invert_y=false (see gfx_resize_framebuffer
// in gfx_pc.cpp for the why — invert_y=true caused entire rooms whose
// interior faces are back-facing under inverted winding to drop out in
// stereo). FBO content is therefore stored right-side-up in texture-space
// and we sample with V matching screen-Y; gl_FragCoord row/column parity
// stays correct for the row/column/checkerboard composites.
#define STEREO_VS \
    "out vec2 vUV;\n" \
    "void main(void) {\n" \
    "  vec2 p = vec2((gl_VertexID == 2) ? 3.0 : -1.0,\n" \
    "                (gl_VertexID == 1) ? 3.0 : -1.0);\n" \
    "  vUV = vec2((p.x + 1.0) * 0.5, (p.y + 1.0) * 0.5);\n" \
    "  gl_Position = vec4(p, 0.0, 1.0);\n" \
    "}\n"

// --- Ghost / crosstalk reduction --------------------------------------------
//
// Every stereo display leaks some of each eye's image into the other. How
// visible that leak is depends on the brightness DIFFERENCE between the eyes,
// so compressing the signal range before it reaches the display reduces what
// you see — the standard range-compression approach from the stereo-crosstalk
// literature. Displays that additionally CANCEL crosstalk themselves (the
// LeiaSR weaver does) pre-subtract a fraction of the opposite eye; that
// inversion drives values past the ends of the range, where the render target
// clamps them, and the clamped part is what survives as a ghost. On such a
// panel, residual ghosting on high-contrast content (bright HUD or subtitles
// over a dark scene) means the correction CLIPPED, not that it is missing.
//
//   uGhostContrast — squeezes toward mid-grey, leaving (1-contrast)/2 of
//     headroom at BOTH ends. Costs contrast across the whole image. This is
//     the lever that works on a passive display, where nothing cancels and so
//     there is no clipping to relieve.
//   uGhostLift     — raises the black floor and leaves white alone.
//     Cancellation clips at the BOTTOM (subtracting the opposite eye drives
//     dark pixels below zero), so this targets that directly instead of
//     spending most of its effect squeezing highlights that were never the
//     problem. The literature calls the resulting margin "foot-room".
//
// The remap runs in LINEAR light (plain pow(2.2), not the piecewise sRGB
// curve) because that is the space a cancelling display's own correction runs
// in, and the two have to agree — pivoting in the wrong space makes things
// worse rather than better. Applied LAST, after the mode's own colour math
// (including the anaglyph matrix), so what gets compressed is what actually
// reaches the display.
//
// Deliberately global and fixed, not adaptive. Both obvious refinements have
// been tried and both fail structurally: ghosting IS inter-eye difference, so
// any spatially localized correction applied to one eye and not the other ADDS
// inter-eye difference and leaks in turn; and a per-frame content-derived
// scalar, while sound (it applies identically to both eyes everywhere), makes
// the whole image visibly pump as content changes. A fixed slider value is
// less distracting than a correct but moving one.
#define STEREO_FS_COMMON \
    "uniform sampler2D uTexL;\n" \
    "uniform sampler2D uTexR;\n" \
    "uniform float uGhostContrast;\n" \
    "uniform float uGhostLift;\n" \
    "in vec2 vUV;\n" \
    "out vec4 fragColor;\n" \
    "vec3 ghostReduce(vec3 c) {\n" \
    "  if (uGhostContrast == 1.0 && uGhostLift == 0.0) return c;\n" \
    "  vec3 lin = pow(clamp(c, 0.0, 1.0), vec3(2.2));\n" \
    "  lin = (lin - 0.5) * uGhostContrast + 0.5;\n" \
    "  lin = lin * (1.0 - uGhostLift) + uGhostLift;\n" \
    "  return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.2));\n" \
    "}\n"

// --- Side-by-Side: left half = L eye, right half = R eye.
#define STEREO_FS_SBS \
    STEREO_FS_COMMON \
    "void main(void) {\n" \
    "  if (vUV.x < 0.5) {\n" \
    "    fragColor = texture(uTexL, vec2(vUV.x * 2.0, vUV.y));\n" \
    "  } else {\n" \
    "    fragColor = texture(uTexR, vec2((vUV.x - 0.5) * 2.0, vUV.y));\n" \
    "  }\n" \
    "  fragColor.rgb = ghostReduce(fragColor.rgb);\n" \
    "}\n"

// --- Top-and-Bottom: top half of screen = L eye, bottom half = R eye.
// The shared vertex shader flips V so vUV.y = 0 at screen-top, 1 at screen-
// bottom; consequently L is selected when vUV.y < 0.5.
#define STEREO_FS_TAB \
    STEREO_FS_COMMON \
    "void main(void) {\n" \
    "  if (vUV.y < 0.5) {\n" \
    "    fragColor = texture(uTexL, vec2(vUV.x, vUV.y * 2.0));\n" \
    "  } else {\n" \
    "    fragColor = texture(uTexR, vec2(vUV.x, (vUV.y - 0.5) * 2.0));\n" \
    "  }\n" \
    "  fragColor.rgb = ghostReduce(fragColor.rgb);\n" \
    "}\n"

// --- Row-Interlaced: even display rows = L, odd = R.
#define STEREO_FS_ROW \
    STEREO_FS_COMMON \
    "void main(void) {\n" \
    "  int row = int(gl_FragCoord.y);\n" \
    "  if ((row & 1) == 0) {\n" \
    "    fragColor = texture(uTexL, vUV);\n" \
    "  } else {\n" \
    "    fragColor = texture(uTexR, vUV);\n" \
    "  }\n" \
    "  fragColor.rgb = ghostReduce(fragColor.rgb);\n" \
    "}\n"

// --- Column-Interlaced: even cols = L, odd = R.
#define STEREO_FS_COL \
    STEREO_FS_COMMON \
    "void main(void) {\n" \
    "  int col = int(gl_FragCoord.x);\n" \
    "  if ((col & 1) == 0) {\n" \
    "    fragColor = texture(uTexL, vUV);\n" \
    "  } else {\n" \
    "    fragColor = texture(uTexR, vUV);\n" \
    "  }\n" \
    "  fragColor.rgb = ghostReduce(fragColor.rgb);\n" \
    "}\n"

// --- Checkerboard: (col + row) parity selects eye (DLP-Link style).
#define STEREO_FS_CHECKER \
    STEREO_FS_COMMON \
    "void main(void) {\n" \
    "  int sum = int(gl_FragCoord.x) + int(gl_FragCoord.y);\n" \
    "  if ((sum & 1) == 0) {\n" \
    "    fragColor = texture(uTexL, vUV);\n" \
    "  } else {\n" \
    "    fragColor = texture(uTexR, vUV);\n" \
    "  }\n" \
    "  fragColor.rgb = ghostReduce(fragColor.rgb);\n" \
    "}\n"

// --- Anaglyph (Eric Dubois optimized red/cyan matrix).
// Each output channel pulls in BOTH eyes; small cross-eye negative terms
// suppress the color smear that naive red/cyan channel-swap produces.
#define STEREO_FS_ANAGLYPH \
    STEREO_FS_COMMON \
    "void main(void) {\n" \
    "  vec3 L = texture(uTexL, vUV).rgb;\n" \
    "  vec3 R = texture(uTexR, vUV).rgb;\n" \
    "  float r = clamp( 0.437*L.r + 0.449*L.g + 0.164*L.b\n" \
    "                  -0.011*R.r - 0.032*R.g - 0.007*R.b, 0.0, 1.0);\n" \
    "  float g = clamp(-0.062*L.r - 0.062*L.g - 0.024*L.b\n" \
    "                  +0.377*R.r + 0.761*R.g + 0.009*R.b, 0.0, 1.0);\n" \
    "  float b = clamp(-0.048*L.r - 0.050*L.g - 0.017*L.b\n" \
    "                  -0.026*R.r - 0.093*R.g + 1.234*R.b, 0.0, 1.0);\n" \
    "  fragColor = vec4(ghostReduce(vec3(r, g, b)), 1.0);\n" \
    "}\n"

#endif
