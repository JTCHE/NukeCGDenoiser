// Single source of truth for renderer AOV naming.
//
// Every layer-name heuristic in the plugin lives here. Adding support for a new
// renderer's pass names is a one-file change with no Nuke knowledge required.
#pragma once

#include <DDImage/Channel.h>
#include <DDImage/ChannelSet.h>

#include <set>
#include <string>

namespace denoiser {

enum class AovKind
{
	Albedo,
	Normal,
	Motion,
};

// Scans `available` for a layer matching `kind`, skipping any layer named in
// `exclude`. Patterns are tried most-specific first, so a renderer-specific name
// (Arnold's "diffuse_albedo") wins over a generic one that could also match a
// different pass. On a match, `result` is filled with every channel of that layer
// and true is returned; otherwise `result` is untouched.
bool findLayer(const DD::Image::ChannelSet& available,
               const std::set<std::string>& exclude,
               AovKind kind,
               DD::Image::ChannelSet& result);

} // namespace denoiser
