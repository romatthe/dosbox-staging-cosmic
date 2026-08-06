// SPDX-FileCopyrightText:  2026 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2014 KoriTama
// SPDX-License-Identifier: GPL-2.0-or-later

#include "automap_wiz6.h"

#include <optional>

#include "cpu/paging.h"
#include "dosbox.h"
#include "utils/checks.h"
#include "utils/string_utils.h"

CHECK_NARROWING();

namespace wiz6 {

namespace {

constexpr std::string_view ExecutableName = "WROOT.EXE";

// "WMAZE\0" sits at a fixed offset into the loaded image, and the data segment
// a fixed distance in front of that. Both constants come from the original mod
// and are the anchor for every other offset in this file.
constexpr uint32_t SignatureOffset        = 0x10580;
constexpr uint32_t SignatureToDataSegment = 0x600;

// Deliberately six bytes: the terminating NUL is part of what is matched, as
// in the original.
constexpr std::string_view Signature = std::string_view("WMAZE\0", 6);

// Set once the signature check passes, and cleared whenever the guest loads
// anything else. Empty means "no game to map".
std::optional<PhysPt> data_segment_addr = {};

// DOS accepts either separator, and a drive-relative path such as
// `C:WROOT.EXE` has no separator at all.
std::string_view dos_basename(const std::string_view path)
{
	const auto separator_pos = path.find_last_of("\\/:");

	return separator_pos == std::string_view::npos
	             ? path
	             : path.substr(separator_pos + 1);
}

bool has_signature_at(const PhysPt addr)
{
	for (size_t i = 0; i < Signature.size(); ++i) {
		uint8_t byte = 0;

		if (mem_readb_checked(addr + static_cast<PhysPt>(i), &byte)) {
			return false;
		}
		if (byte != static_cast<uint8_t>(Signature[i])) {
			return false;
		}
	}

	return true;
}

} // namespace

bool DetectGame(const std::string_view name, const uint16_t loadseg,
                const uint32_t headersize)
{
	data_segment_addr = {};

	if (!iequals(dos_basename(name), ExecutableName)) {
		return false;
	}

	// The name alone proves nothing -- any program can be called
	// WROOT.EXE, and reading the map out of one that isn't Wizardry VI
	// would show nonsense. The signature is what confirms the image, and
	// where it sits is what locates the data segment.
	const auto load_addr      = PhysicalMake(loadseg, 0);
	const auto signature_addr = load_addr + SignatureOffset - headersize;

	if (!has_signature_at(signature_addr)) {
		LOG_WARNING("AUTOMAP: Program is named %s but is not Wizardry VI; ignoring it",
		            ExecutableName.data());
		return false;
	}

	data_segment_addr = signature_addr - SignatureToDataSegment;

	LOG_MSG("AUTOMAP: Detected Wizardry VI with its data segment at %04xh",
	        *data_segment_addr);

	return true;
}

} // namespace wiz6
