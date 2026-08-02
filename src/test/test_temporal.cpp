// Standalone check of the pure temporal core. No Nuke, no OIDN — compile and run:
//   cl /std:c++17 /EHsc /I src src/test/test_temporal.cpp src/core/temporal.cpp
// or let CMake build the `temporal_test` target.
//
// Covers the cases that are expensive to reach through Nuke: sequence boundaries,
// degenerate motion vectors, and the safe-default contract.

#include "../core/temporal.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace denoiser;

namespace {

const unsigned W = 16, H = 16;

std::vector<float> makeFrame(float base)
{
	std::vector<float> f((size_t)W * H * 3);
	for (unsigned y = 0; y < H; y++)
		for (unsigned x = 0; x < W; x++)
			for (int c = 0; c < 3; c++)
				f[((size_t)y * W + x) * 3 + c] = base + 0.01f * (float)x;
	return f;
}

std::vector<float> makeMotion(float u, float v, int chans)
{
	std::vector<float> m((size_t)W * H * chans, 0.f);
	for (size_t p = 0; p < (size_t)W * H; p++)
	{
		m[p * chans + 0] = u;
		m[p * chans + 1] = v;
		if (chans >= 4)
		{
			m[p * chans + 2] = -u;
			m[p * chans + 3] = -v;
		}
	}
	return m;
}

int failures = 0;

void check(const char* name, bool ok)
{
	std::printf("%s  %s\n", ok ? "PASS" : "FAIL", name);
	if (!ok)
		failures++;
}

bool allFinite(const std::vector<float>& v)
{
	for (float f : v)
		if (!std::isfinite(f))
			return false;
	return true;
}

bool sameAs(const std::vector<float>& a, const std::vector<float>& b, float eps = 1e-6f)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); i++)
		if (std::fabs(a[i] - b[i]) > eps)
			return false;
	return true;
}

} // namespace

int main()
{
	const std::vector<float> cur = makeFrame(0.5f);
	const std::vector<float> prev = makeFrame(0.5f);
	const std::vector<float> next = makeFrame(0.5f);
	TemporalParams params;

	// --- identical frames must come out unchanged -------------------------
	{
		auto mv = makeMotion(0.f, 0.f, 4);
		TemporalAccumulator acc(cur.data(), W, H, mv.data(), 4, params);
		acc.addFrame(-1, prev.data());
		acc.addFrame(+1, next.data());
		std::vector<float> out(cur.size());
		acc.finish(out.data());
		check("identical frames are a no-op", sameAs(out, cur, 1e-5f));
	}

	// --- no frames added: safe default is the input ------------------------
	{
		auto mv = makeMotion(1.f, 0.f, 4);
		TemporalAccumulator acc(cur.data(), W, H, mv.data(), 4, params);
		std::vector<float> out(cur.size());
		acc.finish(out.data());
		check("no frames added reproduces input", sameAs(out, cur));
	}

	// --- null motion vectors: unusable, still safe -------------------------
	{
		TemporalAccumulator acc(cur.data(), W, H, nullptr, 0, params);
		check("null motion is not usable", !acc.usable());
		std::vector<float> out(cur.size());
		acc.addFrame(-1, prev.data()); // must be ignored
		acc.finish(out.data());
		check("null motion reproduces input", sameAs(out, cur));
	}

	// --- one-channel motion layer is rejected ------------------------------
	{
		auto mv = makeMotion(1.f, 0.f, 1);
		TemporalAccumulator acc(cur.data(), W, H, mv.data(), 1, params);
		check("1-channel motion is not usable", !acc.usable());
	}

	// --- nullptr frame (sequence boundary) is skipped ----------------------
	{
		auto mv = makeMotion(1.f, 0.f, 4);
		TemporalAccumulator acc(cur.data(), W, H, mv.data(), 4, params);
		acc.addFrame(-1, nullptr); // frame before the first
		acc.addFrame(+1, next.data());
		std::vector<float> out(cur.size());
		acc.finish(out.data());
		check("missing boundary frame does not blend black", allFinite(out) && out[0] > 0.1f);
	}

	// --- strength 0 is an exact no-op --------------------------------------
	{
		auto mv = makeMotion(3.f, 2.f, 4);
		TemporalParams p0 = params;
		p0.strength = 0.f;
		TemporalAccumulator acc(cur.data(), W, H, mv.data(), 4, p0);
		acc.addFrame(-1, prev.data());
		acc.addFrame(+1, next.data());
		std::vector<float> out(cur.size());
		acc.finish(out.data());
		check("strength 0 reproduces input", sameAs(out, cur));
	}

	// --- degenerate motion vectors -----------------------------------------
	{
		const float bad[] = {std::nanf(""), INFINITY, -INFINITY, 1e30f, -1e30f};
		bool ok = true;
		for (float b : bad)
		{
			auto mv = makeMotion(b, b, 4);
			TemporalAccumulator acc(cur.data(), W, H, mv.data(), 4, params);
			acc.addFrame(-1, prev.data());
			acc.addFrame(+1, next.data());
			std::vector<float> out(cur.size());
			acc.finish(out.data());
			ok = ok && allFinite(out) && sameAs(out, cur, 1e-5f);
		}
		check("NaN/Inf/huge vectors leave the frame intact", ok);
	}

	// --- in-place finish ----------------------------------------------------
	{
		auto mv = makeMotion(1.f, 0.f, 4);
		std::vector<float> buf = cur;
		TemporalAccumulator acc(buf.data(), W, H, mv.data(), 4, params);
		acc.addFrame(+1, next.data());
		acc.finish(buf.data());
		check("finish in place stays finite", allFinite(buf));
	}

	// --- writing over every offset in the widest window ---------------------
	{
		auto mv = makeMotion(2.f, 1.f, 4);
		TemporalAccumulator acc(cur.data(), W, H, mv.data(), 4, params);
		for (int d = -3; d <= 3; d++)
			if (d != 0)
				acc.addFrame(d, (d < 0 ? prev : next).data());
		std::vector<float> out(cur.size());
		acc.finish(out.data());
		check("full +/-3 window stays finite", allFinite(out));
	}

	// --- 2-channel forward-only layer --------------------------------------
	{
		auto mv = makeMotion(1.f, 0.f, 2);
		TemporalAccumulator acc(cur.data(), W, H, mv.data(), 2, params);
		check("2-channel motion is usable", acc.usable());
		acc.addFrame(-1, prev.data());
		acc.addFrame(+1, next.data());
		std::vector<float> out(cur.size());
		acc.finish(out.data());
		check("2-channel motion stays finite", allFinite(out));
	}

	std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "OK", failures);
	return failures ? 1 : 0;
}
