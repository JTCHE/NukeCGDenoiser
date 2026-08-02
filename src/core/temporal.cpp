#include "temporal.h"
#include "limits.h"

#include <algorithm>
#include <cmath>

namespace denoiser {
namespace {

// Bilinear fetch from an interleaved float3 buffer. Returns false when the sample
// lands outside the image, so the caller can drop that frame's contribution for
// this pixel rather than replicate an edge sample across a disocclusion.
bool sampleBilinear3(const float* buf, unsigned w, unsigned h,
                     float x, float y, float* out)
{
	// The !(x >= 0) form rejects NaN as well as negatives.
	if (!(x >= 0.f) || !(y >= 0.f) || x > (float)(w - 1) || y > (float)(h - 1))
		return false;

	const int x0 = (int)x, y0 = (int)y;
	const int x1 = std::min(x0 + 1, (int)w - 1);
	const int y1 = std::min(y0 + 1, (int)h - 1);
	const float fx = x - (float)x0, fy = y - (float)y0;

	const size_t i00 = ((size_t)y0 * w + x0) * 3;
	const size_t i10 = ((size_t)y0 * w + x1) * 3;
	const size_t i01 = ((size_t)y1 * w + x0) * 3;
	const size_t i11 = ((size_t)y1 * w + x1) * 3;

	for (int c = 0; c < 3; c++)
	{
		const float top = buf[i00 + c] + (buf[i10 + c] - buf[i00 + c]) * fx;
		const float bot = buf[i01 + c] + (buf[i11 + c] - buf[i01 + c]) * fx;
		out[c] = top + (bot - top) * fy;
	}
	return true;
}

} // namespace

TemporalAccumulator::TemporalAccumulator(const float* colour, unsigned width, unsigned height,
                                         const float* mv, int mvChans,
                                         const TemporalParams& params)
	: m_colour(colour), m_mv(mv), m_width(width), m_height(height),
	  m_mvChans(mvChans), m_params(params)
{
	m_usable = (colour != nullptr && mv != nullptr && width > 0 && height > 0 &&
	            mvChans >= kMinMotionChannels);
	if (!m_usable)
		return;

	const size_t numPixels = (size_t)width * height;
	m_acc.assign(colour, colour + numPixels * 3);
	m_wsum.assign(numPixels, 1.f);
}

void TemporalAccumulator::addFrame(int delta, const float* rgb)
{
	if (!m_usable || rgb == nullptr || delta == 0)
		return;

	const bool hasBackward = (m_mvChans >= kMaxMotionChannels);
	const float vsign = m_params.flipV ? -1.f : 1.f;

	for (unsigned y = 0; y < m_height; y++)
	{
		for (unsigned x = 0; x < m_width; x++)
		{
			const size_t p = (size_t)y * m_width + x;
			const size_t p3 = p * 3;
			const size_t pm = p * m_mvChans;

			const float fu = m_mv[pm + 0];
			const float fv = m_mv[pm + 1];

			// Forward vectors carry a surface point to t+d, backward to t-d.
			float u, v;
			bool useBackward = (delta < 0 && hasBackward);
			if (useBackward)
			{
				u = m_mv[pm + 2];
				v = m_mv[pm + 3];
				// A 4-channel layer whose back half is zero padding rather than
				// real data: a pixel that moves forward cannot also be stationary
				// backwards. Fall back to -forward instead of blending an unwarped
				// frame, which is worse than not blending at all.
				if (u == 0.f && v == 0.f && (fu != 0.f || fv != 0.f))
				{
					u = fu;
					v = fv;
					useBackward = false;
				}
			}
			else
			{
				u = fu;
				v = fv;
			}
			if (!std::isfinite(u) || !std::isfinite(v))
				continue;

			// A backward vector already points into the past so it takes the
			// magnitude; a negated forward vector takes the signed delta.
			const float step = useBackward ? (float)std::abs(delta) : (float)delta;
			const float sx = (float)x + u * m_params.mvScale * step;
			const float sy = (float)y + v * m_params.mvScale * vsign * step;

			float c[3];
			if (!sampleBilinear3(rgb, m_width, m_height, sx, sy, c))
				continue;

			// 3x3 neighbourhood range of the current frame around p.
			float lo[3] = {m_colour[p3 + 0], m_colour[p3 + 1], m_colour[p3 + 2]};
			float hi[3] = {lo[0], lo[1], lo[2]};
			for (int dy = -1; dy <= 1; dy++)
			{
				const int ny = (int)y + dy;
				if (ny < 0 || ny >= (int)m_height)
					continue;
				for (int dx = -1; dx <= 1; dx++)
				{
					const int nx = (int)x + dx;
					if (nx < 0 || nx >= (int)m_width)
						continue;
					const size_t q3 = ((size_t)ny * m_width + nx) * 3;
					for (int ch = 0; ch < 3; ch++)
					{
						lo[ch] = std::min(lo[ch], m_colour[q3 + ch]);
						hi[ch] = std::max(hi[ch], m_colour[q3 + ch]);
					}
				}
			}

			for (int ch = 0; ch < 3; ch++)
				m_acc[p3 + ch] += std::clamp(c[ch], lo[ch], hi[ch]);
			m_wsum[p] += 1.f;
		}
	}
}

void TemporalAccumulator::finish(float* out) const
{
	const size_t numPixels = (size_t)m_width * m_height;

	// Inactive path returns the input unchanged rather than erroring or zeroing.
	if (!m_usable)
	{
		if (out != m_colour && m_colour != nullptr)
			std::copy(m_colour, m_colour + numPixels * 3, out);
		return;
	}

	const float strength = std::clamp(m_params.strength, 0.f, 1.f);
	for (size_t p = 0; p < numPixels; p++)
	{
		const float inv = 1.f / m_wsum[p];
		for (int ch = 0; ch < 3; ch++)
		{
			const size_t i = p * 3 + ch;
			const float orig = m_colour[i];
			out[i] = orig + (m_acc[i] * inv - orig) * strength;
		}
	}
}

} // namespace denoiser
