#include "aov_patterns.h"

#include <regex>
#include <vector>

using namespace DD::Image;

namespace denoiser {
namespace {

const std::vector<std::regex> kAlbedoPatterns = {
	std::regex("diffuse[_ ]?albedo", std::regex::icase),       // Arnold
	std::regex("denois(e|ing)[_ ]?albedo", std::regex::icase), // Cycles denoiser
	std::regex("\\balbedo\\b", std::regex::icase),             // Karma, Redshift, V-Ray, Corona
	std::regex("diff(use)?[_ ]?col(or)?", std::regex::icase),  // Cycles "DiffCol"
};

const std::vector<std::regex> kNormalPatterns = {
	std::regex("denois(e|ing)[_ ]?normal", std::regex::icase), // Cycles denoiser
	std::regex("normals?", std::regex::icase),                 // Normal/Normals/WorldNormals
	std::regex("^nn?$", std::regex::icase),                    // Arnold "N", RenderMan "Nn"
};

const std::vector<std::regex> kMotionPatterns = {
	std::regex("motion[_ ]?vectors?", std::regex::icase), // Arnold "motionvector"
	std::regex("\\bmotion\\b", std::regex::icase),        // Nuke's own motion layer
	std::regex("\\bvelocity\\b", std::regex::icase),      // V-Ray, Redshift
	std::regex("^vector$", std::regex::icase),            // Cycles "Vector"
	std::regex("forward", std::regex::icase),             // forward.u / forward.v
};

const std::vector<std::regex>& patternsFor(AovKind kind)
{
	switch (kind)
	{
	case AovKind::Albedo:
		return kAlbedoPatterns;
	case AovKind::Normal:
		return kNormalPatterns;
	case AovKind::Motion:
	default:
		return kMotionPatterns;
	}
}

} // namespace

bool findLayer(const ChannelSet& available, const std::set<std::string>& exclude,
               AovKind kind, ChannelSet& result)
{
	std::vector<std::string> layers;
	std::set<std::string> seen;
	foreach (z, available)
	{
		const char* name = getLayerName(z);
		if (!name || exclude.count(name) || !seen.insert(name).second)
			continue;
		layers.push_back(name);
	}

	for (const auto& pattern : patternsFor(kind))
	{
		for (const auto& layer : layers)
		{
			if (!std::regex_search(layer, pattern))
				continue;
			result = ChannelSet();
			foreach (z, available)
			{
				const char* name = getLayerName(z);
				if (name && layer == name)
					result += z;
			}
			return true;
		}
	}
	return false;
}

} // namespace denoiser
