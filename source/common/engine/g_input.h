#pragma once

// These were in i_input.h, which differed between platforms and on Windows caused problems with its 
// inclusion of system specific data, so it has been separated into this platform independent file.
void I_PutInClipboard (const char *str);
FString I_GetFromClipboard (bool use_primary_selection);
void I_SetMouseCapture();
void I_ReleaseMouseCapture();
void I_GetEvent();

// Pumps only an ordered prefix of platform mouse-motion messages. Returns
// false without removing the next message when it is not safe to dispatch
// inside an already-acquired render frame.
bool I_GetLateMouseMotion();
