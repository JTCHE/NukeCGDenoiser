#include "channel_io.h"

#include <DDImage/ImagePlane.h>

#include <cstring>

using namespace DD::Image;

namespace denoiser {

bool fetchInterleaved(Op* op, const ChannelSet& channels, const Box& format,
                      float* dst, int numChans, unsigned width, unsigned height,
                      Packing packing)
{
	if (!dst || numChans <= 0 || width == 0 || height == 0)
		return false;

	std::memset(dst, 0, (size_t)width * height * numChans * sizeof(float));

	Iop* inputIop = dynamic_cast<Iop*>(op);
	if (!inputIop || channels.size() == 0)
		return false;

	// Also deliberately no tryValidate() — validating is graph-mutating and this
	// runs on a render thread. Nuke has already validated every op input by the
	// time renderStripe is reached. If one somehow was not, info() yields an empty
	// box, the intersect below fails and we report "no pixels" rather than crash.
	Box imageBounds = inputIop->info();
	imageBounds.intersect(format);
	if (imageBounds.w() <= 0 || imageBounds.h() <= 0)
		return false;

	const int fx = imageBounds.x();
	const int fy = imageBounds.y();
	const int fr = imageBounds.r();
	const int ft = imageBounds.t();
	const int imgW = fr - fx;
	const int imgH = ft - fy;

	// Deliberately no request() here. Requesting is a graph-mutating operation and
	// this runs on a render thread: doing it against the split (other-frame) inputs
	// from several threads at once is a data race that segfaults reproducibly under
	// `nuke.execute`. Everything this function needs is already declared up front in
	// DenoiserIop::getRequests(), which is the one place allowed to ask for pixels.
	ImagePlane inputPlane(imageBounds, false, channels, channels.size());
	inputIop->fetchPlane(inputPlane);

	const auto chanStride = inputPlane.chanStride();
	int planeIdx = 0;
	foreach (z, channels)
	{
		const int ci = (packing == Packing::ByColourIndex) ? (int)colourIndex(z) : planeIdx;
		if (ci >= numChans)
		{
			planeIdx++;
			continue;
		}
		const float* indata = &inputPlane.readable()[chanStride * planeIdx];
		for (int y = 0; y < imgH; y++)
		{
			for (int x = 0; x < imgW; x++)
			{
				const size_t src = (size_t)y * imgW + x;
				const size_t out = ((size_t)(fy + y) * width + (fx + x)) * numChans + ci;
				dst[out] = indata[src];
			}
		}
		planeIdx++;
	}
	return true;
}

} // namespace denoiser
