// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

enum class InputRecordingCaptureMode
{
	Full,
	Screenshots,
	Savestates,
};

struct InputRecordingCaptureMarkerRange
{
	u32 first;
	u32 last;
};

using InputRecordingCaptureMarkerRanges = std::vector<InputRecordingCaptureMarkerRange>;
