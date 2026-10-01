/*
 * Trophy - Unit test support stubs
 *
 * This file provides out-of-line destructors for QObject-based abstract
 * interfaces used by RecordingManager test builds, ensuring the generated
 * vtables are available during linking.
 */

#include "capture/IVideoCaptureSource.h"
#include "audio/IAudioSource.h"
#include "encoding/IEncoder.h"
#include "muxing/IMuxer.h"

namespace TrophyBackend {

// Interface destructors are inline defaulted in their headers.

} // namespace TrophyBackend
