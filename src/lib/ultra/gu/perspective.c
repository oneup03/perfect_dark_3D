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

// Off-axis stereo perspective, in the CLIP-SPACE separation parameterization
// (the NVIDIA / 3Dmigoto convention, `x += separation * (w - convergence)`).
//
// `eyeSign` is -1 for the left eye and +1 for the right (caller honors any
// Stereo.SwapEyes preference). `separation` is the total background disparity
// as a fraction of screen width — see port/include/stereo.h for what that
// means and how it is bounded. `convergence` is the world distance to the
// screen plane.
//
// Math (row-major, vertex_row * M):
//   mf[2][0] is the lens shear. After the perspective divide it contributes a
//   constant clip-X offset of -mf[2][0], independent of depth — so it IS the
//   at-infinity disparity, and setting it to dir * separation is the whole
//   definition of the knob. Note there is no convergence term here: the shear
//   is invariant under convergence, which is the defining property of this
//   parameterization and the reason background depth no longer changes when
//   the user moves the screen plane.
//   mf[3][0] is the eye translate (pre-multiplying by T(d,0,0) contributes
//   d * mf[0][0]). It scales as 1/|z|, so it is what makes parallax
//   depth-dependent, and it is chosen to exactly cancel the shear at
//   z = -convergence. That fixes it at dir * separation * convergence, i.e.
//   a world-space eye offset of dir * separation * tan(fovy/2) * aspect *
//   convergence — a DERIVED quantity that moves with both FoV and convergence
//   rather than the stored constant the old world-units IOD form used.
//
// What is gone versus that old form: the caller no longer multiplies the eye
// separation by tan(fovy/2)/tan(defaultFovy/2) to keep disparity stable under
// sniper zoom. That factor cancelled exactly against mf[0][0]'s own
// cot(fovy/2), so it was multiplying in a term this function immediately
// divided back out. Separation is a clip-space quantity and is FoV-independent
// by construction.
void guStereoPerspectiveF(float mf[4][4], u16 *perspNorm, float fovy, float aspect,
		float near, float far, float scale,
		float separation, float convergence, int eyeSign)
{
	guPerspectiveF(mf, perspNorm, fovy, aspect, near, far, scale);

	if (separation == 0.0f || convergence <= 0.0f || eyeSign == 0) {
		return;
	}

	// Comfort clamp only. Under the old form this was also a numerical
	// necessity — the shear was iod/(2*conv) and genuinely exploded as
	// convergence approached zero — but the shear is now just `separation` and
	// does not care. What is left is keeping the screen plane inside the
	// scene's depth range: past far, everything sits in front of the plane and
	// the whole image is pop-out; nearer than the near plane, close geometry
	// flies off. It also gives scope mode (near=10, far=300) a sensible plane
	// without its own cvar — a global convergence of 300 clamps to 270.
	// stereoEffectiveConvergence() in port/src/stereo.c mirrors this for the
	// standard near=30/far=10000 setup so CPU-side parallax agrees.
	const float lo = near * 1.5f;
	const float hi = far * 0.9f;
	if (convergence < lo) convergence = lo;
	if (convergence > hi) convergence = hi;

	// `scale` multiplies every entry guPerspectiveF wrote, including
	// mf[2][3] = -scale, so clip_w = -z * scale. Both terms below therefore
	// have to carry it too, or the perspective divide scales the disparity by
	// 1/scale. (Every current call site passes scale = 1, which is exactly why
	// this would sit undetected until one didn't.)
	const float dir = -(float)eyeSign;
	const float shear = dir * separation * scale;

	// Post-divide these give NDC_x += (shear/scale) * (convergence/depth - 1):
	// zero at depth == convergence, settling at -shear/scale as depth -> inf.
	mf[3][0] += shear * convergence;
	mf[2][0] += shear;
}
