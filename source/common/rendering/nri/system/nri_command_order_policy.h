#pragma once

#include <cstdint>

// Permission to read after an upload on the graphics queue is not completion.
// The owner retains the original fence for cancellation and retirement.
constexpr bool NRICommandUploadPrecedesRecording(uint64_t uploadFence, uint64_t recordingFence,
    uint64_t lastSubmittedFence, bool recordingAvailable, bool abandoned, bool deviceLost)
{
    return recordingAvailable && !deviceLost && !abandoned && uploadFence != 0 && recordingFence != 0 &&
        (uploadFence == recordingFence || (uploadFence < recordingFence && uploadFence <= lastSubmittedFence));
}
