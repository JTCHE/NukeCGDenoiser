// Single source of truth for moving pixels between a Nuke input and a flat
// interleaved float buffer.
//
// Every fetch in the plugin goes through here so the channel-ordering rule is
// written down exactly once.
#pragma once

#include <DDImage/Iop.h>
#include <DDImage/Box.h>
#include <DDImage/Channel.h>
#include <DDImage/ChannelSet.h>

namespace denoiser {

enum class Packing
{
	// Each channel goes to its colourIndex() within its own layer, NOT to its
	// position in the ChannelSet. A ChannelSet iterates in channel-number order,
	// which is red/green/blue only by luck: for a layer whose channels were
	// registered alphabetically it comes out blue/green/red, silently swapping
	// the albedo, normal or motion vector axes. This is what you want whenever
	// the destination slots mean something (r/g/b, u/v).
	ByColourIndex,
	// Channels go to consecutive slots in iteration order. This is what you want
	// when pulling an arbitrary single channel, where the source's colour index
	// is irrelevant — an alpha channel has colourIndex 3 but belongs at slot 0
	// of a one-channel buffer.
	Sequential,
};

// Fills `dst` (width*height*numChans interleaved) from `op`.
//
// `dst` is zeroed first, so pixels outside the input's bounding box read as 0
// rather than as whatever the buffer held before. Returns false if the op
// produced nothing usable — callers should treat that as "skip this frame"
// rather than as an error.
bool fetchInterleaved(DD::Image::Op* op,
                      const DD::Image::ChannelSet& channels,
                      const DD::Image::Box& format,
                      float* dst, int numChans,
                      unsigned width, unsigned height,
                      Packing packing = Packing::ByColourIndex);

} // namespace denoiser
