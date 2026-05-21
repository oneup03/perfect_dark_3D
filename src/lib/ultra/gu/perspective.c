#include <ultra64.h>

void guPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect, float near, float far, float scale)
{
	float cot;
	int	i, j;

	guMtxIdentF(mf);

	fovy *= 3.1415926f / 180.0f;
	cot = cosf(fovy * 0.5f) / sinf(fovy * 0.5f);

	mf[0][0] = cot / aspect;
	mf[1][1] = cot;
	mf[2][2] = (near + far) / (near - far);
	mf[2][3] = -1;
	mf[3][2] = (2.0f * near * far) / (near - far);
	mf[3][3] = 0;

	for (i = 0; i < 4; i++) {
		for (j = 0; j < 4; j++) {
			mf[i][j] *= scale;
		}
	}

	if (perspNorm != (u16 *) NULL) {
		if (near + far <= 2.0f) {
			*perspNorm = (u16) 0xFFFF;
		} else {
			*perspNorm = (u16) ((2.0f * 65536.0f) / (near + far));

			if (*perspNorm <= 0) {
				*perspNorm = (u16) 0x0001;
			}
		}
	}
}

void guPerspective(Mtx *m, u16 *perspNorm, float fovy, float aspect, float near, float far, float scale)
{
	float mf[4][4];

	guPerspectiveF(mf, perspNorm, fovy, aspect, near, far, scale);

	guMtxF2L(mf, m);
}

// Off-axis stereo perspective: builds the standard symmetric perspective then
// bakes in (a) an eye-translate so parallax depends on depth, and (b) a lens
// shift so the convergence-distance plane lands on the zero-disparity plane.
//
// `eyeSign` is -1 for the left eye and +1 for the right (caller honors any
// Stereo.SwapEyes preference). `iod` is the inter-ocular distance in world
// units; `convergence` is the world distance to the screen plane.
//
// Math (row-major, vertex_row * M):
//   For an eye at world x = +eyeSign * iod/2, the world appears shifted by
//   d = -eyeSign * iod/2 in camera space. We pre-multiply the projection by
//   T(d, 0, 0): mf[3][0] += d * mf[0][0]. This gives a clip-X contribution
//   that scales as d * mf[0][0] / |z|, i.e. depth-dependent parallax.
//   The lens shift in mf[2][0] adds a constant clip-X offset (after divide)
//   of -mf[2][0]; we set it to cancel the depth-dependent shift exactly at
//   z = -convergence, so geometry at the convergence distance has zero
//   disparity (sits at the screen plane).
void guStereoPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect,
		float near, float far, float scale,
		float iod, float convergence, int eyeSign)
{
	guPerspectiveF(mf, perspNorm, fovy, aspect, near, far, scale);

	if (iod == 0.0f || convergence <= 0.0f || eyeSign == 0) {
		return;
	}

	// Clamp convergence into the scene's depth range. Outside [near, far] the
	// lens shift either compresses depth (conv >> far → everything appears at
	// screen plane) or blows it up (conv << near → close objects fly off).
	// This is how scope mode (near=10, far=300) gets a sensible screen plane
	// without needing its own cvar: global convergence of 300 clamps to 270,
	// and extreme global values stay bounded.
	const float lo = near * 1.5f;
	const float hi = far * 0.9f;
	if (convergence < lo) convergence = lo;
	if (convergence > hi) convergence = hi;

	const float d = -(float)eyeSign * iod * 0.5f;
	const float eyeTranslateContrib = d * mf[0][0];

	mf[3][0] += eyeTranslateContrib;
	mf[2][0] += eyeTranslateContrib / convergence;
}
