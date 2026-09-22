// Kiro ghost face for the watch screen. Port of buddy/src/main.cpp into a
// module: moods, blink, gaze, decorations, plus timed/held mood overrides so
// key mappings can flash an expression.
#pragma once
#include <cstdint>

namespace face {

enum Mood : uint8_t { SLEEP, IDLE, THINKING, WORKING, WAITING, QUESTION, ERROR, CELEBRATE, LISTENING, DIZZY, N };

const char* name(Mood m);                 // "sleep", "idle", ... (protocol keys)
int  fromName(const char* key);           // -1 if unknown (accepts buddy aliases)

void begin();                             // allocate sprite (PSRAM on device)
void setBase(Mood m);                     // long-lived mood (idle / sleep by connection state)
// Temporary override: ms > 0 reverts to base after ms; ms == 0 holds until release(tag).
void set(Mood m, uint32_t ms, const char* tag = "");
void release(const char* tag);            // end a held override started with the same tag
Mood current();
void setLevel(float rms01);              // 0..1 microphone level, drives the LISTENING animation

// Draw one frame if due. Status footer lines are drawn under the ghost.
void tick(const char* footer1, const char* footer2, bool force = false);
void invalidate();                        // footer changed

} // namespace face
